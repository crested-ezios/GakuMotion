#pragma once

// =============================================================================
// GkWebPanelBridge.hpp — gakumotion.web-panel/v1 provider bridge
// =============================================================================
// Implements the GakuWebPanelServiceV1 domain semantics for the Loader's
// WebView2 manager panel. The wire contract mirrors the frontend authority
// (frontend/src/motion/protocol.ts and
// frontend/docs/motion-bridge-protocol.md):
//   request  {v:1, id, type:"request", method, params}
//   response {v:1, type:"response", id, ok, result | error:{code,message,details?}}
//   event    {v:1, type:"event", name, payload, seq, sid}
//
// Threading: every ABI callback except MotionShutdown's silent Invalidate runs
// on the Loader desktop UI thread. Core state is read/written under
// Core::stateMutex; mutations are queued through Core::QueueGui exactly like
// the ImGui overlay (GuiAction + revision/session guards) and drain on the
// Unity main thread. The event sink is only ever invoked from UI-thread
// callbacks (OnUiTick / HandleMessage / OnShutdown), never from Unity.
//
// The header is Windows-free so headless contract tests can drive it with a
// real Core and fake host hooks (tests/test_web_panel_bridge.cpp).
// =============================================================================

#include "../include/GakuWebPanel.h"
#include "GakuMotion.hpp"
#include "GkJsonLite.hpp"
#include "GkPortraitCache.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "web/GkBridgeJson.hpp"
#include "web/GkBridgeTypes.hpp"

namespace GakuMotion::webpanel
{
	class Bridge
	{
		using GuiAction = Core::GuiAction;
		using GuiCommand = Core::GuiCommand;
	public:
		Bridge(Core& core, HostHooks hooks) : core_(core), hooks_(std::move(hooks))
		{
			sessionId_ = NextSessionId();
		}

		uint64_t SessionId() const { return sessionId_; }
		bool Closed() const { return closed_.load(std::memory_order_acquire); }

		// ---- ABI surface -----------------------------------------------------

		GakuStatus GetAssets(GakuWebPanelAssetsV1* output);
		void OnShow();
		void OnHide() { visible_ = false; }

		void SetEventSink(GakuWebPanelEventSink sink, void* userData);
		GakuStatus HandleMessage(const char* requestJson, uint32_t responseCapacity, char* responseJson, uint32_t* requiredBytes);
		void OnUiTick();

		// UI thread: announce UI-host detach and reset per-session state so
		// rebinding the panel later starts a fresh session without unloading.
		void OnShutdown();

		// Any thread (MotionShutdown runs on Unity): permanent silent invalidation.
		void Invalidate(const char* /*reason*/);

		// ---- test helpers ----------------------------------------------------

		std::string HandleMessageForTest(const std::string& request) { return DispatchRequest(request.c_str()); }

	private:
		// ---- wire framing ----------------------------------------------------

		static std::string MakeOk(uint64_t id, const std::string& result);
		static std::string MakeError(uint64_t id, const std::string& code, const std::string& message, const std::string& details = "");
		void Emit(const std::string& name, const std::string& payloadJson);
		std::string DispatchRequest(const char* requestJson);

		// ---- parameter helpers ------------------------------------------------

		static const jsonlite::Value* Param(const jsonlite::Value& params, const char* name);
		static bool NumberParam(const jsonlite::Value& params, const char* name, double& out);
		static bool IntParam(const jsonlite::Value& params, const char* name, int64_t& out);
		static bool BoolParam(const jsonlite::Value& params, const char* name, bool& out);
		static bool StringParam(const jsonlite::Value& params, const char* name, std::string& out);
		static bool VecParam(const jsonlite::Value& params, const char* name, Vector3& out);

		// ---- dispatch ----------------------------------------------------------

		static bool IsSlowDomainMutatingMethod(const std::string& method);
		CmdResult Dispatch(const std::string& method, const jsonlite::Value& params);

		// ---- command helpers ----------------------------------------------------

