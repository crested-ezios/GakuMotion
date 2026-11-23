#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "GkCaptureGuard.hpp"
#include "GkCaptureSafety.hpp"
#include "GkGmlClient.hpp"
#include "GakuMotion.hpp"
#include "GakuPluginMetadata.h"
#include "GkAudioPluginClient.hpp"
#include "GkWicCover.hpp"
#include "GkWebPanelBridge.hpp"
#include "GakuMotionControl.h"
#include "GkLog.hpp"
#include "GkCsharpImport.hpp"
#include "GkHotkey.hpp"
#include "GakuSafeBoneLookup.h"
#include "GkExactMethod.hpp"
#include "GkBgmSuppression.hpp"
#include "GkPhotographyEnsembleProbe.hpp"
#include "GkPortraitCache.hpp"
#include "GkCoverCache.hpp"
#include <cstddef>
#include "GkFaceRuntime.hpp"
#include "GkBurstBridgeClient.hpp"
#include "GkBurstBridgeMultiClient.hpp"
#include "GkFallbackPhysics.hpp"
#include "GakuPhysicsDiagnostics.h"
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <fstream>
#include <algorithm>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <limits>
#include <sstream>
#include <iomanip>
#include <locale>
#include "GkWindowsPaths.hpp"

namespace
{
#include "plugin/GkPluginContextAndPhysics.inl"
#include "plugin/GkPluginIl2CppDecls.inl"
#include "plugin/GkPluginTransformHelpers.inl"
#include "plugin/GkPluginDialogsAndImport.inl"
#include "plugin/GkPluginAudioAndFinders.inl"

#include "GkGroundShadowBootstrap.hpp"
#include "GkBodyShadowBootstrap.hpp"
#include "GkCameraBridge.hpp"
#ifdef GKMOTION_ENABLE_SHOE_MESH_PROBE
#include "GkShoeMeshProbe.hpp"
#endif

#include "GkStageRuntime.hpp"
#include "GkActorGroundShadow.hpp"
#include "GkNativeAudio.hpp"
#include "GkGroundShadowProbe.hpp"
#include "GkBodyShadowProbe.hpp"
#include "GkEyeGazeProbe.hpp"
	void RefreshProtectedContext(bool forceOwnerRead = false);
	bool ProtectionOnUnityThread()
	{
		return GakuMotion::host::mainThread.isMainThread &&
			GakuMotion::host::mainThread.isMainThread(GakuMotion::host::mainThread.context);
	}

#include "GkSceneOwnerRuntime.hpp"
#include "GkPhotographyEnsembleProbeRuntime.inl"
#include "GkStageLoadOverride.hpp"
#include "GkStudioRuntime.hpp"
#include "GkStudioLightingRuntime.hpp"
#include "GkStudioVideoRuntime.hpp"
#include "GkStudioDiagnostics.hpp"
#include "GkPageObservation.hpp"

#include "plugin/GkPluginProtectedContext.inl"
#include "plugin/GkPluginHostAndPortraits.inl"
} // namespace

#include "plugin/GkPluginNativeInit.inl"
#include "plugin/GkPluginIl2CppResolve.inl"
#include "plugin/GkPluginRuntimeReady.inl"
#include "plugin/GkPluginSceneActors.inl"
#include "plugin/GkPluginActorTickHelpers.inl"
#include "plugin/GkPluginActorTick.inl"

extern "C" GAKU_PLUGIN_EXPORT int GakuMotion_GetPhysicsDiagnostics(
	void* actor, GakuPhysicsDiagnosticSnapshot* out, uint32_t size)
{
	if (!actor || !out || size != sizeof(*out)) return -1;
	for (const auto& slot : g_ensemblePhysics)
		if (slot.diagnostic.actor == reinterpret_cast<uintptr_t>(actor)) { *out = slot.diagnostic; return 1; }
	if (g_physicsDiagnostic.actor != reinterpret_cast<uintptr_t>(actor)) return 0;
	*out = g_physicsDiagnostic;
	return 1;
}

// Optional read-only observer entry point. Recorder calls on Unity's main
// thread; the lease enables readback on subsequent writes for two seconds.
extern "C" GAKU_PLUGIN_EXPORT int GakuMotion_GetFaceDiagnostics(
	void* actor, GakuFaceDiagnosticSnapshot* out, uint32_t size)
{
	if (!actor || !out || size != sizeof(*out)) return -1;
	auto& core = GakuMotion::Core::Instance();
	auto* track = core.PlayerForActor(actor);
	if (!track) track = &core.player;
	auto it = track->controller.bindings.find(actor);
	if (it == track->controller.bindings.end() || core.player.state == GakuMotion::PlaybackState::Stopped) return 0;
	auto& face = it->second.face;
	face.Observe();
	std::memset(out, 0, sizeof(*out));
	out->version = GAKU_FACE_DIAGNOSTICS_VERSION;
	out->size = sizeof(*out);
	out->status = face.diagnosticStatus;
	out->sequence = face.diagnosticSequence;
	out->qpc_ticks = face.diagnosticTicks;
	out->actor = reinterpret_cast<uintptr_t>(actor);
	out->model = reinterpret_cast<uintptr_t>(face.diagnosticModel);
	out->clip_time = face.diagnosticTime;
	out->playback_state = static_cast<int32_t>(core.player.state);
	out->unity_frame = face.diagnosticUnityFrame;
	out->channel_count = static_cast<uint32_t>(face.diagnosticChannels.size());
	for (size_t i = 0; i < face.diagnosticChannels.size(); ++i)
		out->channels[i] = face.diagnosticChannels[i];
	return 1;
}

