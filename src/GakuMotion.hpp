#pragma once

#include "GkMotionData.hpp"
#include "GkLog.hpp"
#include "GkIdolController.hpp"
#include "GkMotionPlayer.hpp"
#include "GkLookAtManager.hpp"
#include "GkRuntimeConfig.hpp"
#include "GkBoneOverride.hpp"
#include "GkGuiOverlay.hpp"
#include "GkPlaylist.hpp"
#include "GkLibraryState.hpp"
#include "GkLibraryDelete.hpp"
#include "GkInjectionPolicy.hpp"
#include "GkCaptureSafety.hpp"
#include "GkStageLoadSettings.hpp"
#include "GkStageStudioSettings.hpp"
#include "GkDataLayout.hpp"
#include "GkControlRequests.hpp"
#include "GkStageFormation.hpp"
#include "GakuMotionActor.h"
#include "../include/GakuMotionClock.h"
#include "../include/GakuMotionStageState.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <deque>
#include <future>
#include <chrono>
#include <unordered_set>

#include "core/GkCoreTypes.hpp"

namespace GakuMotion
{
	// -----------------------------------------------------------------------------
	// Core：GakuMotion 运行时的唯一聚合根
	// -----------------------------------------------------------------------------
	// 职责边界（阶段一）：
	//   1. 持有播放器 / 视线管理器 / 控制面板骨架 / 运行时配置 / 骨骼覆盖集合；
	//   2. 每帧在 OnLateUpdate 内消费 control.json 的声明式状态与一次性命令；
	//   3. 维护曲目库索引与「当前激活角色」的状态快照（供面板与日志使用）。
	// 正式宿主为 GML；插件入口将版本化服务适配为本模块的有限回调，
	// 本文件不直接依赖 Windows API，便于离线单元测试（tests/test_core.cpp）。
	// -----------------------------------------------------------------------------
	class Core
	{
	public:
		Core()
		{
			player.audioOpenGuardContext = this;
			player.fnCanOpenAudio = [](void* context) { return static_cast<Core*>(context)->CanOpenLoadedAudio(); };
		}
		static Core& Instance()
		{
			static Core s_instance;
			return s_instance;
		}

		GakuStatus ReadSelectedActor(uint32_t size, GakuMotionActorSnapshot* output);

		MotionPlayer player;
		StageFormation formation;
		std::unordered_map<uint64_t, std::unique_ptr<MotionPlayer>> formationTracks;
		std::unordered_map<uint64_t, ActorCustomization> actorCustomizations;
		FacialWeights defaultFacialWeights;
		std::map<std::string, FacialWeights> facialWeightsByCharacter;
		FacialWeights lastSyncedPlayerFacialWeights;
		bool playerFacialWeightsBaselineInit{false};
		Vector3 savedSlot0InitialOffset{0.0f, 0.0f, 0.0f};
		void* formationAnchorActor{nullptr};
		Vector3 formationAnchorPos{0.0f, 0.0f, 0.0f};
		Quaternion formationAnchorRot{0.0f, 0.0f, 0.0f, 1.0f};
		bool formationAnchorCaptured{false};
		uint64_t formationLastFrame{0};
		uint64_t formationCameraFrame{0};
		std::unordered_set<void*> formationFrameWrittenActors;
		bool formationCameraTicked{false};
		unsigned ensembleProbeMissStreak{0};
		bool (*fnEnsembleActorEligible)(void*){nullptr};
		CameraController camera;
		LookAtManager lookAt;
		GuiOverlay gui;
		StageManager stageManager;

		// All scene-node identities and baselines remain provider-owned. Queries
		// copy StageManager snapshots; only admitted Unity-thread callers mutate.
		bool SceneNodesAllowed() const;

		void RefreshSceneNodes();

		scene_nodes::v1::Status AcquireSceneNodes(uint64_t revision, uint64_t* lease);

		scene_nodes::v1::Status ApplySceneNodes(uint64_t lease, uint64_t revision, uint32_t count, const scene_nodes::v1::Visibility* values);

		std::string stageLoadNotice;
		studio::Settings studioSettings;
		uint64_t studioRevision{1};
		std::string studioLightingNotice, studioVideoNotice;
		injection::Policy injectionPolicy;
		// The runtime enables enforcement before initialization. Offline consumers
		// may exercise the playback core without a game or native hook service.
		bool enforceInjectionPolicy{false};
		bool customWasActivated{false};
		bool stageStoppedByTransport{false};
		std::string injectionStatus;
		bool (*fnBeforeInjection)(){nullptr};
		bool InjectionAllowed() const { return !enforceInjectionPolicy || injectionPolicy.CanInject(); }
		bool PrepareInjection();