		// Caller holds stateMutex. Mirrors the ImGui drain admission rules so a
		// refused command reports a structured error instead of silently dropping.
		CmdResult MutationGuardLocked();
		CmdResult TransportSimple(GuiAction action, bool requiresClip);
		CmdResult SeekCommand(const jsonlite::Value& params, GuiAction action);
		void ForcePlayerEvent() { forcePlayerEvent_ = true; }

		CmdResult StageSet(const jsonlite::Value& params);

		// ---- dialogs / drops (implemented, capability-gated) ---------------------

		CmdResult DialogPickFile(const jsonlite::Value& params);
		CmdResult DialogPoll(const jsonlite::Value& params);
		void PollBridgeDialog();
		static bool IsSupportedAudioExtensionPath(const std::string& path);
		static bool IsNetworkAudioPath(const std::string& path);
		static bool IsAcceptedImportFile(const std::string& path);
		static DropPeek NormalizeDropPeek(const DropPeek& raw);
		CmdResult DropSetTarget(const jsonlite::Value& params);
		CmdResult DropPoll();
		CmdResult DropPeekMethod();
		static const char* VmdTendencyName(VmdTendency tendency);
		static std::string TrackTimingJson(const TrackTiming& value);
		static std::string TimingJson(const PerformanceTiming& timing);
		static std::string SlotTimingJson(const TrackTiming& motion, const TrackTiming& facial);
		static bool ParseTrackTimingField(const jsonlite::Value& value, const char* name, TrackTiming& target, std::string& error);
		static bool ParseTiming(const jsonlite::Value& value, PerformanceTiming& timing, std::string& error);
		static bool ParseSlotTiming(const jsonlite::Value& value, TrackTiming& motion, TrackTiming& facial, std::string& error);
		CmdResult LibraryProbeFiles(const jsonlite::Value& params);
		static std::string MotionPositionsJson(const std::vector<MotionPositionEntry>& positions,
		                                       const PerformanceTiming& fallbackTiming = {});
		CmdResult LibraryStartEdit(const jsonlite::Value& params);
		CmdResult LibraryProbeAudio(const jsonlite::Value& params);
		CmdResult LibraryGetCover(const jsonlite::Value& params);
		CmdResult LibrarySetCover(const jsonlite::Value& params);
		CmdResult LibraryReadImagePreview(const jsonlite::Value& params);
		CmdResult ScreenshotCapabilitiesMethod();
		CmdResult ScreenshotCaptureMethod();
		CmdResult ClockSetRunMode(const jsonlite::Value& params);
		CmdResult ClockConfigure(const jsonlite::Value& params);
		CmdResult AudioEffectSet(const jsonlite::Value& params);
		static std::string ImportSlotString(const jsonlite::Value& draft, const char* fileField, const char* modeField, const char* pathField);
		static bool IsImportSlotFile(const std::string& value);
		static CmdResult ValidateImportTrackFile(const std::string& path, const char* label, int kind);
		CmdResult LibraryImport(const jsonlite::Value& params);
		void EnsureFormationSyncedLocked();
		CmdResult ActorsGetPortrait(const jsonlite::Value& params);
		CmdResult FormationSelectSlot(const jsonlite::Value& params);
		CmdResult FormationSwapSlots(const jsonlite::Value& params);
		CmdResult FormationSetSlotOffset(const jsonlite::Value& params);
		CmdResult FormationResetSlotOffset(const jsonlite::Value& params);
		CmdResult FormationSaveSlotOffset(const jsonlite::Value& params);
		CmdResult FormationSetSlotSource(const jsonlite::Value& params);
		CmdResult StageToggleNode(const jsonlite::Value& params);
		CmdResult StageSetLoadReplacement(const jsonlite::Value& params);
		CmdResult BoneTree();
		static bool BoneNameParam(const jsonlite::Value& params, int64_t& hid, std::string& name);
		CmdResult BoneGet(const jsonlite::Value& params);
		CmdResult BoneSet(const jsonlite::Value& params);
		CmdResult BoneReset(const jsonlite::Value& params);
		CmdResult BoneResetAll();
		CmdResult BoneSetMaster(const jsonlite::Value& params);
		CmdResult BoneToggle();
		CmdResult BoneReload();
		CmdResult BoneSaveToMotion();
		CmdResult BoneSaveToGlobal();
		bool GazeRuntimeAvailable() const;
		CmdResult GazeSet(const jsonlite::Value& params);
		CmdResult GazeSetMode(const jsonlite::Value& params);
		CmdResult GazeSetDesktop(const jsonlite::Value& params);
		CmdResult GazeSetWeights(const jsonlite::Value& params);
		CmdResult GazeCycle();
		static bool ParseSoleSettingsValue(const jsonlite::Value& value, SoleMeasurementSettings& settings, std::string& error);
		CmdResult SoleSet(const jsonlite::Value& params);
		CmdResult SolePitchSet(const jsonlite::Value& params);
		static bool ParseStudioSettings(const jsonlite::Value& value, studio::Settings& settings, std::string& error);
		CmdResult StudioSet(const jsonlite::Value& params);
		CmdResult StudioSetLighting(const jsonlite::Value& params);
		CmdResult StudioSetVideo(const jsonlite::Value& params);
		static bool TriStateParam(const jsonlite::Value& params, const char* name, int& target);
		CmdResult ExperimentsSet(const jsonlite::Value& params);
		CmdResult ExperimentsSetMiku(const jsonlite::Value& params);
		CmdResult ExperimentsSetWaistSupport(const jsonlite::Value& params);
		CmdResult ExperimentsSetBeforeQuartz(const jsonlite::Value& params);

