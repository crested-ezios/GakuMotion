#pragma once

#include "GkMotionData.hpp"
#include "GkLog.hpp"
#include "GkBlendshapePlayer.hpp"
#include "GkEyePose.hpp"
#include "GkGroundAnchor.hpp"
#include "GkRootHeightCalibration.hpp"
#include "GkBoneOverride.hpp"   // 全身骨骼旋转「自定义编辑」覆盖层
#include "GkAuxChannelModel.hpp"  // 辅助骨驱动模型：全部系数由实机录制标定（Plan A）
#include "GkWaistSupport.hpp"
#include <unordered_map>
#include <vector>
#include <array>
#include <string>
#include <cstring>
#include <cmath>
#include <functional>
#include <atomic>

namespace GakuMotion
{
	// =========================================================================
	// 辅助骨（_Roll_H 扭转骨 / _H 半角支撑骨）驱动模式 —— 实机靶向对照实验
	// =========================================================================
	// 背景（见 DevAgents/reverse_engineering/official_quartz_pipeline.md §5）：
	//   离线单测 133/133 PASS 证明公式自洽，但实机袖筒依然塌陷 —— 数学自洽 ≠ 实机正确。
	//   为叫停「闭门推导」，提供运行时可切换的实机对照开关，让肉眼事实裁决：
	//     塌陷是「消失 / 减弱 / 完全不变」，从而判定辅助骨是否真为根因。
	enum AuxDriveMode : int
	{
		// 0 = 出厂默认：由「实机录制真值」标定出来的模型（局部空间单轴写入，见 GkAuxChannelModel.hpp）
		kAuxNormal = 0,
		kAuxSilence = 1, // 绝对静默：硬锁「初始绑定局部姿态」(Local Rest Rotation)，刚性随父跟动，零独立形变
		kAuxIdentity = 2, // 绝对归零：局部旋转强制 Identity(0,0,0,1)
		kAuxReplay = 3, // 真值直灌：把官方录制下来的辅助骨四元数逐帧原样写入内存
		kAuxLegacy = 4, // 旧的自洽解算注入（100% Bend + 50% Roll + 世界空间直写），仅保留作对照
		kAuxModeCount = 5
	};

	inline const char* AuxDriveModeName(int mode) noexcept
	{
		switch (mode)
		{
		case kAuxSilence: return "SILENCE(rest-lock)";
		case kAuxIdentity: return "IDENTITY(zero)";
		case kAuxReplay: return "REPLAY(ground-truth)";
		case kAuxLegacy: return "LEGACY(world-quartz)";
		case kAuxNormal:
		default: return "NORMAL(fitted-model)";
		}
	}

	// =========================================================================
	// AuxGroundTruth —— 辅助骨官方真值直灌数据源
	// =========================================================================
	// 由 GakuRecorder（实机内存读取）导出的 aux_replay.json 喂养。
	// 文件格式（与 gkmotion 同目录）：
	//   { "fps": 60.0,
	//     "tracks": [ { "humanoid_id": 13, "channel": "roll_h"|"joint_h",
	//                   "bone": "LeftArm_Roll_H",
	//                   "quaternions": [[x,y,z,w], ...] }, ... ] }
	// quaternions 一律为「局部空间 (localRotation)」逐帧真值，写入时不再做任何变换。
	struct AuxGroundTruth
	{
		struct Track
		{
			int humanoidId{-1};
			int channel{-1}; // 0 = _Roll_H, 1 = _H
			std::string boneName;
			std::vector<Quaternion> frames;
		};

		float fps{60.0f};
		std::vector<Track> tracks;
		bool loaded{false};
		std::string sourcePath;
		std::string lastError;

		bool Empty() const noexcept { return !loaded || tracks.empty(); }

		bool Load(const std::string& path, std::string& err)
		{
			tracks.clear();
			loaded = false;
			lastError.clear();
			jsonlite::Value root;
			if (!jsonlite::ParseFile(path, root, err))
			{
				lastError = err;
				return false;
			}
			if (!root.IsObject())
			{
				lastError = path + " -> top-level must be an object";
				err = lastError;
				return false;
			}

			fps = static_cast<float>(root.FloatPath("fps", 60.0f));
			if (fps <= 0.0f) fps = 60.0f;

			if (const jsonlite::Value* tr = root.Find("tracks"))
			{
				for (size_t i = 0; i < tr->Size(); ++i)
				{
					const jsonlite::Value* t = tr->At(i);
					if (!t || !t->IsObject()) continue;
					Track tk;
					tk.humanoidId = t->IntPath("humanoid_id", -1);
					const std::string chan = t->StringPath("channel", "");
					tk.channel = (chan == "roll_h") ? 0 : ((chan == "joint_h") ? 1 : -1);
					tk.boneName = t->StringPath("bone", "");
					if (tk.humanoidId < 0 || tk.channel < 0) continue;
					if (const jsonlite::Value* qs = t->Find("quaternions"))
					{
						tk.frames.reserve(qs->Size());
						for (size_t f = 0; f < qs->Size(); ++f)
						{
							const jsonlite::Value* q = qs->At(f);
							if (!q || q->Size() < 4)
							{
								tk.frames.clear();
								break;
							}
							tk.frames.emplace_back(
								static_cast<float>(q->At(0)->AsDouble(0.0)),
								static_cast<float>(q->At(1)->AsDouble(0.0)),
								static_cast<float>(q->At(2)->AsDouble(0.0)),
								static_cast<float>(q->At(3)->AsDouble(1.0)));
						}
					}
					if (!tk.frames.empty()) tracks.push_back(std::move(tk));
				}
			}
			loaded = true;
			sourcePath = path;
			return true;
		}

		// 返回 nullptr 表示该通道无真值数据（调用方降级为静默）
		const Quaternion* Sample(int humanoidId, int channel, float timeSeconds) const noexcept
		{
			if (Empty()) return nullptr;
			for (const Track& t : tracks)
			{
				if (t.humanoidId != humanoidId || t.channel != channel) continue;
				if (t.frames.empty()) return nullptr;
				long long idx = static_cast<long long>(std::floor(timeSeconds * fps + 0.5));
				if (idx < 0) idx = 0;
				if (idx >= static_cast<long long>(t.frames.size()))
				{
					idx = static_cast<long long>(t.frames.size()) - 1;
				}
				return &t.frames[static_cast<size_t>(idx)];
			}
			return nullptr;
		}
	};

	// 手臂辅助骨链缓存：包含肩、臂、腕各关节的原生 _Roll_H 与 _H 辅助骨
	struct ArmAuxiliaryChain
	{
		void* shoulder{nullptr};
		Quaternion shoulderRestRel{0.0f, 0.0f, 0.0f, 1.0f};

		void* upperArm{nullptr};
		Quaternion upperArmRestRel{0.0f, 0.0f, 0.0f, 1.0f};
		void* upperArmH{nullptr}; // e.g. LeftArm_H (关节半角支撑)
		Quaternion upperArmHRestLocal{0.0f, 0.0f, 0.0f, 1.0f}; // 静息局部旋转
		Quaternion upperArmHRestRel{0.0f, 0.0f, 0.0f, 1.0f}; // 相对 Actor 的静息世界旋转
		void* upperArmRollH{nullptr}; // e.g. LeftArm_Roll_H (轴向扭转分担)
		Quaternion upperArmRollHRestLocal{0.0f, 0.0f, 0.0f, 1.0f}; // 静息局部旋转
		Quaternion upperArmRollHRestRel{0.0f, 0.0f, 0.0f, 1.0f}; // 相对 Actor 的静息世界旋转

		void* foreArm{nullptr};
		Quaternion foreArmRestRel{0.0f, 0.0f, 0.0f, 1.0f};
		void* foreArmH{nullptr}; // e.g. LeftForeArm_H (肘关节半角支撑)
		Quaternion foreArmHRestLocal{0.0f, 0.0f, 0.0f, 1.0f}; // 静息局部旋转
		Quaternion foreArmHRestRel{0.0f, 0.0f, 0.0f, 1.0f}; // 相对 Actor 的静息世界旋转
		void* foreArmRollH{nullptr}; // e.g. LeftForeArm_Roll_H (前臂扭转分担)
		Quaternion foreArmRollHRestLocal{0.0f, 0.0f, 0.0f, 1.0f}; // 静息局部旋转
		Quaternion foreArmRollHRestRel{0.0f, 0.0f, 0.0f, 1.0f}; // 相对 Actor 的静息世界旋转

		void* hand{nullptr};
		Quaternion handRestRel{0.0f, 0.0f, 0.0f, 1.0f};
		void* handH{nullptr}; // e.g. LeftHand_H (腕关节半角支撑)
		Quaternion handHRestLocal{0.0f, 0.0f, 0.0f, 1.0f}; // 静息局部旋转
		Quaternion handHRestRel{0.0f, 0.0f, 0.0f, 1.0f}; // 相对 Actor 的静息世界旋转

		void Reset()
		{
			shoulder = upperArm = upperArmH = upperArmRollH = nullptr;
			shoulderRestRel = upperArmRestRel = Quaternion::Identity();
			upperArmHRestLocal = upperArmHRestRel = Quaternion::Identity();
			upperArmRollHRestLocal = upperArmRollHRestRel = Quaternion::Identity();

			foreArm = foreArmH = foreArmRollH = nullptr;
			foreArmRestRel = Quaternion::Identity();
			foreArmHRestLocal = foreArmHRestRel = Quaternion::Identity();
			foreArmRollHRestLocal = foreArmRollHRestRel = Quaternion::Identity();

			hand = handH = nullptr;
			handRestRel = Quaternion::Identity();
			handHRestLocal = handHRestRel = Quaternion::Identity();
		}
	};

	// Identity tokens only: no saved pointer may be read or written until the
	// complete live chain has been reacquired from the actor's current humanoid.
	struct WaistSupportChain
	{
		void* hips{nullptr};
		void* pelvis{nullptr};
		void* upperLeg{nullptr};
		void* waistOffset{nullptr};
		void* thighOffset{nullptr};
		void* helper{nullptr};
		Vector3 savedLocalPosition;
		Quaternion savedLocalRotation;
		bool captured{false};

		bool SameTargetIdentity(const WaistSupportChain& other) const noexcept
		{
			return hips == other.hips && pelvis == other.pelvis &&
				waistOffset == other.waistOffset && helper == other.helper;
		}

		bool SameIdentity(const WaistSupportChain& other) const noexcept
		{
			return hips == other.hips && pelvis == other.pelvis && upperLeg == other.upperLeg &&
				waistOffset == other.waistOffset && thighOffset == other.thighOffset && helper == other.helper;
		}
	};

	// Structure caching bone transforms and bind poses for a specific actor
	struct IdolBoneBinding
	{
		void* actor{nullptr};
		BlendshapeBinding face;
		void* actorTransform{nullptr};
		void* rootTransform{nullptr};
		Vector3 restRootPos{0.0f, 0.0f, 0.0f}; // relative to actor
		Vector3 restActorPos{0.0f, 0.0f, 0.0f};
		Quaternion restActorRot{0.0f, 0.0f, 0.0f, 1.0f};

		// Clip-wide floor offset when clipGroundAnchored; legacy bind-pose lift
		// otherwise. bindGroundLift preserves the original fallback across songs.
		float groundLift{0.0f};
		float bindGroundLift{0.0f};
		float calibratedRootHeight{0.0f};
		bool clipGroundAnchored{false};
		bool clipBindingDirty{false};
		float lastGeometryProbeTime{-99.0f};

		// Index corresponds to clip.bones[b]
		std::vector<void*> boneTransforms;
		std::vector<uint16_t> boneHids;
		std::vector<Quaternion> restRotations; // R_rest world (actor-relative)

		// R_rest LOCAL, captured at the same instant. This is the only character-
		// dependent quantity gkclip needs, and it lives here in RAM for the
		// lifetime of the binding: nothing is written to disk, no cache is keyed on
		// character or outfit, and the clip file stays role-agnostic.
		// (DevAgents/architecture/ACTION_FORMATS.md 2.2, option A frozen in #021.)
		std::vector<Quaternion> restLocals;
		const EyeTargetFrame* eyeTargetFrame{nullptr};
		bool eyeFrameResolved{false}, eyeFrameWarningLogged{false}, eyeGainLogged{false};
		bool bindPoseCaptured{false};
		bool defensivelyMuted{false};
		bool eyePoseModified{false};
		std::array<NativeTalkBinding, 2> talks{};
		size_t boundBoneCount{0}; // 成功绑定的骨骼数（日志/面板状态展示）

		void* cachedAnimator{nullptr};
		void* savedAnimatorController{nullptr};
		bool animatorStateCaptured{false};
		bool savedAnimatorEnabled{true};
		bool animatorControllerDetached{false};
		bool animatorWasDisabled{false};
		bool animatorRebound{false};
		void* cachedPlayableDirector{nullptr};

		// 原生手臂辅助骨缓存（以原生骨骼为主、阻尼保底为辅）
		ArmAuxiliaryChain leftArmAux;
		ArmAuxiliaryChain rightArmAux;
		std::array<WaistSupportChain, 2> waistSupport;
		bool waistSupportDiagnosticLogged{false};

		// UpperChest (Spine2, hid=54) 防破坑保护
		void* upperChestTransform{nullptr};
		Quaternion upperChestRestRel{0.0f, 0.0f, 0.0f, 1.0f};
		bool upperChestInClip{false};

		void Reset()
		{
			actor = nullptr;
			face = BlendshapeBinding{};
			actorTransform = nullptr;
			rootTransform = nullptr;
			restRootPos = {0.0f, 0.0f, 0.0f};
			restActorPos = {0.0f, 0.0f, 0.0f};
			restActorRot = {0.0f, 0.0f, 0.0f, 1.0f};
			groundLift = 0.0f;
			bindGroundLift = 0.0f;
			calibratedRootHeight = 0.0f;
			clipGroundAnchored = false;
			clipBindingDirty = false;
			lastGeometryProbeTime = -99.0f;
			boneTransforms.clear();
			boneHids.clear();
			restRotations.clear();
			restLocals.clear();
			eyeTargetFrame = nullptr;
			eyeFrameResolved = eyeFrameWarningLogged = eyeGainLogged = false;
			bindPoseCaptured = false;
			defensivelyMuted = false;
			eyePoseModified = false;
			talks = {};
			boundBoneCount = 0;
			cachedAnimator = nullptr;
			savedAnimatorController = nullptr;
			animatorStateCaptured = false;
			savedAnimatorEnabled = true;
			animatorControllerDetached = false;
			animatorWasDisabled = false;
			animatorRebound = false;
			cachedPlayableDirector = nullptr;
			leftArmAux.Reset();
			rightArmAux.Reset();
			waistSupport = {};
			waistSupportDiagnosticLogged = false;
			upperChestTransform = nullptr;
			upperChestRestRel = Quaternion::Identity();
			upperChestInClip = false;
		}
	};

	class IdolController
	{
	public:
		std::unordered_map<void*, IdolBoneBinding> bindings;
		IdolBoneBinding currentBinding;

		// Records only completed original-pose Foot writes. A consumer clears the
		// actor/mask before Tick and must not infer freshness from clip time alone.
		struct FootPoseWriteStamp
		{
			uint64_t serial{0};
			void* actor{nullptr};
			unsigned mask{0};
			std::array<void*, 2> transforms{};
		} footPoseWriteStamp;

		struct EyePoseWriteStamp
		{
			uint64_t serial{0};
			void* actor{nullptr};
			unsigned mask{0};
			std::array<void*, 2> transforms{};
			std::array<Quaternion, 2> localTargets{};
		} eyePoseWriteStamp;

		// Signature callback types to interface with UnityResolve or Il2Cpp
		using GetBoneTransformFn = void* (*)(void* actor, int humanoidBoneId);
		using GetComponentTransformFn = void* (*)(void* component);
		using SetPositionFn = void (*)(void* transform, float x, float y, float z);
		using SetRotationFn = void (*)(void* transform, float x, float y, float z, float w);
		using GetPositionFn = void (*)(void* transform, float* outPos);
		using GetRotationFn = void (*)(void* transform, float* outRot);
		using SetComponentEnabledFn = void (*)(void* component, bool enabled);
		using GetComponentEnabledFn = bool (*)(void* component);
		using AnimatorRebindFn = void (*)(void* animator);
		using GetAnimatorControllerFn = void* (*)(void* animator);
		using SetAnimatorControllerFn = void (*)(void* animator, void* controller);
		using PlayablePauseFn = void (*)(void* director);
		using GetChildCountFn = int (*)(void* transform);
		using GetChildFn = void* (*)(void* transform, int index);
		using GetTransformNameFn = std::string (*)(void* transform);
		using LogFn = void (*)(const char* fmt, ...);

		// PROBE A3/A6 fire once per clip load, on the first frame that writes bones.
		// Set after each ApplyMotionToBinding() call so one run of one clip produces
		// one block of output instead of 52 lines per frame.
		//
		// It is reset at the top of every rebind (see Rebind()) rather than only at
		// load: PROBE A2 runs on each rebind while A3/A6 used to run only once per
		// PROCESS, so with three clips played in one session the A2 block and the
		// A3 block could describe two different binds. Reading restLocal from one and
		// the delta from the other produced a convincing but completely fictional
		// "the composition disagrees by 86 deg" result. A3 now also restates
		// restLocal and rVmd on the write frame (see below), so its block is
		// self-contained and no cross-block pairing is required.
		bool probeFirstFrameDone{false};

		GetBoneTransformFn fnGetBoneTransform{nullptr};
		GetComponentTransformFn fnGetComponentTransform{nullptr};
		GetComponentTransformFn fnGetTransformParent{nullptr};
		SetPositionFn fnSetWorldPosition{nullptr};
		SetRotationFn fnSetWorldRotation{nullptr};
		GetPositionFn fnGetWorldPosition{nullptr};
		GetRotationFn fnGetWorldRotation{nullptr};
		SetPositionFn fnSetLocalPosition{nullptr};
		SetRotationFn fnSetLocalRotation{nullptr};
		GetPositionFn fnGetLocalPosition{nullptr};
		GetRotationFn fnGetLocalRotation{nullptr};
		std::function<void(void*, float*)> fnGetLossyScale;
		SetComponentEnabledFn fnSetAnimatorEnabled{nullptr};
		GetComponentEnabledFn fnGetAnimatorEnabled{nullptr};
		AnimatorRebindFn fnAnimatorRebind{nullptr};
		GetAnimatorControllerFn fnGetAnimatorController{nullptr};
		SetAnimatorControllerFn fnSetAnimatorController{nullptr};
		PlayablePauseFn fnPausePlayableDirector{nullptr};
		GetChildCountFn fnGetChildCount{nullptr};
		GetChildFn fnGetChild{nullptr};
		GetTransformNameFn fnGetTransformName{nullptr};
		FaceApi faceApi;
		// Runtime supplies the photography-scene boundary. Offline users may omit
		// it; the native plugin fails closed when its scene API is unavailable.
		bool (*fnCanDriveActor)(void*){nullptr};

		bool CanDriveActor(void* actor) const
		{
			return actor && (!fnCanDriveActor || fnCanDriveActor(actor));
		}

		LogFn fnLogInfo{nullptr};
		LogFn fnLogWarn{nullptr};

		// Dynamic arm A-Pose offsets (degrees)
		float armOffsetPitch{0.0f};
		float armOffsetYaw{0.0f};
		float armOffsetRoll{0.0f};

		// If true (default), bone rotations are calibrated relative to actor-relative rest pose
		bool useRestRotations{true};
		void* cachedAnimator{nullptr};
		// Opt-in until hair/cloth and same-frame body input are verified on device.
		bool preserveAnimatorForSecondaryPhysics{false};
		bool captureCheckEnforced{false};
		bool captureRestoreUnconfirmed{false};
		bool facialEnableBaseline{true};
		bool facialMikuMouthFit{true};
		// Published-profile components default to their accepted grade only: the
		// eye map applies, the inverse-distance mouth map stays opt-in.
		bool facialMikuEyeFit{true};
		bool facialMikuFitAllowUnverified{false};
		struct FacialUiSnapshot
		{
			std::atomic<unsigned> value{0};
			FacialUiSnapshot() = default;
			FacialUiSnapshot(const FacialUiSnapshot& other) : value(other.load()) {}
			FacialUiSnapshot& operator=(const FacialUiSnapshot& other) { store(other.load()); return *this; }
			unsigned load(std::memory_order order = std::memory_order_relaxed) const { return value.load(order); }
			void store(unsigned next, std::memory_order order = std::memory_order_relaxed) { value.store(next, order); }
		};
		FacialUiSnapshot guiMikuFitSnapshot; // Coherent GUI read; preserves controller fixture copy semantics.
		float facialBaselineMultiplier{1.0f};
		FacialWeights facialWeights;
		std::string facialCharacterOverride{"auto"};

	private:
		bool experimentalWaistSupportEnabled{true};

		bool WaistSupportApisAvailable() const noexcept
		{
			return fnGetBoneTransform && fnGetChildCount && fnGetChild && fnGetTransformName &&
				fnGetWorldPosition && fnGetWorldRotation && fnGetLocalPosition &&
				fnGetLocalRotation && fnSetLocalPosition && fnSetLocalRotation && fnGetLossyScale;
		}

		void* FindExactWaistChild(void* parent, const char* name) const
		{
			if (!parent) return nullptr;
			const int count = fnGetChildCount(parent);
			if (count < 0 || count > 4096) return nullptr;
			void* found = nullptr;
			for (int index = 0; index < count; ++index)
			{
				void* child = fnGetChild(parent, index);
				if (child && fnGetTransformName(child) == name)
				{
					if (found) return nullptr; // Ambiguous names are not a binding.
					found = child;
				}
			}
			return found;
		}