		bool captureSuspended{false};
		PlaybackState capturePlaybackState{PlaybackState::Stopped};
		float capturePlaybackTime{0.0f};
		std::string captureMotionFile;
		uint64_t injectionSession{0}, captureSession{0};
		capture::RestoreReport captureRestoreReport;
		const char* captureResumeReason{"no_capture_session"};
		bool capturePerformanceWasActive{false};
		// Physics and native audio are released by the runtime before this method.
		bool SuspendForCapture();

		bool ResumeAfterCapture();

		void RevokeInjection();
		void StopPerformance();

		RuntimeConfig config;
		std::vector<MotionEntry> motions;
		PlaylistSettings playlist;
		std::string playlistNotice;
		std::string libraryNotice;
		uint64_t libraryRevision{0}, loadedMotionGeneration{0}, libraryMutationGeneration{0};
		std::string loadedMotionKey;
		MotionEntry loadedMotionEntry;
		MotionFileVersion loadedMotionVersion;
		std::map<std::string, MotionFileVersion> libraryVersions;
		bool loadedSourceMissing{false}, loadedSourceChanged{false};
		std::future<LibraryScanSnapshot> libraryScan;
		uint64_t libraryScanGeneration{0};
		std::string playlistPath;
		std::string initialPositionPath;
		std::string initialPositionNotice;
		std::string facialWeightsNotice;
		std::string musicVolumeNotice;
		SoleMeasurementSettings soleSettings;
		SoleMeasurementSnapshot soleSnapshot;
		sole_pitch::Settings experimentalSolePitchSettings;
		sole_pitch::Snapshot experimentalSolePitchSnapshot;
		uint64_t experimentalSolePitchConsumedSerial{0};
		void* experimentalSolePitchConsumedActor{nullptr}; // identity only, never dereferenced
		bool solePitchSnapshotPending{false};
		long long solePitchSnapshotCommandSeq{0};
		long long burstTraceRequestedSeq{0}; // Consumed by the Unity main-thread plugin after physics dispatch.
		std::string soleNotice;
		void* soleObservedActor{nullptr};
		uint64_t soleCalibrationGeneration{0};
		using PendingImportCover = detail::CorePendingImportCover;

		bool importBusy{false};
		ImportProgressState importProgress;
		ImportProgressState guiImportProgressSnapshot;
		std::string activeImportEditDir;
		std::string pendingImportEditKey;
		std::unordered_set<std::string> pendingImportExistingKeys;
		PendingImportCover pendingImportCover;
		std::string lastImportedKey;
		bool (*fnStartImport)(const MotionImportRequest&, std::string&){nullptr};
		int (*fnPollImport)(std::string&){nullptr};
		int (*fnPollImportProgress)(std::string&, ImportProgressState*){nullptr};
		bool (*fnSaveTrackCoverPng)(const std::vector<uint8_t>& rawBytes, std::string_view mimeHint,
			const std::filesystem::path& targetDir, std::string& outFileName, std::string& outError){nullptr};
		std::string (*fnLoadTrackCoverDataUri)(const std::filesystem::path& coverPath){nullptr};
		bool (*fnPickFile)(int, std::string&){nullptr};
		bool (*fnBeginPickFile)(int, std::string&){nullptr};
		int (*fnPollPickFile)(std::string&, std::string&){nullptr};
		bool cameraImportBusy{false};
		std::string cameraNotice, cameraImportDirectory;
		bool (*fnStartCameraImport)(const CameraImportRequest&, std::string&){nullptr};
		int (*fnPollCameraImport)(std::string&){nullptr};
		// Fresh active actors in the admitted owner scene; managed reads stay on Unity thread.
		std::vector<void*> sceneActors;
		SceneActorRegistry sceneActorRegistry;
		bool (*fnBeforeTargetChange)(){nullptr};

		BoneOverrideSet globalOverrides; // <localRoot>/GakuMotion/bone_overrides.json
		BoneOverrideSet motionOverrides; // <曲目目录>/bone_overrides.json
		BoneOverrideSet effectiveOverrides; // 合成结果（交给 IdolController 只读引用）