		// ---- parsing structured settings -----------------------------------------

		static bool ParseCameraSettings(const jsonlite::Value& value, CameraSettings& settings, std::string& error);
		static bool ParseStageSettings(const jsonlite::Value& value, StageSettings& settings, std::string& error);

		// ---- snapshot serializers (caller holds stateMutex) ------------------------

		std::string BuildHello() const;
		std::string PlayerStateJsonLocked() const;
		std::string PermissionJsonLocked() const;
		std::string LibraryJsonLocked() const;
		std::string LoadedJsonLocked() const;
		std::string PlaylistJsonLocked() const;
		std::string ActorsJsonLocked() const;
		std::string FormationJsonLocked() const;
		static std::string CameraTransformJson(const CameraModeTransform& transform);
		static std::string CameraSettingsJson(const CameraSettings& settings);
		std::string CameraSnapshotJsonLocked(const CameraController::Snapshot& snapshot) const;
		std::string CameraJsonLocked() const;
		static std::string StageSettingsJson(const StageSettings& settings);
		static std::string StageNodesFromVector(const std::vector<StageCandidate>& candidates);
		std::string StageNodesJsonLocked() const;
		std::string StageJsonFromPartsLocked(const StageSettings& settings, const std::string& sceneName,
			const std::string& statusNotice, const std::string& mountedKey, const std::string& nodesJson) const;
		std::string StageJsonWithNodesLocked(const std::string& nodesJson) const;
		std::string StageJsonLocked() const;
		std::string FacialJsonLocked() const;
		std::string PositionJsonLocked() const;
		std::string ImportProgressJsonLocked() const;
		std::string GazeJsonLocked() const;
		std::string BoneJsonLocked() const;
		static std::string SoleSettingsJson(const SoleMeasurementSettings& settings);
		static std::string SoleFootJson(const SoleFootMeasurement& foot);
		std::string SoleJsonLocked() const;
		static std::string SolePitchSettingsJson(const sole_pitch::Settings& settings);
		std::string SolePitchJsonLocked() const;
		static std::string StudioSettingsJson(const studio::Settings& settings);
		std::string StudioJsonLocked() const;
		static const char* MikuStatusText(MikuFitDecision decision);
		std::string ExperimentsJsonLocked() const;
		std::string TelemetryJsonLocked() const;
		static std::string BuildAppearanceJsonFromInfo(const AppearanceInfo& info);
		std::string BuildAppearanceJson() const;
		std::string ClockJsonLocked(std::string* outMeta = nullptr) const;
		static std::string BuildAudioEffectJsonFromState(const AudioEffectState& state);
		std::string BuildAudioEffectJson() const;
		std::string BuildSnapshotLocked() const;

