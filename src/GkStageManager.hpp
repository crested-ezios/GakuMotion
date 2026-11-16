#pragma once

// =============================================================================
// GkStageManager.hpp — 场景与舞台管理器（背景隐藏、纯色绿幕与多舞台切换）
// =============================================================================
// 架构定位（阶段 1）：
//   1. 场景资源枚举与预设定义：内置官方已实装舞台（env_3d_*）及多种摄影纯色幕布；
//   2. 场景节点探测与即时隐藏：自适应检索当前场景的背景 Root 节点，通过 SetActive(false)
//      实现无损、可撤销的背景隐藏；
//   3. 纯色绿幕（Chroma Key）：支持通过相机 SolidColor 清除重写与底色微调，
//      支持实时调色（经典纯绿、纯蓝、黑幕、纯白、中性灰等）；
//   4. 严格线程纪律：ImGui / GUI 线程仅通过 StageSettings / GuiCommand 发起指令，
//      所有 Unity API 调用（GameObject / Camera / Transform）一律在 Unity 主线程安全执行。
// =============================================================================

#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include "GkStageCleanup.hpp"
#include "GkStageLoadPolicy.hpp"

namespace GakuMotion
{
	// -----------------------------------------------------------------------------
	// 1. 舞台与幕布预设
	// -----------------------------------------------------------------------------
	struct StagePreset
	{
		const char* key;
		const char* displayName;
		const char* description;
		bool isChromaScreen;
		float defaultColor[4]; // RGBA
	};

	static const StagePreset kDefaultStagePresets[] = {
		{ "native", "原生场景 (换衣间 / 摄影棚)", "保留游戏原生背景与环境光", false, {0.0f, 0.0f, 0.0f, 1.0f} },
		{ "green_screen", "纯色绿幕 (Chroma Green)", "标准纯绿 (#00FF00)，适合视频抠像与特效合成", true, {0.0f, 1.0f, 0.0f, 1.0f} },
		{ "blue_screen", "纯色蓝幕 (Chroma Blue)", "纯蓝底色 (#0000FF)，适合包含绿色服饰/道具时抠像", true, {0.0f, 0.0f, 1.0f, 1.0f} },
		{ "black_studio", "暗房黑幕 (Studio Black)", "纯黑底色 (#000000)，高对比度突出偶像动作与光效", true, {0.0f, 0.0f, 0.0f, 1.0f} },
		{ "white_studio", "纯白摄影棚 (Studio White)", "纯白高亮背景 (#FFFFFF)，适合通透轻快的摄影展示", true, {1.0f, 1.0f, 1.0f, 1.0f} },
		{ "gray_studio", "摄影中性灰 (18% Gray)", "标准中性灰底 (#2E2E2E)，色彩还原准确、不伤眼", true, {0.18f, 0.18f, 0.18f, 1.0f} },
		{ "env_3d_live_all001-00-noon", "学园通用Live舞台 (通用大舞台)", "【官方内置】学园标准 Live 舞台资产", false, {0.0f, 0.0f, 0.0f, 1.0f} },
		{ "env_3d_prod_hifstage-00-00-noon", "初星舞台 (HIF Stage)", "【官方内置】初星祭专属华丽舞台", false, {0.0f, 0.0f, 0.0f, 1.0f} },
		{ "env_3d_live_courtyard-00-00-noon", "中庭Live舞台 (日中)", "【官方内置】学园中庭露天舞台", false, {0.0f, 0.0f, 0.0f, 1.0f} },
		{ "env_3d_live_courtyard-00-00-evening", "中庭Live舞台 (傍晚夕阳)", "【官方内置】夕阳余晖下的学园中庭", false, {0.0f, 0.0f, 0.0f, 1.0f} },
		{ "env_3d_lesson_danceroom-00-00-noon", "舞蹈练习室 (Dance Room)", "【官方内置】学园专业舞蹈排练厅", false, {0.0f, 0.0f, 0.0f, 1.0f} },
		{ "env_3d_home_classroom-00-00-noon", "普通教室 (白昼)", "【官方内置】偶像日常生活教室", false, {0.0f, 0.0f, 0.0f, 1.0f} },
		{ "env_3d_home_classroom-00-00-evening", "普通教室 (夕阳)", "【官方内置】放学后染上暮色的教室", false, {0.0f, 0.0f, 0.0f, 1.0f} }
	};
	static constexpr int kStagePresetCount = static_cast<int>(sizeof(kDefaultStagePresets) / sizeof(kDefaultStagePresets[0]));

	// -----------------------------------------------------------------------------
	// 2. 场景与绿幕设置（线程安全副本）
	// -----------------------------------------------------------------------------
	struct StageSettings
	{
		bool muteNativeSoundEffects{false};
		bool hideBackground{false};
		bool greenScreenActive{false};
		float greenScreenColor[4]{0.0f, 1.0f, 0.0f, 1.0f}; // RGBA
		float screenDistance{4.0f};      // 幕布在角色后方距离 (米)
		float screenScale{35.0f};         // 幕布宽高 (米)
		float screenHeightOffset{1.0f};   // 垂直中心偏移 (米)
		bool overrideCameraClear{false};  // 相机 SolidColor 清除重写 (仅在纯色幕布激活时生效)
		int selectedPresetIndex{0};       // 预设索引
		std::string customStageKey;       // 自定义舞台 Key
		std::string loadReplacementKey;   // 下次原生摄影背景载入时替换；空值保留原生资源

		// 舞台位姿调整 (米 / 角度)
		float stageFloorYOffset{0.0f};    // 舞台地面高度微调 (-2.0m ~ +2.0m)
		float stageYawOffset{0.0f};       // 舞台水平朝向旋转 (-180° ~ +180°)
		float stagePosXOffset{0.0f};      // 舞台水平 X 偏移 (-10.0m ~ +10.0m)
		float stagePosZOffset{0.0f};      // 舞台水平 Z 偏移 (-10.0m ~ +10.0m)

		// 光影与场景节点控制
		bool lightingEffectsEnabled{true}; // 合并后的“光影效果”开关
		bool customNodeControl{false};     // “自定义场景节点”手动控制模式