		// 日志回调（由插件在 Init 阶段注入宿主 logger）
		using LogFn = void (*)(const char* fmt, ...);
		LogFn fnLogInfo{nullptr};
		LogFn fnLogWarn{nullptr};
		// 播放状态上报（对应宿主 context 的 setMotionPlaying）
		using NotifyMotionPlayingFn = void (*)(bool playing);
		NotifyMotionPlayingFn fnNotifyMotionPlaying{nullptr};
		std::string (*fnActiveAudioEffectId)(){nullptr};
		bool renderCompletionHookObserved{false};
		std::atomic<uint32_t> offscreenCaptureDepth{0};
		uint64_t nextRecordingLeaseId{1};
		performance::ClockConfiguration savedClockBeforeLease{};
		bool savedLoopBeforeLease{true};
		bool hasSavedLeaseState{false};

		// ---- 路径 ----
		std::string localRoot;
		std::string gameRoot;
		std::string motionsRoot;
		std::string configRoot;
		std::string logsRoot, diagnosticsRoot, transactionRoot;
		DataLayout dataLayout;
		std::string globalOverridePath;

		// ---- 运行时状态 ----
		int currentMotionIndex{-1};
		bool masterOverridesEnabled{true};
		bool initialized{false};

		// 「当前激活角色」状态快照（阶段一仅跟踪宿主传入的摄影角色）
		void* currentActor{nullptr};
		size_t currentActorBoundBones{0};
		size_t currentActorTotalBones{0};

		// 上一次「因声明而应用」的配置快照：
		// 用于区分「用户在文件里改了字段」与「运行时被快捷键改过」，
		// 避免用户按快捷键切歌/切视线后，仅因为文件里旧的声明值又被拉回去。
		using AppliedDeclaration = detail::CoreAppliedDeclaration;
		AppliedDeclaration applied;

		CameraSettings appliedCamera;
		bool cameraConfigApplied{false};

		// -------------------------------------------------------------------------
		// GUI 线程与 Unity 主线程安全通信队列（杜绝在 GUI 线程调用 Unity API 导致闪退）
		// -------------------------------------------------------------------------
		using GuiAction = detail::CoreGuiAction;

		std::atomic<int> guiPhysicsBackend{2};
		std::atomic_bool guiTextInputActive{false};
		std::atomic<bool> guiBeforeQuartzExperiment{false};
		std::atomic<bool> guiExperimentalWaistSupport{true};
		std::atomic<bool> guiMuteNativeSoundEffects{true};

		using GuiCommand = detail::CoreGuiCommand;

		std::mutex guiCommandMutex;
		std::recursive_mutex stateMutex; // Host LateUpdate and render share this guard.
		std::deque<GuiCommand> guiCommands;
		control_v0::Requests controlRequests;
		bool controlActive{false};
		uint64_t controlSession{1}, controlObservedInjectionSession{0};

		void QueueGui(GuiCommand cmd)
		{
			std::lock_guard<std::mutex> lock(guiCommandMutex);
			// Only adjacent scrub/placement updates may coalesce. Begin/end/save/
			// transport events retain their order across physical frames.
			if (!guiCommands.empty() &&
				(((cmd.action == GuiAction::ScrubUpdate || cmd.action == GuiAction::InitialPosition) &&
				  guiCommands.back().action == cmd.action) ||
				 (cmd.action == GuiAction::FormationSetSlotOffset &&
				  guiCommands.back().action == GuiAction::FormationSetSlotOffset &&
				  guiCommands.back().candidateIndex == cmd.candidateIndex)))
				guiCommands.back() = std::move(cmd);
			else guiCommands.push_back(std::move(cmd));
		}

#include "GkControlCore.inl"
#include "GkEnsembleCore.inl"