		bool ResolveWaistSupportTarget(const IdolBoneBinding& b, int side, WaistSupportChain& chain) const
		{
			chain = WaistSupportChain{};
			// Restoring saved LOCAL values needs identity and setters, but no
			// scale/world-pose getters. Keep this independent of the solver gate.
			if (!fnGetBoneTransform || !fnGetChildCount || !fnGetChild || !fnGetTransformName ||
				!fnSetLocalPosition || !fnSetLocalRotation || !b.actor || !b.bindPoseCaptured ||
				!CanDriveActor(b.actor) || side < 0 || side > 1)
				return false;
			// Only dereference freshly acquired transforms. b.rootTransform and the
			// saved chain below are comparison tokens, never liveness evidence.
			chain.hips = fnGetBoneTransform(b.actor, 0);
			if (!chain.hips || chain.hips != b.rootTransform || fnGetTransformName(chain.hips) != "Hips") return false;
			chain.pelvis = FindExactWaistChild(chain.hips, "Pelvis");
			if (!chain.pelvis) return false;
			chain.waistOffset = FindExactWaistChild(chain.pelvis, side == 0 ? "LeftWaist_O" : "RightWaist_O");
			chain.helper = FindExactWaistChild(chain.waistOffset, side == 0 ? "LeftWaist_H" : "RightWaist_H");
			return chain.waistOffset && chain.helper;
		}

		bool ResolveWaistSupportReferences(const IdolBoneBinding& b, int side, WaistSupportChain& chain) const
		{
			const char* upperName = side == 0 ? "LeftUpLeg" : "RightUpLeg";
			chain.upperLeg = FindExactWaistChild(chain.pelvis, upperName);
			if (!chain.upperLeg || chain.upperLeg != fnGetBoneTransform(b.actor, side + 1)) return false;
			chain.thighOffset = FindExactWaistChild(chain.upperLeg, side == 0 ? "LeftThigh_O" : "RightThigh_O");
			return chain.thighOffset != nullptr;
		}

		bool WaistSupportScalesValid(const WaistSupportChain& chain) const
		{
			for (void* transform : {
				     chain.hips, chain.pelvis, chain.upperLeg, chain.waistOffset,
				     chain.thighOffset, chain.helper
			     })
			{
				float value[3]{0, 0, 0};
				fnGetLossyScale(transform, value);
				if (!waist_support::IsUnitScale({value[0], value[1], value[2]})) return false;
			}
			return true;
		}

		static bool WaistSupportRotationValid(const Quaternion& q) noexcept
		{
			if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) return false;
			const double norm = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w;
			return std::fabs(norm - 1.) <= 1e-3;
		}

		void RestoreWaistSupport(IdolBoneBinding& b)
		{
			for (int side = 0; side < 2; ++side)
			{
				auto& saved = b.waistSupport[side];
				if (!saved.captured) continue;
				WaistSupportChain live;
				if (ResolveWaistSupportTarget(b, side, live) && saved.SameTargetIdentity(live))
				{
					const auto p = saved.savedLocalPosition;
					const auto q = saved.savedLocalRotation;
					fnSetLocalPosition(live.helper, p.x, p.y, p.z);
					fnSetLocalRotation(live.helper, q.x, q.y, q.z, q.w);
				}
				saved = WaistSupportChain{};
			}
		}

		void RestoreWaistSupportSide(WaistSupportChain& saved, const WaistSupportChain& live)
		{
			if (saved.captured && saved.SameTargetIdentity(live))
			{
				const auto p = saved.savedLocalPosition;
				const auto q = saved.savedLocalRotation;
				fnSetLocalPosition(live.helper, p.x, p.y, p.z);
				fnSetLocalRotation(live.helper, q.x, q.y, q.z, q.w);
			}
			saved = live;
		}

		void ApplyWaistSupport(IdolBoneBinding& b)
		{
			if (!experimentalWaistSupportEnabled) return;
			unsigned applied = 0;
			for (int side = 0; side < 2; ++side)
			{
				auto& saved = b.waistSupport[side];
				WaistSupportChain live;
				if (!ResolveWaistSupportTarget(b, side, live))
				{
					saved = WaistSupportChain{}; // Never touch the former rig.
					continue;
				}
				if (!ResolveWaistSupportReferences(b, side, live))
				{
					// Losing a solver reference does not invalidate a freshly
					// reacquired helper. Roll that same target back before skipping.
					RestoreWaistSupportSide(saved, live);
					continue;
				}
				if (!saved.SameIdentity(live))
				{
					// A reference-only replacement does not create a new local
					// target. Keep the pre-first-write snapshot for that same live
					// helper/parent, rather than capturing our own previous output.
					if (saved.captured && saved.SameTargetIdentity(live))
					{
						live.savedLocalPosition = saved.savedLocalPosition;
						live.savedLocalRotation = saved.savedLocalRotation;
						live.captured = true;
					}
					saved = live;
				}
				if (!WaistSupportApisAvailable() || !WaistSupportScalesValid(live))
				{
					RestoreWaistSupportSide(saved, live);
					continue;
				}
				float pW[3]{}, pT[3]{}, qW[4]{};
				fnGetWorldPosition(live.waistOffset, pW);
				fnGetWorldPosition(live.thighOffset, pT);
				fnGetWorldRotation(live.waistOffset, qW);
				waist_support::Result result;
				if (!waist_support::Solve({pW[0], pW[1], pW[2]}, {qW[0], qW[1], qW[2], qW[3]},
				                          {pT[0], pT[1], pT[2]}, result))
				{
					RestoreWaistSupportSide(saved, live);
					continue;
				}
				if (!saved.captured)
				{
					float originalP[3]{}, originalQ[4]{};
					fnGetLocalPosition(live.helper, originalP);
					fnGetLocalRotation(live.helper, originalQ);
					saved.savedLocalPosition = {originalP[0], originalP[1], originalP[2]};
					saved.savedLocalRotation = {originalQ[0], originalQ[1], originalQ[2], originalQ[3]};
					if (!waist_support::Finite(saved.savedLocalPosition) ||
						!WaistSupportRotationValid(saved.savedLocalRotation))
						continue;
					saved.captured = true;
				}
				const auto& p = result.localPosition;
				const auto& q = result.localRotation;
				fnSetLocalPosition(live.helper, p.x, p.y, p.z);
				fnSetLocalRotation(live.helper, q.x, q.y, q.z, q.w);
				applied |= 1u << side;
			}
			if (!b.waistSupportDiagnosticLogged)
			{
				GM_DEBUG(
					"[GakuMotion][WaistSupport][Experimental] actor=%p appliedMask=%u (Waist_H only; scoped acceptance passed, broader scope unverified)",
					b.actor, applied);
				b.waistSupportDiagnosticLogged = true;
			}
		}

	public:
		bool IsExperimentalWaistSupportEnabled() const noexcept { return experimentalWaistSupportEnabled; }

		bool SetExperimentalWaistSupportEnabled(bool enabled)
		{
			if (enabled == experimentalWaistSupportEnabled) return true;
			if (!enabled)
			{
				for (auto& entry : bindings) RestoreWaistSupport(entry.second);
				auto current = bindings.find(currentBinding.actor);
				if (current != bindings.end()) currentBinding = current->second;
				else RestoreWaistSupport(currentBinding);
			}
			experimentalWaistSupportEnabled = enabled;
			for (auto& entry : bindings) entry.second.waistSupportDiagnosticLogged = false;
			currentBinding.waistSupportDiagnosticLogged = false;
			return true;
		}

		// -------------------------------------------------------------------------
		// 全身骨骼旋转自定义编辑（阶段一：外部 JSON 热加载）
		// -------------------------------------------------------------------------
		// 指向「有效覆盖集合」：由 Core 在曲目切换 / 配置文件变化时重建。
		// 生命周期完全归 Core 所有，IdolController 只读不持有，避免悬垂。
		const BoneOverrideSet* boneOverrides{nullptr};
		int lastAppliedOverrideCount{0}; // 上一帧实际生效的覆盖条目数

		// 实机靶向对照组：辅助骨驱动模式（详见 AuxDriveMode 注释）
		// 由 control.json (`aux.mode`) 声明式驱动，或由热键 F8 现场循环切换。
		int auxDriveMode{kAuxNormal};
		float wristTwistShare{0.0f}; // Opt-in: move hand-axis twist to the forearm while preserving hand world pose.
		float forearmAxisAdjustmentDeg{0.0f}; // Fixed-axis experiment, independent of the wrist rotation.
		bool wristDiagnosticPending{false};
		int wristDiagnosticStatus{0}; // 0=idle, 1=waiting for pose, 2=logged, 3=logger unavailable
		float wristDiagnosticTime{0.0f};
		uint64_t wristDiagnosticCount{0};

		static Quaternion WristTwistShareRotation(const Quaternion& handLocal, float fraction) noexcept
		{
			if (!std::isfinite(fraction) || fraction == 0.0f) return Quaternion::Identity();
			fraction = (std::clamp)(fraction, -1.0f, 1.0f);
			float x = handLocal.x, w = handLocal.w;
			if (w < 0.0f || (w == 0.0f && x < 0.0f)) { x = -x; w = -w; }
			if (x * x + w * w < 1e-12f) return Quaternion::Identity();
			const float halfAngle = std::atan2(x, w) * fraction;
			return Quaternion(std::sin(halfAngle), 0.0f, 0.0f, std::cos(halfAngle));
		}

		// One bounded snapshot, on the Unity thread after the motion and auxiliary
		// writes. It observes transforms only; it does not modify the pose.
		void CaptureWristDiagnostic(const IdolBoneBinding& b, float timeSeconds, int appliedOverrides)
		{
			if (!wristDiagnosticPending) return;
			wristDiagnosticPending = false;
			if (!fnLogInfo) { wristDiagnosticStatus = 3; return; }
			fnLogInfo("[GakuMotion][WristDiag] actor=%p time=%.4f aux=%d twistShare=%.3f forearmAxisDeg=%.1f overrides=%d getters(localRot=%d worldRot=%d localPos=%d worldPos=%d parent=%d)",
			          b.actor, timeSeconds, auxDriveMode, wristTwistShare, forearmAxisAdjustmentDeg, appliedOverrides,
			          fnGetLocalRotation != nullptr, fnGetWorldRotation != nullptr, fnGetLocalPosition != nullptr,
			          fnGetWorldPosition != nullptr, fnGetTransformParent != nullptr);
			auto capture = [&](const char* label, void* transform)
			{
				if (!transform) { fnLogInfo("[GakuMotion][WristDiag] %s missing", label); return; }
				float lr[4]{0,0,0,1}, wr[4]{0,0,0,1}, lp[3]{}, wp[3]{};
				if (fnGetLocalRotation) fnGetLocalRotation(transform, lr);
				if (fnGetWorldRotation) fnGetWorldRotation(transform, wr);
				if (fnGetLocalPosition) fnGetLocalPosition(transform, lp);
				if (fnGetWorldPosition) fnGetWorldPosition(transform, wp);
				void* parent = fnGetTransformParent ? fnGetTransformParent(transform) : nullptr;
				const std::string parentName = parent && fnGetTransformName ? fnGetTransformName(parent) : "";
				fnLogInfo("[GakuMotion][WristDiag] %s parent=%s localRot=(%.6f,%.6f,%.6f,%.6f) worldRot=(%.6f,%.6f,%.6f,%.6f) localPos=(%.6f,%.6f,%.6f) worldPos=(%.6f,%.6f,%.6f)",
				          label, parentName.c_str(), lr[0],lr[1],lr[2],lr[3], wr[0],wr[1],wr[2],wr[3],
				          lp[0],lp[1],lp[2],wp[0],wp[1],wp[2]);
			};
			capture("LeftUpperArm", b.leftArmAux.upperArm);
			capture("LeftForeArm", b.leftArmAux.foreArm);
			capture("LeftForeArm_Roll_H", b.leftArmAux.foreArmRollH);
			capture("LeftForeArm_H", b.leftArmAux.foreArmH);
			capture("LeftHand", b.leftArmAux.hand);
			capture("LeftHand_H", b.leftArmAux.handH);
			capture("RightUpperArm", b.rightArmAux.upperArm);
			capture("RightForeArm", b.rightArmAux.foreArm);
			capture("RightForeArm_Roll_H", b.rightArmAux.foreArmRollH);
			capture("RightForeArm_H", b.rightArmAux.foreArmH);
			capture("RightHand", b.rightArmAux.hand);
			capture("RightHand_H", b.rightArmAux.handH);
			wristDiagnosticTime = timeSeconds;
			++wristDiagnosticCount;
			wristDiagnosticStatus = 2;
		}

		// =====================================================================
		// 真值标定模型驱动（模式 0 / Plan A）
		// =====================================================================
		// 模型表里的每个系数都来自实机录制标定，详见 GkAuxChannelModel.hpp 头部说明。
		// 与旧方案的三个决定性差异：
		//   1. 输入 = 源骨「局部旋转」的 twist / 旋转向量，绝不用世界空间相对量；
		//   2. 输出 = 绕辅助骨自身单轴的纯净局部旋转（官方录制值另外两分量恒为 0）；
		//   3. 写入 = setLocalRotation，彻底废弃会反向扣减父骨旋转的 setWorldRotation。
		static void* ResolveModelSourceTransform(const IdolBoneBinding& b, int hid) noexcept
		{
			switch (hid)
			{
			case 11: return b.leftArmAux.shoulder;
			case 13: return b.leftArmAux.upperArm;
			case 15: return b.leftArmAux.foreArm;
			case 17: return b.leftArmAux.hand;
			case 12: return b.rightArmAux.shoulder;
			case 14: return b.rightArmAux.upperArm;
			case 16: return b.rightArmAux.foreArm;
			case 18: return b.rightArmAux.hand;
			default: return nullptr;
			}
		}

		static void* ResolveModelAuxTransform(const IdolBoneBinding& b, const char* auxBone) noexcept
		{
			if (!auxBone) return nullptr;
			if (std::strcmp(auxBone, "LeftArm_Roll_H") == 0) return b.leftArmAux.upperArmRollH;
			if (std::strcmp(auxBone, "LeftArm_H") == 0) return b.leftArmAux.upperArmH;
			if (std::strcmp(auxBone, "LeftForeArm_Roll_H") == 0) return b.leftArmAux.foreArmRollH;
			if (std::strcmp(auxBone, "LeftForeArm_H") == 0) return b.leftArmAux.foreArmH;
			if (std::strcmp(auxBone, "LeftHand_H") == 0) return b.leftArmAux.handH;
			if (std::strcmp(auxBone, "RightArm_Roll_H") == 0) return b.rightArmAux.upperArmRollH;
			if (std::strcmp(auxBone, "RightArm_H") == 0) return b.rightArmAux.upperArmH;
			if (std::strcmp(auxBone, "RightForeArm_Roll_H") == 0) return b.rightArmAux.foreArmRollH;
			if (std::strcmp(auxBone, "RightForeArm_H") == 0) return b.rightArmAux.foreArmH;
			if (std::strcmp(auxBone, "RightHand_H") == 0) return b.rightArmAux.handH;
			return nullptr;
		}

		void ApplyAuxModelToBinding(const IdolBoneBinding& b) const
		{
			if (!fnSetLocalRotation || !fnGetLocalRotation) return;

			const aux_model::AuxChannelModel* channels = aux_model::GetChannels();
			const int count = aux_model::GetChannelCount();

			Quaternion srcLocal[19];
			bool srcValid[19] = {false};

			for (int i = 0; i < count; ++i)
			{
				const aux_model::AuxChannelModel& m = channels[i];
				void* auxTrans = ResolveModelAuxTransform(b, m.auxBone);
				if (!auxTrans) continue;

				const int hid = m.sourceHid;
				if (hid < 0 || hid >= 19) continue;

				if (!srcValid[hid])
				{
					void* src = ResolveModelSourceTransform(b, hid);
					if (!src) return;
					float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
					fnGetLocalRotation(src, r);
					// q and -q are the same orientation. The fitted model's atan2
					// and rotation-vector paths otherwise produce different auxiliary
					// poses for them. Use the shortest-rotation hemisphere only for
					// model input; never change the source bone's actual rotation.
					// At exactly 180 degrees, choose the first nonzero vector component.
					const bool flip = r[3] < 0.0f || (r[3] == 0.0f &&
						(r[0] < 0.0f || (r[0] == 0.0f &&
						(r[1] < 0.0f || (r[1] == 0.0f && r[2] < 0.0f)))));
					const float sign = flip ? -1.0f : 1.0f;
					srcLocal[hid] = Quaternion(sign * r[0], sign * r[1], sign * r[2], sign * r[3]);
					srcValid[hid] = true;
				}

				float angleDeg = aux_model::EvaluateAuxAngleDeg(m, srcLocal[hid]);
				// 模型是在一支完整舞曲的旋转流形上标定的，超出范围时钳到训练域附近，
				// 避免多项式在未见姿态上的自由外插。
				if (angleDeg > 180.0f) angleDeg = 180.0f;
				else if (angleDeg < -180.0f) angleDeg = -180.0f;

				const float half = angleDeg * 0.5f * 0.01745329252f;
				const float s = std::sin(half);
				const float c = std::cos(half);
				Quaternion q(0.0f, 0.0f, 0.0f, 1.0f);
				if (m.auxAxis == 0) q.x = s;
				else if (m.auxAxis == 1) q.y = s;
				else q.z = s;
				q.w = c;

				fnSetLocalRotation(auxTrans, q.x, q.y, q.z, q.w);
			}
		}

		void SetAuxDriveMode(int mode)
		{
			if (mode < 0 || mode >= kAuxModeCount) mode = kAuxNormal;
			if (mode == auxDriveMode) return;
			auxDriveMode = mode;
			if (fnLogInfo)
			{
				fnLogInfo("[GakuMotion][AuxMode] switched to %d = %s", mode, AuxDriveModeName(mode));
			}
		}

		void CycleAuxDriveMode() { SetAuxDriveMode((auxDriveMode + 1) % kAuxModeCount); }
		bool IsAuxInjectionEnabled() const noexcept { return auxDriveMode == kAuxNormal; }

		// ---- 官方真值直灌数据源（aux_replay.json）----
		AuxGroundTruth auxGroundTruth;

		bool LoadAuxGroundTruth(const std::string& path)
		{
			std::string err;
			const bool ok = auxGroundTruth.Load(path, err);
			if (fnLogInfo)
			{
				fnLogInfo("[GakuMotion][AuxReplay] load %s -> %s (%zu tracks, fps=%.2f)%s%s",
				          path.c_str(), ok ? "OK" : "FAILED", auxGroundTruth.tracks.size(),
				          auxGroundTruth.fps,
				          err.empty() ? "" : " err=", err.c_str());
			}
			return ok;
		}

		// humanoidId: 13/14=UpperArm, 15/16=LowerArm, 17/18=Hand；isRoll: true=_Roll_H, false=_H
		const Quaternion* LookupAuxReplay(int humanoidId, bool isRoll, float timeSeconds) const noexcept
		{
			if (auxGroundTruth.Empty()) return nullptr;
			return auxGroundTruth.Sample(humanoidId, isRoll ? 0 : 1, timeSeconds);
		}

		void SetBoneOverrides(const BoneOverrideSet* overrides) noexcept { boneOverrides = overrides; }

		void InvalidateClipBindings()
		{
			for (auto& entry : bindings) entry.second.clipBindingDirty = true;
			currentBinding.clipBindingDirty = true;
		}

		RootHeightCalibration rootHeightCalibration;
		bool rootHeightCalibrationEnabled{true};

		void LoadRootHeightCalibration(const std::string& path, const MotionClip& clip)
		{
			std::string error;
			if (!rootHeightCalibration.Load(path, clip, error))
			{
				if (fnLogInfo) fnLogInfo("[GakuMotion][RootHeight] ignored %s: %s", path.c_str(), error.c_str());
			}
			else if (rootHeightCalibration.enabled && fnLogInfo)
			{
				fnLogInfo("[GakuMotion][RootHeight] loaded %s: %.5f m (target rig match required)",
				          path.c_str(), rootHeightCalibration.offset);
			}
		}

		void UpdateClipGroundAnchor(IdolBoneBinding& b, const MotionClip& clip)
		{
			b.groundLift = b.bindGroundLift;
			b.calibratedRootHeight = 0.0f;
			b.clipGroundAnchored = false;
			if (clip.version >= 2 &&
				b.actorTransform && fnGetLocalPosition)
			{
				std::array<Vector3, 21> offsets{};
				std::array<bool, 21> present{};
				for (size_t i = 0; i < clip.bones.size(); ++i)
				{
					int hid = clip.bones[i].humanoid_id;
					if (hid >= 21 || !b.boneTransforms[i]) continue;
					float p[3]{};
					fnGetLocalPosition(b.boneTransforms[i], p);
					offsets[hid] = Vector3(p[0], p[1], p[2]);
					present[hid] = true;
				}
				float lift = 0.0f;
				b.clipGroundAnchored = ComputeClipGroundLift(clip, offsets, present, lift);
				if (b.clipGroundAnchored)
				{
					b.groundLift = lift;
					b.calibratedRootHeight = rootHeightCalibration.Match(offsets, present);
				}
				GM_DEBUG(
					"[GakuMotion][GroundAnchor] actor=%p version=%u valid=%d lift=%.4f (foot/toe markers; actor pivot floor)",
					b.actor, clip.version, int(b.clipGroundAnchored), b.groundLift);
			}
		}