		bool operator==(const StageSettings& rhs) const noexcept
		{
			return muteNativeSoundEffects == rhs.muteNativeSoundEffects &&
			       hideBackground == rhs.hideBackground &&
			       greenScreenActive == rhs.greenScreenActive &&
			       greenScreenColor[0] == rhs.greenScreenColor[0] &&
			       greenScreenColor[1] == rhs.greenScreenColor[1] &&
			       greenScreenColor[2] == rhs.greenScreenColor[2] &&
			       greenScreenColor[3] == rhs.greenScreenColor[3] &&
			       std::fabs(screenDistance - rhs.screenDistance) < 1e-4f &&
			       std::fabs(screenScale - rhs.screenScale) < 1e-4f &&
			       std::fabs(screenHeightOffset - rhs.screenHeightOffset) < 1e-4f &&
			       overrideCameraClear == rhs.overrideCameraClear &&
			       selectedPresetIndex == rhs.selectedPresetIndex &&
			       customStageKey == rhs.customStageKey &&
			       loadReplacementKey == rhs.loadReplacementKey &&
			       std::fabs(stageFloorYOffset - rhs.stageFloorYOffset) < 1e-4f &&
			       std::fabs(stageYawOffset - rhs.stageYawOffset) < 1e-4f &&
			       std::fabs(stagePosXOffset - rhs.stagePosXOffset) < 1e-4f &&
			       std::fabs(stagePosZOffset - rhs.stagePosZOffset) < 1e-4f &&
			       lightingEffectsEnabled == rhs.lightingEffectsEnabled &&
			       customNodeControl == rhs.customNodeControl;
		}

		bool operator!=(const StageSettings& rhs) const noexcept { return !(*this == rhs); }
	};

	// -----------------------------------------------------------------------------
	// 3. 检测到的场景节点候选（用于 UI 调试与单独隐藏）
	// -----------------------------------------------------------------------------
	struct StageCandidate
	{
		void* gameObject{nullptr};
		void* transform{nullptr};
		int instanceId{0};
		uint64_t generation{0};
		std::string name;
		bool originalActive{true};
		bool currentlyActive{true};
		bool userMuted{false};
		bool injectionTouched{false}; // Includes setters that may mutate before returning failure.
		bool originalStateConfirmed{true};
		bool isIdentifiedBackground{false};
		bool isColliderLocked{false};

		// 树状层级与光影分类元数据
		std::string displayName;    // 树状展示节点短名（若为空则回退到 name）
		int parentIndex{-1};        // candidates 容器中的父级索引，根节点为 -1
		int depth{0};               // 深度：根节点为 0，子级为 1..kMaxTreeDepth
		bool hasChildren{false};    // 是否存在子节点 (parentIndex == this index)
		bool isLightingRoot{false}; // 节点本身是否为 "Lighting&PostEffects"
		bool isLightingNode{false}; // 节点是否处于任意 "Lighting&PostEffects" 之下
	};

	// -----------------------------------------------------------------------------
	// 4. Unity 原生函数指针绑定
	// -----------------------------------------------------------------------------
	struct StageUnityApi
	{
		using SetActiveFn = bool (*)(void* go, bool value);
		using GetActiveSelfFn = bool (*)(void* go);
		using CameraGetMainFn = void* (*)();
		using CameraSetClearFlagsFn = void (*)(void* camera, int flags);
		using CameraGetClearFlagsFn = int (*)(void* camera);
		using CameraSetBgColorFn = void (*)(void* camera, const float* color);
		using CameraGetBgColorFn = void (*)(void* camera, float* outColor);
		using SetAmbientLightFn = void (*)(const float* color);
		using GetTransformParentFn = void* (*)(void* transform);
		using GetTransformChildCountFn = int (*)(void* transform);
		using GetTransformChildFn = void* (*)(void* transform, int index);
		using GetComponentTransformFn = void* (*)(void* comp);
		using GetComponentGoFn = void* (*)(void* comp);
		using GetObjectNameFn = std::string (*)(void* obj);
		using SetWorldPosFn = void (*)(void* transform, float x, float y, float z);
		using SetWorldRotFn = void (*)(void* transform, float x, float y, float z, float w);
		using GetWorldPosFn = void (*)(void* transform, float* outPos);
		using LoadBackgroundFn = bool (*)(const char* stageKey);
		using UnloadBackgroundFn = bool (*)();
		using SetStageTransformFn = void (*)(float x, float y, float z, float rotY);
		using SetLoadedStageRootFn = void (*)(void* go);
		using FindSceneRootsFn = std::vector<void*> (*)();

		SetActiveFn fnSetActive{nullptr};
		GetActiveSelfFn fnGetActiveSelf{nullptr};
		bool (*fnTryGetActiveSelf)(void*, bool&){nullptr};
		CameraGetMainFn fnCameraGetMain{nullptr};
		CameraSetClearFlagsFn fnCameraSetClearFlags{nullptr};
		CameraGetClearFlagsFn fnCameraGetClearFlags{nullptr};
		CameraSetBgColorFn fnCameraSetBgColor{nullptr};
		CameraGetBgColorFn fnCameraGetBgColor{nullptr};
		SetAmbientLightFn fnSetAmbientLight{nullptr};
		GetTransformParentFn fnGetParent{nullptr};
		GetTransformChildCountFn fnGetChildCount{nullptr};
		GetTransformChildFn fnGetChild{nullptr};
		GetComponentTransformFn fnGetComponentTransform{nullptr};
		GetComponentGoFn fnGetGameObject{nullptr};
		GetObjectNameFn fnGetObjectName{nullptr};
		SetWorldPosFn fnSetWorldPos{nullptr};
		SetWorldRotFn fnSetWorldRot{nullptr};
		GetWorldPosFn fnGetWorldPos{nullptr};
		LoadBackgroundFn fnLoadBackground{nullptr};
		UnloadBackgroundFn fnUnloadBackground{nullptr};
		SetStageTransformFn fnSetStageTransform{nullptr};
		SetLoadedStageRootFn fnSetLoadedStageRoot{nullptr};
		FindSceneRootsFn fnFindSceneRoots{nullptr};
		bool (*fnBeginAccess)(){nullptr};
		void (*fnEndAccess)(){nullptr};
		int (*fnObjectId)(void*){nullptr};
		uint64_t (*fnGeneration)(){nullptr};
		bool (*fnMatches)(void*, int, uint64_t){nullptr};
		void (*fnAbandonScene)(){nullptr};
		// The bridge re-enumerates the exact mounted scene for each call. An
		// unfinished asynchronous load must report false when hiding is requested.
		bool (*fnSetMountedStageActive)(bool){nullptr};
		bool (*fnIsMountedStageObject)(void*){nullptr};
	};