constexpr const char* kShowGuiActionId = "show-gui";
constexpr const char* kReloadOverridesActionId = "reload-overrides";
constexpr const char* kToggleOverridesActionId = "toggle-overrides";

void ReloadOverridesHotkey(void*)
{
	auto& core = GakuMotion::Core::Instance();
	std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
	if (!core.InjectionAllowed()) return;
	core.LoadOverrides(true);
}
void ToggleOverridesHotkey(void*)
{
	auto& core = GakuMotion::Core::Instance();
	std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
	if (!core.InjectionAllowed()) return;
	core.ToggleOverrides();
}

// Runs on the Unity main thread: the loader's hotkey registry dispatches there,
// so touching Core/gui state is safe.
void ShowGuiHotkey(void*)
{
	GakuMotion::Core::Instance().gui.showWindow = true;
	if (GakuMotion::host::desktopWindow.requestShow)
		GakuMotion::host::desktopWindow.requestShow(GakuMotion::host::desktopWindow.context);
	GakuMotion::host::Info("[GakuMotion] Hotkey Ctrl+M: showWindow=TRUE");
}

void MotionShutdown(void*)
{
	std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
	if (!g_pluginInitialized) return;
	g_runtimeReady.store(false, std::memory_order_release);
		if (g_webPanelBridge) g_webPanelBridge->Invalidate("plugin-shutdown");
	GakuMotion::host::UnregisterHotkey(kShowGuiActionId);
	GakuMotion::host::UnregisterHotkey(kReloadOverridesActionId);
	GakuMotion::host::UnregisterHotkey(kToggleOverridesActionId);
	auto& core = GakuMotion::Core::Instance();
	std::lock_guard<std::recursive_mutex> stateLock(core.stateMutex);
	core.WaitForLibraryRefresh();
	core.SetControlActive(false);
	core.injectionPolicy.SetSceneAllowed(false);
	core.injectionPolicy.SetCaptureHooksReady(false);
	if (GakuMotion::host::mainThread.isMainThread &&
		GakuMotion::host::mainThread.isMainThread(GakuMotion::host::mainThread.context))
		RevokeProtectedInjection();
	GakuMotion::capture::Shutdown();
	StageLoadOverride::Shutdown();
	SceneOwnerRuntime::Shutdown();
	ReleasePendingFileDialog();
	core.gui.browsePendingKind = -1;
	NativeAudio::Release(GakuMotion::bgm::ReleaseReason::Shutdown);
	const auto bgmStatus = g_bgmLease.GetSnapshot().status;
	const auto& bgmSnapshot = g_bgmLease.GetSnapshot();
	if (bgmSnapshot.held && g_hostContext.logWarn)
		g_hostContext.logWarn("[GakuMotion][NativeBgm] shutdown restoration pending: %s; held=%d owns_mutation=%d (no off-thread game call)",
			GakuMotion::bgm::StatusName(bgmStatus), bgmSnapshot.held, bgmSnapshot.ownsMutation);
	if (g_voiceLease.GetSnapshot().held && g_hostContext.logWarn)
		g_hostContext.logWarn("[GakuMotion][NativeAudio] Voice shutdown restoration pending: %s (no off-thread game call)",
			GakuMotion::bgm::StatusName(g_voiceLease.GetSnapshot().status));
	if (g_seLease.GetSnapshot().held && g_hostContext.logWarn)
		g_hostContext.logWarn("[GakuMotion][NativeAudio] Se shutdown restoration pending: %s",
			GakuMotion::bgm::StatusName(g_seLease.GetSnapshot().status));
	// Join native jobs before plugin code disappears. NativeAOT stays loaded
	// for the process lifetime; unloading its runtime is unsupported.
	g_trackCoverCache.Stop();
	GakuMotion::csharp_import::Shutdown();
	ClearSecondaryPhysics();
	const bool multiShutdown = g_multiBurstBridge.Shutdown();
	g_burstBridge.Shutdown(!multiShutdown);
	EyeGazeProbe::Shutdown();
	BodyShadowProbe::Shutdown();
	g_bodyShadowProbeAttempted = false;
	GroundShadowProbe::Shutdown();
	g_groundShadowProbeAttempted = false;
	ActorGroundShadow::Reset();
	CameraBridge::Shutdown();
	g_cameraAttempted = false;
	core.AbandonScene();
	HelperAudioStop();
	GakuMotion::host::SetPlaying(false);
	if (g_hostContext.logInfo)
	{
		g_hostContext.logInfo("[GakuMotion] Plugin shut down.");
	}
	g_pluginInitialized = false;
}