		void BindActor(void* actor, const MotionClip& clip, void* animator = nullptr)
		{
			if (!CanDriveActor(actor) || !fnGetBoneTransform) return;

			// 2026-10-07: target gate lives HERE, at bind time -- not in
			// ApplyMotion. A non-target actor must never create a binding at all:
			// binding is what captures the rest pose and fills boneTransforms, and
			// OnActorChanged() clears the whole map, so a binding created for one
			// actor gets wiped by the next actor's rebind anyway. Gating here keeps
			// the map holding exactly one actor's data.
			if (!IsTargetActor(actor))
			{
				return;
			}

			// Preserve the actor's original snapshot across song changes. Bone-table
			// identity is checked by humanoid id, not merely by table size.
			auto it = bindings.find(actor);
			if (it != bindings.end() && it->second.bindPoseCaptured &&
				it->second.boundBoneCount > 0 &&
				it->second.rootTransform == fnGetBoneTransform(actor, 0) &&
				(!animator || it->second.cachedAnimator == animator) &&
				it->second.boneHids.size() == it->second.boneTransforms.size())
			{
				auto& existing = it->second;
				bool sameBones = existing.boneHids.size() == clip.bones.size();
				for (size_t i = 0; sameBones && i < clip.bones.size(); ++i)
					sameBones = existing.boneHids[i] == clip.bones[i].humanoid_id;
				if (existing.clipBindingDirty || !sameBones)
				{
					RestoreWaistSupport(existing);
					existing.waistSupportDiagnosticLogged = false;
					const auto old = existing;
					existing.boneTransforms.assign(clip.bones.size(), nullptr);
					existing.restRotations.assign(clip.bones.size(), Quaternion::Identity());
					existing.restLocals.assign(clip.bones.size(), Quaternion::Identity());
					existing.boneHids.resize(clip.bones.size());
					existing.boundBoneCount = 0;
					existing.upperChestInClip = false;
					for (size_t i = 0; i < clip.bones.size(); ++i)
					{
						const int hid = clip.bones[i].humanoid_id;
						existing.boneHids[i] = static_cast<uint16_t>(hid);
						auto previous = std::find(old.boneHids.begin(), old.boneHids.end(), hid);
						if (previous != old.boneHids.end())
						{
							size_t k = size_t(previous - old.boneHids.begin());
							existing.boneTransforms[i] = old.boneTransforms[k];
							existing.restRotations[i] = old.restRotations[k];
							existing.restLocals[i] = old.restLocals[k];
						}
						else
						{
							// Newly requested bones were never driven by the old clip.
							void* t = fnGetBoneTransform(actor, hid);
							existing.boneTransforms[i] = t;
							float q[4]{0, 0, 0, 1};
							if (t && fnGetLocalRotation)
							{
								fnGetLocalRotation(t, q);
								existing.restLocals[i] = Quaternion(q[0], q[1], q[2], q[3]);
							}
							if (t && fnGetWorldRotation)
							{
								fnGetWorldRotation(t, q);
								existing.restRotations[i] = existing.restActorRot.Inverse() * Quaternion(
									q[0], q[1], q[2], q[3]);
							}
						}
						if (existing.boneTransforms[i]) ++existing.boundBoneCount;
						if (hid == 54) existing.upperChestInClip = true;
					}
					UpdateClipGroundAnchor(existing, clip);
					existing.clipBindingDirty = false;
					existing.lastGeometryProbeTime = -99.0f;
					probeFirstFrameDone = false;
				}
				existing.face.enableBaseline = facialEnableBaseline;
				existing.face.baselineMultiplier = facialBaselineMultiplier;
				existing.face.characterOverride = facialCharacterOverride;
				existing.face.enableMikuMouthFit = facialMikuMouthFit;
				existing.face.enableMikuEyeFit = facialMikuEyeFit;
				existing.face.facialFitAllowUnverified = facialMikuFitAllowUnverified;
				existing.face.Prepare(actor, clip, faceApi);
				currentBinding = it->second;
				return;
			}

			// Actors appear before their humanoid skeleton is ready. Never detach
			// their controller or cache an empty "successful" binding: doing so
			// prevents initialization and leaves Preview/first Play at bones=0.
			if (!fnGetBoneTransform(actor, 0)) return;

			IdolBoneBinding& b = bindings[actor];
			RestoreWaistSupport(b);
			b.face.Restore(actor, faceApi);
			for (int slot = 0; slot < 2; ++slot) b.talks[slot].Restore(actor, slot, faceApi);
			b.Reset();
			b.actor = actor;
			b.face.enableBaseline = facialEnableBaseline;
			b.face.baselineMultiplier = facialBaselineMultiplier;
			b.face.facialWeights = facialWeights;
			b.face.characterOverride = facialCharacterOverride;
			b.face.enableMikuMouthFit = facialMikuMouthFit;
			b.face.enableMikuEyeFit = facialMikuEyeFit;
			b.face.facialFitAllowUnverified = facialMikuFitAllowUnverified;
			b.face.Prepare(actor, clip, faceApi);
			if (animator) b.cachedAnimator = animator;
			cachedAnimator = b.cachedAnimator;

			// ---- PROBE A1: is the actor already in a dance pose at bind time? ----
			// Opt-in only (GAKUMOTION_ENABLE_GEOMETRY_PROBE). The question this
			// answered -- "is BindActor capturing a dance pose instead of rest" --
			// was settled on 2026-10-05; keeping it on costs 9 lines per bind.
#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
			if (fnLogInfo && fnGetLocalRotation)
			{
				static const int kProbeBones[] = {0, 7, 8, 13, 14, 15, 16, 17, 18};
				const int kProbeCount = static_cast<int>(sizeof(kProbeBones) / sizeof(kProbeBones[0]));
				for (int bi = 0; bi < kProbeCount; ++bi)
				{
					void* pt = fnGetBoneTransform(actor, kProbeBones[bi]);
					if (!pt) continue;
					float pre[4]{0.0f, 0.0f, 0.0f, 1.0f};
					fnGetLocalRotation(pt, pre);
					Quaternion qPre(pre[0], pre[1], pre[2], pre[3]);
					float pw[4]{0.0f, 0.0f, 0.0f, 1.0f};
					if (fnGetWorldRotation) fnGetWorldRotation(pt, pw);
					fnLogInfo(
						"[PROBE A1] pre-rebind hid=%2d local=(%.4f,%.4f,%.4f,%.4f) |w|=%.6f world=(%.4f,%.4f,%.4f,%.4f)",
						kProbeBones[bi], pre[0], pre[1], pre[2], pre[3],
						std::sqrt(qPre.x * qPre.x + qPre.y * qPre.y + qPre.z * qPre.z + qPre.w * qPre.w),
						pw[0], pw[1], pw[2], pw[3]);
				}
			}
#endif // GAKUMOTION_ENABLE_GEOMETRY_PROBE

			// Snap actor to clean Prefab Bind Pose before measuring restRotations
			// Detach runtimeAnimatorController to eliminate Idle animation/controller pollution
			//
			// 2026-10-07 A/B SWITCH. Animator.Rebind() was added in 659dd79
			// ("Integrate Animator.Rebind for pristine prefab bind pose capture",
			// 2026-09-30) with the intent of capturing a clean rest pose. On a
			// Unity humanoid it does the opposite: it snaps every bone to the
			// prefab T-POSE, so whatever the engine reports afterwards is that
			// T-Pose, not the pose the rig was actually in.
			//
			// Measured on device, A1 (before Rebind) vs A2 (after):
			//     hid=13 LeftUpperArm  local devId 48.65 deg  ->  restLocal devId 0.00 deg
			//     hid=0  Hips          hips world y  ~0.88      ->  restRootPos.y = -0.0122
			// Every restLocal comes back exactly Identity, and the hips land at floor
			// level. That is the "character sunk into the floor" report, and it dates
			// from this Rebind, not from anything newer.
			//
			// Keep the controller detach (that part is doing real work against Idle
			// pollution) but make the Rebind itself switchable, so the causal claim
			// can be tested on device instead of argued.
			void* anim = animator ? animator : b.cachedAnimator;
			if (!preserveAnimatorForSecondaryPhysics && anim)
			{
				CaptureAnimatorState(b);
				DetachAnimatorController(b);
				if (useAnimatorRebind && fnAnimatorRebind)
				{
					fnAnimatorRebind(anim);
					b.animatorRebound = true;
				}
			}
			if (anim)
			{
				GM_DEBUG("[GakuMotion][Animator] actor=%p policy=%s controllerDetached=%d rebind=%d",
				          actor, preserveAnimatorForSecondaryPhysics ? "preserve-graph(experimental)" : "legacy-mute",
				          int(b.animatorControllerDetached), int(b.animatorRebound));
			}

			// Bone 0 is Hips / Root
			b.rootTransform = fnGetBoneTransform(actor, 0);
			b.actorTransform = fnGetComponentTransform ? fnGetComponentTransform(actor) : nullptr;

			// Capture actor rest world transform
			if (b.actorTransform)
			{
				float aPos[3]{0.0f, 0.0f, 0.0f};
				float aRot[4]{0.0f, 0.0f, 0.0f, 1.0f};
				if (fnGetWorldPosition) fnGetWorldPosition(b.actorTransform, aPos);
				if (fnGetWorldRotation) fnGetWorldRotation(b.actorTransform, aRot);
				b.restActorPos = Vector3(aPos[0], aPos[1], aPos[2]);
				b.restActorRot = Quaternion(aRot[0], aRot[1], aRot[2], aRot[3]);
			}

			// Capture Hips rest position relative to actor
			if (b.rootTransform)
			{
				float hPos[3]{0.0f, 0.0f, 0.0f};
				if (fnGetWorldPosition)
				{
					fnGetWorldPosition(b.rootTransform, hPos);
				}
				else if (fnGetLocalPosition)
				{
					fnGetLocalPosition(b.rootTransform, hPos);
				}
				Vector3 hipsWorld(hPos[0], hPos[1], hPos[2]);
				if (b.actorTransform)
				{
					b.restRootPos = b.restActorRot.Inverse().Rotate(hipsWorld - b.restActorPos);
				}
				else
				{
					b.restRootPos = hipsWorld;
				}
			}

			// Capture Hips rest position relative to actor
			//
			// 2026-10-07 PROBE A10. The character is reported as "sunk into the
			// floor" and restRootPos.y measures -0.0122, i.e. the hips sit at floor
			// level. Two very different causes look identical from the outside:
			//
			//   (a) the hips really are that low because the rig's own Hips bone sits
			//       at the actor origin -- then the number is CORRECT and the fix is
			//       elsewhere (the pose, or the camera reading it from above);
			//   (b) the rest position was captured from the wrong space or after the
			//       rig had already been posed -- then the number is WRONG.
			//
			// The two are distinguishable only by measuring. This prints, all on one
			// line at capture time: the hips in WORLD space, the actor root in WORLD
			// space, the difference, and -- the decisive one -- the length of the
			// head-to-foot vector, which for a standing character must be roughly
			// the character's height. A rig whose hips sit at the actor origin gives
			// a head-to-foot vector equal to the standing height, which is (a).
			// ---- The ground offset, measured per character, not hardcoded ----
			// A Unity humanoid rests with the hips at the actor origin and the feet
			// one stature BELOW that. This scene's floor is at y = 0, so leaving the
			// hips at the origin leaves the feet underground -- which is exactly the
			// "sunk into the floor" report, and exactly what the screenshots at
			// 1.0s / 2.0s / 5.0s show (standing correctly, torso above the boards,
			// legs below them).
			//
			// The fix is the character's OWN stature, which the rig hands us at bind
			// time -- not a tuned constant. Placing the hips at origin + stature puts
			// the feet back where they were measured. Statures differ per character,
			// so this must be captured per binding; a hardcoded number would be
			// correct for exactly one character.
			//
			// OUTSIDE the fnLogInfo guard on purpose: this is production behaviour,
			// not a diagnostic, and must not depend on logging being enabled.
			{
				float hp[3]{0, 0, 0};
				float fp[3]{0, 0, 0};
				bool haveFoot = false;
				if (b.rootTransform && fnGetWorldPosition) fnGetWorldPosition(b.rootTransform, hp);
				if (fnGetBoneTransform)
				{
					void* lf = fnGetBoneTransform(actor, 5); // LeftFoot
					if (lf && fnGetWorldPosition)
					{
						fnGetWorldPosition(lf, fp);
						haveFoot = true;
					}
				}
				if (haveFoot)
				{
					const float stature = hp[1] - fp[1];
					// Guard against a rig that reports a degenerate drop (both points
					// at the origin, or the foot transform missing): a zero lift is
					// the safe fallback because it reproduces the old behaviour.
					b.groundLift = (stature > 0.05f && stature < 5.0f) ? stature : 0.0f;
				}

#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
				if (fnLogInfo)
				{
					float ap[3]{0, 0, 0};
					if (b.actorTransform && fnGetWorldPosition) fnGetWorldPosition(b.actorTransform, ap);
					fnLogInfo("[PROBE A10] actor=%p | actorRootWorld=(%.4f,%.4f,%.4f)"
					          " | hipsWorld=(%.4f,%.4f,%.4f)"
					          " | restRootPos=(%.4f,%.4f,%.4f)",
					          actor, ap[0], ap[1], ap[2], hp[0], hp[1], hp[2],
					          b.restRootPos.x, b.restRootPos.y, b.restRootPos.z);
					if (haveFoot)
					{
						fnLogInfo("[PROBE A10]   LeftFoot world y=%.4f | stature=%.4f m"
						          " | groundLift applied=%.4f",
						          fp[1], hp[1] - fp[1], b.groundLift);
					}

					// ---- PROBE A12: segment lengths, measured HERE ----------------
					// At bind time the rig is in its rest pose, so the distance
					// between a joint and its child IS the segment length. That is
					// the only moment the measurement is meaningful -- during playback
					// the rotations move the joints and the numbers become pose
					// dependent, which is what made an earlier reading useless.
					//
					// Why this matters now: A11 shows torsoVerticality is correct
					// (0.86-1.00, matching the official 0.852-1.000) while the feet
					// sit only 0.15 m below the hips where the official run puts them
					// 0.93 m below. Direction is right, LENGTH is wrong. The only
					// way that happens with correct orientations is a compressed
					// bone offset, and this prints each segment so the compressed
					// one is identified rather than guessed.
					//
					// get_localPosition returned all zeros for these transforms
					// earlier (a silent no-op in the host helper), so the distance is
					// taken from world positions instead.
					auto seg = [&](int aHid, int bHid, const char* label)
					{
						if (!fnGetBoneTransform || !fnGetWorldPosition) return;
						void* ta = fnGetBoneTransform(actor, aHid);
						void* tb = fnGetBoneTransform(actor, bHid);
						if (!ta || !tb) return;
						float pa[3], pb[3];
						fnGetWorldPosition(ta, pa);
						fnGetWorldPosition(tb, pb);
						const float dx = pb[0] - pa[0], dy = pb[1] - pa[1], dz = pb[2] - pa[2];
						fnLogInfo("[PROBE A12] %-22s hid %2d->%2d  len=%.4f m",
						          label, aHid, bHid, std::sqrt(dx * dx + dy * dy + dz * dz));
					};
					seg(0, 1, "Hips->UpperLeg(thigh)");
					seg(1, 3, "UpperLeg->LowerLeg");
					seg(3, 5, "LowerLeg->Foot(shin)");
					seg(5, 19, "Foot->Toes");
					seg(0, 7, "Hips->Spine");
					seg(7, 54, "Spine->UpperChest");
					seg(0, 10, "Hips->Head(whole body)");

					// ---- PROBE A13: each segment's DIRECTION at bind time ----------
					// A12 shows the segment LENGTHS are plausible (thigh 0.3496 +
					// shin 0.3981 = 0.7477 m) yet the arithmetic contradicts A10:
					// two downward segments of that total should put the foot
					// 0.7477 m below the hips, and A10 measured 0.1046 m. So the
					// discrepancy is not length -- it is that the segments do not
					// point where the gkclip says they should.
					//
					// gkclip stores hid=3 LeftLowerLeg delta as EXACT identity, which
					// means "the shin's world rotation equals the thigh's", i.e. the
					// shin must continue straight down. If it does not, something
					// between the delta and the rig is not honouring the local write.
					//
					// verticality: +1 = pointing straight up, -1 = straight down.
					auto segDir = [&](int aHid, int bHid, const char* label)
					{
						if (!fnGetBoneTransform || !fnGetWorldPosition) return;
						void* ta = fnGetBoneTransform(actor, aHid);
						void* tb = fnGetBoneTransform(actor, bHid);
						if (!ta || !tb) return;
						float pa[3], pb[3];
						fnGetWorldPosition(ta, pa);
						fnGetWorldPosition(tb, pb);
						const float dx = pb[0] - pa[0], dy = pb[1] - pa[1], dz = pb[2] - pa[2];
						const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
						if (len < 1e-6f) return;
						fnLogInfo("[PROBE A13] %-22s dy=%+.4f of len=%.4f =>"
						          " verticality=%+.4f  (down = -1)",
						          label, dy, len, dy / len);
					};
					segDir(0, 1, "Hips->UpperLeg");
					segDir(1, 3, "UpperLeg->LowerLeg");
					segDir(3, 5, "LowerLeg->Foot");
					segDir(0, 7, "Hips->Spine");
					segDir(7, 54, "Spine->UpperChest");
				}
#endif // GAKUMOTION_ENABLE_GEOMETRY_PROBE
			}

			// Capture Bind Pose for all mapped bones.
			//   restRotations : actor-relative WORLD  -> gk_world_dump / legacy path
			//   restLocals    : LOCAL                  -> gkclip path (option A)
			// World/local rests are retained for restoration and auxiliary bones.
			// They are not composed into v2+ gkclip main-bone rotations.
			b.boneTransforms.resize(clip.bones.size(), nullptr);
			b.restRotations.resize(clip.bones.size(), Quaternion::Identity());
			b.restLocals.resize(clip.bones.size(), Quaternion::Identity());
			b.boneHids.resize(clip.bones.size());

			size_t boundCount = 0;
			for (size_t i = 0; i < clip.bones.size(); ++i)
			{
				int hid = clip.bones[i].humanoid_id;
				b.boneHids[i] = static_cast<uint16_t>(hid);
				void* trans = fnGetBoneTransform(actor, hid);
				b.boneTransforms[i] = trans;

				if (trans)
				{
					boundCount++;
					float rot[4]{0.0f, 0.0f, 0.0f, 1.0f};
					if (fnGetWorldRotation)
					{
						fnGetWorldRotation(trans, rot);
						Quaternion qWorld(rot[0], rot[1], rot[2], rot[3]);
						b.restRotations[i] = b.restActorRot.Inverse() * qWorld;
					}
					else if (fnGetLocalRotation)
					{
						fnGetLocalRotation(trans, rot);
						b.restRotations[i] = Quaternion(rot[0], rot[1], rot[2], rot[3]);
					}
					// Local rest is captured independently of the world path above:
					// fnGetWorldRotation is usually present, so relying on the
					// fallback branch for this would leave every restLocal at
					// Identity and silently flatten the character's rest pose.
					if (fnGetLocalRotation)
					{
						float lrot[4]{0.0f, 0.0f, 0.0f, 1.0f};
						fnGetLocalRotation(trans, lrot);
						b.restLocals[i] = Quaternion(lrot[0], lrot[1], lrot[2], lrot[3]);
					}
				}
			}

			b.boundBoneCount = boundCount;

			b.bindGroundLift = b.groundLift;
			UpdateClipGroundAnchor(b, clip);

			// ---- PROBE A2: dump the captured restLocal, and compare against the
			// pre-rebind reading above. A non-zero delta means the capture basis is
			// not the rest pose. This is the value every gkclip composition uses, so
			// it must be seen, not assumed.
			//
			// A3/A6 are rearmed here, in the same function that captures restLocal,
			// so the A2 block and the A3 block always describe ONE bind. Leaving the
			// flag set across rebinds made A3 a once-per-process event, which is how
			// a previous analysis ended up comparing one clip's rest against another
			// clip's delta.
			probeFirstFrameDone = false;
#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
			if (fnLogInfo && fnGetLocalRotation)
			{
				static const int kProbeBones[] = {
					0, 7, 8, 54, 9, 10, 13, 14, 15, 16, 17, 18, 19, 20, 1, 2, 3, 4, 5, 6, 11, 12
				};
				const int kProbeCount = static_cast<int>(sizeof(kProbeBones) / sizeof(kProbeBones[0]));
				fnLogInfo(
					"[PROBE A2] captured restLocal (post-rebind) -- reference ONLY, no longer composed into the gkclip write:");
				for (size_t i = 0; i < clip.bones.size(); ++i)
				{
					const int hid = clip.bones[i].humanoid_id;
					bool wanted = false;
					for (int k = 0; k < kProbeCount; ++k)
					{
						if (kProbeBones[k] == hid)
						{
							wanted = true;
							break;
						}
					}
					if (!wanted || i >= b.restLocals.size()) continue;
					const Quaternion& rl = b.restLocals[i];
					// Angle from identity tells us how far the captured rest is from a
					// canonical T-pose. A gkclip delta is only meaningful relative to
					// whichever convention the capture actually used.
					const float w = rl.w < 0.0f ? -rl.w : rl.w;
					const float devDeg = 2.0f * std::acos(w > 1.0f ? 1.0f : w) * 57.29578f;
					fnLogInfo("[PROBE A2]   hid=%2d %-16s restLocal=(%.4f,%.4f,%.4f,%.4f) devFromIdentity=%7.2fdeg",
					          hid, clip.bones[i].name.c_str(), rl.x, rl.y, rl.z, rl.w, devDeg);
				}
			}
#endif // GAKUMOTION_ENABLE_GEOMETRY_PROBE

			// ---------------------------------------------------------------------
			// ---------------------------------------------------------------------
			// 辅助骨精确探查与绑定：
			// 探查 UpperArm, ForeArm, Hand 下的原生 _Roll_H 与 _H 辅助骨
			// 注意：Gakumas 中 Hand_H 位于 ForeArm 的子节点下，必须按精确骨骼名解析杜绝覆盖
			// ---------------------------------------------------------------------
			auto findAuxChildByName = [this](void* parent, const char* targetSubstr) -> void*
			{
				if (!parent || !fnGetChildCount || !fnGetChild || !fnGetTransformName) return nullptr;
				int count = fnGetChildCount(parent);
				for (int i = 0; i < count; ++i)
				{
					void* child = fnGetChild(parent, i);
					if (!child) continue;
					std::string cname = fnGetTransformName(child);
					if (cname.find(targetSubstr) != std::string::npos)
					{
						return child;
					}
				}
				return nullptr;
			};

			auto captureAuxRest = [this, &b](void* trans, Quaternion& outLocal, Quaternion& outActorRel)
			{
				outLocal = Quaternion::Identity();
				outActorRel = Quaternion::Identity();
				if (trans)
				{
					if (fnGetLocalRotation)
					{
						float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
						fnGetLocalRotation(trans, r);
						outLocal = Quaternion(r[0], r[1], r[2], r[3]);
					}
					if (fnGetWorldRotation)
					{
						float rw[4]{0.0f, 0.0f, 0.0f, 1.0f};
						fnGetWorldRotation(trans, rw);
						Quaternion qWorld(rw[0], rw[1], rw[2], rw[3]);
						outActorRel = b.restActorRot.Inverse() * qWorld;
					}
				}
			};

			auto captureBoneRestRel = [this, &b](void* trans, Quaternion& outActorRel)
			{
				outActorRel = Quaternion::Identity();
				if (trans)
				{
					if (fnGetWorldRotation)
					{
						float rw[4]{0.0f, 0.0f, 0.0f, 1.0f};
						fnGetWorldRotation(trans, rw);
						Quaternion qWorld(rw[0], rw[1], rw[2], rw[3]);
						outActorRel = b.restActorRot.Inverse() * qWorld;
					}
					else if (fnGetLocalRotation)
					{
						float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
						fnGetLocalRotation(trans, r);
						outActorRel = Quaternion(r[0], r[1], r[2], r[3]);
					}
				}
			};

			b.leftArmAux.shoulder = fnGetBoneTransform(actor, 11);
			b.leftArmAux.upperArm = fnGetBoneTransform(actor, 13);
			b.leftArmAux.foreArm = fnGetBoneTransform(actor, 15);
			b.leftArmAux.hand = fnGetBoneTransform(actor, 17);

			// 捕获 UpperChest (Spine2, hid=54) 防破坑保护
			b.upperChestTransform = fnGetBoneTransform(actor, 54);
			b.upperChestInClip = false;
			for (const auto& bone : clip.bones)
			{
				if (bone.humanoid_id == 54)
				{
					b.upperChestInClip = true;
					break;
				}
			}
			captureBoneRestRel(b.upperChestTransform, b.upperChestRestRel);

			captureBoneRestRel(b.leftArmAux.shoulder, b.leftArmAux.shoulderRestRel);
			captureBoneRestRel(b.leftArmAux.upperArm, b.leftArmAux.upperArmRestRel);
			captureBoneRestRel(b.leftArmAux.foreArm, b.leftArmAux.foreArmRestRel);
			captureBoneRestRel(b.leftArmAux.hand, b.leftArmAux.handRestRel);

			b.leftArmAux.upperArmRollH = findAuxChildByName(b.leftArmAux.upperArm, "LeftArm_Roll_H");
			b.leftArmAux.upperArmH = findAuxChildByName(b.leftArmAux.upperArm, "LeftArm_H");
			b.leftArmAux.foreArmRollH = findAuxChildByName(b.leftArmAux.foreArm, "LeftForeArm_Roll_H");
			b.leftArmAux.foreArmH = findAuxChildByName(b.leftArmAux.foreArm, "LeftForeArm_H");
			b.leftArmAux.handH = findAuxChildByName(b.leftArmAux.foreArm, "LeftHand_H");
			if (!b.leftArmAux.handH && b.leftArmAux.hand)
			{
				b.leftArmAux.handH = findAuxChildByName(b.leftArmAux.hand, "LeftHand_H");
			}

			captureAuxRest(b.leftArmAux.upperArmH, b.leftArmAux.upperArmHRestLocal, b.leftArmAux.upperArmHRestRel);
			captureAuxRest(b.leftArmAux.upperArmRollH, b.leftArmAux.upperArmRollHRestLocal,
			               b.leftArmAux.upperArmRollHRestRel);
			captureAuxRest(b.leftArmAux.foreArmH, b.leftArmAux.foreArmHRestLocal, b.leftArmAux.foreArmHRestRel);
			captureAuxRest(b.leftArmAux.foreArmRollH, b.leftArmAux.foreArmRollHRestLocal,
			               b.leftArmAux.foreArmRollHRestRel);
			captureAuxRest(b.leftArmAux.handH, b.leftArmAux.handHRestLocal, b.leftArmAux.handHRestRel);

			b.rightArmAux.shoulder = fnGetBoneTransform(actor, 12);
			b.rightArmAux.upperArm = fnGetBoneTransform(actor, 14);
			b.rightArmAux.foreArm = fnGetBoneTransform(actor, 16);
			b.rightArmAux.hand = fnGetBoneTransform(actor, 18);

			captureBoneRestRel(b.rightArmAux.shoulder, b.rightArmAux.shoulderRestRel);
			captureBoneRestRel(b.rightArmAux.upperArm, b.rightArmAux.upperArmRestRel);
			captureBoneRestRel(b.rightArmAux.foreArm, b.rightArmAux.foreArmRestRel);
			captureBoneRestRel(b.rightArmAux.hand, b.rightArmAux.handRestRel);

			b.rightArmAux.upperArmRollH = findAuxChildByName(b.rightArmAux.upperArm, "RightArm_Roll_H");
			b.rightArmAux.upperArmH = findAuxChildByName(b.rightArmAux.upperArm, "RightArm_H");
			b.rightArmAux.foreArmRollH = findAuxChildByName(b.rightArmAux.foreArm, "RightForeArm_Roll_H");
			b.rightArmAux.foreArmH = findAuxChildByName(b.rightArmAux.foreArm, "RightForeArm_H");
			b.rightArmAux.handH = findAuxChildByName(b.rightArmAux.foreArm, "RightHand_H");
			if (!b.rightArmAux.handH && b.rightArmAux.hand)
			{
				b.rightArmAux.handH = findAuxChildByName(b.rightArmAux.hand, "RightHand_H");
			}

			captureAuxRest(b.rightArmAux.upperArmH, b.rightArmAux.upperArmHRestLocal, b.rightArmAux.upperArmHRestRel);
			captureAuxRest(b.rightArmAux.upperArmRollH, b.rightArmAux.upperArmRollHRestLocal,
			               b.rightArmAux.upperArmRollHRestRel);
			captureAuxRest(b.rightArmAux.foreArmH, b.rightArmAux.foreArmHRestLocal, b.rightArmAux.foreArmHRestRel);
			captureAuxRest(b.rightArmAux.foreArmRollH, b.rightArmAux.foreArmRollHRestLocal,
			               b.rightArmAux.foreArmRollHRestRel);
			captureAuxRest(b.rightArmAux.handH, b.rightArmAux.handHRestLocal, b.rightArmAux.handHRestRel);

			// Pointer dumps are suppressed: they were two long lines per bind and
			// buried the lines that actually matter. The warnings below stay --
			// a missing _H bone is a real finding, the addresses are not.
			if (fnLogInfo)
			{
				if (!b.leftArmAux.upperArmH || !b.leftArmAux.foreArmH || !b.leftArmAux.handH)
				{
					fnLogInfo("[GakuMotion][AuxBind][WARNING] One or more Left Arm _H auxiliary bones were not found!");
				}
				if (!b.rightArmAux.upperArmH || !b.rightArmAux.foreArmH || !b.rightArmAux.handH)
				{
					fnLogInfo(
						"[GakuMotion][AuxBind][WARNING] One or more Right Arm _H auxiliary bones were not found!");
				}
			}

			b.bindPoseCaptured = true;
			currentBinding = b;

			if (fnLogInfo)
			{
				fnLogInfo("[GakuMotion] Actor %p bound: %zu / %zu bones (useRestRotations=%s)",
				          actor, boundCount, clip.bones.size(), useRestRotations ? "true" : "false");
			}
		}