		void QueueSelectMotion(int index, bool autoplay);
		void QueueCycleMotion(int delta) { QueueGui({GuiAction::Cycle, static_cast<float>(delta)}); }
		void QueuePlay() { QueueGui({GuiAction::Play}); }
		void QueuePause() { QueueGui({GuiAction::Pause}); }
		void QueueResume() { QueueGui({GuiAction::Resume}); }
		void QueueStop() { QueueGui({GuiAction::Stop}); }
		GuiCommand PositionCommand(GuiAction action, float value = 0);
		void QueueSeek(float seconds) { QueueGui(PositionCommand(GuiAction::Seek, seconds)); }
		void QueueSeekFrame(int64_t frame);
		void QueueStepFrame(int delta) { QueueGui(PositionCommand(GuiAction::StepFrame, static_cast<float>(delta))); }
		void QueueReloadMotion() { QueueGui(PositionCommand(GuiAction::ReloadMotion)); }
		void QueueScrubBegin(float seconds);
		void QueueScrubUpdate(float seconds);
		void QueueScrubEnd(float seconds) { QueueGui(PositionCommand(GuiAction::ScrubEnd, seconds)); }
		void QueueInitialPosition(const Vector3& offset);
		void QueueFormationSelectSlot(int slotIndex);
		void QueueFormationSwapSlots(int fromSlot, int toSlot);
		void QueueFormationSetSlotOffset(int slotIndex, const Vector3& offset, float yawDegrees);
		void QueueFormationResetSlotOffset(int slotIndex);
		void QueueFormationSaveSlotOffset(int slotIndex);
		void QueueFormationSetSlotSource(int slotIndex, int sourceSlotIndex);
		void QueueCameraSettings(const GakuMotion::CameraSettings& settings);
		bool ApplyCameraSettings(const GakuMotion::CameraSettings& settings, bool persistMode = true);
		void QueueSoleSettings(const SoleMeasurementSettings& settings);
		void QueueExperimentalSolePitch(const sole_pitch::Settings& settings);
		bool SetSoleSettings(const SoleMeasurementSettings& settings);
		void QueuePhysicsBackend(int backend);
		void QueueExperimentalWaistSupport(bool enabled);
		void QueueBeforeQuartzExperiment(bool enabled);
		void QueueMuteNativeSoundEffects(bool enabled);
		void QueueStageSettings(const StageSettings& settings);
		void QueueStageLoadReplacement(const std::string& key);
		bool QueueStudioSettings(const studio::Settings& settings);
		void QueueStudioReset() { QueueGui({GuiAction::StudioReset}); }
		void QueueStagePreset(int presetIndex);
		void QueueToggleStageCandidate(int candidateIndex, bool active);
		void QueueScanStageCandidates();
		void QueueMountCustomStage(const std::string& stageKey);
		void QueueUnloadCustomStage();
		void QueueRestoreCameraSkybox();
		void ProcessPendingStageLoadCommands();
		bool HasPendingTargetSelection();
		// File work must keep progressing on host frames even when Photography
		// has not created a dancer yet. Do not pass an earlier transport/scene
		// command: e.g. a queued Stop must still precede a camera import.
		void ProcessPendingLibraryCommands();
		void ProcessPendingGuiCommands();
		bool ApplyTrackCoverToDirectory( const std::filesystem::path& trackDir, bool clear, const std::string& coverDataUri, std::string& outCoverFile, std::string& outError) const;
		bool SetTrackCover( const std::string& key, bool clear, const std::string& coverDataUri, std::string& outThumbnailDataUri, std::string& outError);
		std::string FindNewlyAddedMotionDirLocked() const;
		void PollImport();
		// =========================================================================
		// 生命周期
		// =========================================================================
		void SetGameRoot(const std::string& path) { gameRoot = path; }
		void Initialize(const std::string& rootPath, const DataLayout* selectedLayout = nullptr);
		void OnActorChanged(void* newActor);
		void AbandonScene();
		bool DiscardPreviousActorBindings();
		// =========================================================================
		// 每帧驱动入口（由插件在 OnLateUpdate 中调用）
		// =========================================================================
		void OnLateUpdate(void* actor, void* animator = nullptr, void* playableDirector = nullptr, float deltaTime = 0.0333f, bool advanceClock = true, float cameraDeltaTime = -1.0f, uint64_t physicalFrame = 0);
		void OnLookAtUpdate(void* lookAtController, void* cameraTransform);
		// =========================================================================
		// 曲目库与选曲
		// =========================================================================
		bool ApplyLibrarySnapshot(LibraryScanSnapshot snapshot);
		bool RefreshMotionLibrary() { return ApplyLibrarySnapshot(ReadLibrarySnapshot(motionsRoot)); }
		void BeginLibraryRefresh();
		void PollLibraryRefresh();
		void WaitForLibraryRefresh();
		bool CanOpenLoadedAudio();
		bool ReloadCurrentMotion();
		void ClearCurrentMotion();
		bool SavePlaylist(const std::string& destination = "");
		bool DeleteMotionFiles(const std::string& key, bool confirmed = false);
		bool MoveMotion(const std::string& key, int delta);
		void StartImport(MotionImportRequest request);
		void ReloadCamera();
		void StartCameraImport(CameraImportRequest request);
		void PollCameraImport();
		bool LoadStageLoadSettings();
		bool SetStageLoadReplacement(const std::string& key);
		bool LoadStudioSettings();
		bool SetStudioSettings(const studio::Settings& settings);
		void LoadClockSettings();
		bool SaveClockSettingsLocked() const;
		GakuStatus SetClockRunMode(uint32_t rawMode);
		GakuStatus ConfigureClock(uint32_t clockSource, uint32_t fpsNumerator, uint32_t fpsDenominator);
		GakuStatus AcquireRecordingLease(const char* ownerName, uint32_t fpsNumerator, uint32_t fpsDenominator, uint64_t* outLeaseId);
		GakuStatus ReleaseRecordingLease(uint64_t leaseId);
		GakuStatus RecordingBegin(uint64_t leaseId, double startSeconds);
		GakuStatus RecordingStop(uint64_t leaseId);
		GakuStatus NotifyFrameRendered(uint64_t leaseId, uint64_t evaluatedFrameSerial, uint32_t advanceFrames);
		bool IsInOffscreenCapture() const noexcept;
		GakuStatus BeginOffscreenCapture() noexcept;
		GakuStatus EndOffscreenCapture() noexcept;
		void OnMainCameraRendered(bool fromEndFrameHook = false);
		float EffectiveSimulationDelta(float engineDeltaSeconds);
		mutable std::string cachedClockAudioPath_;
		mutable bool cachedClockAudioExists_{false};
		mutable std::chrono::steady_clock::time_point cachedClockAudioProbeTime_{};
		void InvalidateClockAudioProbeCache() const noexcept;
		GakuStatus ReadClockSnapshot(GakuMotionClockSnapshotV1* output, bool probeAudioFile = true);
		GakuStatus ReadStageState(GakuMotionStageStateV1* output);
		void ResetStudioSettings() { SetStudioSettings(studio::Settings{}); }
		bool LoadCameraSettings();
		bool SaveCameraSettings();
		void ResetSoleObservation();
		// A request is fulfilled only by fresh original Foot writes and current
		// target measurements in this callback. GUI rendering never reads Unity.
		void WriteRequestedSolePitchSnapshot(void* actor);
		void ObserveSoles(void* actor);
		void ApplyExperimentalSolePitch(void* actor);
		bool LoadSoleSettings();
		bool SaveSoleSettings();
		bool LoadMusicVolume();
		bool SaveMusicVolume();
		bool LoadNativeAudioSettings();
		bool SaveNativeAudioSettings();
		bool LoadFacialWeights();
		bool SaveFacialWeights();
		bool LoadInitialPosition();
		bool SaveInitialPosition();
		// 曲目目录若携带 aux_replay.json（官方实机录制的辅助骨逐帧局部四元数）则挂载为真值源。
		// 文件不存在属正常情况，不报错、不刷日志噪音。
		void TryLoadAuxGroundTruth(const std::string& motionDirectory);
		bool SelectMotionByIndex(int index, bool autoPlay = false, bool forcePreview = false);
		// 上一曲 / 下一曲（delta = ±1）。若当前正在播放，则无缝续播新曲目。
		// source 仅用于诊断日志（"hotkey" / "gui" / "config"），便于实机定位
		// 「谁触发了切换」这类问题（参见 handoff：暂停后幽灵切换）。
		bool CycleMotion(int delta, const char* source = "unknown");
		const MotionEntry* CurrentMotion() const;
		// =========================================================================
		// 声明式配置应用
		// =========================================================================
		void ApplyDeclarativeConfig();
		// =========================================================================
		// 一次性命令
		// =========================================================================
		void ConsumeConfigCommands();
		// =========================================================================
		// 骨骼覆盖
		// =========================================================================
		std::string CurrentMotionOverridePath() const;
		void LoadOverrides(bool verbose);
		void RebuildEffectiveOverrides();
		bool ToggleOverrides();
		// ---- Target actor --------------------------------------------------------
		// 宿主对每个 actor 分别回调；当前只允许一个已确认的实际目标。
		// 不向 GUI 暴露托管指针；发布实际名单和带生命周期的选择 token。
		// Called only from a fresh, pinned Unity-thread actor search in the owner scene.
		// Missing actors are abandoned without touching possibly destroyed objects.
		bool PublishSceneActors(const std::vector<SceneActorObservation>& observations);
		// Kept for offline/legacy callers; production publishes fresh typed snapshots.
		void NoteActor(void* a);
		unsigned KnownActorCount() const { return static_cast<unsigned>(sceneActorRegistry.Entries().size()); }
		bool IsTargetActor(void* a) const { return a && player.controller.targetActor == a; }
		void SetTargetActor(void* a);
		void CycleTargetActor();
		void PickBoundActorAsTarget();
		// =========================================================================
		// 视线
		// =========================================================================
		int CycleGazeMode();
		static const char* GazeModeName(int mode);
		// =========================================================================
		// 状态摘要（面板与日志共用）
		// =========================================================================
		std::string StatusSummary() const;
		static const char* PlaybackStateName(PlaybackState s);
		void SetLiveBoneOverride(int hid, const char* name, float pitch, float yaw, float roll, bool world, bool enabled, bool symmetrical);
		void ResetLiveBoneOverride(int hid, const char* name, bool symmetrical);
		void ResetAllLiveOverrides();
		bool SaveLiveOverridesToMotion();
		bool SaveLiveOverridesToGlobal();
		bool GetLiveBoneOverride(int hid, const char* name, float* outPitch, float* outYaw, float* outRoll, bool* outWorld, bool* outEnabled) const;
		// =========================================================================
		// 面板接口（启用 ImGui 时由 GML Desktop UI capability 调用）
		// =========================================================================
#if defined(GKMOTION_ENABLE_IMGUI) || defined(HAS_IMGUI)
		void OnRenderGui(bool hostedPanel = false);
#endif