		// ---- UI tick: async polling + change events -------------------------------

		struct CachedStageNode
		{
			int instanceId{0};
			int parentIndex{-1};
			int depth{0};
			uint8_t flags{0};
			std::string name;
			std::string displayName;
		};
		struct CachedActorEntry
		{
			uint64_t token{0};
			void* actor{nullptr};
			std::string character;
			std::string displayName;
			std::string shortName;
			std::string presetName;
		};

		static uint64_t NextSessionId();
		void ResetChangeTracking();
		void Tick();

		// Keep an ImGui-started dialog progressing while the WebView panel is
		// selected (the ImGui draw loop that normally polls it is not running).
		void PollImGuiDialog();

		// ---- state ---------------------------------------------------------------

		Core& core_;
		HostHooks hooks_;
		uint64_t sessionId_{0};
		uint64_t seq_{0};
		std::atomic<bool> closed_{false};
		bool visible_{false};
		bool baselinePending_{true};
		mutable std::mutex sinkMutex_;
		GakuWebPanelEventSink sink_{nullptr};
		void* sinkUserData_{nullptr};
		std::wstring assetsDirCache_;

		// Dialog state (bridge-owned ticket; host service owns the OS dialog).
		bool dialogPending_{false};
		int dialogKind_{-1};
		uint64_t dialogId_{0};
		std::string dialogState_{"pending"};
		std::vector<std::string> dialogPaths_;
		std::string dialogNotice_;
		bool dropEnabled_{false};
		bool lastDropTargetSent_{false};
		DropPeek cachedDropPeek_{};
		std::vector<std::string> pendingDrops_;
		std::string boneNotice_, gazeNotice_, solePitchNotice_, experimentsNotice_;
		std::mutex pendingCoverInvalidateMutex_;
		std::string pendingImportCoverInvalidateKey_;

		// Event change detection.
		bool forcePlayerEvent_{false};
		std::atomic<bool> forceDomainRefresh_{true};
		std::atomic<bool> forceAudioEffectRefresh_{true};
		bool lastPermissionAllowed_{false};
		uint64_t lastInjectionSession_{0};
		std::chrono::steady_clock::time_point lastPlayerEmit_{};
		std::chrono::steady_clock::time_point lastClockEmit_{};
		std::chrono::steady_clock::time_point lastSlowDomainPoll_{};
		std::chrono::steady_clock::time_point lastAudioEffectPoll_{};
		std::string lastPermission_, lastPlayer_, lastLibrary_, lastLoaded_, lastPlaylist_,
			lastActors_, lastFormation_, lastCamera_, lastStage_, lastStageNodes_, lastFacial_, lastPosition_,
			lastImportProgress_, lastAppearance_, lastGaze_, lastBone_, lastSole_,
			lastSolePitch_, lastStudio_, lastExperiments_, lastClock_, lastClockMeta_, lastAudioEffect_;
		int lastPhysicsBackend_{-1};
		uint64_t lastPolledLibraryRevision_{0};
		uint64_t lastPolledFormationRevision_{0};
		bool lastPolledImportActive_{false};
		bool lastPolledImportFinished_{false};
		float lastPolledImportProgress_{-1.0f};

		// Lightweight pre-serialization dirty-check caches (avoid 10Hz/60Hz JSON churn).
		bool domainCacheValid_{false};
		bool lastPermInjectionAllowed_{false};
		std::string lastPermInjectionStatus_;
		PlaybackState lastPlayerState_{PlaybackState::Stopped};
		std::string lastPlayerLoadedKey_;
		uint64_t lastPlayerLibRev_{0};
		float lastPlayerTime_{0.0f};
		float lastPlayerDuration_{0.0f};
		uint32_t lastPlayerTotalFrames_{0};
		float lastPlayerFps_{0.0f};
		float lastPlayerSpeed_{1.0f};
		float lastPlayerVolume_{1.0f};
		PlaylistMode lastPlayerPlaylistMode_{};
		GakuMotionClockSnapshotV1 lastClockSnapshot_{};
		bool lastClockSnapshotValid_{false};