		void DiscardBindings()
		{
			guiMikuFitSnapshot.store(0, std::memory_order_relaxed);
			captureRestoreUnconfirmed = false;
			bindings.clear();
			currentBinding.Reset();
			cachedAnimator = nullptr;
		}

		void OnActorChanged(void* newActor)
		{
			// 2026-10-07 REGRESSION FIX. This unconditionally did
			//
			//     bindings.clear();  currentBinding.Reset();  cachedAnimator = nullptr;
			//
			// but the host calls OnLateUpdate once per actor per frame, so with four
			// characters on stage it ran four times a frame, each time throwing away
			// the binding the previous actor had just built. The device symptom was
			// brutal: PROBE A2 reported the RAW T-POSE instead of the captured rest
			// pose -- LeftUpperArm and RightUpperArm came back bit-identical
			// (-0.0000, 0.2442, 0.3317, 0.9112), devId 48.65 deg where the correct
			// value is 82.92 deg -- because the capture never got past the first bone
			// before the next actor wiped it. Every format stopped animating while
			// audio kept playing, and the status line sat at "Preview(f0) bones=0/0".
			//
			// Switching characters must not happen by accident. If a DIFFERENT actor
			// arrives while the current one is still bound and animating, keep the
			// current binding and ignore the newcomer. A genuine scene change is
			// still handled, because a gone actor leaves no live binding to keep.
			if (newActor && currentBinding.actor && currentBinding.bindPoseCaptured
				&& currentBinding.actor != newActor
				&& currentBinding.boundBoneCount > 0)
			{
				if (fnLogInfo)
				{
					fnLogInfo("[GakuMotion] OnActorChanged: ignoring %p, still driving %p"
					          " (multi-actor scene; use the GUI target selector)",
					          newActor, currentBinding.actor);
				}
				return;
			}
			if (fnLogInfo)
			{
				fnLogInfo(
					"[GakuMotion] IdolController::OnActorChanged: %p -> %p (clearing all bindings and controllers)",
					currentBinding.actor, newActor);
			}
			DiscardBindings();
		}

		void CaptureAnimatorState(IdolBoneBinding& b)
		{
			if (!b.cachedAnimator || b.animatorStateCaptured) return;
			b.savedAnimatorController = fnGetAnimatorController ? fnGetAnimatorController(b.cachedAnimator) : nullptr;
			b.savedAnimatorEnabled = fnGetAnimatorEnabled ? fnGetAnimatorEnabled(b.cachedAnimator) : true;
			b.animatorStateCaptured = true;
		}

		void DetachAnimatorController(IdolBoneBinding& b)
		{
			if (!b.cachedAnimator || !fnGetAnimatorController || !fnSetAnimatorController) return;
			if (void* controller = fnGetAnimatorController(b.cachedAnimator))
			{
				// An official timeline may replace the controller while we own it.
				// Preserve that replacement for Stop instead of resurrecting an old one.
				b.savedAnimatorController = controller;
				fnSetAnimatorController(b.cachedAnimator, nullptr);
				b.animatorControllerDetached = true;
			}
		}

		void RestoreAnimatorState(IdolBoneBinding& b, bool rebind)
		{
			void* anim = b.cachedAnimator;
			if (!anim || !b.animatorStateCaptured) return;
			if (b.animatorControllerDetached && fnSetAnimatorController)
			{
				fnSetAnimatorController(anim, b.savedAnimatorController);
			}
			// Rebinding an untouched, live graph resets its native dynamic state.
			// Also avoid evaluating it twice through currentBinding + cachedAnimator.
			if (rebind && (b.animatorControllerDetached || b.animatorWasDisabled || b.animatorRebound) &&
				fnAnimatorRebind)
			{
				fnAnimatorRebind(anim);
			}
			if (b.animatorWasDisabled && fnSetAnimatorEnabled)
			{
				fnSetAnimatorEnabled(anim, b.savedAnimatorEnabled);
			}
			b.animatorStateCaptured = false;
			b.savedAnimatorController = nullptr;
			b.animatorControllerDetached = false;
			b.animatorWasDisabled = false;
			b.animatorRebound = false;
		}

		void DefensiveMute(void* animatorComponent, void* playableDirector)
		{
			DefensiveMute(currentBinding.actor, animatorComponent, playableDirector);
		}

		void DefensiveMute(void* actor, void* animatorComponent, void* playableDirector)
		{
			void* act = actor ? actor : currentBinding.actor;
			if (!act) return;

			auto it = bindings.find(act);
			// Never mutate the current actor when a non-target callback arrives.
			if (!IsTargetActor(act) || it == bindings.end() || !it->second.bindPoseCaptured ||
				it->second.boundBoneCount == 0)
				return;
			IdolBoneBinding* bPtr = &it->second;

			if (animatorComponent)
			{
				bPtr->cachedAnimator = animatorComponent;
				cachedAnimator = animatorComponent;
			}
			if (playableDirector)
			{
				bPtr->cachedPlayableDirector = playableDirector;
			}

			// The official timeline can replace a controller or enable its Animator.
			// Legacy continually suppresses it; the experiment keeps its graph live.
			if (preserveAnimatorForSecondaryPhysics)
			{
				// Undo only our own legacy takeover on a hot A/B switch. Do not
				// freeze Animator.speed: its deltaTime also feeds the swing jobs.
				if (bPtr->animatorControllerDetached || bPtr->animatorWasDisabled || bPtr->animatorRebound)
				{
					RestoreAnimatorState(*bPtr, false);
				}
			}
			else if (bPtr->cachedAnimator)
			{
				CaptureAnimatorState(*bPtr);
				DetachAnimatorController(*bPtr);
				if (fnSetAnimatorEnabled)
				{
					fnSetAnimatorEnabled(bPtr->cachedAnimator, false);
					bPtr->animatorWasDisabled = true;
				}
			}

			if (!preserveAnimatorForSecondaryPhysics && bPtr->cachedPlayableDirector && fnPausePlayableDirector)
			{
				fnPausePlayableDirector(bPtr->cachedPlayableDirector);
			}

			bPtr->defensivelyMuted = true;
		}

		void ResetToRestPose(IdolBoneBinding& b)
		{
			if (!b.actor || !b.bindPoseCaptured) return;

			// 1. 物理回写根骨骼站姿标高，防止停顿残留导致下蹲过深
			if (b.rootTransform)
			{
				Vector3 worldHips = b.restActorPos + b.restActorRot.Rotate(b.restRootPos);
				if (fnSetWorldPosition)
				{
					fnSetWorldPosition(b.rootTransform, worldHips.x, worldHips.y, worldHips.z);
				}
				else if (fnSetLocalPosition)
				{
					fnSetLocalPosition(b.rootTransform, b.restRootPos.x, b.restRootPos.y, b.restRootPos.z);
				}
			}

			// 2. 物理回写所有已绑定骨骼的纯净静息旋转
			for (size_t i = 0; i < b.boneTransforms.size() && i < b.restRotations.size(); ++i)
			{
				void* trans = b.boneTransforms[i];
				if (!trans) continue;
				Quaternion qWorld = b.restActorRot * b.restRotations[i];
				if (fnSetWorldRotation)
				{
					fnSetWorldRotation(trans, qWorld.x, qWorld.y, qWorld.z, qWorld.w);
				}
				else if (fnSetLocalRotation)
				{
					fnSetLocalRotation(trans, b.restRotations[i].x, b.restRotations[i].y, b.restRotations[i].z,
					                   b.restRotations[i].w);
				}
			}

			// 3. 物理回写全部手臂辅助骨（_Roll_H 扭转骨 与 _H 关节半角骨）的初始静息局部/世界姿态
			auto restoreAuxRest = [this, &b](void* trans, const Quaternion& qRestLocal, const Quaternion& qRestRel)
			{
				if (!trans) return;
				if (fnSetWorldRotation)
				{
					Quaternion qWorld = b.restActorRot * qRestRel;
					fnSetWorldRotation(trans, qWorld.x, qWorld.y, qWorld.z, qWorld.w);
				}
				else if (fnSetLocalRotation)
				{
					fnSetLocalRotation(trans, qRestLocal.x, qRestLocal.y, qRestLocal.z, qRestLocal.w);
				}
			};
			restoreAuxRest(b.leftArmAux.upperArmRollH, b.leftArmAux.upperArmRollHRestLocal,
			               b.leftArmAux.upperArmRollHRestRel);
			restoreAuxRest(b.leftArmAux.upperArmH, b.leftArmAux.upperArmHRestLocal, b.leftArmAux.upperArmHRestRel);
			restoreAuxRest(b.leftArmAux.foreArmRollH, b.leftArmAux.foreArmRollHRestLocal,
			               b.leftArmAux.foreArmRollHRestRel);
			restoreAuxRest(b.leftArmAux.foreArmH, b.leftArmAux.foreArmHRestLocal, b.leftArmAux.foreArmHRestRel);
			restoreAuxRest(b.leftArmAux.handH, b.leftArmAux.handHRestLocal, b.leftArmAux.handHRestRel);

			restoreAuxRest(b.rightArmAux.upperArmRollH, b.rightArmAux.upperArmRollHRestLocal,
			               b.rightArmAux.upperArmRollHRestRel);
			restoreAuxRest(b.rightArmAux.upperArmH, b.rightArmAux.upperArmHRestLocal, b.rightArmAux.upperArmHRestRel);
			restoreAuxRest(b.rightArmAux.foreArmRollH, b.rightArmAux.foreArmRollHRestLocal,
			               b.rightArmAux.foreArmRollHRestRel);
			restoreAuxRest(b.rightArmAux.foreArmH, b.rightArmAux.foreArmHRestLocal, b.rightArmAux.foreArmHRestRel);
			restoreAuxRest(b.rightArmAux.handH, b.rightArmAux.handHRestLocal, b.rightArmAux.handHRestRel);
			if (!b.upperChestInClip)
				restoreAuxRest(b.upperChestTransform, Quaternion::Identity(), b.upperChestRestRel);
		}

		BodyFaceRestoreResult RestoreForCapture()
		{
			BodyFaceRestoreResult result;
			if (captureRestoreUnconfirmed)
			{
				result.bodyModified = true;
				result.bodyConfirmed = false;
			}
			auto closePosition = [](const float* actual, const Vector3& expected)
			{
				return std::isfinite(actual[0]) && std::isfinite(actual[1]) && std::isfinite(actual[2]) &&
					std::fabs(actual[0] - expected.x) <= 1e-4f && std::fabs(actual[1] - expected.y) <= 1e-4f &&
					std::fabs(actual[2] - expected.z) <= 1e-4f;
			};
			auto checkRotation = [this](void* transform, const Quaternion& expected)
			{
				if (!transform || !fnGetWorldRotation) return false;
				float q[4]{NAN, NAN, NAN, NAN}; fnGetWorldRotation(transform, q);
				const Quaternion actual{q[0], q[1], q[2], q[3]};
				const float norm = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
				return std::isfinite(norm) && std::fabs(norm - 1.0f) <= 1e-3f &&
					std::fabs(Quaternion::Dot(actual, expected)) >= 1.0f - 1e-4f;
			};
			auto restoreOne = [&](IdolBoneBinding& b)
			{
				const bool bodyOwned = b.animatorStateCaptured || b.defensivelyMuted || b.eyePoseModified;
				result.bodyModified |= bodyOwned;
				result.gazeModified |= b.eyePoseModified;
				bool bodyOk = true;
				if (bodyOwned)
				{
					const auto before = b;
					if (!b.actor || !b.bindPoseCaptured || !fnSetWorldRotation || !fnGetWorldRotation ||
						!fnSetWorldPosition || !fnGetWorldPosition) bodyOk = false;
					else
					{
						ResetToRestPose(b);
						if (b.rootTransform)
						{
							float p[3]{NAN,NAN,NAN}; fnGetWorldPosition(b.rootTransform, p);
							if (!closePosition(p, b.restActorPos + b.restActorRot.Rotate(b.restRootPos))) bodyOk = false;
						}
						if (b.boneTransforms.size() != b.restRotations.size()) bodyOk = false;
						for (size_t i = 0; i < b.boneTransforms.size() && i < b.restRotations.size(); ++i)
							if (b.boneTransforms[i] && !checkRotation(b.boneTransforms[i], b.restActorRot * b.restRotations[i])) bodyOk = false;
						auto aux = [&](void* t, const Quaternion& q) { if (t && !checkRotation(t, b.restActorRot * q)) bodyOk = false; };
						for (const auto* chain : {&b.leftArmAux, &b.rightArmAux})
						{
							aux(chain->upperArmRollH, chain->upperArmRollHRestRel); aux(chain->upperArmH, chain->upperArmHRestRel);
							aux(chain->foreArmRollH, chain->foreArmRollHRestRel); aux(chain->foreArmH, chain->foreArmHRestRel);
							aux(chain->handH, chain->handHRestRel);
						}
						if (!b.upperChestInClip) aux(b.upperChestTransform, b.upperChestRestRel);
					}
					RestoreAnimatorState(b, true);
					if (before.animatorStateCaptured)
					{
						if (!before.cachedAnimator || !fnGetAnimatorController || !fnGetAnimatorEnabled ||
							fnGetAnimatorController(before.cachedAnimator) != before.savedAnimatorController ||
							fnGetAnimatorEnabled(before.cachedAnimator) != before.savedAnimatorEnabled) bodyOk = false;
					}
					// No saved PlayableDirector play-state or resume API exists here.
					// A graph actually paused by GM cannot be certified as restored.
					if (!preserveAnimatorForSecondaryPhysics && before.cachedPlayableDirector && fnPausePlayableDirector) bodyOk = false;
					for (int side=0; side<2; ++side)
					{
						const auto saved = before.waistSupport[side];
						if (!saved.captured) continue;
						WaistSupportChain live;
						if (!ResolveWaistSupportTarget(b, side, live) || !saved.SameTargetIdentity(live) ||
							!fnGetLocalPosition || !fnGetLocalRotation) { bodyOk = false; continue; }
						RestoreWaistSupport(b);
						float p[3]{NAN,NAN,NAN}, q[4]{NAN,NAN,NAN,NAN};
						fnGetLocalPosition(live.helper, p); fnGetLocalRotation(live.helper, q);
						if (!closePosition(p,saved.savedLocalPosition) ||
							!std::isfinite(q[0]+q[1]+q[2]+q[3]) ||
							std::fabs(Quaternion::Dot({q[0],q[1],q[2],q[3]},saved.savedLocalRotation)) < 1.0f-1e-4f) bodyOk=false;
					}
					b.defensivelyMuted = !bodyOk && before.defensivelyMuted;
					if (!bodyOk)
					{
						b.animatorStateCaptured = before.animatorStateCaptured;
						b.savedAnimatorController = before.savedAnimatorController;
						b.savedAnimatorEnabled = before.savedAnimatorEnabled;
						b.animatorControllerDetached = before.animatorControllerDetached;
						b.animatorWasDisabled = before.animatorWasDisabled;
						b.animatorRebound = before.animatorRebound;
						b.waistSupport = before.waistSupport;
					}
					result.bodyConfirmed &= bodyOk;
					if (b.eyePoseModified) result.gazeConfirmed &= bodyOk;
					if (bodyOk) b.eyePoseModified = false;
				}
				const auto face = b.face.RestoreForCapture(b.actor, faceApi);
				result.faceModified |= face.modified; result.faceConfirmed &= face.confirmed;
				for (int slot=0; slot<2; ++slot)
				{
					const auto talk = b.talks[slot].RestoreForCapture(b.actor, slot, faceApi);
					result.faceModified |= talk.modified; result.faceConfirmed &= talk.confirmed;
				}
			};
			try
			{
				for (auto& entry : bindings) restoreOne(entry.second);
				auto current = bindings.find(currentBinding.actor);
				if (current != bindings.end()) currentBinding = current->second;
				else restoreOne(currentBinding);
				cachedAnimator = nullptr;
			}
			catch (...) { captureRestoreUnconfirmed = true; result.bodyConfirmed = result.faceConfirmed = result.gazeConfirmed = false; }
			return result;
		}