	struct StageAccessScope
	{
		StageUnityApi& api;
		bool ready;
		explicit StageAccessScope(StageUnityApi& value) : api(value), ready(!api.fnBeginAccess || api.fnBeginAccess()) {}
		~StageAccessScope() { if (ready && api.fnEndAccess) api.fnEndAccess(); }
	};

	// -----------------------------------------------------------------------------
	// 5. StageManager 主管理器
	// -----------------------------------------------------------------------------
	class StageManager
	{
	public:
		StageSettings settings;
		std::vector<StageCandidate> candidates;
		std::string currentSceneName;
		std::string statusNotice;

		// 依赖的 Unity API 桥接
		StageUnityApi api;

		// 运行时状态
		bool candidatesDiscovered{false};
		bool backgroundCurrentlyHidden{false};
		bool lightingCurrentlyEnabled{true};
		bool cameraStateSaved{false};
		bool cameraOriginalStateConfirmed{false};
		void* savedCameraToken{nullptr}; // Equality only; use a fresh main-camera reference.
		bool cameraRestoreRequested{false};
		bool injectionEnabled{false};
		bool suspensionRestored{true};
		bool visibilityRefreshRequested{true};
		int savedClearFlags{1};
		float savedBgColor[4]{0.0f, 0.0f, 0.0f, 1.0f};

		// 官方 / 自定义舞台挂载运行时状态
		std::string mountedStageKey;
		std::string pendingStageKey;
		bool stageLoadRequested{false};
		bool stageUnloadRequested{false};
		bool stageMountFailed{false};
		void* mountedStageRootGo{nullptr};

		// 线程锁
		mutable std::mutex stageMutex;

		StageManager() = default;

		std::string GetLoadReplacementKey() const
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			return settings.loadReplacementKey;
		}