		uint64_t lastLibRev_{0};
		size_t lastLibCount_{0};
		bool lastLibScanning_{false};
		std::string lastLibPlaylistNotice_;

		std::string lastLoadedKey_;
		uint64_t lastLoadedGen_{0};
		uint64_t lastLoadedSession_{0};
		bool lastLoadedSourceMissing_{false};
		bool lastLoadedSourceChanged_{false};

		int lastPlaylistMode_{-1};
		std::string lastPlaylistNotice_;

		uint64_t lastActorsEpoch_{0};
		void* lastActorsTarget_{nullptr};
		void* lastActorsBound_{nullptr};
		std::vector<CachedActorEntry> lastActorsEntries_;

		StageFormation lastFormationState_;
		bool lastFormationHasLoadedMotion_{false};

		CameraController::Snapshot lastCameraSnapshot_;
		std::string lastCameraNotice_;

		std::vector<CachedStageNode> lastStageNodesCache_;
		StageSettings lastStageSettings_;
		std::string lastStageSceneName_;
		std::string lastStageStatusNotice_;
		std::string lastStageMountedKey_;
		std::string lastStageLoadNotice_;

		FacialWeights lastFacialWeights_;
		std::string lastFacialNotice_;

		Vector3 lastInitialPosition_{0.0f, 0.0f, 0.0f};
		std::string lastInitialPositionNotice_;

		bool lastImportActive_{false};
		bool lastImportFinished_{false};
		bool lastImportSucceeded_{false};
		float lastImportProgressVal_{-1.0f};
		std::string lastImportStage_;
		std::string lastImportStatusText_;
		std::string lastImportErrorDetail_;
		std::string lastImportKey_;

		int lastAppearanceTheme_{-1};
		float lastAppearanceUiScale_{-1.0f};

		int lastGazeMode_{-1};
		bool lastGazeDesktop_{true};
		float lastGazeEye_{0.0f};
		float lastGazeHead_{0.0f};
		float lastGazeBody_{0.0f};
		bool lastGazeRuntimeAvail_{false};
		bool lastGazeVrifyAvail_{false};
		bool lastGazeVrifyActive_{false};
		std::string lastGazeNotice_;

		uint64_t lastBoneRevision_{0};
		size_t lastBoneCount_{0};
		bool lastBoneMasterEnabled_{true};
		std::string lastBoneNotice_;

		SoleMeasurementSettings lastSoleSettings_;
		uint64_t lastSoleGen_{0};
		std::string lastSoleNotice_;
		uint64_t lastSoleActorToken_{0};
		SoleMeasurementSnapshot lastSoleSnapshot_;

		sole_pitch::Settings lastSolePitchSettings_;
		sole_pitch::Snapshot lastSolePitchSnapshot_;
		std::string lastSolePitchNotice_;

		studio::Settings lastStudioSettings_;
		uint64_t lastStudioRevision_{0};
		std::string lastStudioLightingNotice_;
		std::string lastStudioVideoNotice_;

		unsigned lastExpMikuSnapshot_{0};
		int lastExpMikuMouth_{-1};
		int lastExpMikuEye_{-1};
		int lastExpMikuAllow_{-1};
		bool lastExpCtrlMouth_{false};
		bool lastExpCtrlEye_{false};
		bool lastExpCtrlAllow_{false};
		bool lastExpPlaying_{false};
		bool lastExpBeforeQuartz_{false};
		bool lastExpMuteSe_{false};
		std::string lastExpNotice_;

		AudioEffectState lastAudioEffectState_;
	};
} // namespace GakuMotion::webpanel

#include "web/GkBridgeFraming.inl"
#include "web/GkBridgeDispatch.inl"
#include "web/GkBridgeStageAndFormation.inl"
#include "web/GkBridgeFilesAndDrops.inl"
#include "web/GkBridgeLibraryOps.inl"
#include "web/GkBridgeImport.inl"
#include "web/GkBridgeControls.inl"
#include "web/GkBridgeParsers.inl"
#include "web/GkBridgeSnapshotCore.inl"
#include "web/GkBridgeSnapshotDetail.inl"
#include "web/GkBridgeTick.inl"