		void RestoreMute(void* animatorComponent = nullptr, void* playableDirector = nullptr)
		{
			if (captureCheckEnforced) { RestoreForCapture(); return; }
			(void)animatorComponent;
			(void)playableDirector;
			auto restoreOne = [this](IdolBoneBinding& b)
			{
				if (!b.actor || !b.bindPoseCaptured) return;
				if (b.animatorStateCaptured || b.defensivelyMuted)
				{
					// 停止播放瞬间，立即回写纯净静息姿态，杜绝停在动作中间帧
					ResetToRestPose(b);
					RestoreAnimatorState(b, true);
					b.defensivelyMuted = false;
				}
				// Controller activation/Rebind may reset facial data too. Restore
				// the pre-takeover face only after those Animator mutations finish.
				b.face.Restore(b.actor, faceApi);
				for (int slot = 0; slot < 2; ++slot) b.talks[slot].Restore(b.actor, slot, faceApi);
				RestoreWaistSupport(b);
				b.eyePoseModified = false;
			};

			// Scene transitions discard this map before it can be restored. Within
			// the same scene restore each owned actor once, including ALL-target mode.
			for (auto& entry : bindings) restoreOne(entry.second);
			auto current = bindings.find(currentBinding.actor);
			if (current != bindings.end())
			{
				currentBinding = current->second;
			}
			else
			{
				restoreOne(currentBinding);
			}
			cachedAnimator = nullptr;
		}

		// Helper: Euler degrees to Quaternion (XYZ)
		static Quaternion FromEulerXYZ(float pitchDeg, float yawDeg, float rollDeg)
		{
			return Quaternion::FromEulerDegreesXYZ(pitchDeg, yawDeg, rollDeg);
		}

		// =========================================================================
		// 官方 QuartzDriver 原生骨骼解算与保角共形数学变换引擎 (Phase 3 源码级移植)
		// 逆向自: ActorAnimation.ActorAnimationMathUtility & ActorAnimationQuartzDriver
		// 特性: 100% 纯静态内联 C++ 数学实现，零虚表调用、零游戏内部指针依赖
		// =========================================================================
		struct OfficialQuartzMath
		{
			static constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
			static constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;

			// 1. FormatRangeDegree (RVA 0x9b02d0): 官方角度折叠收敛至 [-180, +180] 区间
			static inline float FormatRangeDegree(float deg) noexcept
			{
				if (deg > 180.0f) return deg - 360.0f;
				if (deg < -180.0f) return deg + 360.0f;
				return deg;
			}

			// 2. SingleCoefficientConversion (RVA 0x9b0760): 官方单轴限位与系数缩放映射
			static inline float SingleCoefficientConversion(float angle, float coef, float minVal = -180.0f,
			                                                float maxVal = 180.0f) noexcept
			{
				float clamped = angle;
				if (clamped < minVal) clamped = minVal;
				if (clamped > maxVal) clamped = maxVal;
				return clamped * coef;
			}

			// 3. FromToRotation (RVA 0x9b0300): 官方向量正交基旋转四元数求取（零扭转测地大圆 Swing）
			static inline Quaternion FromToRotation(const Vector3& from, const Vector3& to) noexcept
			{
				float lenFrom = std::sqrt(from.x * from.x + from.y * from.y + from.z * from.z);
				float lenTo = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
				if (lenFrom <= 1e-6f || lenTo <= 1e-6f) return Quaternion::Identity();

				Vector3 v0(from.x / lenFrom, from.y / lenFrom, from.z / lenFrom);
				Vector3 v1(to.x / lenTo, to.y / lenTo, to.z / lenTo);

				float d = v0.x * v1.x + v0.y * v1.y + v0.z * v1.z;
				if (d >= 0.999999f)
				{
					return Quaternion::Identity();
				}
				if (d <= -0.999999f)
				{
					Vector3 ortho = std::abs(v0.x) > 0.5f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
					Vector3 axis(
						v0.y * ortho.z - v0.z * ortho.y,
						v0.z * ortho.x - v0.x * ortho.z,
						v0.x * ortho.y - v0.y * ortho.x
					);
					float axisLen = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
					if (axisLen > 1e-6f)
					{
						axis.x /= axisLen;
						axis.y /= axisLen;
						axis.z /= axisLen;
					}
					return Quaternion(axis.x, axis.y, axis.z, 0.0f);
				}

				Vector3 c(
					v0.y * v1.z - v0.z * v1.y,
					v0.z * v1.x - v0.x * v1.z,
					v0.x * v1.y - v0.y * v1.x
				);
				Quaternion q(c.x, c.y, c.z, 1.0f + d);
				return q.Normalized();
			}

			// 4. BendRoll 解耦结果
			struct BendRollResult
			{
				float rollAngleDeg{0.0f}; // 沿主骨轴的纯 Roll 角（度）
				float bendYDeg{0.0f}; // 侧摆角 Y（度）
				float bendZDeg{0.0f}; // 侧摆角 Z（度）
				Quaternion qBend; // 纯保角侧摆（Swing）四元数
				Quaternion qRoll; // 纯轴向扭转（Roll）四元数
			};

			// 5. EulerToBendroll (RVA 0x9affd0): 官方保角共形投影与轴向分解核心函数
			// 沿骨轴方向向量 uLocal，将旋转解算为零扭转侧摆 qBend 与纯轴向扭转 qRoll
			static inline BendRollResult EulerToBendroll(
				const Quaternion& q,
				const Vector3& boneAxis) noexcept
			{
				BendRollResult res;
				float axisLen = std::sqrt(boneAxis.x * boneAxis.x + boneAxis.y * boneAxis.y + boneAxis.z * boneAxis.z);
				Vector3 u = (axisLen > 1e-6f)
					            ? Vector3(boneAxis.x / axisLen, boneAxis.y / axisLen, boneAxis.z / axisLen)
					            : Vector3(1.0f, 0.0f, 0.0f);

				// 1. 将骨轴经 q 旋转得到目标朝向矢量 V
				Vector3 V = q.Rotate(u);

				// 2. 官方 FromToRotation: 计算零扭转测地大圆侧摆四元数 qBend
				res.qBend = FromToRotation(u, V);

				// 3. 从总旋转剥离出纯轴向扭转: q = qBend * qRoll => qRoll = qBend^-1 * q
				res.qRoll = (res.qBend.Inverse() * q).Normalized();

				// 消除 4D 球面最短路径符号翻转
				if (res.qRoll.w < 0.0f)
				{
					res.qRoll = Quaternion(-res.qRoll.x, -res.qRoll.y, -res.qRoll.z, -res.qRoll.w);
				}

				// 4. 计算纯 Roll 角（沿骨轴投影）并经 FormatRangeDegree 约束
				float rollProj = res.qRoll.x * u.x + res.qRoll.y * u.y + res.qRoll.z * u.z;
				float rawRollAngle = 2.0f * std::atan2(rollProj, res.qRoll.w) * kRad2Deg;
				res.rollAngleDeg = FormatRangeDegree(rawRollAngle);

				// 5. 官方保角共形投影侧摆角: tan(theta/2) = V_lat / (V_ax + 1)
				float uDotV = u.x * V.x + u.y * V.y + u.z * V.z;
				float denom = std::max(0.0001f, uDotV + 1.0f);
				Vector3 lat(V.x - u.x * uDotV, V.y - u.y * uDotV, V.z - u.z * uDotV);
				res.bendYDeg = FormatRangeDegree(2.0f * std::atan2(lat.y, denom) * kRad2Deg);
				res.bendZDeg = FormatRangeDegree(2.0f * std::atan2(lat.z, denom) * kRad2Deg);

				return res;
			}

			// 6. AxisrotationToQuaternionWithBendRoll (RVA 0x9ae2f0): 官方正交重构公式
			// 将纯侧摆四元数 qBend 与经过系数换算后的 Roll 角度合成最终四元数
			static inline Quaternion AxisrotationToQuaternionWithBendRoll(
				const Quaternion& qBend,
				float rollAngleDeg,
				const Vector3& boneAxis) noexcept
			{
				float axisLen = std::sqrt(boneAxis.x * boneAxis.x + boneAxis.y * boneAxis.y + boneAxis.z * boneAxis.z);
				Vector3 u = (axisLen > 1e-6f)
					            ? Vector3(boneAxis.x / axisLen, boneAxis.y / axisLen, boneAxis.z / axisLen)
					            : Vector3(1.0f, 0.0f, 0.0f);

				float halfRad = (rollAngleDeg * 0.5f) * kDeg2Rad;
				float sinH = std::sin(halfRad);
				float cosH = std::cos(halfRad);
				Quaternion qRoll(u.x * sinH, u.y * sinH, u.z * sinH, cosH);
				return (qBend * qRoll).Normalized();
			}

			// 7. AxisrotationToQuaternionWithBendAngles (RVA 0x9ae2f0 纯角度闭环版):
			// 官方反向立体保角共形投影: 从 (bendYDeg, bendZDeg) 与 rollAngleDeg 纯角度直接重构四元数 (100% 零Slerp)
			static inline Quaternion AxisrotationToQuaternionWithBendAngles(
				float bendYDeg,
				float bendZDeg,
				float rollAngleDeg,
				const Vector3& boneAxis) noexcept
			{
				float axisLen = std::sqrt(boneAxis.x * boneAxis.x + boneAxis.y * boneAxis.y + boneAxis.z * boneAxis.z);
				Vector3 u = (axisLen > 1e-6f)
					            ? Vector3(boneAxis.x / axisLen, boneAxis.y / axisLen, boneAxis.z / axisLen)
					            : Vector3(1.0f, 0.0f, 0.0f);

				float halfRadY = (bendYDeg * 0.5f) * kDeg2Rad;
				float halfRadZ = (bendZDeg * 0.5f) * kDeg2Rad;
				float tY = std::tan(halfRadY);
				float tZ = std::tan(halfRadZ);
				float sq = tY * tY + tZ * tZ;
				float denom = 1.0f + sq;

				Vector3 V(
					(u.x < 0.0f) ? -(1.0f - sq) / denom : (1.0f - sq) / denom,
					(2.0f * tY) / denom,
					(2.0f * tZ) / denom
				);

				Quaternion qBend = FromToRotation(u, V);

				float halfRollRad = (rollAngleDeg * 0.5f) * kDeg2Rad;
				float sinH = std::sin(halfRollRad);
				float cosH = std::cos(halfRollRad);
				Quaternion qRoll(u.x * sinH, u.y * sinH, u.z * sinH, cosH);

				return (qBend * qRoll).Normalized();
			}
		};

		// -------------------------------------------------------------------------
		// 官方原生 QuartzDriver 局部保角解算器 (Pure Conformal Local Solver)
		// 1. 严格在局部关节增量空间 (dQ_local) 中求取纯净保角分解
		// 2. 官方 EulerToBendroll 分离纯轴向扭转 (Roll) 与保角侧摆角 (BendY, BendZ)
		// 3. 严格禁止 4D 四元数 Slerp，全链路采用 SingleCoefficientConversion 线性保角缩放
		// 4. AxisrotationToQuaternionWithBendAngles 正交合成最终纯净局部增量
		// -------------------------------------------------------------------------
		struct QuartzDriverAuxResult
		{
			Quaternion qMainLocal; // 主骨骼局部增量（100% 原始动力学，保持肘关节与前臂铰链朝向原态）
			Quaternion qRollHLocal; // _Roll_H 局部相对姿态（100% Bend + 50% Roll，用于消除糖果纸扭转）
			Quaternion qJointHLocal; // _H 关节半角支撑骨局部增量（50% Bend + 0% Roll）
		};

		static inline QuartzDriverAuxResult SolveQuartzAuxiliaryBones(
			const Quaternion& dQ_local,
			const Vector3& boneAxis,
			bool hasRollH) noexcept
		{
			QuartzDriverAuxResult res;
			OfficialQuartzMath::BendRollResult bendRoll = OfficialQuartzMath::EulerToBendroll(dQ_local, boneAxis);

			// 1. _H 纯弯曲半角支撑骨：0% Roll, 50% Bend Y, 50% Bend Z
			float halfBendY = OfficialQuartzMath::SingleCoefficientConversion(bendRoll.bendYDeg, 0.5f);
			float halfBendZ = OfficialQuartzMath::SingleCoefficientConversion(bendRoll.bendZDeg, 0.5f);
			res.qJointHLocal = OfficialQuartzMath::AxisrotationToQuaternionWithBendAngles(
				halfBendY, halfBendZ, 0.0f, boneAxis);

			// 2. _Roll_H 轴向扭转分流骨：100% Bend Y, 100% Bend Z, 50% Roll
			if (hasRollH)
			{
				float halfRollDeg = OfficialQuartzMath::SingleCoefficientConversion(bendRoll.rollAngleDeg, 0.5f);
				res.qRollHLocal = OfficialQuartzMath::AxisrotationToQuaternionWithBendAngles(
					bendRoll.bendYDeg, bendRoll.bendZDeg, halfRollDeg, boneAxis);
			}
			else
			{
				res.qRollHLocal = Quaternion::Identity();
			}

			// 3. 主骨骼承担 100% 全量位姿（100% Bend + 100% Roll）
			res.qMainLocal = dQ_local;

			return res;
		}