		bool HasCustomizations() const
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			if (settings.hideBackground || settings.greenScreenActive || settings.overrideCameraClear ||
				!settings.lightingEffectsEnabled || settings.customNodeControl ||
				!settings.customStageKey.empty() || !mountedStageKey.empty() || stageLoadRequested ||
				stageUnloadRequested || backgroundCurrentlyHidden || !lightingCurrentlyEnabled || cameraStateSaved) return true;
			for (const auto& c : candidates) if (c.injectionTouched || c.userMuted || c.currentlyActive != c.originalActive) return true;
			return false;
		}

		// Unity main thread only. This does not edit the user's desired settings.
		// Call false before a capture and use the return value as a capture gate.
		bool SetInjectionEnabled(bool enabled, void* camera = nullptr)
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			if (enabled && !injectionEnabled && !suspensionRestored)
			{
				suspensionRestored = RestoreForSuspension(camera);
				if (!suspensionRestored) return false;
			}
			if (enabled && !injectionEnabled) visibilityRefreshRequested = true;
			injectionEnabled = enabled;
			if (!enabled) suspensionRestored = RestoreForSuspension(camera);
			return enabled || suspensionRestored;
		}

		// A new actor in the same Scene does not destroy the background manager
		// or the additive stage. Restore, then refresh identities without abandoning
		// the mounted-stage token, queued intent, or exact-identity mute choices.
		bool OnActorSetChanged(void* activeActor)
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			injectionEnabled = false;
			suspensionRestored = RestoreForSuspension(nullptr);
			if (!suspensionRestored) return false;
			candidatesDiscovered = false;
			DiscoverCandidates(activeActor, currentSceneName);
			visibilityRefreshRequested = true;
			return true;
		}

		void ApplySettings(const StageSettings& newSettings)
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			StageSettings sanitized = newSettings;
			const bool invalidReplacement = !sanitized.loadReplacementKey.empty() &&
				!stage_load::IsTarget(sanitized.loadReplacementKey);
			if (invalidReplacement) sanitized.loadReplacementKey.clear();
			const bool replacementChanged = settings.loadReplacementKey != sanitized.loadReplacementKey;
			if (settings.customStageKey != newSettings.customStageKey) stageMountFailed = false;
			const bool hideChanged = (settings.hideBackground != newSettings.hideBackground);
			const bool lightingChanged = (settings.lightingEffectsEnabled != newSettings.lightingEffectsEnabled);
			const bool customChanged = (settings.customNodeControl != newSettings.customNodeControl);
			const bool chromaDisabled = (settings.greenScreenActive && !newSettings.greenScreenActive) ||
			                            (settings.overrideCameraClear && !newSettings.overrideCameraClear);
			settings = sanitized;
			if (invalidReplacement)
				statusNotice = "不支持的舞台替换目标；下次载入使用原生摄影背景";
			else if (replacementChanged)
				statusNotice = settings.loadReplacementKey.empty()
					? "已选择原生背景；下次原生切换背景或重新进入摄影时恢复"
					: "已选择下次载入替换舞台: " + settings.loadReplacementKey;
			if (customChanged)
			{
				if (settings.customNodeControl)
				{
					// 进入自定义节点控制：不重置 currentlyActive，继承当前画面节点状态，停止自动刷新
					visibilityRefreshRequested = false;
				}
				else
				{
					// 退出自定义节点控制：清除所有候选项的 userMuted
					for (auto& c : candidates)
					{
						c.userMuted = false;
					}
					if (injectionEnabled)
					{
						// 先将手动修改过的非背景、非光影节点恢复至 originalActive
						StageAccessScope access(api);
						if (access.ready && api.fnSetActive)
						{
							for (auto& c : candidates)
							{
								if (c.isColliderLocked || IsActorOrProtectedNode(c.name) || !c.gameObject) continue;
								if (api.fnMatches && !api.fnMatches(c.gameObject, c.instanceId, c.generation)) continue;
								if (!c.isIdentifiedBackground && !c.isLightingNode && !c.isLightingRoot && c.injectionTouched)
								{
									if (api.fnSetActive(c.gameObject, c.originalActive))
									{
										c.currentlyActive = c.originalActive;
										c.injectionTouched = false;
									}
								}
							}
						}
						if (ApplyBackgroundVisibility(settings.hideBackground))
							backgroundCurrentlyHidden = settings.hideBackground;
						if (ApplyLightingVisibility(settings.lightingEffectsEnabled))
							lightingCurrentlyEnabled = settings.lightingEffectsEnabled;
					}
					visibilityRefreshRequested = true;
				}
			}
			else if (!settings.customNodeControl)
			{
				if (hideChanged)
				{
					if (injectionEnabled)
					{
						if (ApplyBackgroundVisibility(settings.hideBackground))
							backgroundCurrentlyHidden = settings.hideBackground;
					}
					else
					{
						visibilityRefreshRequested = true;
					}
				}
				if (lightingChanged)
				{
					if (injectionEnabled)
					{
						if (ApplyLightingVisibility(settings.lightingEffectsEnabled))
							lightingCurrentlyEnabled = settings.lightingEffectsEnabled;
					}
					else
					{
						visibilityRefreshRequested = true;
					}
				}
			}
			if (chromaDisabled && injectionEnabled)
			{
				ForceRestoreCameraClear(api.fnCameraGetMain ? api.fnCameraGetMain() : nullptr);
			}
		}

		void RequestMountStage(const std::string& stageKey)
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			if (stageKey.empty()) return;
			stageMountFailed = false;
			settings.hideBackground = true;
			settings.greenScreenActive = false;
			settings.overrideCameraClear = false;
			if (injectionEnabled && ApplyBackgroundVisibility(true))
				backgroundCurrentlyHidden = true;
			if (injectionEnabled) ForceRestoreCameraClear(api.fnCameraGetMain ? api.fnCameraGetMain() : nullptr);
			settings.customStageKey = stageKey;
			pendingStageKey = stageKey;
			stageLoadRequested = true;
			statusNotice = "请求挂载舞台: " + stageKey;
		}

		void RequestUnloadStage()
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			stageUnloadRequested = true;
			stageMountFailed = false;
			settings.customStageKey.clear();
			settings.selectedPresetIndex = 0; // "native"
			settings.hideBackground = false;
			settings.greenScreenActive = false;
			settings.overrideCameraClear = false;
			if (injectionEnabled && ApplyBackgroundVisibility(false))
				backgroundCurrentlyHidden = false;
			lightingCurrentlyEnabled = true;
			if (injectionEnabled) ForceRestoreCameraClear(api.fnCameraGetMain ? api.fnCameraGetMain() : nullptr);
			statusNotice = "已请求卸载当前舞台并恢复原生场景与天空盒";
		}

		void ApplyPreset(int presetIndex)
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			if (presetIndex < 0 || presetIndex >= kStagePresetCount) return;
			stageMountFailed = false;
			settings.selectedPresetIndex = presetIndex;
			const auto& p = kDefaultStagePresets[presetIndex];
			if (p.isChromaScreen)
			{
				settings.customStageKey.clear();
				settings.hideBackground = true;
				settings.greenScreenActive = true;
				settings.overrideCameraClear = true;
				settings.greenScreenColor[0] = p.defaultColor[0];
				settings.greenScreenColor[1] = p.defaultColor[1];
				settings.greenScreenColor[2] = p.defaultColor[2];
				settings.greenScreenColor[3] = p.defaultColor[3];
				if (injectionEnabled && ApplyBackgroundVisibility(true))
					backgroundCurrentlyHidden = true;
				if (!mountedStageKey.empty())
				{
					stageUnloadRequested = true;
				}
				statusNotice = std::string("已切换幕布: ") + p.displayName;
			}
			else if (std::strcmp(p.key, "native") == 0)
			{
				settings.hideBackground = false;
				settings.greenScreenActive = false;
				settings.overrideCameraClear = false;
				settings.customStageKey.clear();
				if (injectionEnabled && ApplyBackgroundVisibility(false))
					backgroundCurrentlyHidden = false;
			lightingCurrentlyEnabled = true;
				if (!mountedStageKey.empty())
				{
					stageUnloadRequested = true;
				}
				if (injectionEnabled) ForceRestoreCameraClear(api.fnCameraGetMain ? api.fnCameraGetMain() : nullptr);
				statusNotice = "已恢复原生场景背景与天空盒";
			}
			else
			{
				// 官方 Live 舞台挂载
				settings.hideBackground = true;
				settings.greenScreenActive = false;
				settings.overrideCameraClear = false;
				if (injectionEnabled && ApplyBackgroundVisibility(true))
					backgroundCurrentlyHidden = true;
				if (injectionEnabled) ForceRestoreCameraClear(api.fnCameraGetMain ? api.fnCameraGetMain() : nullptr);
				settings.customStageKey = p.key;
				pendingStageKey = p.key;
				stageLoadRequested = true;
				statusNotice = std::string("请求挂载官方舞台: ") + p.displayName;
			}
		}

		void ToggleCandidate(int index, bool active)
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			if (index < 0 || index >= static_cast<int>(candidates.size())) return;
			if (!injectionEnabled && !settings.customNodeControl) return;
			StageAccessScope access(api);
			if (!access.ready) { statusNotice = "场景未就绪，未修改背景"; return; }
			auto& c = candidates[index];
			if (api.fnMatches && !api.fnMatches(c.gameObject, c.instanceId, c.generation))
			{
				candidatesDiscovered = false;
				statusNotice = "场景对象已变化，请刷新背景列表";
				return;
			}
			if (c.isColliderLocked)
			{
				statusNotice = "安全保护：碰撞与相机活动边界锁定开启，禁止关闭以防摄像机掉落世界";
				return;
			}
			if (IsActorOrProtectedNode(c.name))
			{
				statusNotice = "安全保护：禁止关闭角色身体或核心系统节点 (" + c.name + ")";
				return;
			}
			if (api.fnSetActive && c.gameObject)
			{
				if (!c.injectionTouched && api.fnTryGetActiveSelf)
				{
					c.originalStateConfirmed = api.fnTryGetActiveSelf(c.gameObject, c.originalActive);
					if (!c.originalStateConfirmed) { statusNotice = "无法确认原生背景状态"; return; }
					c.currentlyActive = c.originalActive;
				}
				c.injectionTouched |= active != c.originalActive;
				if (!api.fnSetActive(c.gameObject, active)) { statusNotice = "背景修改未完成"; return; }
				c.userMuted = !active;
				c.currentlyActive = active;
				if (active == c.originalActive) c.injectionTouched = false;
			}
		}

		void RescanCandidates(void* activeActor)
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			candidatesDiscovered = false;
			DiscoverCandidates(activeActor, currentSceneName);
		}

		void AbandonScene()
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			if (api.fnAbandonScene) api.fnAbandonScene();
			candidates.clear();
			candidatesDiscovered = false;
			backgroundCurrentlyHidden = false;
			lightingCurrentlyEnabled = true;
			cameraStateSaved = false;
			cameraOriginalStateConfirmed = false;
			savedCameraToken = nullptr;
			cameraRestoreRequested = false;
			currentSceneName.clear();
			mountedStageKey.clear();
			pendingStageKey.clear();
			stageLoadRequested = false;
			stageUnloadRequested = false;
			stageMountFailed = false;
			mountedStageRootGo = nullptr;
			// Persist desired stage/preset/offset settings across disallowed scenes.
			injectionEnabled = false;
			suspensionRestored = true;
			visibilityRefreshRequested = true;
		}

		// Same-scene owner replacement can retain the room. Restore only identities
		// found in a NEW engine snapshot, then let AbandonScene discard all tokens.
		void ReleaseCurrentSceneOverrides()
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			StageAccessScope access(api);
			if (!access.ready || !api.fnSetActive) return;
			for (const auto& c : candidates)
			{
				if (c.isColliderLocked || IsActorOrProtectedNode(c.name) || !c.gameObject) continue;
				if (api.fnMatches && !api.fnMatches(c.gameObject, c.instanceId, c.generation)) continue;
				if (c.isIdentifiedBackground || c.isLightingNode || c.userMuted) api.fnSetActive(c.gameObject, c.originalActive);
			}
		}

		void ForceRestoreCameraClear(void* camera)
		{
			cameraRestoreRequested = true;
			if (!camera && api.fnCameraGetMain)
			{
				camera = api.fnCameraGetMain();
			}
			if (camera)
			{
				ApplyCameraClearFlags(camera, false);
				cameraRestoreRequested = false;
			}
		}

		// -------------------------------------------------------------------------
		// 节点探测（在 Unity 主线程执行）
		// -------------------------------------------------------------------------
		void DiscoverCandidates(void* activeActor, const std::string& sceneName)
		{
			if (!api.fnGetParent || !api.fnGetChildCount || !api.fnGetChild ||
				!api.fnGetGameObject || !api.fnGetObjectName)
				return;
			StageAccessScope access(api);
			if (!access.ready) { candidates.clear(); candidatesDiscovered = false; return; }

			currentSceneName = sceneName;
			const auto previous = std::move(candidates);
			candidates.clear();
			auto preserve = [&](StageCandidate& c)
			{
				for (const auto& old : previous)
				{
					bool match = false;
					if (c.gameObject && old.gameObject == c.gameObject)
					{
						if (c.instanceId != 0 || c.generation != 0 || old.instanceId != 0 || old.generation != 0)
							match = (old.instanceId == c.instanceId && old.generation == c.generation);
						else
							match = (old.name == c.name);
					}
					else if (c.instanceId != 0 && old.instanceId == c.instanceId && old.generation == c.generation)
					{
						match = true;
					}
					if (match)
					{
						c.originalActive = old.originalActive;
						c.userMuted = old.userMuted;
						c.injectionTouched = old.injectionTouched;
						c.originalStateConfirmed = old.originalStateConfirmed;
						break;
					}
				}
			};

			// 1. 明确识别角色顶级根 Transform，彻底从场景候选中排除角色本身及肢体骨骼
			void* actorRootTr = nullptr;
			if (activeActor && api.fnGetComponentTransform)
			{
				void* cur = api.fnGetComponentTransform(activeActor);
				if (cur)
				{
					void* parent = nullptr;
					int depthLimit = 16;
					while (depthLimit-- > 0 && (parent = api.fnGetParent(cur)) != nullptr)
					{
						cur = parent;
					}
					actorRootTr = cur;
				}
			}

			// 2. 收集全场景根节点
			std::vector<void*> sceneRoots;
			if (api.fnFindSceneRoots)
			{
				sceneRoots = api.fnFindSceneRoots();
			}

			// 离线单元测试或退避降级：若未配置场景查找器，且存在 actorRootTr，则扫描其同级或子项（含严格过滤）
			if (sceneRoots.empty() && actorRootTr)
			{
				int childCount = api.fnGetChildCount(actorRootTr);
				for (int i = 0; i < childCount; ++i)
				{
					void* childTr = api.fnGetChild(actorRootTr, i);
					if (childTr) sceneRoots.push_back(childTr);
				}
			}

			// 3. 递归遍历全场景根节点并建立多层级候选树 (最多探测至深度 kMaxTreeDepth = 6)
			constexpr int kMaxTreeDepth = 6;
			constexpr int kMaxChildrenPerNode = 128;

			auto scanNode = [&](auto& self, void* tr, const std::string& parentPath, int parentIdx, int depth, bool underLighting) -> void
			{
				if (!tr || depth >= kMaxTreeDepth) return;
				void* go = api.fnGetGameObject(tr);
				if (!go) return;
				std::string shortName = api.fnGetObjectName(go);
				if (shortName.empty()) return;
				if (IsActorOrProtectedNode(shortName)) return;

				const bool isBg = IsBackgroundName(shortName);
				const bool isCol = IsColliderOrBoundNode(shortName);
				const bool isEnvGroup = (shortName.starts_with("mdl_env_") || shortName.find("env_grp") != std::string::npos);
				const bool isLRoot = (depth == 0 || !underLighting) && IsLightingRootName(shortName);
				const bool isLNode = underLighting;

				bool isColliderLocked = false;
				bool isIdentifiedBackground = false;
				if (depth == 0)
				{
					isColliderLocked = isCol || isEnvGroup;
					isIdentifiedBackground = !isCol && !isEnvGroup && !isLRoot && isBg;
				}
				else
				{
					isColliderLocked = isCol;
					isIdentifiedBackground = !isCol && !isLRoot && !isLNode;
				}

				bool active = true;
				if (api.fnGetActiveSelf) active = api.fnGetActiveSelf(go);

				const std::string fullPath = (depth == 0) ? shortName : (parentPath + " / " + shortName);

				StageCandidate c;
				c.gameObject = go;
				c.transform = tr;
				c.instanceId = api.fnObjectId ? api.fnObjectId(go) : 0;
				c.generation = api.fnGeneration ? api.fnGeneration() : 0;
				c.name = fullPath;
				c.displayName = shortName;
				c.parentIndex = parentIdx;
				c.depth = depth;
				c.hasChildren = false;
				c.isLightingRoot = isLRoot;
				c.isLightingNode = isLNode;
				c.originalActive = active;
				c.currentlyActive = active;
				c.userMuted = false;
				c.isColliderLocked = isColliderLocked;
				c.isIdentifiedBackground = isIdentifiedBackground;
				preserve(c);

				const int myIndex = static_cast<int>(candidates.size());
				if (parentIdx >= 0 && parentIdx < static_cast<int>(candidates.size()))
				{
					candidates[parentIdx].hasChildren = true;
				}
				candidates.push_back(std::move(c));

				// 捕获新挂载的舞台根节点
				if (!mountedStageKey.empty() && (api.fnIsMountedStageObject
					? api.fnIsMountedStageObject(go)
					: shortName == mountedStageKey || shortName == mountedStageKey + "(Clone)"))
				{
					mountedStageRootGo = go;
					if (api.fnSetLoadedStageRoot)
					{
						api.fnSetLoadedStageRoot(go);
					}
				}

				const bool shouldRecurse = (depth + 1 < kMaxTreeDepth) &&
					(isLRoot || underLighting || isBg || isEnvGroup || isCol ||
					 shortName.find("Scene") != std::string::npos ||
					 shortName.find("Stage") != std::string::npos ||
					 shortName.find("Room") != std::string::npos ||
					 shortName.find("parts") != std::string::npos ||
					 shortName.starts_with("env_") || shortName.starts_with("Env_") ||
					 depth > 0);

				if (shouldRecurse)
				{
					const bool nextUnderLighting = underLighting || isLRoot;
					const int childCount = api.fnGetChildCount(tr);
					for (int j = 0; j < childCount && j < kMaxChildrenPerNode; ++j)
					{
						void* childTr = api.fnGetChild(tr, j);
						if (childTr)
						{
							self(self, childTr, fullPath, myIndex, depth + 1, nextUnderLighting);
						}
					}
				}
			};

			for (void* rootTr : sceneRoots)
			{
				if (!rootTr) continue;
				if (actorRootTr && rootTr == actorRootTr) continue;
				scanNode(scanNode, rootTr, "", -1, 0, false);
			}

			candidatesDiscovered = true;
		}

		// -------------------------------------------------------------------------
		// 每帧更新与应用（在 Unity 主线程 LateUpdate 执行）
		// -------------------------------------------------------------------------
		void OnLateUpdate(void* activeActor, float /*deltaTime*/, const std::string& sceneName, void* mainCamera)
		{
			std::lock_guard<std::mutex> lock(stageMutex);
			currentSceneName = sceneName;
			if (!injectionEnabled)
			{
				suspensionRestored = RestoreForSuspension(mainCamera);
				return;
			}
			if (stageMountFailed)
			{
				suspensionRestored = RestoreForSuspension(mainCamera);
				return;
			}
			if (mountedStageKey.empty() && !settings.customStageKey.empty() && !stageLoadRequested && !stageUnloadRequested)
			{
				pendingStageKey = settings.customStageKey;
				stageLoadRequested = true;
			}

			// 首次或场景重入时探测候选节点
			if (!candidatesDiscovered)
			{
				DiscoverCandidates(activeActor, sceneName);
			}

			// 1. 舞台异步挂载处理
			if (stageLoadRequested && api.fnLoadBackground)
			{
				bool success = api.fnLoadBackground(pendingStageKey.c_str());
				if (success)
				{
					mountedStageKey = pendingStageKey;
					statusNotice = "已调用官方加载管线挂载舞台: " + mountedStageKey;
					candidatesDiscovered = false; // 触发重新扫描以捕获新舞台节点
				}
				else
				{
					stageMountFailed = true;
					suspensionRestored = RestoreForSuspension(mainCamera);
					statusNotice = "舞台当前不可用（退役资源需重启游戏）；已保留设置与原生背景: " + pendingStageKey;
				}
				stageLoadRequested = false;
				if (stageMountFailed) return;
			}

			// 2. 舞台卸载处理
			if (stageUnloadRequested && api.fnUnloadBackground)
			{
				if (api.fnUnloadBackground())
				{
					mountedStageKey.clear();
					mountedStageRootGo = nullptr;
					statusNotice = "已请求卸载挂载舞台";
				}
				else statusNotice = "舞台所属场景已变化，未执行卸载";
				stageUnloadRequested = false;
				candidatesDiscovered = false;
			}

			// 3. 舞台 Transform 地基与朝向实时对齐
			if (!mountedStageKey.empty() && api.fnSetStageTransform)
			{
				api.fnSetStageTransform(
					settings.stagePosXOffset,
					settings.stageFloorYOffset,
					settings.stagePosZOffset,
					settings.stageYawOffset
				);
			}

			// 4. 背景与光影隐藏 / 恢复控制
			if (!settings.customNodeControl)
			{
				if (visibilityRefreshRequested || settings.hideBackground != backgroundCurrentlyHidden)
				{
					if (ApplyBackgroundVisibility(settings.hideBackground))
					{
						backgroundCurrentlyHidden = settings.hideBackground;
					}
				}
				if (visibilityRefreshRequested || settings.lightingEffectsEnabled != lightingCurrentlyEnabled)
				{
					if (ApplyLightingVisibility(settings.lightingEffectsEnabled))
					{
						lightingCurrentlyEnabled = settings.lightingEffectsEnabled;
					}
				}
				if ((backgroundCurrentlyHidden == settings.hideBackground) &&
					(lightingCurrentlyEnabled == settings.lightingEffectsEnabled))
				{
					visibilityRefreshRequested = false;
				}
			}
			if (!mountedStageKey.empty() && api.fnSetMountedStageActive)
				api.fnSetMountedStageActive(true);

			// 5. 相机底色与清除控制 (Chroma Key / SolidColor Clear)
			const bool shouldChroma = settings.greenScreenActive && settings.overrideCameraClear;
			if (mainCamera && shouldChroma)
			{
				ApplyCameraClearFlags(mainCamera, true);
				cameraRestoreRequested = false;
			}
			else if (mainCamera && (cameraStateSaved || cameraRestoreRequested))
			{
				ApplyCameraClearFlags(mainCamera, false);
				cameraRestoreRequested = false;
			}
		}

		static bool IsActorOrProtectedNode(const std::string& name)
		{
			if (name.empty()) return true;
			// 角色肢体与核心骨架
			if (name.find("Root_Face") != std::string::npos) return true;
			if (name.find("Root_Body") != std::string::npos) return true;
			if (name.find("Root_Hair") != std::string::npos) return true;
			if (name.find("Root_Costume") != std::string::npos) return true;
			if (name.find("Actor") != std::string::npos) return true;
			if (name.find("Chara") != std::string::npos) return true;
			if (name.find("Position") != std::string::npos) return true;
			// 相机与渲染基础设施
			if (name.find("Camera") != std::string::npos) return true;
			if (name.find("Cinemachine") != std::string::npos) return true;
			// UI 与系统事件
			if (name.find("EventSystem") != std::string::npos) return true;
			if (name.find("Canvas") != std::string::npos) return true;
			if (name.find("UIRoot") != std::string::npos || name.starts_with("UI")) return true;
			// 游戏系统管理类、单例、网络与插件黑名单
			if (name.find("Manager") != std::string::npos) return true;
			if (name.find("Singleton") != std::string::npos) return true;
			if (name.find("Presenter") != std::string::npos) return true;
			if (name.find("Dispatcher") != std::string::npos) return true;
			if (name.find("Plugin") != std::string::npos) return true;
			if (name.find("Wrapper") != std::string::npos) return true;
			if (name.find("Util") != std::string::npos) return true;
			if (name.find("CRIWARE") != std::string::npos || name.find("CriWare") != std::string::npos) return true;
			if (name.find("Octo") != std::string::npos) return true;
			if (name.find("AndroidBackKey") != std::string::npos) return true;
			if (name == "API" || name.starts_with("API_")) return true;
			if (name.find("DOTween") != std::string::npos) return true;
			if (name.find("Crashlytics") != std::string::npos) return true;
			if (name.find("Collider") != std::string::npos && name.find("Actor") != std::string::npos) return true;
			return false;
		}

		static bool IsColliderOrBoundNode(const std::string& name)
		{
			if (name.empty()) return false;
			if (name.find("Col") != std::string::npos || name.find("col") != std::string::npos) return true;
			if (name.find("Collider") != std::string::npos || name.find("collider") != std::string::npos) return true;
			if (name.find("Bound") != std::string::npos || name.find("bound") != std::string::npos) return true;
			if (name.find("Trigger") != std::string::npos || name.find("trigger") != std::string::npos) return true;
			return false;
		}

		static bool IsLightingRootName(const std::string& name)
		{
			if (name.empty()) return false;
			return name == "Lighting&PostEffects" ||
			       name.starts_with("Lighting&PostEffects ") ||
			       name.starts_with("Lighting&PostEffects(");
		}

		static bool IsBackgroundName(const std::string& name)
		{
			if (name.starts_with("env_") || name.starts_with("Env_") || name.starts_with("mdl_env_") || name.find("env_grp") != std::string::npos) return true;
			if (name.find("stg-parts") != std::string::npos || name.find("stg-") != std::string::npos) return true;
			if (name.find("cmn-parts") != std::string::npos || name.find("cmn-") != std::string::npos) return true;
			if (name == "eff" || name.starts_with("eff_") || name.find("eff") != std::string::npos) return true;
			if (name == "decal" || name.starts_with("decal_") || name.find("decal") != std::string::npos) return true;
			// Lighting / Light / PostEffect decoupled from background
			if (name.find("Background") != std::string::npos || name.find("background") != std::string::npos) return true;
			if (name.find("Stage") != std::string::npos || name.find("stage") != std::string::npos) return true;
			if (name.find("Room") != std::string::npos || name.find("room") != std::string::npos) return true;
			if (name.find("FittingRoom") != std::string::npos || name.find("Dress") != std::string::npos) return true;
			if (name.find("PhotographyScene") != std::string::npos) return true;
			if (name.find("Desk") != std::string::npos || name.find("Floor") != std::string::npos || name.find("Wall") != std::string::npos) return true;
			if (name.find("Prop") != std::string::npos || name.find("Chair") != std::string::npos) return true;
			return false;
		}

		bool ApplyBackgroundVisibility(bool hide)
		{
			if (settings.customNodeControl) return true;
			if (!injectionEnabled) return false;
			StageAccessScope access(api);
			if (!access.ready || !api.fnSetActive) return false;
			if (!candidatesDiscovered) DiscoverCandidates(nullptr, currentSceneName);
			if (!candidatesDiscovered) return false;
			bool complete = true;

			for (auto& c : candidates)
			{
				if (!c.gameObject) continue;
				if (api.fnIsMountedStageObject && api.fnIsMountedStageObject(c.gameObject)) continue;
				if (c.isColliderLocked || c.isLightingRoot || c.isLightingNode) continue; // 碰撞与光影节点不随背景隐藏被关闭
				if (IsActorOrProtectedNode(c.name)) continue;
				if (c.isIdentifiedBackground || c.userMuted)
				{
					// Compare identity in a fresh, rooted snapshot before any object call.
					if (api.fnMatches && !api.fnMatches(c.gameObject, c.instanceId, c.generation))
					{
						candidatesDiscovered = false;
						complete = false;
						continue;
					}
					if (!c.injectionTouched && api.fnTryGetActiveSelf)
					{
						c.originalStateConfirmed = api.fnTryGetActiveSelf(c.gameObject, c.originalActive);
						if (!c.originalStateConfirmed) { complete = false; continue; }
						c.currentlyActive = c.originalActive;
					}
					const bool targetActive = hide ? false : (c.originalActive && !c.userMuted);
					c.injectionTouched |= targetActive != c.originalActive;
					if (api.fnSetActive(c.gameObject, targetActive))
					{
						c.currentlyActive = targetActive;
						if (targetActive == c.originalActive) c.injectionTouched = false;
					}
					else complete = false;
				}
			}
			return complete;
		}

		bool ApplyLightingVisibility(bool enabled)
		{
			if (settings.customNodeControl) return true;
			if (!injectionEnabled) return false;
			StageAccessScope access(api);
			if (!access.ready || !api.fnSetActive) return false;
			if (!candidatesDiscovered) DiscoverCandidates(nullptr, currentSceneName);
			if (!candidatesDiscovered) return false;
			bool complete = true;

			bool hasLightingChildren = false;
			for (const auto& c : candidates)
			{
				if (c.isLightingNode) { hasLightingChildren = true; break; }
			}

			for (auto& c : candidates)
			{
				if (!c.gameObject) continue;
				if (c.isColliderLocked || IsActorOrProtectedNode(c.name)) continue;

				const bool isTarget = hasLightingChildren ? c.isLightingNode : (c.isLightingRoot && !c.hasChildren);
				if (!isTarget)
				{
					// 若具有子节点的 Lighting&PostEffects 容器在开启光影时处于非活跃状态，则恢复它为活跃
					if (c.isLightingRoot && c.hasChildren && enabled)
					{
						if (api.fnMatches && !api.fnMatches(c.gameObject, c.instanceId, c.generation)) continue;
						if (!c.currentlyActive)
						{
							if (api.fnSetActive(c.gameObject, true)) c.currentlyActive = true;
						}
					}
					continue;
				}

				if (api.fnMatches && !api.fnMatches(c.gameObject, c.instanceId, c.generation))
				{
					candidatesDiscovered = false;
					complete = false;
					continue;
				}
				if (!c.injectionTouched && api.fnTryGetActiveSelf)
				{
					c.originalStateConfirmed = api.fnTryGetActiveSelf(c.gameObject, c.originalActive);
					if (!c.originalStateConfirmed) { complete = false; continue; }
					c.currentlyActive = c.originalActive;
				}
				const bool targetActive = enabled ? (c.originalActive && !c.userMuted) : false;
				c.injectionTouched |= targetActive != c.originalActive;
				if (api.fnSetActive(c.gameObject, targetActive))
				{
					c.currentlyActive = targetActive;
					if (targetActive == c.originalActive) c.injectionTouched = false;
				}
				else complete = false;
			}
			return complete;
		}

	private:
		bool RestoreForSuspension(void* camera)
		{
			bool complete = true;
			bool hasChangedCandidates = false;
			for (const auto& c : candidates)
				if (c.injectionTouched || c.currentlyActive != c.originalActive) { hasChangedCandidates = true; break; }
			if (hasChangedCandidates)
			{
				StageAccessScope access(api);
				if (!access.ready || !api.fnSetActive) complete = false;
				else for (auto& c : candidates)
				{
					if ((!c.injectionTouched && c.currentlyActive == c.originalActive) || c.isColliderLocked ||
						IsActorOrProtectedNode(c.name) || !c.gameObject) continue;
					if (!c.originalStateConfirmed) { complete = false; continue; }
					if ((api.fnMatches && !api.fnMatches(c.gameObject, c.instanceId, c.generation)) ||
						!api.fnSetActive(c.gameObject, c.originalActive)) { complete = false; continue; }
					c.currentlyActive = c.originalActive;
					c.injectionTouched = false;
				}
			}
			if (cameraStateSaved)
			{
				if (!camera && api.fnCameraGetMain) camera = api.fnCameraGetMain();
				if (!cameraOriginalStateConfirmed || !camera || camera != savedCameraToken ||
					!api.fnCameraSetClearFlags || !api.fnCameraSetBgColor ||
					!api.fnCameraGetClearFlags || !api.fnCameraGetBgColor) complete = false;
				else
				{
					// A native solid-color camera is valid. Capture restoration must use
					// its exact saved state rather than substitute a Skybox default.
					api.fnCameraSetClearFlags(camera, savedClearFlags);
					api.fnCameraSetBgColor(camera, savedBgColor);
					float actual[4];
					for (float& v : actual) v = std::numeric_limits<float>::quiet_NaN();
					api.fnCameraGetBgColor(camera, actual);
					bool restored = api.fnCameraGetClearFlags(camera) == savedClearFlags;
					for (int i = 0; i < 4; ++i)
						if (!std::isfinite(actual[i]) || std::fabs(actual[i] - savedBgColor[i]) > 1e-5f) restored = false;
					if (restored) { cameraStateSaved = false; cameraRestoreRequested = false;
						cameraOriginalStateConfirmed = false; savedCameraToken = nullptr; }
					else complete = false;
				}
			}
			if (!mountedStageKey.empty())
			{
				if (!api.fnSetMountedStageActive || !api.fnSetMountedStageActive(false)) complete = false;
			}
			if (complete) { backgroundCurrentlyHidden = false; lightingCurrentlyEnabled = true; }
			visibilityRefreshRequested = true;
			return complete;
		}

		void ApplyCameraClearFlags(void* camera, bool activateChroma)
		{
			if (!camera) return;

			if (activateChroma)
			{
				if (!cameraStateSaved)
				{
					savedCameraToken = camera;
					cameraOriginalStateConfirmed = api.fnCameraGetClearFlags && api.fnCameraGetBgColor;
					savedClearFlags = 0;
					for (float& v : savedBgColor) v = std::numeric_limits<float>::quiet_NaN();
					if (api.fnCameraGetClearFlags)
					{
						savedClearFlags = api.fnCameraGetClearFlags(camera);
					}
					if (api.fnCameraGetBgColor)
					{
						api.fnCameraGetBgColor(camera, savedBgColor);
					}
					cameraStateSaved = true;
					cameraOriginalStateConfirmed = cameraOriginalStateConfirmed && savedClearFlags > 0;
					for (float v : savedBgColor) if (!std::isfinite(v)) cameraOriginalStateConfirmed = false;
				}
				// Keep an uncertain ownership record and stop extending it to another
				// camera. Capture must not restore defaults or an unrelated camera.
				if (!cameraOriginalStateConfirmed || camera != savedCameraToken) return;

				// 2 = CameraClearFlags.SolidColor
				if (api.fnCameraSetClearFlags)
				{
					api.fnCameraSetClearFlags(camera, 2);
				}
				if (api.fnCameraSetBgColor)
				{
					api.fnCameraSetBgColor(camera, settings.greenScreenColor);
				}
			}
			else
			{
				if (cameraStateSaved)
				{
					// Ordinary preset/Stop restoration uses the same checked path.
					// Failed setters must not erase the evidence before capture.
					suspensionRestored = RestoreForSuspension(camera);
					return;
				}
				// With no owned native snapshot there is nothing to restore. Do
				// not introduce an untracked camera mutation by writing defaults.
				cameraRestoreRequested = false;
			}
		}
	};
} // namespace GakuMotion