namespace {
#include "plugin/GkPluginWebPanelHost.inl"
#include "plugin/GkPluginWebPanelCapture.inl"
#include "plugin/GkPluginServiceApis.inl"
}

namespace GakuMotion::Log {
// Single choke point for every GakuMotion diagnostic that goes through GkLog.
// Kept out of the anonymous namespace above so the header declaration resolves
// to external linkage; the wire levels stay exactly the three the ABI defines.
void Write(Level level, const char* format, ...)
{
	if (!format) return;
	char buffer[2048]{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	switch (level)
	{
	case Level::Error: MotionLogError("%s", buffer); break;
	case Level::Warn: MotionLogWarn("%s", buffer); break;
	case Level::Info: MotionLogInfo("%s", buffer); break;
	case Level::Debug:
		// Reachable only from a development build: GM_DEBUG compiles to nothing
		// when GAKUMOTION_LOG_FLOOR > 0, so release never emits this path.
		MotionLogInfo("[D] %s", buffer);
		break;
	}
}
} // namespace GakuMotion::Log

extern "C" GAKU_PLUGIN_EXPORT GakuStatus GakuPlugin_Query(
	uint32_t requestedAbi, uint32_t outputSize, GakuPluginDescriptor* output)
{
	if (requestedAbi != GAKU_ABI_VERSION) return GAKU_E_VERSION;
	if (!output) return GAKU_E_INVALID_ARGUMENT;
	if (outputSize < sizeof(GakuPluginDescriptor)) return GAKU_E_SIZE;
	const GakuPluginDescriptor descriptor{GAKU_ABI_VERSION, sizeof(GakuPluginDescriptor),
		"gakumotion", "GakuMotion", "0.2.0-beta.1",
		MotionActivate, MotionRuntimeReady, MotionTick, MotionActorTick,
#if defined(GKMOTION_ENABLE_IMGUI) || defined(HAS_IMGUI)
		RenderGmlMotion,
#else
		nullptr,
#endif
		MotionShutdown};
	*output = descriptor;
	return GAKU_OK;
}

extern "C" GAKU_PLUGIN_EXPORT GakuStatus GakuPlugin_QueryMetadata(
	uint32_t requestedAbi, uint32_t outputSize, GakuPluginMetadataV1* output)
{
	if (requestedAbi != GAKU_METADATA_ABI_VERSION) return GAKU_E_VERSION;
	if (!output) return GAKU_E_INVALID_ARGUMENT;
	if (outputSize < sizeof(GakuPluginMetadataV1)) return GAKU_E_SIZE;

	static const char* const kMaintainers[] = {"crested-ezios"};
	static const GakuPluginLocalizedTextV1 kLocalizedSummaries[] = {
		{"en", "Custom VMD/motion playback, facial retargeting, camera, and stage studio plugin for GakuLoader."},
		{"zh-Hans", "适用于 GakuLoader 的自定义 VMD/动作回放、表情重定向、镜头与舞台摄影棚插件。"},
		{"zh-Hant", "適用於 GakuLoader 的自訂 VMD/動作回放、表情重定向、鏡頭與舞台攝影棚外掛。"},
		{"ja", "GakuLoader 向けのカスタム VMD・モーション再生、表情リターゲット、カメラおよびステージスタジオプラグイン。"},
	};
	static const GakuPluginDependencyV1 kDependencies[] = {
		{"gakuaudio", "GakuAudio", ">=0.1.0", GAKU_DEPENDENCY_OPTIONAL, 0u},
	};
	static const char* const kAuxiliaryDlls[] = {
		"GakuBurstBridge.dll",
		"GakuMotion.VmdImporter.Native.dll",
	};

	const GakuPluginMetadataV1 metadata{
		GAKU_METADATA_ABI_VERSION,
		sizeof(GakuPluginMetadataV1),
		"GakuMotion Contributors",
		static_cast<uint32_t>(std::size(kMaintainers)),
		0u,
		kMaintainers,
		"",
		"beta",
		"Custom VMD/motion playback, facial retargeting, camera, and stage studio plugin for GakuLoader.",
		static_cast<uint32_t>(std::size(kLocalizedSummaries)),
		0u,
		kLocalizedSummaries,
		"https://github.com/crested-ezios/GakuMotion",
		"https://github.com/crested-ezios/GakuMotion/releases",
		"https://github.com/crested-ezios/GakuMotion/issues",
		"MIT",
		"1.1.0",
		static_cast<uint32_t>(std::size(kDependencies)),
		0u,
		kDependencies,
		static_cast<uint32_t>(std::size(kAuxiliaryDlls)),
		0u,
		kAuxiliaryDlls,
	};
	*output = metadata;
	return GAKU_OK;
}