		void ApplyMotionToBinding(IdolBoneBinding& b, const MotionClip& clip, float currentTimeSeconds,
		                          float facialTimeSeconds = -1.0f)
		{
			if (!b.actor || !b.bindPoseCaptured || b.boneTransforms.empty()) return;

			++footPoseWriteStamp.serial;
			footPoseWriteStamp.actor = b.actor;
			footPoseWriteStamp.mask = 0;
			footPoseWriteStamp.transforms = {};
			++eyePoseWriteStamp.serial;
			eyePoseWriteStamp.actor = b.actor;
			eyePoseWriteStamp.mask = 0;
			eyePoseWriteStamp.transforms = {};

			Vector3 vmdRootPos;
			std::vector<Quaternion> vmdRotations;
			clip.Sample(currentTimeSeconds, vmdRootPos, vmdRotations);
			Quaternion wristShare[2]{Quaternion::Identity(), Quaternion::Identity()};
			bool wristShareActive[2]{false, false};
			if (clip.IsLocalDeltaStream() && (wristTwistShare != 0.0f || forearmAxisAdjustmentDeg != 0.0f))
			{
				for (int side = 0; side < 2; ++side)
				{
					const int foreHid = 15 + side, handHid = 17 + side;
					int foreIndex = -1, handIndex = -1;
					for (size_t i = 0; i < clip.bones.size() && i < b.boneTransforms.size(); ++i)
					{
						if (!b.boneTransforms[i]) continue;
						if (clip.bones[i].humanoid_id == foreHid) foreIndex = static_cast<int>(i);
						if (clip.bones[i].humanoid_id == handHid) handIndex = static_cast<int>(i);
					}
					if (foreIndex >= 0 && handIndex > foreIndex && static_cast<size_t>(handIndex) < vmdRotations.size() &&
					    static_cast<size_t>(handIndex) < clip.bone_parent_hids.size() &&
					    clip.bone_parent_hids[handIndex] == foreHid)
					{
						wristShare[side] = WristTwistShareRotation(vmdRotations[handIndex], wristTwistShare) *
							Quaternion::FromEulerDegreesXYZ(forearmAxisAdjustmentDeg, 0.0f, 0.0f);
						wristShareActive[side] = true;
					}
				}
			}
			if (facialTimeSeconds < 0.0f) facialTimeSeconds = currentTimeSeconds;
			if (facialTimeSeconds != currentTimeSeconds && std::any_of(clip.bones.begin(), clip.bones.end(),
				[](const auto& bone) { return bone.head_space_eye_delta; }))
			{
				// Eye rotations imported from the facial VMD share the pose stream,
				// but must follow that file's timing together with its morphs.
				Vector3 unusedRoot;
				std::vector<Quaternion> facialRotations;
				clip.Sample(facialTimeSeconds, unusedRoot, facialRotations);
				for (size_t i = 0; i < clip.bones.size(); ++i)
					if (clip.bones[i].head_space_eye_delta) vmdRotations[i] = facialRotations[i];
			}
			b.face.enableBaseline = facialEnableBaseline;
			b.face.baselineMultiplier = facialBaselineMultiplier;
			b.face.facialWeights = facialWeights;
			b.face.characterOverride = facialCharacterOverride;
			b.face.enableMikuMouthFit = facialMikuMouthFit;
			b.face.enableMikuEyeFit = facialMikuEyeFit;
			b.face.facialFitAllowUnverified = facialMikuFitAllowUnverified;
			b.face.Apply(b.actor, clip, facialTimeSeconds, faceApi);
			// Roles come from the binding, never from pointer arithmetic on the
			// compiled table: an external profile has no home there.
			const unsigned fittedRole = b.face.fitRole;
			const unsigned eyeDecision = static_cast<unsigned>(b.face.mikuEyeDecision);
			guiMikuFitSnapshot.store(static_cast<unsigned>(b.face.mikuFitDecision) | (fittedRole << 8) |
				(eyeDecision << 16), std::memory_order_relaxed);

			// 1. Current Actor World Transform
			Vector3 curActorPos = b.restActorPos;
			Quaternion curActorRot = b.restActorRot;
			if (b.actorTransform && fnGetWorldPosition && fnGetWorldRotation)
			{
				float aPos[3]{0.0f, 0.0f, 0.0f};
				float aRot[4]{0.0f, 0.0f, 0.0f, 1.0f};
				fnGetWorldPosition(b.actorTransform, aPos);
				fnGetWorldRotation(b.actorTransform, aRot);
				curActorPos = Vector3(aPos[0], aPos[1], aPos[2]);
				curActorRot = Quaternion(aRot[0], aRot[1], aRot[2], aRot[3]);
			}
			const Quaternion placementYaw = initialYawDegrees == 0.0f ? Quaternion::Identity() :
				Quaternion::FromEulerDegreesXYZ(0.0f, initialYawDegrees, 0.0f);

			// Anchor supported v2+ clips to the actor pivot using one clip-wide
			// FK floor offset. Bind-time hips height never enters that path.
			// Other clips retain the legacy baseline. The user's initial placement
			// is a constant WORLD-space translation added after actor rotation;
			// it never enters the captured rest pose or clip's movement stream.
			if (b.rootTransform)
			{
				Vector3 localTarget(
					b.restRootPos.x + vmdRootPos.x,
					(b.clipGroundAnchored ? 0.0f : b.restRootPos.y) + vmdRootPos.y + b.groundLift +
					(rootHeightCalibrationEnabled ? b.calibratedRootHeight : 0.0f),
					b.restRootPos.z + vmdRootPos.z
				);
				// Rotate the clip's travel around its first sampled root, not the scene
				// origin: changing facing preserves the chosen starting placement.
				Vector3 yawTravelAdjustment{};
				if (initialYawDegrees != 0.0f && !clip.root_positions.empty())
				{
					const Vector3 worldTravel = curActorRot.Rotate(vmdRootPos - clip.root_positions.front());
					yawTravelAdjustment = placementYaw.Rotate(worldTravel) - worldTravel;
				}
				Vector3 worldTarget = curActorPos + curActorRot.Rotate(localTarget) + initialPositionOffset + yawTravelAdjustment;
				if (fnSetWorldPosition)
				{
					fnSetWorldPosition(b.rootTransform, worldTarget.x, worldTarget.y, worldTarget.z);
				}
				else if (fnSetLocalPosition)
				{
					localTarget = localTarget + curActorRot.Inverse().Rotate(initialPositionOffset + yawTravelAdjustment);
					fnSetLocalPosition(b.rootTransform, localTarget.x, localTarget.y, localTarget.z);
				}
			}
			// Turn the pose and clip travel without changing the actor transform;
			// yaw neither orbits the scene origin nor accumulates across frames.
			if (initialYawDegrees != 0.0f) curActorRot = placementYaw * curActorRot;

			// 3. Bone World Rotations
			bool hasDynamicArm = (armOffsetPitch != 0.0f || armOffsetYaw != 0.0f || armOffsetRoll != 0.0f);
			Quaternion qArmLeft = hasDynamicArm
				                      ? FromEulerXYZ(armOffsetPitch, armOffsetYaw, armOffsetRoll)
				                      : Quaternion::Identity();
			Quaternion qArmRight = hasDynamicArm
				                       ? FromEulerXYZ(armOffsetPitch, -armOffsetYaw, -armOffsetRoll)
				                       : Quaternion::Identity();

			// 4. 全身骨骼旋转自定义编辑（外部 JSON 热加载覆盖表）
			const bool overridesActive =
				(boneOverrides != nullptr) && boneOverrides->enabled && !boneOverrides->Empty();
			int appliedOverrides = 0;

			// 缓存本帧已写入骨骼的世界旋转，供下游子关节与半角支撑骨实时引用
			std::array<Quaternion, 64> writtenWorldRots;
			std::array<bool, 64> writtenWorldRotValid{};
			writtenWorldRotValid.fill(false);

			// PROBE A3/A6 fire once, on the first frame that actually writes a bone.
			bool probeA3Pending = probeFirstFrameDone ? false : true;

			for (size_t i = 0; i < clip.bones.size() && i < b.boneTransforms.size(); ++i)
			{
				void* trans = b.boneTransforms[i];
				if (!trans || i >= vmdRotations.size()) continue;

				const Quaternion& rVmd = vmdRotations[i];
				const Quaternion& rRestRel = (i < b.restRotations.size()) ? b.restRotations[i] : Quaternion::Identity();

				const int hid = clip.bones[i].humanoid_id;
				const bool isLegBone = (hid >= 1 && hid <= 6) || (hid == 19 || hid == 20);
				const bool isHandOrFingerBone = (hid == 17 || hid == 18) || (hid >= 24 && hid <= 53);
				const bool useRestBase = isLegBone || isHandOrFingerBone;

				// 姿态分流驱动架构：
				// 1. GKMO Version >= 2 (实机录制纯净流)：
				//    全身 52+ 根骨骼直接为 Actor 相对姿态，直接采用 curActorRot * rVmd 驱动，消除 R_rest_rel 误差与腿部飘移。
				// 2. GKMO Version 1 (传统 MMD 动作流)：
				//    下肢骨骼与手腕/手指骨骼必须以模型原生静息姿态 rRestRel 为基准累乘 (curActorRot * (rVmd * rRestRel))，防止一字马与折手。
				const bool isActorRelativeStream = (clip.version >= 2);
				Quaternion rTarget;
				if (!isActorRelativeStream && useRestRotations && useRestBase)
				{
					rTarget = curActorRot * (rVmd * rRestRel);
				}
				else
				{
					rTarget = curActorRot * rVmd;
				}

				// ---- gkclip: canonical LOCAL deltas ----------------------------
				// A gkclip stores, per bone, a full WORLD->LOCAL decomposition of
				// the source animation:
				//
				//     L[i] = inv(W[parent(i)]) * W[i]
				//
				// (see tools/vmd2gkmotion/gkclip_format.py, world_to_canonical_deltas).
				// To play that back so the target rig reproduces the SOURCE WORLD
				// orientations, the target's locals must equal the source's locals,
				// bone for bone. Proof by induction on the chain:
				//
				//   root:  W_t[0] = L_t[0], want = W_s[0] = L_s[0]  => L_t = L_s
				//   child: W_t[i] = W_t[p] * L_t[i], and assuming W_t[p] == W_s[p],
				//          want W_s[p] * L_s[i]  =>  L_t[i] = L_s[i]
				//
				// The target's REST POSE NEVER APPEARS. Rest affects where the bones
				// end up (bone lengths, offsets) but not the rotation composition --
				// so pre-multiplying restLocal re-bases every bone onto a second,
				// incompatible rest and destroys the pose.
				//
				// ★ SCOPE CORRECTION 2026-10-07. The proof is sound and still holds
				// for the TORSO, but its premise is that the rig's rest equals the
				// canonical all-Identity rest the gkclip format is defined against.
				// For a Unity humanoid that premise holds only approximately: torso
				// locals are small (Spine 2.47 deg, Neck 9.92 deg, shoulders ~5 deg)
				// so the bare delta lands correctly there, while the legs are NOT
				// close to identity (thighs 106.59 / 119.05 deg) and the bare delta
				// collapses them to horizontal. Hence legRestBasis below, scoped to
				// the leg chain: the same rule, applied only where the premise
				// actually fails.
				//
				// This is what produced the 2026-10-05 "twisted, floating" collapse:
				//   * PROBE A2 measured 10 of 22 bones with devFromIdentity > 45 deg
				//     (RightShoulder EXACTLY 180.00 deg, thighs 107/119 deg), so
				//     restLocal is a large, non-identity, per-bone rotation.
				//   * Composing it re-based each bone independently, and the
				//     RightShoulder's exact 180 deg pre-multiply cancelled the Y
				//     component of the clip's own shoulder rotation, pinning the
				//     shoulder near 180 deg at every frame (measured 179.6 / 166.6 /
				//     178.4 deg at frames 0 / 150 / 300).
				//   * Cross-check against the converter's own physical diagnostics:
				//     accumulated deltas match ground_truth_real.json's world
				//     rotations to 0.5-1.7 deg, whereas the raw deltas differ by
				//     12-34 deg. thigh-axis spread comes out 1.5-103 deg against an
				//     official leg_spread_deg of 1.06-70.5; composing restLocal
				//     instead gives 137-180 deg, i.e. the thighs pointing in
				//     OPPOSITE directions.
				//
				// PROBE A3 confirms the rest of the path is sound and is NOT the
				// cause: A4 15/15 IL2CPP bindings resolved, A5 confirms the
				// setLocalRotation branch, and write-vs-readback is 0.000 deg for
				// Hips / RightUpperArm / RightHand. The writes land; the composition
				// was wrong.
				Quaternion gkLocalOut = rTarget;
				const bool isGkClip = clip.IsLocalDeltaStream();
				if (isGkClip)
				{
					// The delta IS the local rotation. See the proof above.
					gkLocalOut = rVmd;
					if (hid == 0 && initialYawDegrees != 0.0f && !fnSetLocalRotation)
					{
						// The degraded path writes world rotations, so apply yaw in world space.
						gkLocalOut = placementYaw * gkLocalOut;
					}
					else if (hid == 0 && initialYawDegrees != 0.0f && fnGetTransformParent && fnGetWorldRotation)
					{
						// Convert the world-up turn to the actual Hips parent frame;
						// descendants keep their authored locals and follow the root.
						Quaternion parentRotation = Quaternion::Identity();
						bool parentRotationValid = true;
						if (void* parent = fnGetTransformParent(trans))
						{
							float q[4]{0, 0, 0, 1};
							fnGetWorldRotation(parent, q);
							const float length = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
							parentRotationValid = std::isfinite(length) && length > 1e-6f;
							if (parentRotationValid)
								parentRotation = Quaternion(q[0]/length, q[1]/length, q[2]/length, q[3]/length);
						}
						if (parentRotationValid)
						{
							const Quaternion localYaw = parentRotation.Inverse() * placementYaw * parentRotation;
							gkLocalOut = localYaw * gkLocalOut;
						}
					}
					if (clip.bones[i].head_space_eye_delta)
					{
						if (!b.eyeFrameResolved)
						{
							b.eyeTargetFrame = faceApi.getActorName ? FindEyeTargetFrame(faceApi.getActorName(b.actor)) : nullptr;
							b.eyeFrameResolved = true;
						}
						bool supported = false;
						if (b.eyeTargetFrame && fnGetTransformParent && fnGetBoneTransform && fnGetWorldRotation)
						{
							void* parent = fnGetTransformParent(trans);
							void* head = fnGetBoneTransform(b.actor, 10);
							if (parent && head)
							{
								float hp[4]{0,0,0,1}, pp[4]{0,0,0,1};
								fnGetWorldRotation(head, hp);
								fnGetWorldRotation(parent, pp);
								const Quaternion h(hp[0],hp[1],hp[2],hp[3]), p(pp[0],pp[1],pp[2],pp[3]);
								// Scale the head-space delta by this character's measured
								// response ratio before the parent-frame conjugation. A
								// gain of exactly 1 (or the switch off) is the raw source
								// rotation, so the control path stays bit-identical.
								const float eyeGain = eyePoseRelativeGain ? b.eyeTargetFrame->gain : 1.0f;
								supported = MapHeadSpaceEyeDelta(*b.eyeTargetFrame, hid, rVmd, h.Inverse()*p, eyeGain, gkLocalOut);
								if (supported && !b.eyeGainLogged)
								{
									GM_DEBUG("[GakuMotion][Eye] %s relative gain %.4f (%s)",
									          b.eyeTargetFrame->character, eyeGain,
									          eyePoseRelativeGain ? "geometry-relative" : "raw 1:1");
									b.eyeGainLogged = true;
								}
							}
						}
						if (!supported)
						{
							if (!b.eyeFrameWarningLogged && fnLogWarn)
								fnLogWarn("[GakuMotion][Eye] Eye delta skipped: unverified character or mounted parent frame/API; body unchanged");
							b.eyeFrameWarningLogged = true;
							continue;
						}
					}

					// ---- 2026-10-07 A/B SWITCH: leg rest basis -------------------
					// The proof above says the rest pose must not appear in the
					// gkclip composition, and the torso agrees: torsoVerticality
					// measures 0.86-1.00 against an official 0.852-1.000, i.e. the
					// spine and arms are exactly right.
					//
					// The legs are not. Same probe, same frame:
					//     segmentverticality   at bind     during playback
					//       thigh                -0.984          -0.157
					//       shin                 -0.935          -0.048
					//       thigh+shin drop      -0.776          -0.087
					// The bind-time numbers are a perfectly upright standing figure,
					// so the geometry is fine and the playback pose is what collapses
					// the legs to horizontal.
					//
					// A gkclip delta is defined against a canonical rest in which
					// every local is Identity. Unity's humanoid legs are NOT in that
					// state -- PROBE A2 measures LeftUpperLeg restLocal at 106.59 deg
					// from identity and RightUpperLeg at 119.05 deg -- so writing the
					// bare delta lands the leg on a different rest than the clip
					// assumed and the chain folds.
					//
					// The torso tolerates this because its rest locals are small
					// (Spine 2.47, Neck 9.92, shoulders ~5), which is why the arms
					// looked fine while the legs did not. That asymmetry is the
					// signature of a rest-basis error and nothing else.
					//
					// Settled on device, not by argument: the A/B table is on the
					// flag's declaration. thighVert -0.157 -> -0.856 and drop -0.085
					// -> -0.562, against a bind-time baseline of -0.984 / -0.776.
					//
					// ★ SCOPE 2026-10-07, gkclip version 2. v2 files carry
					// frame-ALIGNED deltas (the converter re-based every leg bone
					// by the constant K that maps the target's child-offset axis
					// onto the source's, so the joints land on the source's world
					// child directions). The measured proof is in
					// tools/vmd2gkmotion/verify/ab_knee_retarget.py: with aligned
					// deltas the bare H1 formula reproduces the official knee
					// flexion frame-by-frame (mean 3.4 deg, cross-pipeline noise),
					// whereas on the SAME clip the rest composition freezes the
					// knee at a constant 83 deg no matter what the choreography
					// says. Composing any rest into a v2 file RE-INTRODUCES that
					// frozen knee, so v2 skips this block entirely -- which also
					// makes leg playback independent of the bind-time rest
					// capture (a capture that is provably state-dependent:
					// same character, Foot restLocal 93.67 deg vs 101.39 deg in
					// two binds of one session).
					if (legRestBasis && clip.version < 2 && i < b.restLocals.size())
					{
						const Quaternion& rl = b.restLocals[i];
						const float w = rl.w < 0.0f ? -rl.w : rl.w;
						const float devDeg = 2.0f * std::acos(w > 1.0f ? 1.0f : w) * 57.29578f;
						const bool legish = (hid >= 1 && hid <= 6) || hid == 19 || hid == 20;
						// Only where the rest actually differs from identity: applying
						// it everywhere would undo the fix that cured the arm folding.
						if (legish && devDeg > 5.0f)
						{
							gkLocalOut = rl * rVmd;
						}
					}

					// rTarget is consumed downstream as a WORLD rotation (the aux
					// solver and writtenWorldRots[]), so it must be converted along
					// the parent chain -- NOT by multiplying a local quantity with
					// the actor rotation. For the chain root the local IS the world;
					// for every other bone the parent's world rotation has to come
					// from what this same loop already wrote for it.
					//
					// Bones are emitted in topological (parents-first) order, so a
					// single forward pass suffices. When the parent is absent from
					// the clip, the chain root's own local IS its world rotation --
					// exactly what world_to_canonical_deltas() emits for entries
					// missing from gk_parents.
					//
					// ★ 2026-10-07 ROOT FIX. This used to seed the walk with
					// `curActorRot`:
					//
					//     Quaternion parentWorld = curActorRot;   // WRONG
					//
					// curActorRot is the ACTOR ROOT's world orientation, not a bone's.
					// Seeding the chain with it injected an extra rigid Ry(90)
					// (measured exactly: curActorRot = (0, 0.7071, 0, 0.7071) =Ry90)
					// into every world rotation this function computes.
					//
					// Proof it is wrong, from the device (PROBE A7 + A8):
					//   ang(my pure-FK prediction, official truth)  =  0.89 - 17.67 deg
					//   ang(engine world rotation, official truth)  = 88.48 - 94.96 deg
					//   ang(Ry(-90) * engine world, official truth) =  0.00 - 17.71 deg
					// The +90 was mine, not the engine's, and removing it is what
					// makes the engine land back on the official pose.
					//
					// It also explains all three reported symptoms at once, which is
					// why this went unnoticed for so long:
					//   * thighs 90 deg off  -> horizontal legs      -> 一字马
					//   * pelvis 90 deg off  -> height collapses     -> "sits down"
					//   * whole body 90 deg off -> faces sideways    -> "面朝侧方"
					// The "sitting pose" screenshot was this bug seen from a distance.
					//
					// The root's world rotation is its local, full stop. The actor's
					// own orientation is applied by Unity when it evaluates the
					// transform, and double-applying it here is what broke the pose.
					//
					// CRITICAL: the lookup below is into writtenWorldRots[], which is
					// indexed BY HUMANOID ID, so the parent must come from
					// clip.bone_parent_hids -- NOT clip.bone_parents, which holds
					// `bones` indices. In a real gkclip 51 of 52 bones have
					// index != hid, so using bone_parents here gave 48 bones the
					// wrong parent world (Chest inherited LeftUpperLeg, LeftUpperArm
					// inherited RightFoot, ...) and collapsed the whole pose. An
					// earlier revision did exactly that; see
					// BuildBoneParentHids() for the full account.
					Quaternion parentWorld = Quaternion::Identity();
					const int parentHid = (i > 0 && i < clip.bone_parent_hids.size())
						                      ? clip.bone_parent_hids[i]
						                      : -1;
					if (parentHid >= 0 && parentHid < 64 && writtenWorldRotValid[parentHid])
					{
						parentWorld = writtenWorldRots[parentHid];
					}
					rTarget = parentWorld * gkLocalOut;
				}

				// 4a. 覆盖表注入：local = 局部后乘；world = 世界左乘
				//
				// In the gkclip path the quantity actually written is gkLocalOut
				// (see the write at the bottom of this loop), so the correction has
				// to be applied to BOTH: gkLocalOut for the local write, rTarget for
				// the aux solver and writtenWorldRots[]. Correcting only rTarget --
				// which is what an earlier revision did -- silently discarded every
				// override and every dynamic-arm offset for .gkclip playback.
				if (overridesActive)
				{
					if (const BoneRotationOffset* off = boneOverrides->Find(hid, clip.bones[i].name))
					{
						const Quaternion qOff = off->Resolve();
						rTarget = off->spaceWorld ? (qOff * rTarget) : (rTarget * qOff);
						if (isGkClip)
						{
							// A world-space offset cannot be pushed into a local
							// write; convert it to the bone's parent frame first.
							// Quaternions commute with the parent extraction only if
							// the offset is conjugated by the parent, so do it
							// explicitly rather than silently dropping the override.
							// writtenWorldRots[] is hid-indexed -> bone_parent_hids,
							// for the same reason as the main path above.
							//
							// 2026-10-07: this seeded the parent with curActorRot, the
							// same defect fixed in the main path above -- curActorRot
							// is the actor ROOT's orientation, not a bone's, and
							// seeding a bone chain with it injects a rigid Ry(90).
							// A chain root's world rotation is its local, full stop.
							const int pHid = (i > 0 && i < clip.bone_parent_hids.size())
								                 ? clip.bone_parent_hids[i]
								                 : -1;
							Quaternion parentWorld = Quaternion::Identity();
							if (pHid >= 0 && pHid < 64 && writtenWorldRotValid[pHid])
							{
								parentWorld = writtenWorldRots[pHid];
							}
							const Quaternion qOffLocal = parentWorld.Inverse() * qOff * parentWorld;
							gkLocalOut = off->spaceWorld
								             ? (qOffLocal * gkLocalOut)
								             : (gkLocalOut * qOff);
						}
						++appliedOverrides;
					}
				}

				// 4b. Optional Dynamic arm offset微调 (LeftUpperArm = 13, RightUpperArm = 14)
				// Right-multiplied: this is a joint-space tweak, and in the gkclip
				// path it must also reach gkLocalOut for the same reason as 4a.
				if (hasDynamicArm)
				{
					if (hid == 13)
					{
						rTarget = rTarget * qArmLeft;
						if (isGkClip) gkLocalOut = gkLocalOut * qArmLeft;
					}
					else if (hid == 14)
					{
						rTarget = rTarget * qArmRight;
						if (isGkClip) gkLocalOut = gkLocalOut * qArmRight;
					}
				}

				// Transfer axial rotation from hand to forearm. Forearm offsets lie
				// along local X; the inverse hand pre-rotation preserves P*L exactly.
				if (isGkClip && hid >= 15 && hid <= 18)
				{
					const int side = (hid == 15 || hid == 17) ? 0 : 1;
					if (wristShareActive[side])
					{
						if (hid == 15 || hid == 16)
						{
							gkLocalOut = gkLocalOut * wristShare[side];
							rTarget = rTarget * wristShare[side];
						}
						else
						{
							gkLocalOut = wristShare[side].Inverse() * gkLocalOut;
							if (writtenWorldRotValid[15 + side]) rTarget = writtenWorldRots[15 + side] * gkLocalOut;
						}
					}
				}

				// 4c. 原生辅助骨分流：
				// (1) 官方 QuartzDriver 保角共形重构 + _Roll_H 局部 50% 继承（主管骨骼轴向肌肉扭转）
				// (2) 关节 50% 弯曲半角支撑骨 _H（主管肩袖腋下、手肘、手腕弯曲支撑，根除抬手夹角凹陷破面）
				void* auxRollH = nullptr;
				Quaternion qAuxRollHWorld;
				const Quaternion* auxRollHRestLocal = nullptr;

				void* auxJointH = nullptr;
				Quaternion qJointHWorld;
				const Quaternion* auxJointHRestLocal = nullptr;
				Quaternion qParentForLocal = rTarget;

				if (hid == 13 || hid == 14)
				{
					// 大臂骨（LeftUpperArm=13, RightUpperArm=14）
					const bool isLeft = (hid == 13);
					const Vector3 uLocal = isLeft ? Vector3(-1.0f, 0.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
					const int shoulderHid = isLeft ? 11 : 12;

					Quaternion qShoulderWorld = curActorRot;
					if (shoulderHid < 64 && writtenWorldRotValid[shoulderHid])
					{
						qShoulderWorld = writtenWorldRots[shoulderHid];
					}
					else
					{
						void* shoulderTrans = isLeft ? b.leftArmAux.shoulder : b.rightArmAux.shoulder;
						if (shoulderTrans && fnGetWorldRotation)
						{
							float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
							fnGetWorldRotation(shoulderTrans, r);
							qShoulderWorld = Quaternion(r[0], r[1], r[2], r[3]);
						}
						else
						{
							qShoulderWorld = curActorRot * (isLeft
								                                ? b.leftArmAux.shoulderRestRel
								                                : b.rightArmAux.shoulderRestRel);
						}
					}
					const Quaternion qShoulderRestWorld = curActorRot * (isLeft
						                                                     ? b.leftArmAux.shoulderRestRel
						                                                     : b.rightArmAux.shoulderRestRel);
					const Quaternion qUpperArmRestWorld = curActorRot * (isLeft
						                                                     ? b.leftArmAux.upperArmRestRel
						                                                     : b.rightArmAux.upperArmRestRel);

					// 纯局部空间坐标变换（无任何世界空间增量或非线性 Slerp）：
					const Quaternion qLocalRest = qShoulderRestWorld.Inverse() * qUpperArmRestWorld;
					const Quaternion qLocalCur = qShoulderWorld.Inverse() * rTarget;
					const Quaternion dQ_local = qLocalRest.Inverse() * qLocalCur;

					const bool hasRollH = isLeft
						                      ? (b.leftArmAux.upperArmRollH != nullptr)
						                      : (b.rightArmAux.upperArmRollH != nullptr);
					QuartzDriverAuxResult aux = SolveQuartzAuxiliaryBones(dQ_local, uLocal, hasRollH);

					// 1. 主骨骼保持 100% 原始动力学位姿（rTarget 不做人为截断，确保肘关节与前臂铰链朝向原态）

					// 2. UpperArm_Roll_H 世界空间承担 100% Bend + 50% Roll
					auxRollH = isLeft ? b.leftArmAux.upperArmRollH : b.rightArmAux.upperArmRollH;
					auxRollHRestLocal = isLeft
						                    ? &b.leftArmAux.upperArmRollHRestLocal
						                    : &b.rightArmAux.upperArmRollHRestLocal;
					if (auxRollH)
					{
						qAuxRollHWorld = qShoulderWorld * (qLocalRest * aux.qRollHLocal);
					}

					// 3. 肩袖/腋下半角支撑骨 (LeftArm_H / RightArm_H): 承载 50% Bend, 0% Roll
					auxJointH = isLeft ? b.leftArmAux.upperArmH : b.rightArmAux.upperArmH;
					auxJointHRestLocal = isLeft ? &b.leftArmAux.upperArmHRestLocal : &b.rightArmAux.upperArmHRestLocal;
					if (auxJointH)
					{
						qJointHWorld = qShoulderWorld * (qLocalRest * aux.qJointHLocal);
						qParentForLocal = rTarget;
					}
				}
				else if (hid == 15 || hid == 16)
				{
					// 前臂骨（LeftLowerArm=15, RightLowerArm=16）
					const bool isLeft = (hid == 15);
					const Vector3 uLocal = isLeft ? Vector3(-1.0f, 0.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
					const int upperArmHid = isLeft ? 13 : 14;

					Quaternion qUpperArmWorld = curActorRot;
					if (upperArmHid < 64 && writtenWorldRotValid[upperArmHid])
					{
						qUpperArmWorld = writtenWorldRots[upperArmHid];
					}
					else
					{
						void* upperArmTrans = isLeft ? b.leftArmAux.upperArm : b.rightArmAux.upperArm;
						if (upperArmTrans && fnGetWorldRotation)
						{
							float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
							fnGetWorldRotation(upperArmTrans, r);
							qUpperArmWorld = Quaternion(r[0], r[1], r[2], r[3]);
						}
						else
						{
							qUpperArmWorld = curActorRot * (isLeft
								                                ? b.leftArmAux.upperArmRestRel
								                                : b.rightArmAux.upperArmRestRel);
						}
					}
					const Quaternion qUpperArmRestWorld = curActorRot * (isLeft
						                                                     ? b.leftArmAux.upperArmRestRel
						                                                     : b.rightArmAux.upperArmRestRel);
					const Quaternion qLowerArmRestWorld = curActorRot * rRestRel;

					// 纯局部空间坐标变换：
					const Quaternion qLocalRest = qUpperArmRestWorld.Inverse() * qLowerArmRestWorld;
					const Quaternion qLocalCur = qUpperArmWorld.Inverse() * rTarget;
					const Quaternion dQ_local = qLocalRest.Inverse() * qLocalCur;

					// 官方管线实证：前臂为严格 1-DOF 局部铰链，ForeArm_Roll_H 保持静息局部姿态 (hasRollH = false)
					QuartzDriverAuxResult aux = SolveQuartzAuxiliaryBones(dQ_local, uLocal, false);

					auxRollH = isLeft ? b.leftArmAux.foreArmRollH : b.rightArmAux.foreArmRollH;
					auxRollHRestLocal = isLeft
						                    ? &b.leftArmAux.foreArmRollHRestLocal
						                    : &b.rightArmAux.foreArmRollHRestLocal;
					if (auxRollH)
					{
						qAuxRollHWorld = rTarget * (isLeft
							                            ? b.leftArmAux.foreArmRollHRestLocal
							                            : b.rightArmAux.foreArmRollHRestLocal);
					}

					// 肘关节半角支撑骨 (LeftForeArm_H / RightForeArm_H): 承载纯弯曲半角 (50% Bend, 0% Roll)
					auxJointH = isLeft ? b.leftArmAux.foreArmH : b.rightArmAux.foreArmH;
					auxJointHRestLocal = isLeft ? &b.leftArmAux.foreArmHRestLocal : &b.rightArmAux.foreArmHRestLocal;
					if (auxJointH)
					{
						qJointHWorld = qUpperArmWorld * (qLocalRest * aux.qJointHLocal);
						qParentForLocal = rTarget;
					}
				}
				else if (hid == 17 || hid == 18)
				{
					// 手掌骨（LeftHand=17, RightHand=18）
					const bool isLeft = (hid == 17);
					const Vector3 uLocal = isLeft ? Vector3(-1.0f, 0.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
					const int foreArmHid = isLeft ? 15 : 16;

					Quaternion qForeArmWorld = curActorRot;
					if (foreArmHid < 64 && writtenWorldRotValid[foreArmHid])
					{
						qForeArmWorld = writtenWorldRots[foreArmHid];
					}
					else
					{
						void* foreArmTrans = isLeft ? b.leftArmAux.foreArm : b.rightArmAux.foreArm;
						if (foreArmTrans && fnGetWorldRotation)
						{
							float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
							fnGetWorldRotation(foreArmTrans, r);
							qForeArmWorld = Quaternion(r[0], r[1], r[2], r[3]);
						}
						else
						{
							qForeArmWorld = curActorRot * (isLeft
								                               ? b.leftArmAux.foreArmRestRel
								                               : b.rightArmAux.foreArmRestRel);
						}
					}
					const Quaternion qForeArmRestWorld = curActorRot * (isLeft
						                                                    ? b.leftArmAux.foreArmRestRel
						                                                    : b.rightArmAux.foreArmRestRel);
					const Quaternion qHandRestWorld = curActorRot * rRestRel;

					// 纯局部空间坐标变换：
					const Quaternion qLocalRest = qForeArmRestWorld.Inverse() * qHandRestWorld;
					const Quaternion qLocalCur = qForeArmWorld.Inverse() * rTarget;
					const Quaternion dQ_local = qLocalRest.Inverse() * qLocalCur;

					QuartzDriverAuxResult aux = SolveQuartzAuxiliaryBones(dQ_local, uLocal, false);

					// 腕关节半角支撑骨 (LeftHand_H / RightHand_H): 承载 50% Bend, 0% Roll
					auxJointH = isLeft ? b.leftArmAux.handH : b.rightArmAux.handH;
					auxJointHRestLocal = isLeft ? &b.leftArmAux.handHRestLocal : &b.rightArmAux.handHRestLocal;
					if (auxJointH)
					{
						qJointHWorld = qForeArmWorld * (qLocalRest * aux.qJointHLocal);
						qParentForLocal = qForeArmWorld; // LeftHand_H 是 LeftForeArm 的子节点
					}
				}

				// 写入主骨骼旋转
				//
				// gkclip MUST go through setLocalRotation. Handing a canonical delta
				// to setWorldRotation makes the engine solve for a local rotation
				// that back-subtracts the parent chain, which is a documented
				// failure mode in this project (it is what the "thin waist trough"
				// bug turned out to be). gk_world_dump keeps the absolute world
				// write, which is exactly what that format records.
				bool mainRotationWritten = false;
				if (isGkClip)
				{
					if (fnSetLocalRotation)
					{
						fnSetLocalRotation(trans, gkLocalOut.x, gkLocalOut.y, gkLocalOut.z, gkLocalOut.w);
						mainRotationWritten = true;
					}
					else if (fnSetWorldRotation)
					{
						// Degraded path: no local setter available, so fall back to
						// the world write using the already-composed rTarget.
						fnSetWorldRotation(trans, rTarget.x, rTarget.y, rTarget.z, rTarget.w);
						mainRotationWritten = true;
					}
				}
				else if (fnSetWorldRotation)
				{
					fnSetWorldRotation(trans, rTarget.x, rTarget.y, rTarget.z, rTarget.w);
					mainRotationWritten = true;
				}
				else if (fnSetLocalRotation)
				{
					fnSetLocalRotation(trans, rTarget.x, rTarget.y, rTarget.z, rTarget.w);
					mainRotationWritten = true;
				}
				if (mainRotationWritten && (hid == 5 || hid == 6))
				{
					const unsigned side = static_cast<unsigned>(hid - 5);
					footPoseWriteStamp.mask |= 1u << side;
					footPoseWriteStamp.transforms[side] = trans;
				}
				if (mainRotationWritten && (hid == 21 || hid == 22)) b.eyePoseModified = true;
				if (mainRotationWritten && clip.bones[i].head_space_eye_delta && (hid == 21 || hid == 22))
				{
					const unsigned side = static_cast<unsigned>(hid - 21);
					eyePoseWriteStamp.mask |= 1u << side;
					eyePoseWriteStamp.transforms[side] = trans;
					eyePoseWriteStamp.localTargets[side] = gkLocalOut;
				}

				if (hid >= 0 && hid < 64)
				{
					writtenWorldRots[hid] = rTarget;
					writtenWorldRotValid[hid] = true;
				}

				// ---- PROBE A7: the leg chain, self-contained ------------------------
				// WHY THIS EXISTS. The offline reconstruction says candidate B (write
				// the gkclip delta directly) reproduces the official leg spread to
				// 4.5 deg mean error, and that no sampled frame comes near 180 deg.
				// The on-device render shows ~180 deg. Both cannot be true, so the
				// disagreement must live in something the offline model lacks: the
				// engine's own post-write state, or a second code path.
				//
				// Everything is printed in ONE line so cross-frame pairing is
				// impossible (probe self-containment rule):
				//   wrote     - what we handed to setLocalRotation
				//   readback  - get_localRotation right after
				//   engWorld  - get_worldRotation right after. THIS is what the eye
				//               sees, and the only number comparable to a screenshot.
				//   predWorld - what pure FK over the raw deltas we just wrote predicts
				//   dWR / dPE - write-vs-readback and prediction-vs-engine
				// If engWorld tracks predWorld the engine obeyed us and the defect is
				// upstream (data, or a later pass). If it diverges, the engine rewrites
				// us and every offline model is moot.
				//
				// Bones 1..6 and 19,20: the whole leg chain including toes, since a
				// "horizontal split" could equally be a foot-pitch problem.
#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
				if (probeA3Pending && isGkClip && fnGetLocalRotation && fnGetWorldRotation
					&& ((hid >= 1 && hid <= 6) || hid == 19 || hid == 20))
				{
					// angle between two rotations, via |dot|. NEVER via the squared
					// norm: for a unit quaternion x^2+y^2+z^2+w^2 is identically 1,
					// so acos of it is always 0 (the old PROBE A6 bug).
					auto ang = [](const Quaternion& a, const Quaternion& c) -> float
					{
						float d = std::fabs(Quaternion::Dot(a, c));
						if (d > 1.0f) d = 1.0f;
						return 2.0f * std::acos(d) * 57.29578f;
					};
					auto devId = [&](const Quaternion& a) -> float
					{
						float w = a.w < 0.0f ? -a.w : a.w;
						if (w > 1.0f) w = 1.0f;
						return 2.0f * std::acos(w) * 57.29578f;
					};

					float rb[4]{0, 0, 0, 1};
					fnGetLocalRotation(trans, rb);
					const Quaternion qBack(rb[0], rb[1], rb[2], rb[3]);
					float gw[4]{0, 0, 0, 1};
					fnGetWorldRotation(trans, gw);
					const Quaternion qEngWorld(gw[0], gw[1], gw[2], gw[3]);

					// Parent lookup in BOTH id spaces, and print both so the reader
					// can check they agree (mixing them was the#025 hardware bug).
					// bone_parents[i] is an index into bones[]; bone_parent_hids[i] is
					// a humanoid id. Parent of bone i is bones[bone_parents[i]].
					int pIdx = -1;
					if (i < clip.bone_parents.size())
					{
						const int raw = clip.bone_parents[i];
						if (raw >= 0 && raw < (int)clip.bones.size()) pIdx = raw;
					}
					int pHidDirect = (i < clip.bone_parent_hids.size()) ? clip.bone_parent_hids[i] : -1;
					int pHid = (pIdx >= 0) ? clip.bones[pIdx].humanoid_id : -1;

					// Pure-FK prediction: walk the ancestor chain multiplying the raw
					// deltas we just wrote. Uses NO rest term and nothing the engine
					// did, so it is an honest prediction of the engine's FK.
					//
					// GUARD BUG, FIXED 2026-10-06. This loop read `cur > 0`, but the
					// PELVIS ROOT has humanoid_id 0. The condition therefore stopped
					// the walk before it did anything, and predWorld collapsed to
					// gkLocalOut -- the bone's own delta with the pelvis missing --
					// so every dPE in the first device run (a uniform 116.26 deg) was
					// meaningless. The loop must run until the parent is ABSENT, not
					// until the id is positive. Termination is guaranteed by the
					// guard counter and by the bone_parents lookup returning -1 at
					// the root, never by comparing an id to zero.
					Quaternion qPred = gkLocalOut;
					{
						Quaternion acc = gkLocalOut;
						int cur = pHid;
						for (int guard = 0; guard < 64 && cur >= 0; ++guard)
						{
							// locate the clip entry with this hid
							size_t k = clip.bones.size();
							for (size_t m = 0; m < clip.bones.size(); ++m)
							{
								if (clip.bones[m].humanoid_id == cur)
								{
									k = m;
									break;
								}
							}
							if (k >= clip.bones.size()) break;
							if (k >= vmdRotations.size()) break;
							acc = vmdRotations[k] * acc; // parent * child
							const int pi = clip.bone_parents[k];
							if (pi < 0 || pi >= (int)clip.bones.size()) break;
							cur = clip.bones[pi].humanoid_id;
						}
						qPred = acc;
					}

					if (fnLogInfo)
					{
						fnLogInfo("[PROBE A7] actor=%p hid=%2d %-15s t=%.3f pHid=%d(pIdx=%d,pHidTbl=%d)"
						          " | wrote=(%.4f,%.4f,%.4f,%.4f) readback=(%.4f,%.4f,%.4f,%.4f)"
						          " dWR=%6.2f | engWorld=(%.4f,%.4f,%.4f,%.4f)"
						          " predWorld=(%.4f,%.4f,%.4f,%.4f) dPE=%7.2f | engDevId=%7.2f",
						          b.actor, hid, clip.bones[i].name.c_str(), currentTimeSeconds,
						          pHid, pIdx, pHidDirect,
						          gkLocalOut.x, gkLocalOut.y, gkLocalOut.z, gkLocalOut.w,
						          rb[0], rb[1], rb[2], rb[3], ang(gkLocalOut, qBack),
						          gw[0], gw[1], gw[2], gw[3],
						          qPred.x, qPred.y, qPred.z, qPred.w, ang(qPred, qEngWorld),
						          devId(qEngWorld));
					}
				}
#endif // GAKUMOTION_ENABLE_GEOMETRY_PROBE

				// ---- PROBE A3: write-then-readback on the first playback frame ----
				// Rules out the last class of silent failure: the value being handed
				// to set_localRotation is correct in our memory but never reaching
				// the Transform (wrong method resolved, an Animator overriding us on
				// the following line, or a host that recomputes locals internally).
				// Reading back what the engine actually stored cannot lie.
				// One frame only, three representative bones.
#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
				if (probeA3Pending && fnGetLocalRotation && (hid == 0 || hid == 14 || hid == 18))
				{
					float back[4]{0.0f, 0.0f, 0.0f, 1.0f};
					fnGetLocalRotation(trans, back);
					const Quaternion qBack(back[0], back[1], back[2], back[3]);
					const float dotWant = std::fabs(gkLocalOut.x * back[0] + gkLocalOut.y * back[1]
						+ gkLocalOut.z * back[2] + gkLocalOut.w * back[3]);
					const float angWant = 2.0f * std::acos(dotWant > 1.0f ? 1.0f : dotWant) * 57.29578f;
					if (fnLogInfo)
					{
						fnLogInfo(
							"[PROBE A3] hid=%2d %-16s wrote=(%.4f,%.4f,%.4f,%.4f) readback=(%.4f,%.4f,%.4f,%.4f) diff=%7.3fdeg %s",
							hid, clip.bones[i].name.c_str(),
							gkLocalOut.x, gkLocalOut.y, gkLocalOut.z, gkLocalOut.w,
							back[0], back[1], back[2], back[3], angWant,
							angWant < 0.5f ? "OK" : "*** WRITE DID NOT LAND ***");
						// Everything needed to re-derive this line offline, on ONE
						// line. An earlier revision dumped restLocal at rebind time
						// (PROBE A2) and the delta at playback time (A6) as separate
						// blocks, which is unsound: A2 fires on every Rebind while
						// A3/A6 fire once per process, so with several clips played
						// in one session the two blocks can describe different binds.
						// That mistake produced a bogus "rest*delta disagrees with
						// wrote by 86 deg" conclusion. restLocal and rVmd are
						// therefore restated HERE, at the exact frame of the write,
						// together with which write branch actually ran.
						const Quaternion& rlNow = (i < b.restLocals.size())
							                          ? b.restLocals[i]
							                          : Quaternion::Identity();
						fnLogInfo(
							"[PROBE A3]   same-frame inputs: restLocal=(%.4f,%.4f,%.4f,%.4f) rVmd/delta=(%.4f,%.4f,%.4f,%.4f) writePath=%s overrides=%s dynamicArm=%s",
							rlNow.x, rlNow.y, rlNow.z, rlNow.w,
							rVmd.x, rVmd.y, rVmd.z, rVmd.w,
							fnSetLocalRotation
								? "setLocalRotation"
								: (fnSetWorldRotation ? "setWorldRotation(DEGRADED)" : "NONE"),
							overridesActive ? "ON" : "off",
							hasDynamicArm ? "ON" : "off");
						// PROBE A6: the root delta is what orients the whole pelvis.
						// Print it explicitly -- a ~154 deg Y rotation here rotates
						// every world-space direction that is derived downstream,
						// including the root translation direction.
						if (hid == 0)
						{
							// The angle from identity must come from |w|, NOT from the
							// squared norm. For a unit quaternion x^2+y^2+z^2+w^2 is
							// identically 1, so acos() of it is always 0 -- an earlier
							// revision of this probe printed angleFromIdentity=0.00deg
							// for a delta that was in fact 153.80deg off identity, which
							// is exactly the kind of plausible-looking nonsense that
							// sends the next person down the wrong path.
							const float dw = rVmd.w < 0.0f ? -rVmd.w : rVmd.w;
							const float dAng = 2.0f * std::acos(dw > 1.0f ? 1.0f : dw) * 57.29578f;
							const float qn = std::sqrt(rVmd.x * rVmd.x + rVmd.y * rVmd.y
								+ rVmd.z * rVmd.z + rVmd.w * rVmd.w);
							if (fnLogInfo)
							{
								fnLogInfo(
									"[PROBE A6] Hips root delta at t=%.3fs = (%.4f,%.4f,%.4f,%.4f) |q|=%.6f angleFromIdentity=%.2fdeg",
									currentTimeSeconds, rVmd.x, rVmd.y, rVmd.z, rVmd.w, qn, dAng);
								fnLogInfo(
									"[PROBE A6]   restLocal=(%.4f,%.4f,%.4f,%.4f)  gkLocalOut(written)=(%.4f,%.4f,%.4f,%.4f)",
									b.restLocals[i].x, b.restLocals[i].y, b.restLocals[i].z, b.restLocals[i].w,
									gkLocalOut.x, gkLocalOut.y, gkLocalOut.z, gkLocalOut.w);
								if (fnGetWorldRotation)
								{
									float bw[4]{0, 0, 0, 1};
									fnGetWorldRotation(trans, bw);
									fnLogInfo("[PROBE A6]   Hips world rotation after write = (%.4f,%.4f,%.4f,%.4f)",
									          bw[0], bw[1], bw[2], bw[3]);
								}
								if (fnGetWorldPosition)
								{
									float bp[3]{0, 0, 0};
									fnGetWorldPosition(trans, bp);
									fnLogInfo(
										"[PROBE A6]   Hips world position after write = (%.4f,%.4f,%.4f)  [restRootPos=(%.4f,%.4f,%.4f)]",
										bp[0], bp[1], bp[2], b.restRootPos.x, b.restRootPos.y, b.restRootPos.z);
								}
							}
						}
					}
				}
#endif // GAKUMOTION_ENABLE_GEOMETRY_PROBE

				// 51 骨骼动作缺失 UpperChest 保护：自动同步 Chest (hid=8) 旋转，彻底根治肩膀-胸腔接缝破坑
				if (hid == 8 && !b.upperChestInClip && b.upperChestTransform)
				{
					if (fnSetWorldRotation)
					{
						fnSetWorldRotation(b.upperChestTransform, rTarget.x, rTarget.y, rTarget.z, rTarget.w);
					}
					else if (fnSetLocalRotation)
					{
						fnSetLocalRotation(b.upperChestTransform, 0.0f, 0.0f, 0.0f, 1.0f);
					}
					writtenWorldRots[54] = rTarget;
					writtenWorldRotValid[54] = true;
				}

				// ------------------------------------------------------------------
				// 辅助骨写入闸门（实机靶向对照实验核心）
				// ------------------------------------------------------------------
				// kAuxNormal   : 原路径，逐帧注入 Quartz 解算出的半角 / 半扭转
				// kAuxSilence  : 硬锁初始绑定局部姿态 -> 辅助骨变成父骨的刚性延伸，零贡献
				// kAuxIdentity : 局部旋转强制 Identity
				// kAuxReplay   : 真值直灌（由 AuxGroundTruth 提供，缺失数据时自动降级为 Silence）
				// 判定含义：若切到 Silence/Identity 后「细腰波谷」完全消失 =>
				//          塌陷 100% 由现行注入公式造成（公式错）；
				//          若塌陷纹丝不变 => 塌陷与辅助骨无关，须排查 Sleeve 骨 / Position 位移 / 材质形变。
				// ------------------------------------------------------------------
				const Quaternion kIdentityForAux = Quaternion::Identity();
				if (auxRollH && !auxRollHRestLocal) auxRollHRestLocal = &kIdentityForAux;
				if (auxJointH && !auxJointHRestLocal) auxJointHRestLocal = &kIdentityForAux;

				if (auxDriveMode == kAuxNormal) continue; // 模式 0 由帧末统一模型后处理接管

				for (int pass = 0; pass < 2; ++pass)
				{
					void* auxTrans = (pass == 0) ? auxRollH : auxJointH;
					if (!auxTrans) continue;

					Quaternion qRun;
					if (auxDriveMode == kAuxLegacy)
					{
						qRun = (pass == 0) ? qAuxRollHWorld : qJointHWorld;
					}
					else if (auxDriveMode == kAuxReplay)
					{
						const Quaternion* qRecorded = LookupAuxReplay(hid, pass == 0, currentTimeSeconds);
						if (qRecorded)
						{
							qRun = *qRecorded; // 官方逐帧真值（局部空间）
						}
						else
						{
							qRun = (pass == 0) ? *auxRollHRestLocal : *auxJointHRestLocal; // 无数据 -> 降级为静默
						}
					}
					else if (auxDriveMode == kAuxIdentity)
					{
						qRun = Quaternion::Identity();
					}
					else
					{
						qRun = (pass == 0) ? *auxRollHRestLocal : *auxJointHRestLocal;
					}

					if (auxDriveMode == kAuxLegacy)
					{
						// 旧契约：世界空间直写（仅保留作对照 —— 已确认这正是「细腰波谷」的直接成因）
						if (fnSetWorldRotation)
						{
							fnSetWorldRotation(auxTrans, qRun.x, qRun.y, qRun.z, qRun.w);
						}
						else if (fnSetLocalRotation)
						{
							const Quaternion qLocal = qParentForLocal.Inverse() * qRun;
							fnSetLocalRotation(auxTrans, qLocal.x, qLocal.y, qLocal.z, qLocal.w);
						}
					}
					else if (fnSetLocalRotation)
					{
						// 静默 / 归零 / 真值：一律局部空间硬写，语义最干净
						fnSetLocalRotation(auxTrans, qRun.x, qRun.y, qRun.z, qRun.w);
					}
				}
			}

			if (auxDriveMode == kAuxNormal)
			{
				ApplyAuxModelToBinding(b);
			}
			// This opt-in geometric support sees the completed main pose and arm
			// auxiliary postpass. It writes only exact Waist_H local transforms.
			ApplyWaistSupport(b);
			CaptureWristDiagnostic(b, currentTimeSeconds, appliedOverrides);

			// ---- PROBE A9: WHO is this actor, and what are its real bone lengths? ----
			// WHY THIS EXISTS. A8 measured thighLen 0.3496 m against 0.4558 m in
			// ground_truth_real.json -- a 23.3% mismatch -- and GakuMotion NEVER
			// WRITES BONE LENGTHS. It writes rotations and the root transform only.
			// Bone length is owned by the rig's skeleton, so a mismatch cannot be a
			// scale bug in our code; it means the two numbers describe DIFFERENT
			// CHARACTERS. Cross-checking confirmed it: the device's restLocal differs
			// from gakumas_bind_pose.json (source: official_fmw_saki.json) on every
			// bone by 7-52 deg, and Hips by 147 deg. Meanwhile ground_truth_real.json
			// declares model = "YYB Hatsune Miku_default_1.0ver". So the device actor
			// is NEITHER Saki NOR that Miku, and every absolute number I have been
			// comparing against is from the wrong skeleton.
			//
			// That makes this a correctness prerequisite, not a nicety: the leg-spread
			// verdict (A8: 166 deg vs truth 8.96 deg) is only meaningful once the
			// reference is the same character. Note the A8 signal that does NOT depend
			// on any reference: verticalFraction 0.044/0.211 means the thighs are
			// near-horizontal, and 1.0 means standing. That is geometry, not a
			// comparison, and it already condemns the pose.
			//
			// What this prints, one self-contained block:
			//   - the actor transform's own name and each measured bone's name,
			//     so the character can be identified from the log alone
			//   - the BONE LENGTHS as the rig actually has them at bind time, from
			//     each bone's own local position -- these are the numbers any
			//     comparison must use
			//   - the actor's local position, to catch a scaled actor root
			//
			// The bone index lookup lives OUTSIDE both probes' guards on purpose: A8
			// needs it too, and A8's guard (fnGetWorldPosition) is independent of
			// A9's (fnGetTransformName). Declaring it inside A9's if-block would
			// make A8 fail to compile-scope the moment the name getter is absent.
#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
			int iHips = -1, iLThigh = -1, iRThigh = -1, iLKnee = -1, iRKnee = -1;
			int iLFoot = -1, iRFoot = -1, iRoot = -1, iLToes = -1, iRToes = -1;
			for (size_t k = 0; k < clip.bones.size(); ++k)
			{
				switch (clip.bones[k].humanoid_id)
				{
				case 0: iHips = (int)k;
					break;
				case 1: iLThigh = (int)k;
					break;
				case 2: iRThigh = (int)k;
					break;
				case 3: iLKnee = (int)k;
					break;
				case 4: iRKnee = (int)k;
					break;
				case 5: iLFoot = (int)k;
					break;
				case 6: iRFoot = (int)k;
					break;
				case 19: iLToes = (int)k;
					break;
				case 20: iRToes = (int)k;
					break;
				default: break;
				}
			}
			if (probeA3Pending && fnGetTransformName)
			{
				if (iHips >= 0 && (size_t)iHips < b.boneTransforms.size())
				{
					fnLogInfo("[PROBE A9] actor=%p name=\"%s\" | hips transform name = \"%s\"",
					          b.actor,
					          b.actorTransform ? fnGetTransformName(b.actorTransform).c_str() : "(none)",
					          fnGetTransformName(b.boneTransforms[iHips]).c_str());
				}
				else if (b.actorTransform)
				{
					fnLogInfo("[PROBE A9] actor=%p name=\"%s\" (no hips transform)",
					          b.actor, fnGetTransformName(b.actorTransform).c_str());
				}
				// Bone-local positions give the rig's real segment lengths.
				if (fnGetLocalPosition)
				{
					auto blen = [&](int boneIdx, const char* label) -> void
					{
						if (boneIdx < 0 || (size_t)boneIdx >= b.boneTransforms.size()) return;
						float lp[3]{0, 0, 0};
						fnGetLocalPosition(b.boneTransforms[boneIdx], lp);
						const float l = std::sqrt(lp[0] * lp[0] + lp[1] * lp[1] + lp[2] * lp[2]);
						fnLogInfo("[PROBE A9]   localPos %-12s (%2d) = (%+.4f,%+.4f,%+.4f) |len|=%.4f",
						          label, boneIdx, lp[0], lp[1], lp[2], l);
					};
					blen(iLThigh, "LUpperLeg");
					blen(iLKnee, "LLowerLeg");
					blen(iLFoot, "LFoot");
					blen(iRThigh, "RUpperLeg");
					blen(iRKnee, "RLowerLeg");
					blen(iRFoot, "RFoot");
					blen(iLToes, "LToes");
					blen(iRToes, "RToes");
					blen(iHips, "Hips");
				}
				if (b.actorTransform && fnGetLocalPosition)
				{
					float ap[3]{0, 0, 0};
					fnGetLocalPosition(b.actorTransform, ap);
					fnLogInfo("[PROBE A9] actor localPos = (%+.4f,%+.4f,%+.4f)  (a non-1 scale here"
					          " would mean the whole body is scaled)", ap[0], ap[1], ap[2]);
				}
			}

			// ---- PROBE A8: POSITION, and the leg spread computed from it ----------
			// WHY. A7 settled the rotations: engWorld = Ry(90.000 deg) * predWorld, the
			// same rigid rotation on all eight leg bones and on the root (spread across
			// bones 0.005 deg). A rigid left-multiplication cannot change the angle
			// between two bones, and indeed the engine's two thigh world rotations sit
			// 14.49 deg apart against our predicted 14.53 deg and a ground truth of
			// 8.96 deg. So bone ORIENTATIONS are essentially right and the ~180 deg
			// render has to be a POSITION defect. A7 could not test that: world
			// rotations carry no child offsets, so no spread can be derived from them.
			//
			// This probe therefore reads positions, and it computes the leg spread the
			// same way the official extractor does (tools/extract_ground_truth_real.py
			// :673-676) -- hip->knee vectors, NOT the pelvis-width shortcut that
			// produced a fake 83.141 deg constant earlier.
			//
			// Reference numbers from the OFFICIAL run of this very motion:
			//   Hips.y 0.857 - 1.197 (standing, ~1.05 typical)
			//   LFoot.y 0.106 - 0.613, RFoot.y 0.106 - 1.404 (feet on the floor)
			//   leg_spread: 0.43 - 136.26 deg, and at f0 it is 8.96 deg
			// So: feet near y=0 and spread well under 90 => correct. Feet high, or
			// spread near 180 => position defect, and the y numbers say how much.
			//
			// ONE self-contained line, after every bone has been written, so the
			// positions are the final ones and cannot be paired across frames.
			if (probeA3Pending && fnGetWorldPosition)
			{
				auto wp = [&](void* t, Vector3& out) -> bool
				{
					if (!t) return false;
					float p[3]{0, 0, 0};
					fnGetWorldPosition(t, p);
					out = Vector3(p[0], p[1], p[2]);
					return true;
				};
				// iHips / iLThigh / iRThigh / iLKnee / iRKnee / iLFoot / iRFoot are
				// resolved once in PROBE A9 above so both probes share one lookup.
				Vector3 pHips, pLTh, pRTh, pLK, pRK, pLF, pRF, pRoot;
				const bool ok = (iHips >= 0)
					&& wp((size_t)iHips < b.boneTransforms.size() ? b.boneTransforms[iHips] : nullptr, pHips)
					&& wp((size_t)iLThigh < b.boneTransforms.size() ? b.boneTransforms[iLThigh] : nullptr, pLTh)
					&& wp((size_t)iRThigh < b.boneTransforms.size() ? b.boneTransforms[iRThigh] : nullptr, pRTh)
					&& wp((size_t)iLKnee < b.boneTransforms.size() ? b.boneTransforms[iLKnee] : nullptr, pLK)
					&& wp((size_t)iRKnee < b.boneTransforms.size() ? b.boneTransforms[iRKnee] : nullptr, pRK)
					&& wp((size_t)iLFoot < b.boneTransforms.size() ? b.boneTransforms[iLFoot] : nullptr, pLF)
					&& wp((size_t)iRFoot < b.boneTransforms.size() ? b.boneTransforms[iRFoot] : nullptr, pRF);
				const bool okRoot = b.rootTransform && wp(b.rootTransform, pRoot);

				if (ok)
				{
					// Official formula: thigh axis = hip -> knee.
					auto vsub = [](const Vector3& a, const Vector3& c)
					{
						return Vector3(c.x - a.x, c.y - a.y, c.z - a.z);
					};
					auto vlen = [](const Vector3& v)
					{
						return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
					};
					auto vdot = [](const Vector3& u, const Vector3& v)
					{
						return u.x * v.x + u.y * v.y + u.z * v.z;
					};
					auto vang = [&vlen, &vdot](const Vector3& u, const Vector3& v)
					{
						const float lu = vlen(u), lv = vlen(v);
						if (lu < 1e-9f || lv < 1e-9f) return 0.0f;
						float d = vdot(u, v) / (lu * lv);
						if (d > 1.0f) d = 1.0f;
						if (d < -1.0f) d = -1.0f;
						return std::acos(d) * 57.29578f;
					};
					const float spread = vang(vsub(pHips, pLK), vsub(pHips, pRK));
					const float thighLenL = vlen(vsub(pLTh, pLK));
					const float thighLenR = vlen(vsub(pRTh, pRK));
					// A splayed leg is short in Y and long in X/Z; a straight standing
					// leg is long in Y. This ratio says which one we actually have,
					// and it needs no angle convention at all.
					const float yFracL = std::fabs(pLK.y - pLTh.y) / (thighLenL > 1e-9f ? thighLenL : 1.0f);
					const float yFracR = std::fabs(pRK.y - pRTh.y) / (thighLenR > 1e-9f ? thighLenR : 1.0f);

					if (fnLogInfo)
					{
						fnLogInfo("[PROBE A8] POSITION actor=%p t=%.3f | Hips=(%.4f,%.4f,%.4f)"
						          " LThigh=(%.4f,%.4f,%.4f) LKnee=(%.4f,%.4f,%.4f)"
						          " RThigh=(%.4f,%.4f,%.4f) RKnee=(%.4f,%.4f,%.4f)"
						          " LFoot=(%.4f,%.4f,%.4f) RFoot=(%.4f,%.4f,%.4f)",
						          b.actor, currentTimeSeconds,
						          pHips.x, pHips.y, pHips.z,
						          pLTh.x, pLTh.y, pLTh.z, pLK.x, pLK.y, pLK.z,
						          pRTh.x, pRTh.y, pRTh.z, pRK.x, pRK.y, pRK.z,
						          pLF.x, pLF.y, pLF.z, pRF.x, pRF.y, pRF.z);
						fnLogInfo("[PROBE A8]   OFFICIAL leg_spread=%.2f deg (hip->knee) |"
						          " thighLen L=%.4f R=%.4f | verticalFraction L=%.3f R=%.3f"
						          " | Hips.y=%.4f LFoot.y=%.4f RFoot.y=%.4f",
						          spread, thighLenL, thighLenR, yFracL, yFracR,
						          pHips.y, pLF.y, pRF.y);
						fnLogInfo("[PROBE A8]   reference: official run gives Hips.y~1.05"
						          " feet~0.10-0.25 spread<=136.26 (f0=8.96). Written root"
						          " local=(%.4f,%.4f,%.4f) restRootPos=(%.4f,%.4f,%.4f)"
						          " restActorPos=(%.4f,%.4f,%.4f) rootWorld=%s(%.4f,%.4f,%.4f)",
						          b.restRootPos.x + vmdRootPos.x,
						          b.restRootPos.y + vmdRootPos.y,
						          b.restRootPos.z + vmdRootPos.z,
						          b.restRootPos.x, b.restRootPos.y, b.restRootPos.z,
						          b.restActorPos.x, b.restActorPos.y, b.restActorPos.z,
						          okRoot ? "yes" : "unread",
						          pRoot.x, pRoot.y, pRoot.z);
						fnLogInfo("[PROBE A8]   curActorRot=(%.4f,%.4f,%.4f,%.4f)"
						          " restActorRot=(%.4f,%.4f,%.4f,%.4f) vmdRootPos=(%.4f,%.4f,%.4f)",
						          curActorRot.x, curActorRot.y, curActorRot.z, curActorRot.w,
						          b.restActorRot.x, b.restActorRot.y, b.restActorRot.z, b.restActorRot.w,
						          vmdRootPos.x, vmdRootPos.y, vmdRootPos.z);
					}
				}
				else if (fnLogInfo)
				{
					fnLogInfo("[PROBE A8] POSITION UNAVAILABLE (missing transform:"
					          " hips=%d lthigh=%d rthigh=%d lknee=%d rknee=%d lfoot=%d rfoot=%d)",
					          iHips, iLThigh, iRThigh, iLKnee, iRKnee, iLFoot, iRFoot);
				}
			}
#endif // GAKUMOTION_ENABLE_GEOMETRY_PROBE

			// Opt-in geometry diagnostics; normal playback must not stream probes.
#if defined(GAKUMOTION_ENABLE_GEOMETRY_PROBE) && GAKUMOTION_ENABLE_GEOMETRY_PROBE
			// ---- PROBE A11: torso uprightness, EVERY frame --------------------
			// The screenshots at1.5s and 4.5s show the character leaning far back
			// with the legs thrown forward and the body nearly horizontal. The
			// official ground truth for this very motion says otherwise:
			//
			//     torso verticality (hips->head vs world up): min 0.852, median 0.993
			//     Hips.y: 0.97 - 1.08   LFoot.y: 0.11 - 0.20  (feet on the floor)
			//
			// i.e. the character is UPRIGHT for the whole 82 seconds and never drops
			// below hip height 0.97 m. So the render is not "a bit off", it is
			// qualitatively wrong, and the measurement that decides it is the torso
			// verticality -- not any bone angle, because the bone angles can all be
			// self-consistent while the whole body is tipped over.
			//
			// Unlike A8 this runs EVERY frame while the clip plays, so a trend is
			// visible rather than a single sample, and it is throttled to ~2 Hz so
			// the log stays readable over an 82-second track.
			//
			// Read it as: 1.0 upright, 0.0 horizontal, negative head-below-hips.
			if (fnLogInfo && fnGetWorldPosition && fnGetBoneTransform
				&& b.boneTransforms.size() > 6)
			{
				if (currentTimeSeconds < b.lastGeometryProbeTime ||
					currentTimeSeconds - b.lastGeometryProbeTime > 0.5f)
				{
					b.lastGeometryProbeTime = currentTimeSeconds;
					auto wpos = [&](int boneIdx, float* out) -> bool
					{
						if (boneIdx < 0 || (size_t)boneIdx >= b.boneTransforms.size()) return false;
						if (!b.boneTransforms[boneIdx]) return false;
						fnGetWorldPosition(b.boneTransforms[boneIdx], out);
						return true;
					};
					// Head is humanoid id 10, Hips is 0, feet are 5 and 6.
					int idxHips = -1, idxHead = -1, idxLFoot = -1, idxRFoot = -1;
					for (size_t k = 0; k < clip.bones.size(); ++k)
					{
						switch (clip.bones[k].humanoid_id)
						{
						case 0: idxHips = (int)k;
							break;
						case 10: idxHead = (int)k;
							break;
						case 5: idxLFoot = (int)k;
							break;
						case 6: idxRFoot = (int)k;
							break;
						default: break;
						}
					}
					float hp[3], hd[3], lf[3], rf[3];
					if (wpos(idxHips, hp) && wpos(idxHead, hd)
						&& wpos(idxLFoot, lf) && wpos(idxRFoot, rf))
					{
						const float dx = hd[0] - hp[0], dy = hd[1] - hp[1], dz = hd[2] - hp[2];
						const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
						const float vert = (len > 1e-6f) ? (dy / len) : 0.0f;
						// The same segment verticality A13 measured at bind time, now
						// during playback. At bind the chain summed to -0.776 (the full
						// stature: thigh -0.984, shin -0.935), so if playback shows a
						// much smaller drop the leg is folding somewhere, and the
						// per-segment numbers say WHERE -- which no bone-angle check
						// can tell us, because every local rotation can be perfectly
						// self-consistent while the chain folds.
						int idxKnee = -1;
						for (size_t k = 0; k < clip.bones.size(); ++k)
						{
							if (clip.bones[k].humanoid_id == 3)
							{
								idxKnee = (int)k;
								break;
							}
						}
						float kp[3]{};
						const bool haveKnee = wpos(idxKnee, kp);
						float vThigh = 0.0f, vShin = 0.0f;
						if (haveKnee)
						{
							const float tdx = kp[0] - hp[0], tdy = kp[1] - hp[1], tdz = kp[2] - hp[2];
							const float tl = std::sqrt(tdx * tdx + tdy * tdy + tdz * tdz);
							if (tl > 1e-6f) vThigh = tdy / tl;
							const float sdx = lf[0] - kp[0], sdy = lf[1] - kp[1], sdz = lf[2] - kp[2];
							const float sl = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz);
							if (sl > 1e-6f) vShin = sdy / sl;
						}
						const float drop = lf[1] - hp[1];
						auto kneeAngle = [&](int thigh, int knee, int foot) -> float
						{
							float t[3]{}, k[3]{}, f[3]{};
							if (!wpos(thigh, t) || !wpos(knee, k) || !wpos(foot, f)) return -1.0f;
							float dot = 0, aa = 0, bb = 0;
							for (int j = 0; j < 3; ++j)
							{
								float a = k[j] - t[j], c = f[j] - k[j];
								dot += a * c;
								aa += a * a;
								bb += c * c;
							}
							if (aa * bb < 1e-12f) return -1.0f;
							return std::acos(std::clamp(dot / std::sqrt(aa * bb), -1.0f, 1.0f)) * 57.2957795f;
						};
						fnLogInfo("[PROBE A11] actor=%p t=%6.2f | torsoVert=%+.4f"
						          " | Hips.y=%+.4f LFoot.y=%+.4f RFoot.y=%+.4f"
						          " | footY-meanHipY=%+.4f"
						          " | thighVert=%+.4f shinVert=%+.4f"
						          " | thigh+shin drop=%+.4f | kneeDeg L/R=%.2f/%.2f",
						          b.actor, currentTimeSeconds, vert,
						          hp[1], lf[1], rf[1],
						          (lf[1] + rf[1]) * 0.5f - hp[1],
						          vThigh, vShin, drop,
						          kneeAngle(iLThigh, iLKnee, iLFoot), kneeAngle(iRThigh, iRKnee, iRFoot));
					}
				}
			}

#endif
			lastAppliedOverrideCount = appliedOverrides;
			probeFirstFrameDone = true; // PROBE A3/A6 are strictly one-shot
		}

		// ---- Target actor gating ------------------------------------------------
		// The host calls OnLateUpdate once per actor per frame, in scene order
		// (hski -> jsna -> hume -> ttmr), so without a gate EVERY character in the
		// scene is driven by the clip. That produced two separate failures:
		//
		//  1. Visual: all four characters dance, not just the one being watched.
		//  2. Diagnostic: PROBE output from different actors is interleaved and
		//     IDENTICAL in shape, so a log read top-to-bottom mixes characters.
		//     On 2026-10-07 that showed up as two PROBE A8 blocks in the SAME
		//     frame reporting leg_spread 166.02 and 29.65 -- one character standing
		//     (correct, after the Ry(90) fix) and one still splayed.
		//
		// nullptr means "drive every actor", which preserves the old behaviour for
		// single-character scenes. The plugin sets it when the user picks a target.
		void* targetActor{nullptr};

		void SetTargetActor(void* a) { targetActor = a; }
		bool IsTargetActor(void* a) const { return targetActor == nullptr || targetActor == a; }

		// A/B switch for Animator.Rebind() at bind time. See the comment at the call
		// site: on a Unity humanoid it snaps every bone to the prefab T-Pose, so the
		// "rest pose" captured after it is the T-Pose and the hips land at floor
		// level. Default OFF because the device measurement says that is what sinks
		// the character. RestoreMute()'s Rebind is a different thing and unaffected.
		bool useAnimatorRebind{false};

		// Fold the rig's local rest basis into the gkclip write, for the LEG chain
		// only. PROBE A2 measures the humanoid's leg rest far from Identity
		// (LeftUpperLeg 106.59 deg, RightUpperLeg 119.05 deg), and a gkclip delta is
		// defined against a canonical all-Identity rest, so writing the bare delta
		// lands the leg on a rest the clip never assumed.
		//
		// Settled on device by an A/B over the whole clip (tools/ab_leg_rest.py),
		// measuring per-segment verticality against the bind-time baseline:
		//
		//   condition      torso      thigh      shin       drop
		//   bare delta    +0.9895    -0.1565   -0.0464    -0.0852
		//   with basis    +0.9891    -0.8559   -0.5564    -0.5624
		//   BIND baseline ~ +0.990   -0.984    -0.935     -0.7764
		//
		// The torso is unmoved either way (+0.99), which is why the arm-folding fix
		// was unaffected and why this could hide for so long: an error of 106-119 deg
		// on the legs is invisible to every bone-angle check that only compares
		// relatives.
		//
		// Scoped to the legs deliberately. Applying it to the whole body would undo
		// the H1 fix that cured the arm folding, since the torso's rest locals are
		// small (Spine 2.47 deg, Neck 9.92 deg) and the bare delta is correct there.
		bool legRestBasis{true};

		// HID21/22 gkclip deltas are scaled by the character's measured eye-region
		// response ratio (see GkEyePose.hpp and GkEyeTargetFrames.hpp). This is not
		// LookAt and not a late write: it is the same single eye write, only with a
		// geometry-derived angle. false = raw 1:1 source rotation (the rejected
		// baseline and the A/B control).
		bool eyePoseRelativeGain{true};

		// User's initial placement relative to the original scene position (metres,
		// WORLD axes). Extends the old Y-only rootHeightOffset to X/Y/Z. Applied to
		// driven poses, not actor/rest snapshots, so replays and song switches cannot
		// accumulate translation. RestoreMute still returns to the original scene.
		// Zero this extra placement before measuring the unmodified motion stream.
		Vector3 initialPositionOffset{0.0f, 0.0f, 0.0f};
		float initialYawDegrees{0.0f};

		bool SetInitialPositionOffset(const Vector3& offset)
		{
			if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) return false;
			initialPositionOffset = offset;
			return true;
		}

		bool SetInitialYawDegrees(float degrees)
		{
			if (!std::isfinite(degrees)) return false;
			initialYawDegrees = std::remainder(degrees, 360.0f);
			return true;
		}

		void ApplyMotion(const MotionClip& clip, float currentTimeSeconds, void* actor = nullptr,
		                 float facialTimeSeconds = -1.0f)
		{
			if (actor)
			{
				auto it = bindings.find(actor);
				if (it != bindings.end())
				{
					ApplyMotionToBinding(it->second, clip, currentTimeSeconds, facialTimeSeconds);
					currentBinding = it->second;
					return;
				}
			}
			// 2026-10-07 self-heal: the target actor was set, but no binding exists
			// for it this frame. Either the scene reloaded (the pointer is stale and
			// every actor would be rejected) or the target never bound. Binding
			// nothing means the character freezes in its rest pose -- audio plays,
			// the status line reads bones=0/0, and nothing moves. So if the target
			// produced no binding, fall back to driving whatever actor we DO have a
			// binding for rather than silently doing nothing.
			if (currentBinding.actor && currentBinding.bindPoseCaptured)
			{
				ApplyMotionToBinding(currentBinding, clip, currentTimeSeconds, facialTimeSeconds);
			}
		}
	};
} // namespace GakuMotion