		GuiContext BuildGuiContext();
	private:
		void NotifyMotionPlaying(bool playing) const;
		void LogInfo(const std::string& message) const;
		void LogWarn(const std::string& message) const;
		// Release builds compile this to nothing (GAKUMOTION_LOG_FLOOR), which is
		// exactly what a scene-selection trace should do.
		void LogDebug(const std::string& message) const;
		// 相对路径一律相对 localRoot 解析；绝对路径原样返回
		std::string ResolvePath(const std::string& candidate, const std::string& fallback) const;
		// 生成「不启用」的全局覆盖模板，便于用户发现该能力
		bool EnsureGlobalOverrideTemplate();
	};
} // namespace GakuMotion

#include "core/GkCoreCaptureAndScene.inl"
#include "core/GkCoreGuiQueue.inl"
#include "core/GkCoreGuiDispatch.inl"
#include "core/GkCoreLifecycle.inl"
#include "core/GkCoreLateUpdate.inl"
#include "core/GkCoreLibrary.inl"
#include "core/GkCoreLibraryImport.inl"
#include "core/GkCoreClock.inl"
#include "core/GkCoreSoles.inl"
#include "core/GkCoreSettingsStageCam.inl"
#include "core/GkCoreSettingsMedia.inl"
#include "core/GkCoreConfigSync.inl"
#include "core/GkCoreActorsAndOverrides.inl"
#include "core/GkCoreGuiContext.inl"

namespace GakuMotion
{
	// -----------------------------------------------------------------------------
	// 便捷自由函数（保持既有调用契约不变：插件侧仍可自由函数调用）
	// -----------------------------------------------------------------------------
	inline void Initialize(const std::string& rootPath)
	{
		Core::Instance().Initialize(rootPath);
	}

	inline void OnLateUpdate(void* actor, void* animator = nullptr, void* playableDirector = nullptr,
	                         float deltaTime = 0.0333f, bool advanceClock = true,
	                         float cameraDeltaTime = -1.0f, uint64_t physicalFrame = 0)
	{
		Core::Instance().OnLateUpdate(actor, animator, playableDirector, deltaTime, advanceClock,
		                             cameraDeltaTime, physicalFrame);
	}

	inline void OnLookAtUpdate(void* lookAtController, void* cameraTransform)
	{
		Core::Instance().OnLookAtUpdate(lookAtController, cameraTransform);
	}

	inline void OnActorChanged(void* newActor)
	{
		Core::Instance().OnActorChanged(newActor);
	}
} // namespace GakuMotion

using GakuMotion::VmdProbeInfo;
