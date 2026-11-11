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
#include "GkDataLayout.hpp"

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

namespace GakuMotion
{
	// -----------------------------------------------------------------------------
	// 日志格式化小工具（避免在 Core 各处重复写 snprintf 样板）
	// -----------------------------------------------------------------------------
	namespace detail
	{
		inline std::string FormatMsg(const char* fmt, ...)
		{
			char buf[1024]{};
			va_list args;
			va_start(args, fmt);
			std::vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);
			return std::string(buf);
		}
	} // namespace detail

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

		MotionPlayer player;
		CameraController camera;
		LookAtManager lookAt;
		GuiOverlay gui;
		StageManager stageManager;
		std::string stageLoadNotice;
		injection::Policy injectionPolicy;
		// The runtime enables enforcement before initialization. Offline consumers
		// may exercise the playback core without a game or native hook service.
		bool enforceInjectionPolicy{false};
		bool customWasActivated{false};
		bool stageStoppedByTransport{false};
		std::string injectionStatus;
		bool (*fnBeforeInjection)(){nullptr};
		bool InjectionAllowed() const { return !enforceInjectionPolicy || injectionPolicy.CanInject(); }
		bool PrepareInjection()
		{
			return InjectionAllowed() && (!fnBeforeInjection || fnBeforeInjection());
		}

		bool captureSuspended{false};
		PlaybackState capturePlaybackState{PlaybackState::Stopped};
		float capturePlaybackTime{0.0f};
		std::string captureMotionFile;
		uint64_t injectionSession{0}, captureSession{0};
		capture::RestoreReport captureRestoreReport;
		const char* captureResumeReason{"no_capture_session"};
		bool capturePerformanceWasActive{false};
		// Physics and native audio are released by the runtime before this method.
		bool SuspendForCapture()
		{
			if (captureSuspended) return captureRestoreReport.AllConfirmed();
			captureSuspended = true;
			captureRestoreReport = {};
			capturePlaybackState = player.state;
			capturePlaybackTime = player.currentTime;
			captureMotionFile = player.loadedMotionFile;
			captureSession = injectionSession;
			capturePerformanceWasActive = player.state != PlaybackState::Stopped ||
				(!stageStoppedByTransport && stageManager.HasCustomizations());
			const bool customAudioOwned = player.audioStarted;
			if (customAudioOwned && player.fnPauseAudio) player.fnPauseAudio();
			if (customAudioOwned)
			{
				const auto audioState = player.ObserveAudio();
				captureRestoreReport.nativeAudio = player.fnObserveAudio && audioState.confirmed && audioState.errorCode == 0 &&
					(audioState.state == audio::PlaybackState::Paused || audioState.state == audio::PlaybackState::Stopped)
					? capture::RestoreState::Confirmed : capture::RestoreState::Unconfirmed;
			}
			// Disabled GUI cancels its drag; retain the sampled time, not a scrub
			// ownership flag whose release event can no longer arrive.
			player.scrubbing = false;
			player.audioStarted = false;
			player.state = PlaybackState::Stopped;
			const auto actorRestore = player.controller.RestoreForCapture();
			auto status = [](bool modified, bool confirmed) {
				return !confirmed ? capture::RestoreState::Unconfirmed :
					modified ? capture::RestoreState::Confirmed : capture::RestoreState::NotModified;
			};
			captureRestoreReport.body = status(actorRestore.bodyModified, actorRestore.bodyConfirmed);
			captureRestoreReport.face = status(actorRestore.faceModified, actorRestore.faceConfirmed);
			captureRestoreReport.gaze = status(actorRestore.gazeModified, actorRestore.gazeConfirmed);
			const auto cameraRestore = camera.ReleaseForCapture();
			captureRestoreReport.camera = status(cameraRestore.modified, cameraRestore.confirmed);
			const bool stageModified = stageManager.HasCustomizations();
			captureRestoreReport.stage = status(stageModified, stageManager.SetInjectionEnabled(false));
			NotifyMotionPlaying(false);
			return captureRestoreReport.AllConfirmed();
		}

		bool ResumeAfterCapture()
		{
			if (!captureSuspended) return false;
			captureSuspended = false;
			if (!captureRestoreReport.AllConfirmed()) { captureResumeReason = "restore_unconfirmed"; return false; }
			if (!InjectionAllowed()) { captureResumeReason = "injection_not_allowed"; return false; }
			if (captureSession != injectionSession) { captureResumeReason = "stale_session"; return false; }
			if (captureMotionFile != player.loadedMotionFile) { captureResumeReason = "clip_changed"; return false; }
			player.currentTime = capturePlaybackTime;
			player.state = capturePlaybackState;
			if (capturePlaybackState == PlaybackState::Playing)
			{
				player.state = PlaybackState::Paused;
				player.Resume();
			}
			// Preview and paused states retain their exact time and do not start audio.
			NotifyMotionPlaying(player.state == PlaybackState::Playing);
			const bool resumeStage = capturePerformanceWasActive && !stageStoppedByTransport &&
				stageManager.HasCustomizations();
			const bool stageEnabled = resumeStage && stageManager.SetInjectionEnabled(true);
			captureResumeReason = player.state != PlaybackState::Stopped ? "same_session_restored" :
				stageEnabled ? "same_session_stage_reenabled" : resumeStage ? "stage_reenable_unconfirmed" : "gm_was_off";
			// This restores runtime ownership; pose/stage pixels update on the next tick.
			return player.state != PlaybackState::Stopped || stageEnabled;
		}

		void RevokeInjection()
		{
			captureResumeReason = "gm_revoked";
			++injectionSession;
			captureSuspended = false;
			if (player.state != PlaybackState::Stopped) player.Stop();
			camera.Release();
			stageManager.SetInjectionEnabled(false);
			NotifyMotionPlaying(false);
		}
		void StopPerformance()
		{
			++injectionSession;
			captureSuspended = false;
			captureResumeReason = "gm_stopped";
			stageStoppedByTransport = true;
			player.Stop();
			camera.Release();
			stageManager.SetInjectionEnabled(false);
			NotifyMotionPlaying(false);
		}

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
		bool importBusy{false};
		ImportProgressState importProgress;
		ImportProgressState guiImportProgressSnapshot;
		std::string activeImportEditDir;
		bool (*fnStartImport)(const MotionImportRequest&, std::string&){nullptr};
		int (*fnPollImport)(std::string&){nullptr};
		int (*fnPollImportProgress)(std::string&, ImportProgressState*){nullptr};
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

		// ---- 路径 ----
		std::string localRoot;
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
		struct AppliedDeclaration
		{
			std::string selectedMotion;
			int gazeMode{-1};
			int desktopLookAt{-1};
			float eyeWeight{-1.0f};
			float headWeight{-1.0f};
			float bodyWeight{-1.0f};
			int useRestRotations{-1};
			int legRestBasis{-1};
			int eyePoseRelativeGain{-1};
			int preserveAnimatorForSecondaryPhysics{-1};
			int overridesEnabled{-1};
			int auxMode{-1};
			int experimentalWaistSupport{-1};
			std::string globalFile;
			std::string motionFile;
			float playbackSpeed{-1.0f};
			int facialEnableBaseline{-1};
			int facialMikuMouthFit{-1};
			float facialBaselineMultiplier{-1.0f};
			std::string facialCharacterOverride;
		} applied;

		CameraSettings appliedCamera;
		bool cameraConfigApplied{false};

		// -------------------------------------------------------------------------
		// GUI 线程与 Unity 主线程安全通信队列（杜绝在 GUI 线程调用 Unity API 导致闪退）
		// -------------------------------------------------------------------------
		enum class GuiAction
		{
			Select, Cycle, Play, Pause, Resume, Stop, Seek,
			ScrubBegin, ScrubUpdate, ScrubEnd, Refresh, Move,
			SaveList, LoadList, Mode, Mute, Speed, Import, ClearTarget, NextTarget, PickTarget,
			InitialPosition, SaveInitialPosition, PhysicsBackend,
			CameraSettings, SaveCameraSettings, CameraImport, ReloadCamera,
			SoleSettings, SaveSoleSettings, LoadSoleSettings, ExperimentalSolePitch, ExperimentalWaistSupport,
			BeforeQuartzExperiment,
			StageSettings, StagePreset, ToggleStageCandidate, ScanStageCandidates,
			MountCustomStage, UnloadCustomStage, RestoreCameraSkybox,
			SeekFrame, StepFrame, ReloadMotion, DeleteFiles, StageLoadReplacement, SelectTarget
		};

		std::atomic<int> guiPhysicsBackend{2};
		std::atomic_bool guiTextInputActive{false};
		std::atomic<bool> guiBeforeQuartzExperiment{false};
		std::atomic<bool> guiExperimentalWaistSupport{true};

		struct GuiCommand
		{
			GuiAction action;
			float value{0};
			bool flag{false};
			std::string key;
			MotionImportRequest import;
			Vector3 position;
			GakuMotion::CameraSettings cameraSettings;
			CameraImportRequest cameraImport;
			SoleMeasurementSettings soleSettings;
			sole_pitch::Settings experimentalSolePitchSettings;
			GakuMotion::StageSettings stageSettings;
			int stagePresetIndex{0};
			int candidateIndex{-1};
			int64_t frame{0};
			uint64_t loadedGeneration{0}, sceneSession{0}, libraryRevision{0};
			uint64_t actorToken{0}, actorEpoch{0};
		};

		std::mutex guiCommandMutex;
		std::recursive_mutex stateMutex; // Host LateUpdate and render share this guard.
		std::deque<GuiCommand> guiCommands;

		void QueueGui(GuiCommand cmd)
		{
			std::lock_guard<std::mutex> lock(guiCommandMutex);
			// Only adjacent scrub/placement updates may coalesce. Begin/end/save/
			// transport events retain their order across physical frames.
			if ((cmd.action == GuiAction::ScrubUpdate || cmd.action == GuiAction::InitialPosition) && !guiCommands.
				empty() &&
				guiCommands.back().action == cmd.action)
				guiCommands.back() = std::move(cmd);
			else guiCommands.push_back(std::move(cmd));
		}

		void QueueSelectMotion(int index, bool autoplay)
		{
			if (index >= 0 && index < static_cast<int>(motions.size()))
				QueueGui({GuiAction::Select, 0, autoplay, motions[index].name});
		}

		void QueueCycleMotion(int delta) { QueueGui({GuiAction::Cycle, static_cast<float>(delta)}); }
		void QueuePlay() { QueueGui({GuiAction::Play}); }
		void QueuePause() { QueueGui({GuiAction::Pause}); }
		void QueueResume() { QueueGui({GuiAction::Resume}); }
		void QueueStop() { QueueGui({GuiAction::Stop}); }
		GuiCommand PositionCommand(GuiAction action, float value = 0)
		{
			GuiCommand command{action, value, false, player.loadedMotionFile};
			command.loadedGeneration = loadedMotionGeneration;
			command.sceneSession = injectionSession;
			return command;
		}
		void QueueSeek(float seconds) { QueueGui(PositionCommand(GuiAction::Seek, seconds)); }
		void QueueSeekFrame(int64_t frame)
		{
			auto command = PositionCommand(GuiAction::SeekFrame);
			command.frame = frame;
			QueueGui(std::move(command));
		}
		void QueueStepFrame(int delta) { QueueGui(PositionCommand(GuiAction::StepFrame, static_cast<float>(delta))); }
		void QueueReloadMotion() { QueueGui(PositionCommand(GuiAction::ReloadMotion)); }

		void QueueScrubBegin(float seconds)
		{
			QueueGui(PositionCommand(GuiAction::ScrubBegin, seconds));
		}

		void QueueScrubUpdate(float seconds)
		{
			QueueGui(PositionCommand(GuiAction::ScrubUpdate, seconds));
		}

		void QueueScrubEnd(float seconds) { QueueGui(PositionCommand(GuiAction::ScrubEnd, seconds)); }

		void QueueInitialPosition(const Vector3& offset)
		{
			GuiCommand cmd{GuiAction::InitialPosition};
			cmd.position = offset;
			QueueGui(std::move(cmd));
		}

		void QueueCameraSettings(const GakuMotion::CameraSettings& settings)
		{
			GuiCommand cmd{GuiAction::CameraSettings};
			cmd.cameraSettings = settings;
			QueueGui(std::move(cmd));
		}

		void QueueSoleSettings(const SoleMeasurementSettings& settings)
		{
			GuiCommand cmd{GuiAction::SoleSettings};
			cmd.soleSettings = settings;
			QueueGui(std::move(cmd));
		}

		void QueueExperimentalSolePitch(const sole_pitch::Settings& settings)
		{
			GuiCommand cmd{GuiAction::ExperimentalSolePitch};
			cmd.experimentalSolePitchSettings = settings;
			QueueGui(std::move(cmd));
		}

		bool SetSoleSettings(const SoleMeasurementSettings& settings)
		{
			if (!settings.IsValid())
			{
				soleNotice = "鞋底测量设置无效，保留已应用设置";
				return false;
			}
			soleSettings = settings;
			soleSnapshot = {};
			soleNotice = "已应用测量设置（姿态不变，尚未保存）";
			return true;
		}

		void QueuePhysicsBackend(int backend)
		{
			if (backend >= 0 && backend <= 2) QueueGui({GuiAction::PhysicsBackend, static_cast<float>(backend)});
		}

		void QueueExperimentalWaistSupport(bool enabled)
		{
			QueueGui({GuiAction::ExperimentalWaistSupport, 0, enabled});
		}

		void QueueBeforeQuartzExperiment(bool enabled)
		{
			QueueGui({GuiAction::BeforeQuartzExperiment, 0, enabled});
		}

		void QueueStageSettings(const StageSettings& settings)
		{
			GuiCommand cmd{GuiAction::StageSettings};
			cmd.stageSettings = settings;
			QueueGui(std::move(cmd));
		}

		void QueueStageLoadReplacement(const std::string& key)
		{
			GuiCommand cmd{GuiAction::StageLoadReplacement};
			cmd.key = key;
			QueueGui(std::move(cmd));
		}

		void QueueStagePreset(int presetIndex)
		{
			GuiCommand cmd{GuiAction::StagePreset};
			cmd.stagePresetIndex = presetIndex;
			QueueGui(std::move(cmd));
		}

		void QueueToggleStageCandidate(int candidateIndex, bool active)
		{
			GuiCommand cmd{GuiAction::ToggleStageCandidate};
			cmd.candidateIndex = candidateIndex;
			cmd.flag = active;
			QueueGui(std::move(cmd));
		}

		void QueueScanStageCandidates()
		{
			QueueGui({GuiAction::ScanStageCandidates});
		}

		void QueueMountCustomStage(const std::string& stageKey)
		{
			GuiCommand cmd{GuiAction::MountCustomStage};
			cmd.key = stageKey;
			QueueGui(std::move(cmd));
		}

		void QueueUnloadCustomStage()
		{
			QueueGui({GuiAction::UnloadCustomStage});
		}

		void QueueRestoreCameraSkybox()
		{
			QueueGui({GuiAction::RestoreCameraSkybox});
		}

		void ProcessPendingStageLoadCommands()
		{
			std::deque<GuiCommand> commands;
			{
				std::lock_guard<std::mutex> lock(guiCommandMutex);
				for (auto it = guiCommands.begin(); it != guiCommands.end();)
				{
					if (it->action != GuiAction::StageLoadReplacement) { ++it; continue; }
					commands.push_back(std::move(*it));
					it = guiCommands.erase(it);
				}
			}
			for (const auto& command : commands) SetStageLoadReplacement(command.key);
		}

		bool HasPendingTargetSelection()
		{
			std::lock_guard<std::mutex> lock(guiCommandMutex);
			return std::any_of(guiCommands.begin(), guiCommands.end(), [](const auto& command)
				{ return command.action == GuiAction::SelectTarget; });
		}

		// File work must keep progressing on host frames even when Photography
		// has not created a dancer yet. Do not pass an earlier transport/scene
		// command: e.g. a queued Stop must still precede a camera import.
		void ProcessPendingLibraryCommands()
		{
			std::lock_guard<std::recursive_mutex> lock(stateMutex);
			std::deque<GuiCommand> commands;
			{
				std::lock_guard<std::mutex> queueLock(guiCommandMutex);
				while (!guiCommands.empty())
				{
					const auto action = guiCommands.front().action;
					if (action != GuiAction::Import && action != GuiAction::CameraImport && action != GuiAction::Refresh) break;
					commands.push_back(std::move(guiCommands.front()));
					guiCommands.pop_front();
				}
			}
			for (const auto& command : commands)
			{
				if (command.action == GuiAction::Import) StartImport(command.import);
				else if (command.action == GuiAction::CameraImport) StartCameraImport(command.cameraImport);
				else BeginLibraryRefresh();
			}
			PollImport();
			PollCameraImport();
			PollLibraryRefresh();
		}

		void ProcessPendingGuiCommands()
		{
			std::deque<GuiCommand> commands;
			{
				std::lock_guard<std::mutex> lock(guiCommandMutex);
				commands.swap(guiCommands);
			}
			for (const auto& cmd : commands)
			{
				// This command stores a future Photography-load preference only.
				// It never touches Unity, playback or the current background.
				if (cmd.action == GuiAction::StageLoadReplacement)
				{
					SetStageLoadReplacement(cmd.key);
					continue;
				}
				// Never replay controls queued before a scene/capture boundary.
				if (!InjectionAllowed()) continue;
				const bool canStartInjection = cmd.action == GuiAction::Play || cmd.action == GuiAction::Resume ||
					cmd.action == GuiAction::Select || cmd.action == GuiAction::Cycle || cmd.action == GuiAction::Seek ||
					cmd.action == GuiAction::ScrubBegin || cmd.action == GuiAction::StageSettings ||
					cmd.action == GuiAction::StagePreset || cmd.action == GuiAction::ToggleStageCandidate ||
					cmd.action == GuiAction::MountCustomStage || cmd.action == GuiAction::SeekFrame ||
					cmd.action == GuiAction::StepFrame || cmd.action == GuiAction::ReloadMotion;
				const bool positionCommand = (cmd.action >= GuiAction::Seek && cmd.action <= GuiAction::ScrubEnd) ||
					cmd.action == GuiAction::SeekFrame || cmd.action == GuiAction::StepFrame || cmd.action == GuiAction::ReloadMotion;
				if (positionCommand && (cmd.key != player.loadedMotionFile ||
					cmd.loadedGeneration != loadedMotionGeneration || cmd.sceneSession != injectionSession))
					continue;
				if (canStartInjection && !PrepareInjection()) continue;
				if (canStartInjection) stageStoppedByTransport = false;
#ifdef GKMOTION_SCRUB_TRACE
				if (cmd.action >= GuiAction::ScrubBegin && cmd.action <= GuiAction::ScrubEnd)
					player.Probe("[GakuMotion][ProbeS1][CORE] dequeued event=%d value=%.3f",
					             static_cast<int>(cmd.action), cmd.value);
#endif
				switch (cmd.action)
				{
				case GuiAction::Select: SelectMotionByIndex(FindPlaylistIndex(motions, cmd.key), cmd.flag, !cmd.flag);
					break;
				case GuiAction::Cycle: CycleMotion(static_cast<int>(cmd.value), "gui");
					break;
				case GuiAction::Play: player.Play();
					break;
				case GuiAction::Pause: player.Pause();
					break;
				case GuiAction::Resume: player.Resume();
					break;
				case GuiAction::Stop: StopPerformance();
					break;
				case GuiAction::Seek: player.Seek(cmd.value);
					break;
				case GuiAction::SeekFrame: player.SeekFrame(cmd.frame);
					break;
				case GuiAction::StepFrame: player.StepFrame(static_cast<int>(cmd.value));
					break;
				case GuiAction::ReloadMotion: ReloadCurrentMotion();
					break;
				case GuiAction::ScrubBegin: player.BeginScrub(cmd.value);
					break;
				case GuiAction::ScrubUpdate: player.UpdateScrub(cmd.value);
					break;
				case GuiAction::ScrubEnd: player.EndScrub(cmd.value);
					break;
				case GuiAction::Refresh: BeginLibraryRefresh();
					break;
				case GuiAction::DeleteFiles:
					if (cmd.libraryRevision == libraryRevision) DeleteMotionFiles(cmd.key, cmd.flag);
					else playlistNotice = "曲库已变化，请重新选择并确认删除";
					break;
				case GuiAction::Move: MoveMotion(cmd.key, static_cast<int>(cmd.value));
					break;
				case GuiAction::SaveList: SavePlaylist(cmd.key);
					break;
				case GuiAction::LoadList:
					if (playlist.Load(cmd.key.empty() ? playlistPath : cmd.key, playlistNotice))
					{
						player.loop = playlist.mode == PlaylistMode::SingleLoop;
						RefreshMotionLibrary();
						SavePlaylist();
					}
					break;
				case GuiAction::Mode:
					playlist.mode = static_cast<PlaylistMode>(std::clamp(static_cast<int>(cmd.value), 0, 3));
					player.loop = playlist.mode == PlaylistMode::SingleLoop;
					playlist.shuffleRemaining.clear();
					SavePlaylist();
					break;
				case GuiAction::Mute: player.SetMuted(cmd.flag);
					break;
				case GuiAction::Speed:
					player.SetPlaybackSpeed(cmd.value);
					break;
				case GuiAction::Import: StartImport(cmd.import);
					break;
				case GuiAction::CameraSettings:
					if (cmd.cameraSettings.IsValid())
					{
						const auto prevMode = camera.settings.mode;
						const auto prevBehavior = camera.settings.followBehavior;
						const bool prevEnabled = camera.settings.enabled;
						camera.settings = cmd.cameraSettings;
						if (!camera.settings.enabled) camera.Release();
						camera.Publish();
						if ((prevEnabled != cmd.cameraSettings.enabled || prevMode != cmd.cameraSettings.mode || prevBehavior != cmd.cameraSettings.followBehavior) &&
							!configRoot.empty() && std::filesystem::exists(MotionPathFromUtf8(configRoot + "/camera.json")))
						{
							SaveCameraSettings();
						}
					}
					break;
				case GuiAction::SaveCameraSettings: SaveCameraSettings();
					break;
				case GuiAction::CameraImport: StartCameraImport(cmd.cameraImport);
					break;
				case GuiAction::ReloadCamera:
					if (!cameraImportBusy) ReloadCamera();
					break;
				case GuiAction::SoleSettings: SetSoleSettings(cmd.soleSettings);
					break;
				case GuiAction::SaveSoleSettings: SaveSoleSettings();
					break;
				case GuiAction::LoadSoleSettings: LoadSoleSettings();
					break;
				case GuiAction::ExperimentalSolePitch:
					if (cmd.experimentalSolePitchSettings.IsValid()) experimentalSolePitchSettings = cmd.
						experimentalSolePitchSettings;
					break;
				case GuiAction::ExperimentalWaistSupport:
					config.experimentalWaistSupport = cmd.flag ? 1 : 0;
					ApplyDeclarativeConfig();
					break;
				case GuiAction::BeforeQuartzExperiment:
					config.beforeQuartzExperiment = cmd.flag && config.secondaryPhysicsBackend == 2;
					ApplyDeclarativeConfig();
					break;
				case GuiAction::SelectTarget:
					if (void* actor = sceneActorRegistry.Resolve(cmd.actorToken, cmd.actorEpoch)) SetTargetActor(actor);
					break;
				case GuiAction::ClearTarget: PickBoundActorAsTarget();
					break;
				case GuiAction::NextTarget: CycleTargetActor();
					break;
				case GuiAction::PickTarget: PickBoundActorAsTarget();
					break;
				case GuiAction::InitialPosition: player.controller.SetInitialPositionOffset(cmd.position);
					break;
				case GuiAction::SaveInitialPosition: SaveInitialPosition();
					break;
				case GuiAction::PhysicsBackend:
					{
						const int backend = static_cast<int>(cmd.value);
						if (backend < 0 || backend > 2) break;
						config.secondaryPhysicsBackend = backend;
						config.preserveAnimatorForSecondaryPhysics = backend == 2 ? 1 : 0;
						ApplyDeclarativeConfig();
						LogInfo(detail::FormatMsg("[GakuMotion][Physics] GUI selected %s",
						                          backend == 2 ? "native(experimental)" : backend == 1 ? "fallback" : "off"));
						break;
					}
				case GuiAction::StageSettings:
					{
						StageSettings settings = cmd.stageSettings;
						// A stale visual-settings snapshot cannot overwrite the separately
						// saved load-time selection from the dedicated control.
						settings.loadReplacementKey = stageManager.GetLoadReplacementKey();
						stageManager.ApplySettings(settings);
					}
					break;
				case GuiAction::StagePreset:
					stageManager.ApplyPreset(cmd.stagePresetIndex);
					break;
				case GuiAction::ToggleStageCandidate:
					stageManager.ToggleCandidate(cmd.candidateIndex, cmd.flag);
					break;
				case GuiAction::ScanStageCandidates:
					stageManager.RescanCandidates(currentActor);
					break;
				case GuiAction::MountCustomStage:
					stageManager.RequestMountStage(cmd.key);
					break;
				case GuiAction::UnloadCustomStage:
					stageStoppedByTransport = false; // Explicit cleanup may run while transport is stopped.
					stageManager.RequestUnloadStage();
					break;
				case GuiAction::RestoreCameraSkybox:
					stageManager.ForceRestoreCameraClear(nullptr);
					break;
				}
			}
			PollImport();
			PollCameraImport();
			PollLibraryRefresh();
			NotifyMotionPlaying(player.state == PlaybackState::Playing);
		}

		void PollImport()
		{
			if (!importBusy) return;
			std::string result;
			int state = 0;
			{
				std::lock_guard<std::recursive_mutex> lock(stateMutex);
				state = fnPollImportProgress ? fnPollImportProgress(result, &importProgress)
				                             : (fnPollImport ? fnPollImport(result) : 0);
				if (state != 0)
				{
					importBusy = false;
					importProgress.active = false;
					importProgress.finished = true;
					importProgress.succeeded = (state > 0);
					if (state > 0)
					{
						importProgress.progress = 1.0f;
						if (importProgress.statusText.empty()) importProgress.statusText = "导入完成";
						RefreshMotionLibrary();
						SavePlaylist();
						// Keep every loaded channel on one version until explicit reload.
						if (!activeImportEditDir.empty() && player.currentMotionDir == activeImportEditDir)
							loadedSourceChanged = true;
					}
					else
					{
						if (importProgress.errorDetail.empty()) importProgress.errorDetail = result;
						if (importProgress.statusText.empty()) importProgress.statusText = "导入失败";
					}
					activeImportEditDir.clear();
					playlistNotice = result;
				}
			}
		}

		// =========================================================================
		// 生命周期
		// =========================================================================
		void Initialize(const std::string& rootPath, const DataLayout* selectedLayout = nullptr)
		{
			if (initialized) return;

			// PROBE S1：GUI 侧与播放器侧的探针都汇入宿主 logger，宿主按 ANSI 解码，
			// 故探针文案一律 ASCII（中文会变乱码）。挂在这里保证任何早于面板首次
			// 绘制的拖动动作也能出日志。
			player.fnProbe = [](const char* line)
			{
				auto& c = Core::Instance();
				if (c.fnLogInfo) c.fnLogInfo("%s", line);
			};

			localRoot = rootPath;
			dataLayout = selectedLayout ? *selectedLayout : DataLayout::Legacy(rootPath);
			motionsRoot = dataLayout.Motions();
            // A fresh empty library is a valid first-run state. Create it
            // before the initial scan; permission and I/O errors remain visible.
            std::error_code libraryError;
            std::filesystem::create_directories(MotionPathFromUtf8(motionsRoot), libraryError);
            if (libraryError) LogWarn("[GakuMotion][Paths] Cannot create library: " + libraryError.message());
			configRoot = dataLayout.configs;
			logsRoot = dataLayout.logs;
			diagnosticsRoot = dataLayout.managed ? logsRoot + "/diagnostics" : configRoot;
			transactionRoot = dataLayout.managed ? dataLayout.temp + "/transactions" : "";
			LoadStageLoadSettings();
			playlistPath = dataLayout.Playlist();
			initialPositionPath = configRoot + "/initial_position.json";
			LoadInitialPosition();
			globalOverridePath = configRoot + "/bone_overrides.json";

			gui.motionsRoot = motionsRoot;

			// 1. 控制文件：首次运行生成模板，随后立即读取一次
			config.rootDir = configRoot;
			config.path = configRoot + "/control.json";
			const bool createdConfig = config.EnsureFileExists();
			config.Poll(0.0f, /*forceReload=*/true);
			if (!config.lastError.empty())
			{
				LogWarn("[GakuMotion][Config] control file parse failed: " + config.lastError);
			}

			// 2. 全局骨骼覆盖文件：首次运行生成「模板但不启用」的空表
			const bool createdOverrides = EnsureGlobalOverrideTemplate();

			// 3. 曲目库
			if (std::filesystem::exists(MotionPathFromUtf8(playlistPath))) playlist.Load(playlistPath, playlistNotice);
			player.loop = playlist.mode == PlaylistMode::SingleLoop;
			RefreshMotionLibrary();

			// 4. 初始曲目：优先取控制文件声明，其次取列表第一个
			if (!config.selectedMotion.empty())
			{
				const int idx = FindMotionIndex(motions, config.selectedMotion);
				if (idx >= 0)
				{
					SelectMotionByIndex(idx, false);
					applied.selectedMotion = config.selectedMotion;
				}
				else
				{
					LogWarn("[GakuMotion][Config] selected_motion not found: " + config.selectedMotion);
				}
			}
			if (currentMotionIndex < 0 && !motions.empty())
			{
				SelectMotionByIndex(0, false);
			}

			// 5. 配置与覆盖首次应用
			ApplyDeclarativeConfig();
			LoadCameraSettings();
			LoadSoleSettings();
			LoadOverrides(true);

			// Loading the persisted selection is not a request to take over the
			// first actor created by the game (home/work/official Live).
			player.state = PlaybackState::Stopped;
			config.ClearCommands();

			initialized = true;
			LogInfo(detail::FormatMsg(
				"[GakuMotion] Core initialized. motions=%zu, current=%s, control=%s, globalOverrides=%s",
				motions.size(),
				(currentMotionIndex >= 0 && currentMotionIndex < static_cast<int>(motions.size()))
					? motions[currentMotionIndex].name.c_str()
					: "<none>",
				config.path.c_str(),
				globalOverridePath.c_str()));
			if (createdConfig) LogInfo("[GakuMotion][Config] generated control-file template (hot reload ~0.5s)");
			if (createdOverrides) LogInfo(
				"[GakuMotion][Overrides] generated global bone-override template (enabled=false, inactive by default)");
		}

		void OnActorChanged(void* newActor)
		{
			++injectionSession;
			if (!newActor) camera.Release(false);
			currentActor = newActor;
			player.OnActorChanged(newActor);
			ResetSoleObservation();
		}

		void AbandonScene()
		{
			captureResumeReason = "scene_changed";
			++injectionSession;
			captureSuspended = false;
			// The old scene may already be unloading. Drop references without
			// calling RestoreMute/Rebind on its destroyed Unity objects.
			camera.Release(false);
			player.AbandonScene();
			stageManager.AbandonScene();
			ResetSoleObservation();
			sceneActors.clear();
			sceneActorRegistry.Clear();
			currentActor = nullptr;
			currentActorBoundBones = currentActorTotalBones = 0;
			NotifyMotionPlaying(false);
		}

		bool DiscardPreviousActorBindings()
		{
			// Stop restores animation but keeps rest snapshots for switching songs.
			// When the actor set changes, those snapshots are no longer safe, even
			// if playback was already stopped before the actor was destroyed.
			if (player.controller.bindings.empty()) return false;
			++injectionSession;
			// Actor replacement in the SAME scene must retain stage ownership.
			// The stage manager restores/rescans independently before this call.
			captureSuspended = false;
			camera.Release(false);
			player.AbandonScene();
			ResetSoleObservation();
			sceneActors.clear();
			sceneActorRegistry.Clear();
			currentActor = nullptr;
			currentActorBoundBones = currentActorTotalBones = 0;
			NotifyMotionPlaying(false);
			return true;
		}

		// =========================================================================
		// 每帧驱动入口（由插件在 OnLateUpdate 中调用）
		// =========================================================================
		void OnLateUpdate(void* actor, void* animator = nullptr,
		                  void* playableDirector = nullptr, float deltaTime = 0.0333f,
		                  bool advanceClock = true, float cameraDeltaTime = -1.0f,
		                  uint64_t physicalFrame = 0)
		{
			currentActor = actor;

			// 在 Unity 主线程安全执行由 GUI 线程发出的所有控制指令
			if (advanceClock)
			{
				ProcessPendingGuiCommands();
			}

			// 1. 配置轮询（低频，内部自带时间累加；无变化时几乎零开销）
			if (advanceClock && config.Poll(deltaTime))
			{
				if (!config.lastChangeLog.empty())
				{
					LogInfo("[GakuMotion][Config] " + config.lastChangeLog);
				}
				if (!config.lastError.empty())
				{
					LogWarn("[GakuMotion][Config] " + config.lastError);
				}
				ApplyDeclarativeConfig();
				ConsumeConfigCommands();
			}

			if (!InjectionAllowed())
			{
				config.ClearCommands();
				return;
			}
			if (player.state != PlaybackState::Stopped && !PrepareInjection()) return;

			// 2. 骨骼覆盖集合始终指向最新合成结果
			player.controller.SetBoneOverrides(&effectiveOverrides);

			// 3. 姿态灌入（仅新物理帧首个 Actor 推进全局时钟，同帧其余 Actor 仅应用姿态）
			// Marker proves that this callback wrote each Foot's original motion.
			// No historical delta/cache is retained, so pause/seek cannot integrate it.
			player.controller.footPoseWriteStamp.actor = nullptr;
			player.controller.footPoseWriteStamp.mask = 0;
			player.controller.footPoseWriteStamp.transforms = {};
			player.controller.eyePoseWriteStamp.actor = nullptr;
			player.controller.eyePoseWriteStamp.mask = 0;
			player.controller.eyePoseWriteStamp.transforms = {};
			player.Tick(deltaTime, actor, animator, playableDirector, advanceClock);
			ObserveSoles(actor); // Read-only Unity queries, on the same main-thread frame.
			ApplyExperimentalSolePitch(actor); // Opt-in demo, separate from #078 evidence.
			WriteRequestedSolePitchSnapshot(actor);
			if (advanceClock && player.finished)
			{
				player.finished = false;
				const int next = playlist.Next(motions, currentMotionIndex);
				if (next >= 0) SelectMotionByIndex(next, true);
				else NotifyMotionPlaying(false);
			}

			// A stable target prevents multiple actors from fighting over one camera.
			void* cameraActor = player.controller.targetActor;
			if (cameraActor && camera.fnAlive && !camera.fnAlive(cameraActor)) cameraActor = nullptr;
			if (!cameraActor)
			{
				for (void* candidate : sceneActors)
				{
					if (!camera.fnAlive || camera.fnAlive(candidate))
					{
						cameraActor = candidate;
						break;
					}
				}
			}
			if (!cameraActor) cameraActor = actor;
			if (player.state == PlaybackState::Stopped) camera.Release();
			else if (actor == cameraActor)
			{
				camera.LoadTrack(player.currentMotionDir, player.clip);
				player.cameraSourceDuration = camera.track.frames.empty() ? 0.0f :
					static_cast<float>(camera.track.frames.size()) / camera.track.fps;
				camera.Tick(player.state == PlaybackState::Playing || player.state == PlaybackState::Paused || player.state == PlaybackState::Preview,
				            player.CameraSourceTime(), cameraDeltaTime >= 0.0f ? cameraDeltaTime : deltaTime,
				            actor, physicalFrame);
			}

			// 4. 角色状态快照（供面板/日志）
			currentActorBoundBones = player.controller.currentBinding.boundBoneCount;
			currentActorTotalBones = player.controller.currentBinding.boneTransforms.size();
		}

		void OnLookAtUpdate(void* lookAtController, void* cameraTransform)
		{
			if (!InjectionAllowed() || player.state == PlaybackState::Stopped || !PrepareInjection()) return;
			lookAt.Update(lookAtController, cameraTransform);
		}

		// =========================================================================
		// 曲目库与选曲
		// =========================================================================
		bool ApplyLibrarySnapshot(LibraryScanSnapshot snapshot)
		{
			if (!snapshot.result.Succeeded())
			{
				libraryNotice = "刷新失败，仍显示上次结果: " + snapshot.result.error;
				LogWarn("[GakuMotion][Library] " + snapshot.result.error);
				return false;
			}
			std::error_code rootError;
			if (!std::filesystem::is_directory(MotionPathFromUtf8(motionsRoot), rootError))
			{
				libraryNotice = "刷新失败，曲库目录已不可访问；仍显示上次结果";
				return false;
			}
			const std::string current = !loadedMotionKey.empty() ? loadedMotionKey :
				(CurrentMotion() ? CurrentMotion()->name : "");
			auto found = std::move(snapshot.result.motions);
			const bool complete = snapshot.result.Complete();
			if (!complete)
			{
				// Unreadable/half-copied files are not evidence that an old song was deleted.
				for (const auto& old : motions)
					if (FindPlaylistIndex(found, old.name) < 0)
					{
						std::error_code error;
						const bool exists = std::filesystem::exists(MotionPathFromUtf8(old.dir), error);
						if (exists || error)
						{
							found.push_back(old);
							if (libraryVersions.contains(old.name)) snapshot.versions[old.name] = libraryVersions[old.name];
						}
					}
			}
			playlist.Apply(found);
			size_t added = 0, removed = 0, changed = 0;
			for (const auto& entry : found)
			{
				if (FindPlaylistIndex(motions, entry.name) < 0) ++added;
				else if (libraryVersions.contains(entry.name) && snapshot.versions.contains(entry.name) &&
					libraryVersions[entry.name] != snapshot.versions[entry.name]) ++changed;
			}
			for (const auto& entry : motions) if (FindPlaylistIndex(found, entry.name) < 0) ++removed;
			motions = std::move(found);
			libraryVersions = std::move(snapshot.versions);
			currentMotionIndex = current.empty() ? -1 : FindPlaylistIndex(motions, current);
			++libraryRevision;
			++libraryMutationGeneration;
			if (!player.currentMotionDir.empty())
			{
				loadedSourceMissing = FindMotionFile(player.currentMotionDir).empty();
				loadedSourceChanged = !loadedSourceMissing && ReadMotionFileVersion(player.currentMotionDir) != loadedMotionVersion;
			}
			libraryNotice = added || removed || changed
				? detail::FormatMsg("刷新完成：新增 %zu，移除 %zu，更新 %zu", added, removed, changed)
				: "曲库已是最新";
			if (!snapshot.result.warnings.empty())
				libraryNotice += detail::FormatMsg("；%zu 项扫描提示：", snapshot.result.warnings.size()) + snapshot.result.warnings.front();
			return true;
		}

		bool RefreshMotionLibrary() { return ApplyLibrarySnapshot(ReadLibrarySnapshot(motionsRoot)); }

		void BeginLibraryRefresh()
		{
			if (libraryScan.valid()) return;
			if (importBusy || cameraImportBusy) { libraryNotice = "正在导入，请在完成后刷新"; return; }
			libraryScanGeneration = libraryMutationGeneration;
			const auto root = motionsRoot;
			try
			{
				libraryScan = std::async(std::launch::async, [root] { return ReadLibrarySnapshot(root); });
				libraryNotice = "正在刷新曲库…";
			}
			catch (const std::exception& error) { libraryNotice = "无法启动刷新: " + std::string(error.what()); }
		}

		void PollLibraryRefresh()
		{
			if (!libraryScan.valid() || libraryScan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
			try
			{
				auto snapshot = libraryScan.get();
				if (libraryScanGeneration == libraryMutationGeneration && !importBusy && !cameraImportBusy)
					ApplyLibrarySnapshot(std::move(snapshot));
				else libraryNotice = "曲库在扫描期间已变化，请重新刷新";
			}
			catch (const std::exception& error) { libraryNotice = "刷新失败，仍显示上次结果: " + std::string(error.what()); }
		}

		void WaitForLibraryRefresh()
		{
			if (libraryScan.valid()) { libraryScan.wait(); libraryScan = {}; }
		}

		bool CanOpenLoadedAudio()
		{
			if (loadedMotionKey.empty() || player.currentMotionDir.empty()) return true;
			if (ReadMotionFileVersion(player.currentMotionDir).files[4] == loadedMotionVersion.files[4]) return true;
			loadedSourceChanged = true;
			libraryNotice = "音乐文件已更新或移除，请先重新加载当前曲目再播放";
			return false;
		}

		bool ReloadCurrentMotion()
		{
			if (importBusy || cameraImportBusy) { libraryNotice = "正在导入，完成后可重新加载"; return false; }
			const auto directory = player.currentMotionDir;
			const auto file = FindMotionFile(directory);
			if (file.empty()) { libraryNotice = "重新加载失败：源动作文件已移除"; loadedSourceMissing = true; return false; }
			const auto version = ReadMotionFileVersion(directory);
			MotionClip next;
			CameraTrack nextCamera;
			if (!next.LoadFromFile(file) || !CameraController::ReadTrack(directory, next, nextCamera))
			{
				libraryNotice = "重新加载失败，保留当前版本：" + (nextCamera.error.empty() ? std::string("动作文件无效") : nextCamera.error);
				return false;
			}
			if (version != ReadMotionFileVersion(directory)) { libraryNotice = "文件仍在更新，请稍后重新加载"; return false; }
			const float time = player.currentTime;
			const bool preview = player.state == PlaybackState::Stopped || player.state == PlaybackState::Preview;
			player.AdoptLoadedMotion(directory, file, std::move(next));
			camera.InstallTrack(directory, std::move(nextCamera));
			player.cameraSourceDuration = camera.track.frames.empty() ? 0.0f :
				static_cast<float>(camera.track.frames.size()) / camera.track.fps;
			loadedMotionEntry.timing = player.timing;
			player.Seek(time);
			player.state = preview ? PlaybackState::Preview : PlaybackState::Paused;
			++loadedMotionGeneration;
			loadedMotionVersion = version;
			loadedSourceMissing = loadedSourceChanged = false;
			LoadOverrides(false);
			TryLoadAuxGroundTruth(directory);
			NotifyMotionPlaying(false);
			libraryNotice = "当前曲目已重新加载，保持静止；点击播放继续";
			return true;
		}

		void ClearCurrentMotion()
		{
			player.Stop();
			camera.Release();
			player.clip = MotionClip{};
			player.currentMotionDir.clear();
			player.loadedMotionFile.clear();
			currentMotionIndex = -1;
			loadedMotionKey.clear();
			loadedMotionEntry = {};
			loadedSourceMissing = loadedSourceChanged = false;
			++loadedMotionGeneration;
			motionOverrides = BoneOverrideSet{};
			LoadOverrides(false);
			NotifyMotionPlaying(false);
		}

		bool SavePlaylist(const std::string& destination = "")
		{
			std::string error;
			const bool ok = playlist.Save(destination.empty() ? playlistPath : destination, error);
			playlistNotice = ok ? "播放列表已保存" : "保存失败: " + error;
			return ok;
		}

		bool DeleteMotionFiles(const std::string& key, bool confirmed = false)
		{
			if (!confirmed) return false;
			if (importBusy || cameraImportBusy) { playlistNotice = "正在导入，完成后可删除曲目"; return false; }
			const int index = FindPlaylistIndex(motions, key);
			if (index < 0) return false;
			const auto root = MotionPathFromUtf8(motionsRoot);
			const auto source = MotionPathFromUtf8(motions[index].dir);
			if (source.filename() != MotionPathFromUtf8(key).filename() || MotionPathFromUtf8(key).has_parent_path())
			{ playlistNotice = "删除失败：曲目路径与标识不一致"; return false; }
			std::filesystem::path checkedRoot, checkedSource;
			std::string error;
			if (!LibraryDelete::ResolveRoot(root, checkedRoot, error) || !LibraryDelete::ResolveChild(checkedRoot, source, checkedSource, error))
			{ playlistNotice = "删除失败，原文件保留: " + error; return false; }
			// Verify persistence before irreversible work. A later external write or
			// lock can still fail; that must never be reported as a file rollback.
			if (!playlist.Save(playlistPath, error))
			{ playlistNotice = "删除失败，原文件保留；列表无法保存: " + error; return false; }
			const auto current = CurrentMotion() ? CurrentMotion()->name : "";
			const bool deletingLoaded = loadedMotionKey == key ||
				(!player.currentMotionDir.empty() && DataLayout::SamePath(player.currentMotionDir, motions[index].dir));
			if (deletingLoaded) { player.Stop(); camera.Release(); NotifyMotionPlaying(false); }
			bool changed = false;
			if (!LibraryDelete::PermanentlyDelete(root, source, changed, error))
			{
				if (changed)
				{
					if (deletingLoaded) ClearCurrentMotion();
					++libraryMutationGeneration;
					const bool refreshed = RefreshMotionLibrary();
					playlistNotice = std::string("删除未完成，部分文件可能已永久删除；") +
						(refreshed ? "已刷新曲库: " : "曲库刷新失败，请稍后刷新: ") + error;
				}
				else playlistNotice = "删除失败，原文件保留: " + error;
				return false;
			}
			if (deletingLoaded) ClearCurrentMotion();
			motions.erase(motions.begin() + index);
			playlist.Remember(motions);
			libraryVersions.erase(key);
			currentMotionIndex = deletingLoaded ? -1 : FindPlaylistIndex(motions, current);
			++libraryRevision; ++libraryMutationGeneration;
			if (!playlist.Save(playlistPath, error))
				playlistNotice = "曲目目录已永久删除；列表保存失败，请稍后保存: " + error;
			else playlistNotice = "已永久删除曲目目录: " + MotionPathToUtf8(checkedSource);
			return true;
		}

		bool MoveMotion(const std::string& key, int delta)
		{
			const int index = FindPlaylistIndex(motions, key);
			const int target = index + delta;
			if (index < 0 || motions[index].name != key || target < 0 || target >= static_cast<int>(motions.size()))
				return false;
			const std::string current = CurrentMotion() ? CurrentMotion()->name : "";
			std::swap(motions[index], motions[target]);
			++libraryRevision;
			++libraryMutationGeneration;
			playlist.Remember(motions);
			currentMotionIndex = FindPlaylistIndex(motions, current);
			SavePlaylist();
			return true;
		}

		void StartImport(MotionImportRequest request)
		{
			std::lock_guard<std::recursive_mutex> lock(stateMutex);
			if (importBusy || cameraImportBusy)
			{
				playlistNotice = "正在导入，请等待完成";
				return;
			}
			++libraryMutationGeneration; // Any in-flight scan predates this import.
			importProgress = {};
			importProgress.active = true;
			importProgress.progress = 0.02f;
			importProgress.stage = "INIT";
			importProgress.statusText = "正在启动原生转换后端...";

			if (!fnStartImport || (!fnPollImport && !fnPollImportProgress))
			{
				importProgress.active = false;
				importProgress.finished = true;
				importProgress.succeeded = false;
				importProgress.statusText = "导入服务未就绪";
				importProgress.errorDetail = "导入服务未就绪";
				playlistNotice = "导入服务未就绪";
				return;
			}

			if (!request.editKey.empty())
			{
				const int idx = FindPlaylistIndex(motions, request.editKey);
				if (idx < 0)
				{
					importProgress.active = false;
					importProgress.finished = true;
					importProgress.succeeded = false;
					importProgress.statusText = "目标曲目不存在";
					importProgress.errorDetail = "未找到待编辑曲目: " + request.editKey;
					playlistNotice = importProgress.errorDetail;
					return;
				}
				request.editDir = motions[idx].dir;
				activeImportEditDir = request.editDir;
			}
			else
			{
				activeImportEditDir.clear();
			}

			request.motionsRoot = motionsRoot;
			request.transactionRoot = transactionRoot;
			request.log = logsRoot + "/import.log";
			request.useCsharp = true;
			if (request.useCsharp)
			{
				importProgress.statusText = "正在启动原生转换后端...";
				importProgress.stage = "INIT";
			}
			importBusy = fnStartImport(request, playlistNotice);
			if (!importBusy)
			{
				importProgress.active = false;
				importProgress.finished = true;
				importProgress.succeeded = false;
				importProgress.statusText = "启动导入失败";
				importProgress.errorDetail = playlistNotice;
			}
			else
			{
				playlistNotice = request.editDir.empty() ? "正在转换 VMD，完成后将自动加入列表…"
				                                        : "正在更新曲目配置与轨道…";
			}
		}

		void ReloadCamera()
		{
			ReloadCurrentMotion();
			cameraNotice = libraryNotice;
		}

		void StartCameraImport(CameraImportRequest request)
		{
			if (cameraImportBusy || importBusy)
			{
				cameraNotice = "正在导入镜头，请等待完成";
				return;
			}
			if (player.state == PlaybackState::Playing || player.state == PlaybackState::Paused)
			{
				cameraNotice = "请先停止播放，再导入镜头";
				return;
			}
			const int index = FindPlaylistIndex(motions, request.motionKey);
			if (index < 0 || motions[index].name != request.motionKey)
			{
				cameraNotice = "目标曲目已不在列表中";
				return;
			}
			if (!fnStartCameraImport || !fnPollCameraImport)
			{
				cameraNotice = "镜头导入服务未就绪";
				return;
			}
			request.motionDirectory = motions[index].dir;
			request.log = logsRoot + "/camera_import.log";
			cameraImportDirectory = request.motionDirectory;
			++libraryMutationGeneration;
			cameraImportBusy = fnStartCameraImport(request, cameraNotice);
			if (cameraImportBusy) cameraNotice = "正在导入镜头，完成后可重新加载当前曲目…";
		}

		void PollCameraImport()
		{
			if (!cameraImportBusy || !fnPollCameraImport) return;
			std::string result;
			const int state = fnPollCameraImport(result);
			if (!state) return;
			cameraImportBusy = false;
			if (state > 0)
			{
				RefreshMotionLibrary();
				if (player.currentMotionDir == cameraImportDirectory) loadedSourceChanged = true;
			}
			cameraNotice = result;
		}

		bool LoadStageLoadSettings()
		{
			stage_load::Settings saved;
			std::string error;
			const auto path = configRoot + "/stage.json";
			std::error_code ec;
			const bool exists = std::filesystem::exists(MotionPathFromUtf8(path), ec);
			const bool loaded = exists && !ec && saved.Load(path, error);
			StageSettings settings = stageManager.settings;
			settings.loadReplacementKey = loaded ? saved.key : "";
			stageManager.ApplySettings(settings);
			if (ec || (exists && !loaded))
			{
				stageLoadNotice = "舞台设置读取失败，已使用原生背景: " + (ec ? ec.message() : error);
				LogWarn("[GakuMotion][StageLoad] saved settings rejected: " + (ec ? ec.message() : error));
				return false;
			}
			stageLoadNotice = loaded ? "摄影背景选择已载入；下次进入偶像摄影时生效" :
				"将使用原生背景；可预设下次进入偶像摄影时使用的舞台";
			return loaded;
		}

		bool SetStageLoadReplacement(const std::string& key)
		{
			if ((!key.empty() && !stage_load::IsTarget(key)) || configRoot.empty())
			{
				stageLoadNotice = "舞台选择未保存：选项无效或配置目录尚未就绪";
				return false;
			}
			stage_load::Settings saved{key};
			std::string error;
			if (!saved.Save(configRoot + "/stage.json", error))
			{
				stageLoadNotice = "舞台选择保存失败，保留原有选择: " + error;
				LogWarn("[GakuMotion][StageLoad] cannot save settings: " + error);
				return false;
			}
			StageSettings settings = stageManager.settings;
			settings.loadReplacementKey = key;
			stageManager.ApplySettings(settings);
			stageLoadNotice = key.empty() ? "已记住恢复原生背景；下次进入偶像摄影时生效" :
				"舞台选择已保存；下次进入偶像摄影时生效（跨区域和重启保留）";
			LogInfo("[GakuMotion][StageLoad] saved next Photography key=" + (key.empty() ? std::string("native") : key));
			return true;
		}

		bool LoadCameraSettings()
		{
			const auto path = configRoot + "/camera.json";
			if (!std::filesystem::exists(MotionPathFromUtf8(path))) return false;
			RuntimeConfig savedConfig;
			savedConfig.path = path;
			if (!savedConfig.Reload() || !savedConfig.cameraDeclared || !savedConfig.camera.IsValid())
			{
				cameraNotice = "镜头设置载入失败: " + savedConfig.lastError;
				return false;
			}
			camera.settings = savedConfig.camera;
			camera.Publish();
			cameraNotice = "已载入保存的镜头设置";
			return true;
		}

		bool SaveCameraSettings()
		{
			const auto& s = camera.settings;
			if (!s.IsValid())
			{
				cameraNotice = "镜头设置无效，无法保存";
				return false;
			}
			std::error_code ec;
			std::filesystem::create_directories(MotionPathFromUtf8(configRoot), ec);
			if (ec)
			{
				cameraNotice = "镜头设置保存失败: " + ec.message();
				return false;
			}
			const auto path = configRoot + "/camera.json", temporary = path + ".tmp";
			std::ofstream out(MotionPathFromUtf8(temporary), std::ios::binary | std::ios::trunc);
			auto vector = [&](const Vector3& v)
			{
				out << '[' << jsonlite::FormatNumber(v.x, 6) << ", " << jsonlite::FormatNumber(v.y, 6)
					<< ", " << jsonlite::FormatNumber(v.z, 6) << ']';
			};
			auto transform = [&](const char* key, const CameraModeTransform& t)
			{
				out << ",\n    \"" << key << "\": {\"pos\": ["
					<< jsonlite::FormatNumber(t.position.x, 6) << ", "
					<< jsonlite::FormatNumber(t.position.y, 6) << ", "
					<< jsonlite::FormatNumber(t.position.z, 6) << "], \"rot\": ["
					<< jsonlite::FormatNumber(t.rotation.x, 6) << ", "
					<< jsonlite::FormatNumber(t.rotation.y, 6) << ", "
					<< jsonlite::FormatNumber(t.rotation.z, 6) << "]}";
			};
			out << "{\n  \"camera\": {\n    \"enabled\": " << (s.enabled ? "true" : "false")
				<< ",\n    \"mode\": " << static_cast<int>(s.mode)
				<< ",\n    \"follow_behavior\": " << static_cast<int>(s.followBehavior)
				<< ",\n    \"follow_target\": \"" << CameraFollowTargetName(s.followTarget) << '"'
				<< ",\n    \"follow_actor\": " << (s.followActor ? "true" : "false")
				<< ",\n    \"limit_to_scene\": " << (s.limitToScene ? "true" : "false") << ",\n    \"bounds_min\": ";
			vector(s.boundsMin);
			out << ",\n    \"bounds_max\": ";
			vector(s.boundsMax);
			out << ",\n    \"follow_offset\": ";
			vector(s.followOffset);
			out << ",\n    \"target_offset\": ";
			vector(s.targetOffset);
			out << ",\n    \"vmd_offset\": ";
			vector(s.vmdOffset);
			out << ",\n    \"follow_factor\": ";
			vector(s.followFactor);
			out << ",\n    \"follow_rate\": ";
			vector(s.followRate);
			out << ",\n    \"follow_fov\": " << jsonlite::FormatNumber(s.followFov, 6)
				<< ",\n    \"actor_shadow_experiment_mode\": " << s.actorShadowExperimentMode
				<< ",\n    \"use_per_mode_transforms\": " << (s.usePerModeTransforms ? "true" : "false");
			transform("transform_track", s.transformTrack);
			transform("transform_follow_move", s.transformFollowMove);
			transform("transform_look_at", s.transformLookAt);
			out << "\n  }\n}\n";
			out.close();
			if (!out)
			{
				cameraNotice = "镜头设置保存失败：无法写入文件";
				return false;
			}
			std::filesystem::copy_file(MotionPathFromUtf8(temporary), MotionPathFromUtf8(path), std::filesystem::copy_options::overwrite_existing, ec);
			if (ec)
			{
				cameraNotice = "镜头设置保存失败: " + ec.message();
				return false;
			}
			std::filesystem::remove(MotionPathFromUtf8(temporary), ec);
			cameraNotice = "镜头设置已保存，下次启动自动载入";
			return true;
		}

		void ResetSoleObservation()
		{
			solePitchSnapshotPending = false;
			experimentalSolePitchSettings.experimentalSolePitchCorrection = false;
			experimentalSolePitchSnapshot = {};
			experimentalSolePitchConsumedSerial = 0;
			experimentalSolePitchConsumedActor = nullptr;
			soleSnapshot = {};
			soleObservedActor = nullptr;
			++soleCalibrationGeneration;
			// A saved shape remains a draft when the actor/scene changes. Neither the
			// costume identity nor the physical floor is inferred from actor pivot.
			soleSettings.shoeCalibrated = false;
			soleSettings.planeCalibrated = false;
		}

		// A request is fulfilled only by fresh original Foot writes and current
		// target measurements in this callback. GUI rendering never reads Unity.
		void WriteRequestedSolePitchSnapshot(void* actor)
		{
			if (!solePitchSnapshotPending || configRoot.empty() || !actor ||
				player.state == PlaybackState::Stopped || soleSnapshot.actor != actor ||
				!soleSnapshot.available || soleSnapshot.clipTime != player.currentTime)
				return;
			auto& controller = player.controller;
			const auto& stamp = controller.footPoseWriteStamp;
			if (!controller.CanDriveActor(actor) || stamp.actor != actor || (stamp.mask & 3u) != 3u ||
				!stamp.transforms[0] || !stamp.transforms[1] || !controller.fnGetWorldPosition ||
				!controller.fnGetWorldRotation)
				return;
			if (experimentalSolePitchSettings.experimentalSolePitchCorrection &&
				(experimentalSolePitchSnapshot.actor != actor || experimentalSolePitchSnapshot.poseSerial != stamp.
					serial ||
					experimentalSolePitchSnapshot.clipTime != player.currentTime))
				return;
			std::array<SoleFootMeasurement, 2> after;
			for (int side = 0; side < 2; ++side)
			{
				auto pose = soleSnapshot.feet[side].pose;
				float p[3]{}, q[4]{};
				controller.fnGetWorldPosition(stamp.transforms[side], p);
				controller.fnGetWorldRotation(stamp.transforms[side], q);
				pose.ankle = {p[0], p[1], p[2]};
				pose.footRotation = {q[0], q[1], q[2], q[3]};
				after[side] = MeasureSole(pose, soleSettings, side);
				if (!after[side].valid) return;
			}
			auto number = [](double v) { return std::isfinite(v) ? jsonlite::FormatNumber(v, 9) : "null"; };
			auto boolean = [](bool v) { return v ? "true" : "false"; };
			auto vector = [&](const Vector3& v)
			{
				return "[" + number(v.x) + "," + number(v.y) + "," + number(v.z) + "]";
			};
			auto rotation = [&](const Quaternion& q)
			{
				return "[" + number(q.x) + "," + number(q.y) + "," + number(q.z) + "," + number(q.w) + "]";
			};
			auto measurement = [&](const SoleFootMeasurement& foot)
			{
				std::string out = "{\"valid\":" + std::string(boolean(foot.valid)) + ",\"minimum_distance_m\":" +
					number(foot.minimumDistanceM) + ",\"maximum_penetration_m\":" + number(foot.maximumPenetrationM) +
					",\"ankle_world_m\":" + vector(foot.pose.ankle) + ",\"foot_world_quaternion_xyzw\":" + rotation(
						foot.pose.footRotation) +
					",\"proxy_corner_world_m\":[";
				for (int patch = 0; patch < 2; ++patch)
					for (int corner = 0; corner < 4; ++corner)
					{
						if (patch || corner) out += ",";
						out += vector(foot.patches[patch].corners[corner].world);
					}
				return out + "]}";
			};
			char actorId[40]{};
			std::snprintf(actorId, sizeof(actorId), "%p", actor);
			std::string content = "{\"schema_version\":1,\"command_seq\":" + std::to_string(solePitchSnapshotCommandSeq)
				+
				",\"actor_identity\":\"" + actorId + "\",\"state\":\"" + PlaybackStateName(player.state) +
				"\",\"motion\":\"" + jsonlite::EscapeString(CurrentMotion() ? CurrentMotion()->name : "") +
				"\",\"motion_name\":\"" + jsonlite::EscapeString(CurrentMotion() ? CurrentMotion()->name : "") +
				"\",\"motion_dir\":\"" + jsonlite::EscapeString(player.currentMotionDir) +
				"\",\"clip_time_s\":" + number(player.currentTime) + ",\"pose_serial\":" + std::to_string(stamp.serial)
				+
				",\"initial_position_offset_m\":" + vector(controller.initialPositionOffset) +
				",\"plane_normal\":" + vector(soleSettings.planeNormal) + ",\"plane_offset_m\":" + number(
					soleSettings.planeOffsetM) +
				",\"plane_calibrated\":" + boolean(soleSettings.planeCalibrated) +
				",\"use_boot_proxy\":" + boolean(soleSettings.useBootProxy) +
				",\"experimental_enabled\":" + boolean(experimentalSolePitchSettings.experimentalSolePitchCorrection) +
				",\"use_mesh_envelope\":" + boolean(experimentalSolePitchSettings.useMeshEnvelope) +
				",\"max_pitch_degrees\":" + number(experimentalSolePitchSettings.maxPitchDegrees) + ",\"feet\":[";
			for (int side = 0; side < 2; ++side)
			{
				if (side) content += ",";
				const auto& trace = experimentalSolePitchSnapshot.feet[side];
				content += "{\"side\":\"" + std::string(side ? "right" : "left") + "\",\"before\":" + measurement(
						soleSnapshot.feet[side]) +
					",\"after\":" + measurement(after[side]) + ",\"solver_valid\":" + boolean(trace.solution.valid) +
					",\"fresh_pose\":" + boolean(trace.freshPose) + ",\"pitch_degrees\":" + number(
						trace.solution.pitchDegrees) +
					",\"applied\":" + boolean(trace.applied) + ",\"setter_readback_matches\":" + boolean(
						trace.setterReadbackMatches) +
					",\"solver_used_mesh_envelope\":" + boolean(trace.solution.usedMeshEnvelope) +
					",\"envelope_readback_valid\":" + boolean(trace.measuredAfterValid) +
					",\"envelope_after_minimum_distance_m\":" + (trace.measuredAfterValid
						                                             ? number(trace.measuredAfterMinimumDistanceM)
						                                             : "null") +
					",\"status\":\"" + jsonlite::EscapeString(trace.status ? trace.status : "off") + "\"}";
			}
			content += "]}\n";
			std::error_code ec;
			std::filesystem::create_directories(MotionPathFromUtf8(diagnosticsRoot), ec);
			if (ec) return;
			const auto path = diagnosticsRoot + "/sole_pitch_snapshot.json", temporary = path + ".tmp";
			{
				std::ofstream out(MotionPathFromUtf8(temporary), std::ios::binary | std::ios::trunc);
				out << content;
				out.flush();
				if (!out) return;
			}
			std::filesystem::copy_file(MotionPathFromUtf8(temporary), MotionPathFromUtf8(path), std::filesystem::copy_options::overwrite_existing, ec);
			if (ec) return;
			std::filesystem::remove(MotionPathFromUtf8(temporary), ec);
			solePitchSnapshotPending = false;
			LogInfo("[GakuMotion][SolePitchCheck] snapshot saved seq=" + std::to_string(solePitchSnapshotCommandSeq));
		}

		void ObserveSoles(void* actor)
		{
			if (!soleSettings.enabled)
			{
				soleSnapshot = {};
				return;
			}
			auto& controller = player.controller;
			if (!actor || !controller.CanDriveActor(actor))
			{
				if (actor == soleObservedActor) soleSnapshot = {};
				return;
			}
			void* selected = controller.targetActor;
			if (!selected) selected = sceneActors.empty() ? actor : sceneActors.front();
			if (actor != selected) return;
			if (soleObservedActor && soleObservedActor != actor) ResetSoleObservation();
			soleObservedActor = actor;
			soleSnapshot = {};
			soleSnapshot.actor = actor;
			soleSnapshot.clipTime = player.currentTime;
			if (!controller.fnGetBoneTransform || !controller.fnGetWorldPosition || !controller.fnGetWorldRotation)
				return;
			const int hids[2][4] = {{1, 3, 5, 19}, {2, 4, 6, 20}};
			for (int side = 0; side < 2; ++side)
			{
				SoleFootPose pose;
				Vector3* positions[] = {&pose.hip, &pose.knee, &pose.ankle, &pose.toe};
				bool valid = true;
				for (int i = 0; i < 4; ++i)
				{
					void* transform = controller.fnGetBoneTransform(actor, hids[side][i]);
					if (!transform)
					{
						valid = false;
						break;
					}
					float p[3]{};
					controller.fnGetWorldPosition(transform, p);
					*positions[i] = {p[0], p[1], p[2]};
					if (i == 2)
					{
						float q[4]{};
						controller.fnGetWorldRotation(transform, q);
						pose.footRotation = {q[0], q[1], q[2], q[3]};
					}
				}
				pose.valid = valid;
				soleSnapshot.feet[side] = MeasureSole(pose, soleSettings, side);
			}
			soleSnapshot.available = soleSnapshot.feet[0].valid && soleSnapshot.feet[1].valid;
			soleSnapshot.calibrated = soleSnapshot.available && soleSettings.planeCalibrated && soleSettings.
				shoeCalibrated;
		}

		void ApplyExperimentalSolePitch(void* actor)
		{
			if (!experimentalSolePitchSettings.experimentalSolePitchCorrection)
			{
				experimentalSolePitchSnapshot = {};
				return;
			}
			auto& controller = player.controller;
			if (soleSnapshot.actor != actor) return; // Other actors never replace target telemetry.
			const auto& stamp = controller.footPoseWriteStamp;
			if (stamp.actor == actor && stamp.mask && experimentalSolePitchConsumedSerial == stamp.serial &&
				experimentalSolePitchConsumedActor == actor)
				return;
			if (stamp.actor == actor && stamp.mask)
			{
				experimentalSolePitchConsumedSerial = stamp.serial;
				experimentalSolePitchConsumedActor = actor;
			}
			experimentalSolePitchSnapshot = {};
			experimentalSolePitchSnapshot.actor = actor;
			experimentalSolePitchSnapshot.clipTime = player.currentTime;
			experimentalSolePitchSnapshot.poseSerial = stamp.serial;
			for (int side = 0; side < 2; ++side)
			{
				auto& trace = experimentalSolePitchSnapshot.feet[side];
				trace.status = "等待本回调原动作 Foot 写入";
				if (player.state == PlaybackState::Stopped || !actor || !controller.CanDriveActor(actor) ||
					stamp.actor != actor || !(stamp.mask & (1U << side)) || !stamp.transforms[side])
					continue;
				trace.freshPose = true;
				if (!controller.fnGetLocalRotation || !controller.fnSetLocalRotation || !controller.fnGetWorldRotation)
				{
					trace.status = "Foot rotation API 不可用";
					continue;
				}
				const auto& original = soleSnapshot.feet[side];
				trace.solution = sole_pitch::Solve(original.pose, soleSettings, side, experimentalSolePitchSettings);
				if (!trace.solution.valid)
				{
					trace.status = "代理 / 已校准水平地面 / 姿态条件不满足";
					continue;
				}
				trace.after = original;
				trace.measuredAfterMinimumDistanceM = trace.solution.beforeMinimumDistanceM;
				trace.measuredAfterValid = true;
				if (!trace.solution.actionable)
				{
					trace.status = trace.solution.fullyCleared ? "无需修正" : "角限内纯 pitch 无法改善";
					continue;
				}
				float local[4]{};
				controller.fnGetLocalRotation(stamp.transforms[side], local);
				const Quaternion base(local[0], local[1], local[2], local[3]);
				if (!sole::ValidRotation(base))
				{
					trace.status = "原动作 Foot local rotation 无效";
					continue;
				}
				const Quaternion output = (base.Normalized() * trace.solution.localDelta).Normalized();
				controller.fnSetLocalRotation(stamp.transforms[side], output.x, output.y, output.z, output.w);
				trace.applied = true;
				float readback[4]{}, world[4]{};
				controller.fnGetLocalRotation(stamp.transforms[side], readback);
				controller.fnGetWorldRotation(stamp.transforms[side], world);
				const Quaternion read(readback[0], readback[1], readback[2], readback[3]);
				auto sameRotation = [](const Quaternion& a, const Quaternion& b)
				{
					const auto q = a.Normalized(), p = b.Normalized();
					const float minus = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) + (q.z - p.z) * (q.z - p.
						z) + (q.w - p.w) * (q.w - p.w);
					const float plus = (q.x + p.x) * (q.x + p.x) + (q.y + p.y) * (q.y + p.y) + (q.z + p.z) * (q.z + p.z)
						+ (q.w + p.w) * (q.w + p.w);
					return (std::min)(minus, plus) < 1e-10f;
				};
				const Quaternion worldRead(world[0], world[1], world[2], world[3]);
				trace.setterReadbackMatches = sole::ValidRotation(read) && sameRotation(read, output) &&
					sole::ValidRotation(worldRead) && sameRotation(worldRead, trace.solution.correctedWorldRotation);
				auto correctedPose = original.pose;
				correctedPose.footRotation = {world[0], world[1], world[2], world[3]};
				trace.after = MeasureSole(correctedPose, soleSettings, side);
				trace.measuredAfterMinimumDistanceM = sole_pitch::MinimumDistance(
					correctedPose, soleSettings, side, experimentalSolePitchSettings);
				trace.measuredAfterValid = trace.measuredAfterMinimumDistanceM != -(std::numeric_limits<float>::max)();
				trace.status = trace.setterReadbackMatches && trace.after.valid ? "已施加实验 pitch" : "已尝试 setter，读回条件未满足";
			}
		}

		bool LoadSoleSettings()
		{
			const auto path = configRoot + "/sole_measurement.json";
			if (!std::filesystem::exists(MotionPathFromUtf8(path))) return false;
			jsonlite::Value root;
			std::string error;
			SoleMeasurementSettings next;
			if (!jsonlite::ParseFile(path, root, error) || !ParseSoleSettings(root, next, error))
			{
				soleNotice = "鞋底测量载入失败: " + error;
				return false;
			}
			// World floor and costume must be confirmed in this session; saved point
			// vectors are retained, but previous calibration is not scene detection.
			next.planeCalibrated = false;
			next.shoeCalibrated = false;
			SetSoleSettings(next);
			soleNotice = next.useBootProxy ? "已载入培育靴粗略代理；请核对本场景地面和代理范围" : "已载入代理点；请重新确认本场景地面和靴子";
			return true;
		}

		bool SaveSoleSettings()
		{
			const auto content = SerializeSoleSettings(soleSettings);
			if (content.empty())
			{
				soleNotice = "鞋底测量设置无效，无法保存";
				return false;
			}
			std::error_code ec;
			std::filesystem::create_directories(MotionPathFromUtf8(configRoot), ec);
			if (ec)
			{
				soleNotice = "鞋底测量保存失败: " + ec.message();
				return false;
			}
			const auto path = configRoot + "/sole_measurement.json", temporary = path + ".tmp";
			std::ofstream out(MotionPathFromUtf8(temporary), std::ios::binary | std::ios::trunc);
			out << content;
			out.close();
			if (!out)
			{
				soleNotice = "鞋底测量保存失败：无法写入文件";
				return false;
			}
			std::filesystem::copy_file(MotionPathFromUtf8(temporary), MotionPathFromUtf8(path), std::filesystem::copy_options::overwrite_existing, ec);
			if (ec)
			{
				soleNotice = "鞋底测量保存失败: " + ec.message();
				return false;
			}
			std::filesystem::remove(MotionPathFromUtf8(temporary), ec);
			soleNotice = "鞋底测量设置已保存";
			return true;
		}

		bool LoadInitialPosition()
		{
			std::error_code ec;
			if (!std::filesystem::exists(MotionPathFromUtf8(initialPositionPath), ec)) return false;
			jsonlite::Value root;
			std::string error;
			if (!jsonlite::ParseFile(initialPositionPath, root, error))
			{
				initialPositionNotice = "初始位置载入失败: " + error;
				return false;
			}
			const auto* values = root.Find("offset_m");
			if (root.IntPath("version", 0) != 1 || root.StringPath("space", "") != "world" ||
				!values || !values->IsArray() || values->Size() != 3 ||
				!values->At(0)->IsNumber() || !values->At(1)->IsNumber() || !values->At(2)->IsNumber() ||
				!player.controller.SetInitialPositionOffset(
					Vector3(values->At(0)->AsFloat(0), values->At(1)->AsFloat(0), values->At(2)->AsFloat(0))))
			{
				initialPositionNotice = "初始位置文件无效：需要 world 坐标下的三个有限数值";
				return false;
			}
			initialPositionNotice = "已载入保存的初始位置";
			return true;
		}

		bool SaveInitialPosition()
		{
			const auto offset = player.controller.initialPositionOffset;
			if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z))
			{
				initialPositionNotice = "初始位置保存失败：坐标必须为有限数值";
				return false;
			}
			std::error_code ec;
			std::filesystem::create_directories(MotionPathFromUtf8(configRoot), ec);
			if (ec)
			{
				initialPositionNotice = "初始位置保存失败: " + ec.message();
				return false;
			}
			const auto temporary = initialPositionPath + ".tmp";
			std::ofstream out(MotionPathFromUtf8(temporary), std::ios::binary | std::ios::trunc);
			out << "{\n  \"version\": 1,\n  \"space\": \"world\",\n  \"offset_m\": ["
				<< jsonlite::FormatNumber(offset.x, 6) << ", " << jsonlite::FormatNumber(offset.y, 6) << ", "
				<< jsonlite::FormatNumber(offset.z, 6) << "]\n}\n";
			out.close();
			if (!out)
			{
				initialPositionNotice = "初始位置保存失败：无法写入文件";
				return false;
			}
			std::filesystem::copy_file(MotionPathFromUtf8(temporary), MotionPathFromUtf8(initialPositionPath),
			                           std::filesystem::copy_options::overwrite_existing, ec);
			if (ec)
			{
				initialPositionNotice = "初始位置保存失败: " + ec.message();
				return false;
			}
			std::filesystem::remove(MotionPathFromUtf8(temporary), ec);
			initialPositionNotice = "初始位置已保存，下次启动自动载入";
			return true;
		}

		// 曲目目录若携带 aux_replay.json（官方实机录制的辅助骨逐帧局部四元数）则挂载为真值源。
		// 文件不存在属正常情况，不报错、不刷日志噪音。
		void TryLoadAuxGroundTruth(const std::string& motionDirectory)
		{
			std::error_code ec;
			const std::string auxPath = motionDirectory + "/aux_replay.json";
			if (!std::filesystem::exists(MotionPathFromUtf8(auxPath), ec) || ec) return;
			player.controller.LoadAuxGroundTruth(auxPath);
		}

		bool SelectMotionByIndex(int index, bool autoPlay = false, bool forcePreview = false)
		{
			if (index < 0 || index >= static_cast<int>(motions.size()))
			{
				LogWarn(detail::FormatMsg("[GakuMotion][Config] motion index out of range: %d", index));
				return false;
			}

			const MotionEntry& entry = motions[index];
			const bool wasPlaying = (player.state == PlaybackState::Playing);
			const bool wasPaused = (player.state == PlaybackState::Paused);

			// 切曲前先干净落停：停止音频、恢复官方 Animator，避免残留姿态串台。
			// 注意 LoadMotion 内部会把状态置为 Preview（静止保持第 0 帧），
			// 因此「选中但未播放」不再退回官方待机造型。
			if (player.state != PlaybackState::Stopped)
			{
				player.Stop();
				NotifyMotionPlaying(false);
			}

			if (!player.LoadMotion(entry.dir))
			{
				LogWarn("[GakuMotion][Config] failed to load motion: " + entry.dir);
				return false;
			}

			currentMotionIndex = index;
			loadedMotionKey = entry.name;
			loadedMotionEntry = entry;
			loadedMotionVersion = ReadMotionFileVersion(entry.dir);
			loadedSourceMissing = loadedSourceChanged = false;
			++loadedMotionGeneration;
			camera.LoadTrack(player.currentMotionDir, player.clip);
			player.cameraSourceDuration = camera.track.frames.empty() ? 0.0f :
				static_cast<float>(camera.track.frames.size()) / camera.track.fps;
			LoadOverrides(false); // 曲目级覆盖随曲目切换

			// 若本曲目目录自带辅助骨官方真值（aux_replay.json）则顺带挂载，供 mode=3 直接对照
			TryLoadAuxGroundTruth(entry.dir);

			LogInfo(detail::FormatMsg("[GakuMotion][Config] selected: %s | %s | overrides: %s",
			                          entry.name.c_str(),
			                          entry.DisplayLabel().c_str(),
			                          effectiveOverrides.Describe().c_str()));

			if (!forcePreview && (autoPlay || wasPlaying || config.autoPlayOnSelect))
			{
				player.Play();
				NotifyMotionPlaying(true);
			}
			else if (wasPaused && !forcePreview)
			{
				// 暂停中切曲：保持暂停语义（停在第 0 帧），不自动恢复播放
				player.state = PlaybackState::Paused; // No audio started for this new clip yet.
				NotifyMotionPlaying(false);
			}
			else
			{
				// Preview：已载入、静止保持第 0 帧姿态，不播音频。
				LogInfo(detail::FormatMsg(
					"[GakuMotion][Config] preview frame 0 of '%s' (loaded, not playing)", entry.name.c_str()));
			}
			return true;
		}

		// 上一曲 / 下一曲（delta = ±1）。若当前正在播放，则无缝续播新曲目。
		// source 仅用于诊断日志（"hotkey" / "gui" / "config"），便于实机定位
		// 「谁触发了切换」这类问题（参见 handoff：暂停后幽灵切换）。
		bool CycleMotion(int delta, const char* source = "unknown")
		{
			if (motions.empty())
			{
				LogWarn("[GakuMotion][Config] motion library is empty; cannot switch");
				return false;
			}
			const bool wasPlaying = (player.state == PlaybackState::Playing);
			int index = currentMotionIndex;
			if (index < 0)
			{
				index = (delta >= 0) ? 0 : static_cast<int>(motions.size()) - 1;
			}
			else
			{
				const int count = static_cast<int>(motions.size());
				index = ((index + delta) % count + count) % count;
			}
			const bool ok = SelectMotionByIndex(index, wasPlaying);
			if (ok)
			{
				LogInfo(detail::FormatMsg("[GakuMotion][Config] switch %s -> %s (source=%s)",
				                          delta > 0 ? "next" : "prev",
				                          motions[index].name.c_str(),
				                          source ? source : "unknown"));
			}
			return ok;
		}

		const MotionEntry* CurrentMotion() const
		{
			if (currentMotionIndex < 0 || currentMotionIndex >= static_cast<int>(motions.size()))
				return loadedMotionKey.empty() ? nullptr : &loadedMotionEntry;
			return &motions[currentMotionIndex];
		}

		// =========================================================================
		// 声明式配置应用
		// =========================================================================
		void ApplyDeclarativeConfig()
		{
			if (config.secondaryPhysicsBackend != 2) config.beforeQuartzExperiment = false;
			guiPhysicsBackend.store(config.secondaryPhysicsBackend);
			guiBeforeQuartzExperiment.store(config.beforeQuartzExperiment);
			if (config.cameraDeclared && (!cameraConfigApplied || !(config.camera == appliedCamera)))
			{
				camera.settings = config.camera;
				appliedCamera = config.camera;
				cameraConfigApplied = true;
				camera.Publish();
			}
			// ---- 1. 选曲（只在「文件里的值发生变化」时动作，避免快捷键切歌被旧值拉回） ----
			if (!config.selectedMotion.empty() &&
				config.selectedMotion != applied.selectedMotion)
			{
				const int idx = FindMotionIndex(motions, config.selectedMotion);
				if (idx >= 0)
				{
					SelectMotionByIndex(idx, false);
					applied.selectedMotion = config.selectedMotion;
				}
				else
				{
					LogWarn("[GakuMotion][Config] selected_motion not found: " + config.selectedMotion);
				}
			}

			// ---- 2. 视线 ----
			bool gazeChanged = false;
			if (config.gazeMode >= 0 && config.gazeMode != applied.gazeMode)
			{
				lookAt.gazeMode.store(static_cast<GazeMode>(config.gazeMode));
				applied.gazeMode = config.gazeMode;
				gazeChanged = true;
			}
			if (config.desktopLookAtEnabled >= 0 && config.desktopLookAtEnabled != applied.desktopLookAt)
			{
				lookAt.desktopLookAtEnabled.store(config.desktopLookAtEnabled != 0);
				applied.desktopLookAt = config.desktopLookAtEnabled;
				gazeChanged = true;
			}
			if (config.eyeWeight >= 0.0f && config.eyeWeight != applied.eyeWeight)
			{
				lookAt.eyeWeight.store(config.eyeWeight);
				applied.eyeWeight = config.eyeWeight;
				gazeChanged = true;
			}
			if (config.headWeight >= 0.0f && config.headWeight != applied.headWeight)
			{
				lookAt.headWeight.store(config.headWeight);
				applied.headWeight = config.headWeight;
				gazeChanged = true;
			}
			if (config.bodyWeight >= 0.0f && config.bodyWeight != applied.bodyWeight)
			{
				lookAt.bodyWeight.store(config.bodyWeight);
				applied.bodyWeight = config.bodyWeight;
				gazeChanged = true;
			}
			if (gazeChanged)
			{
				LogInfo(detail::FormatMsg(
					"[GakuMotion][Gaze] mode=%s desktop=%s eye=%.2f head=%.2f body=%.2f",
					GazeModeName(static_cast<int>(lookAt.gazeMode.load())),
					lookAt.desktopLookAtEnabled.load() ? "on" : "off",
					lookAt.eyeWeight.load(), lookAt.headWeight.load(), lookAt.bodyWeight.load()));
			}

			// ---- 3. 静息姿态相对校准 ----
			if (config.useRestRotations >= 0 && config.useRestRotations != applied.useRestRotations)
			{
				player.controller.useRestRotations = (config.useRestRotations != 0);
				applied.useRestRotations = config.useRestRotations;
				LogInfo(detail::FormatMsg("[GakuMotion][Config] use_rest_rotations=%s (applies on next bind)",
				                          player.controller.useRestRotations ? "true" : "false"));
			}

			// ---- 3b. gkclip 腿部静息基准（2026-10-07 A/B 实验）----
			if (config.legRestBasis >= 0 && config.legRestBasis != applied.legRestBasis)
			{
				player.controller.legRestBasis = (config.legRestBasis != 0);
				applied.legRestBasis = config.legRestBasis;
				LogInfo(detail::FormatMsg("[GakuMotion][Config] leg_rest_basis=%s",
				                          player.controller.legRestBasis ? "ON" : "off"));
			}

			// ---- 3b-2. gkclip 眼骨几何相对缩放（#150 §8）----
			// Gain comes from the measured per-character table, never from this file;
			// the switch only chooses between the table and the raw 1:1 control.
			if (config.eyePoseRelativeGain >= 0 && config.eyePoseRelativeGain != applied.eyePoseRelativeGain)
			{
				player.controller.eyePoseRelativeGain = (config.eyePoseRelativeGain != 0);
				applied.eyePoseRelativeGain = config.eyePoseRelativeGain;
				LogInfo(detail::FormatMsg("[GakuMotion][Config] eye_pose_relative_gain=%s",
				                          player.controller.eyePoseRelativeGain ? "geometry-relative" : "raw 1:1"));
			}

			// ---- 3c. 辅助骨驱动模式（实机靶向对照实验）----
			const int preserveAnimator = config.secondaryPhysicsBackend == 2
				                             ? 1
				                             : (config.preserveAnimatorForSecondaryPhysics >= 0
					                                ? config.preserveAnimatorForSecondaryPhysics
					                                : 0);
			if (preserveAnimator >= 0 && preserveAnimator != applied.preserveAnimatorForSecondaryPhysics)
			{
				applied.preserveAnimatorForSecondaryPhysics = preserveAnimator;
				player.controller.preserveAnimatorForSecondaryPhysics =
					preserveAnimator != 0;
				LogInfo(detail::FormatMsg(
					"[GakuMotion][SecondaryPhysics] preserve_animator=%s (experimental; device validation required)",
					player.controller.preserveAnimatorForSecondaryPhysics ? "ON" : "off"));
			}

			// ---- Auxiliary bone drive mode ----
			if (config.auxMode >= 0 && config.auxMode != applied.auxMode)
			{
				applied.auxMode = config.auxMode;
				player.controller.SetAuxDriveMode(config.auxMode);
			}
			if (config.experimentalWaistSupport >= 0 &&
				config.experimentalWaistSupport != applied.experimentalWaistSupport)
			{
				player.controller.SetExperimentalWaistSupportEnabled(config.experimentalWaistSupport != 0);
				applied.experimentalWaistSupport = config.experimentalWaistSupport;
				LogInfo(std::string("[GakuMotion][WaistSupport] experimental ") +
					(config.experimentalWaistSupport
						 ? "ON (scope-specific acceptance passed; broader scope unverified)"
						 : "off"));
			}
			guiExperimentalWaistSupport.store(player.controller.IsExperimentalWaistSupportEnabled());

			// ---- Character facial default prior (adapter v2) ----
			if (config.facialMikuMouthFit >= 0 && config.facialMikuMouthFit != applied.facialMikuMouthFit)
			{
				applied.facialMikuMouthFit = config.facialMikuMouthFit;
				player.controller.facialMikuMouthFit = config.facialMikuMouthFit != 0;
				for (auto& pair : player.controller.bindings)
					pair.second.face.enableMikuMouthFit = player.controller.facialMikuMouthFit;
				player.controller.currentBinding.face.enableMikuMouthFit = player.controller.facialMikuMouthFit;
			}
			if (config.facialEnableBaseline >= 0 && config.facialEnableBaseline != applied.facialEnableBaseline)
			{
				applied.facialEnableBaseline = config.facialEnableBaseline;
				player.controller.facialEnableBaseline = (config.facialEnableBaseline != 0);
				for (auto& pair : player.controller.bindings)
				{
					pair.second.face.enableBaseline = player.controller.facialEnableBaseline;
				}
				player.controller.currentBinding.face.enableBaseline = player.controller.facialEnableBaseline;
			}
			if (config.facialBaselineMultiplier >= 0.0f && config.facialBaselineMultiplier != applied.
				facialBaselineMultiplier)
			{
				applied.facialBaselineMultiplier = config.facialBaselineMultiplier;
				player.controller.facialBaselineMultiplier = config.facialBaselineMultiplier;
				for (auto& pair : player.controller.bindings)
				{
					pair.second.face.baselineMultiplier = player.controller.facialBaselineMultiplier;
				}
				player.controller.currentBinding.face.baselineMultiplier = player.controller.facialBaselineMultiplier;
			}
			// Facial identity belongs to the bound actor, never an old manual override.
			if (player.controller.facialCharacterOverride != "auto" || applied.facialCharacterOverride != "auto")
			{
				applied.facialCharacterOverride = "auto";
				player.controller.facialCharacterOverride = "auto";
				for (auto& pair : player.controller.bindings) pair.second.face.characterOverride = "auto";
				player.controller.currentBinding.face.characterOverride = "auto";
			}

			// ---- 3b. Global transport rate (multiplied by each file's own speed). ----
			if (config.playbackSpeedMultiplier > 0.0f && config.playbackSpeedMultiplier != applied.playbackSpeed)
			{
				applied.playbackSpeed = config.playbackSpeedMultiplier;
				player.SetPlaybackSpeed(config.playbackSpeedMultiplier);
				LogInfo(detail::FormatMsg("[GakuMotion][Config] playback_speed_multiplier=%.3f",
				                          player.playbackSpeedMultiplier));
			}

			// ---- 4. 骨骼覆盖：总开关与可选路径 ----
			if (config.overridesEnabled >= 0 && config.overridesEnabled != applied.overridesEnabled)
			{
				masterOverridesEnabled = (config.overridesEnabled != 0);
				applied.overridesEnabled = config.overridesEnabled;
				RebuildEffectiveOverrides();
			}
			if (!config.overrideGlobalFile.empty() && config.overrideGlobalFile != applied.globalFile)
			{
				applied.globalFile = config.overrideGlobalFile;
				globalOverridePath = ResolvePath(config.overrideGlobalFile, globalOverridePath);
				LoadOverrides(true);
			}
			if (config.overrideMotionFile != applied.motionFile)
			{
				applied.motionFile = config.overrideMotionFile;
				LoadOverrides(true);
			}
		}

		// =========================================================================
		// 一次性命令
		// =========================================================================
		void ConsumeConfigCommands()
		{
			if (!config.HasPendingCommands()) return;
			if (!InjectionAllowed() || ((config.cmdPlay || config.cmdSeek || config.cmdSolePitchCheck) &&
				!PrepareInjection())) { config.ClearCommands(); return; }
			if (config.cmdBurstTrace) burstTraceRequestedSeq = config.appliedCommandSeq;

			if (config.cmdSolePitchCheck)
			{
				const auto& command = config.cmdSolePitchSettings;
				auto offset = player.controller.initialPositionOffset;
				offset.y = command.initialYM;
				auto settings = soleSettings;
				settings.enabled = true;
				settings.useBootProxy = true;
				if (command.hasPlane)
				{
					settings.planeNormal = {0, 1, 0};
					settings.planeOffsetM = command.planeOffsetM;
					settings.planeCalibrated = true;
				}
				if (settings.IsValid() && sole::Finite(offset) && player.controller.SetInitialPositionOffset(offset))
				{
					SetSoleSettings(settings);
					soleObservedActor = nullptr; // The explicit check starts a new observation on the current target.
					experimentalSolePitchSettings = {command.enabled, command.maxPitchDegrees};
					experimentalSolePitchSettings.useMeshEnvelope = command.useMeshEnvelope;
					experimentalSolePitchSnapshot = {};
					experimentalSolePitchConsumedSerial = 0;
					experimentalSolePitchConsumedActor = nullptr;
					LogInfo("[GakuMotion][SolePitchCheck] applied seq=" + std::to_string(config.appliedCommandSeq));
				}
				else LogWarn("[GakuMotion][SolePitchCheck] rejected runtime settings; experiment unchanged");
			}
			if (config.cmdSolePitchSnapshot)
			{
				solePitchSnapshotCommandSeq = config.appliedCommandSeq;
				solePitchSnapshotPending = true;
			}

			if (config.cmdPlay)
			{
				stageStoppedByTransport = false;
				player.Play();
				NotifyMotionPlaying(true);
				LogInfo("[GakuMotion][Config] command: play");
			}
			if (config.cmdPause)
			{
				player.Pause();
				NotifyMotionPlaying(false);
				LogInfo("[GakuMotion][Config] command: pause");
			}
			if (config.cmdStop)
			{
				StopPerformance();
				NotifyMotionPlaying(false);
				LogInfo("[GakuMotion][Config] command: stop");
			}
			if (config.cmdSeek)
			{
				player.Seek(config.cmdSeekSeconds);
				LogInfo(detail::FormatMsg("[GakuMotion][Config] command: seek %.2fs", config.cmdSeekSeconds));
			}
			if (config.cmdReloadOverrides)
			{
				LoadOverrides(true);
				LogInfo("[GakuMotion][Config] command: reload_overrides");
			}
			if (config.cmdToggleOverrides)
			{
				ToggleOverrides();
				LogInfo("[GakuMotion][Config] command: toggle_overrides");
			}
			config.ClearCommands();
		}

		// =========================================================================
		// 骨骼覆盖
		// =========================================================================
		std::string CurrentMotionOverridePath() const
		{
			if (!config.overrideMotionFile.empty())
			{
				return ResolvePath(config.overrideMotionFile, std::string());
			}
			const MotionEntry* m = CurrentMotion();
			if (m) return m->dir + "/bone_overrides.json";
			if (!player.currentMotionDir.empty()) return player.currentMotionDir + "/bone_overrides.json";
			return std::string();
		}

		void LoadOverrides(bool verbose)
		{
			// 全局
			globalOverrides.LoadFromFile(globalOverridePath);
			if (!globalOverrides.lastError.empty())
			{
				LogWarn("[GakuMotion][Overrides] global override load failed: " + globalOverrides.lastError);
			}
			for (const auto& w : globalOverrides.warnings)
			{
				LogWarn("[GakuMotion][Overrides] " + w);
			}

			// 曲目级
			motionOverrides.Clear();
			const std::string motionPath = CurrentMotionOverridePath();
			if (!motionPath.empty() && std::filesystem::exists(MotionPathFromUtf8(motionPath)))
			{
				motionOverrides.LoadFromFile(motionPath);
				if (!motionOverrides.lastError.empty())
				{
					LogWarn("[GakuMotion][Overrides] per-motion override load failed: " + motionOverrides.lastError);
				}
				for (const auto& w : motionOverrides.warnings)
				{
					LogWarn("[GakuMotion][Overrides] " + w);
				}
			}

			RebuildEffectiveOverrides();
			if (verbose)
			{
				LogInfo(detail::FormatMsg(
					"[GakuMotion][Overrides] composed | global: %s | motion: %s | effective=%zu (master=%s)",
					globalOverrides.Describe().c_str(),
					motionPath.empty() ? "<none>" : motionOverrides.Describe().c_str(),
					effectiveOverrides.offsets.size(),
					masterOverridesEnabled ? "on" : "off"));
			}
		}

		void RebuildEffectiveOverrides()
		{
			BoneOverrideSet::BuildEffective(globalOverrides, motionOverrides,
			                                masterOverridesEnabled, effectiveOverrides);
			player.controller.SetBoneOverrides(&effectiveOverrides);
		}

		bool ToggleOverrides()
		{
			masterOverridesEnabled = !masterOverridesEnabled;
			RebuildEffectiveOverrides();
			LogInfo(detail::FormatMsg("[GakuMotion][Overrides] master switch -> %s (effective entries=%zu)",
			                          masterOverridesEnabled ? "ON" : "OFF",
			                          effectiveOverrides.offsets.size()));
			return masterOverridesEnabled;
		}

		// ---- Target actor --------------------------------------------------------
		// 宿主对每个 actor 分别回调；当前只允许一个已确认的实际目标。
		// 不向 GUI 暴露托管指针；发布实际名单和带生命周期的选择 token。
		// Called only from a fresh, pinned Unity-thread actor search in the owner scene.
		// Missing actors are abandoned without touching possibly destroyed objects.
		bool PublishSceneActors(const std::vector<SceneActorObservation>& observations)
		{
			const auto* old = sceneActorRegistry.Find(player.controller.targetActor);
			const uint64_t oldToken = old ? old->token : 0;
			sceneActorRegistry.Publish(observations);
			sceneActors.clear();
			for (const auto& item : sceneActorRegistry.Entries()) sceneActors.push_back(item.actor);
			const auto* selected = sceneActorRegistry.Find(player.controller.targetActor);
			const bool lost = player.controller.targetActor && (!selected || selected->token != oldToken);
			if (lost)
			{
				++injectionSession;
				camera.Release(false);
				player.AbandonScene();
				currentActor = nullptr;
				ResetSoleObservation();
				NotifyMotionPlaying(false);
			}
			if (!lost && !player.controller.targetActor && !sceneActors.empty())
			{
				void* bound = player.controller.currentBinding.actor;
				SetTargetActor(sceneActorRegistry.Find(bound) ? bound : sceneActors.front());
			}
			return lost;
		}

		// Kept for offline/legacy callers; production publishes fresh typed snapshots.
		void NoteActor(void* a)
		{
			if (!a || sceneActorRegistry.Find(a)) return;
			std::vector<SceneActorObservation> observations;
			for (const auto& item : sceneActorRegistry.Entries()) observations.push_back(item);
			observations.push_back({a, static_cast<int>(sceneActorRegistry.Entries().size() + 1), {}});
			PublishSceneActors(observations);
		}

		unsigned KnownActorCount() const { return static_cast<unsigned>(sceneActorRegistry.Entries().size()); }
		bool IsTargetActor(void* a) const { return a && player.controller.targetActor == a; }

		void SetTargetActor(void* a)
		{
			if (!a || !sceneActorRegistry.Find(a) || player.controller.targetActor == a) return;
			if (fnBeforeTargetChange && !fnBeforeTargetChange())
			{
				LogWarn("[GakuMotion][Target] selection rejected: secondary physics restore unconfirmed");
				return;
			}
			// Restore only a still-live old target, then release its cached body/face state.
			if (!player.controller.bindings.empty())
			{
				const auto restored = player.controller.RestoreForCapture();
				if (!restored.bodyConfirmed || !restored.faceConfirmed || !restored.gazeConfirmed)
				{
					LogWarn("[GakuMotion][Target] selection rejected: old actor restore unconfirmed");
					return;
				}
			}
			player.controller.DiscardBindings();
			camera.Release();
			++injectionSession;
			ResetSoleObservation();
			player.controller.SetTargetActor(a);
			LogDebug("[GakuMotion][Target] selected one current scene actor");
		}

		void CycleTargetActor()
		{
			if (sceneActors.empty()) return;
			const auto found = std::find(sceneActors.begin(), sceneActors.end(), player.controller.targetActor);
			const size_t next = found == sceneActors.end() ? 0 : (std::distance(sceneActors.begin(), found) + 1) % sceneActors.size();
			SetTargetActor(sceneActors[next]);
		}

		void PickBoundActorAsTarget()
		{
			void* bound = player.controller.currentBinding.actor;
			if (sceneActorRegistry.Find(bound)) SetTargetActor(bound);
			else if (!sceneActors.empty()) SetTargetActor(sceneActors.front());
		}

		// =========================================================================
		// 视线
		// =========================================================================
		int CycleGazeMode()
		{
			const int current = static_cast<int>(lookAt.gazeMode.load());
			const int next = (current + 1) % 3;
			lookAt.gazeMode.store(static_cast<GazeMode>(next));
			LogInfo(detail::FormatMsg("[GakuMotion][Gaze] mode: %s -> %s (VRifyLiveGaze=%s)",
			                          GazeModeName(current), GazeModeName(next),
			                          lookAt.IsVRifyLiveGazeActive() ? "active" : "inactive"));
			return next;
		}

		static const char* GazeModeName(int mode)
		{
			switch (mode)
			{
			case 0: return "Auto(defer to VRify)";
			case 1: return "ForceOff(pure VMD head)";
			case 2: return "DesktopOn(desktop look-at)";
			default: return "Unknown";
			}
		}

		// =========================================================================
		// 状态摘要（面板与日志共用）
		// =========================================================================
		std::string StatusSummary() const
		{
			const MotionEntry* m = CurrentMotion();
			// PROBE S1：把 scrub 累计计数与播放器侧时钟来源并入摘要。这样即使玩家
			// 没注意到拖动瞬间的日志，也能从 5 秒一次的状态行里看出
			// 「BEGIN/UPDATE/END 各发生了几次」以及「当前时钟由谁在推进」。
			return detail::FormatMsg(
				"motion=%s | state=%s | time=%.2f/%.2fs | bones=%zu/%zu | gaze=%s | overrides=%s(%zu) | seeks=%d corr=%.3fs | scrub=%d/%d/%d clk=%s | secondary=%s",
				m ? m->name.c_str() : "<none>",
				PlaybackStateName(player.state),
				player.currentTime,
				player.TimelineDuration(),
				currentActorBoundBones,
				currentActorTotalBones,
				GazeModeName(static_cast<int>(lookAt.gazeMode.load())),
				masterOverridesEnabled ? "ON" : "OFF",
				effectiveOverrides.offsets.size(),
				player.SeekCount(),
				player.AudioSyncOffsetSeconds(),
				gui.probeScrubBegin, gui.probeScrubUpdate, gui.probeScrubEnd,
				player.lastClockSource ? player.lastClockSource : "none",
				config.secondaryPhysicsBackend == 2
					? "native(optional; fallback on failure)"
					: config.secondaryPhysicsBackend == 1
					? "fallback"
					: "off");
		}

		static const char* PlaybackStateName(PlaybackState s)
		{
			switch (s)
			{
			case PlaybackState::Stopped: return "Stopped";
			case PlaybackState::Playing: return "Playing";
			case PlaybackState::Paused: return "Paused";
			case PlaybackState::Preview: return "Preview(f0)";
			}
			return "?";
		}

		void SetLiveBoneOverride(int hid, const char* name, float pitch, float yaw, float roll, bool world,
		                         bool enabled, bool symmetrical)
		{
			std::string bname = name ? name : "";
			if (bname.empty() && hid >= 0)
			{
				bname = BoneOverrideSet::GetBoneNameByHumanoidId(hid);
			}
			Vector3 euler(pitch, yaw, roll);

			auto applyOne = [&](int h, const std::string& n, const Vector3& e)
			{
				auto& eff = effectiveOverrides.GetOrAddOffset(h, n);
				eff.eulerDeg = e;
				eff.spaceWorld = world;
				eff.enabled = enabled;
				eff.useQuaternion = false;

				auto& mot = motionOverrides.GetOrAddOffset(h, n);
				mot.eulerDeg = e;
				mot.spaceWorld = world;
				mot.enabled = enabled;
				mot.useQuaternion = false;
			};

			applyOne(hid, bname, euler);

			if (symmetrical)
			{
				int symmHid = BoneOverrideSet::GetSymmetricalHumanoidId(hid);
				if (symmHid >= 0)
				{
					std::string symmName = BoneOverrideSet::GetBoneNameByHumanoidId(symmHid);
					Vector3 symmEuler(euler.x, -euler.y, -euler.z); // MMD mirror formula
					applyOne(symmHid, symmName, symmEuler);
				}
			}

			effectiveOverrides.enabled = true;
			effectiveOverrides.RebuildIndex();
			motionOverrides.enabled = true;
			motionOverrides.RebuildIndex();
			masterOverridesEnabled = true;
		}

		void ResetLiveBoneOverride(int hid, const char* name, bool symmetrical)
		{
			std::string bname = name ? name : "";
			effectiveOverrides.RemoveOffset(hid, bname);
			motionOverrides.RemoveOffset(hid, bname);

			if (symmetrical)
			{
				int symmHid = BoneOverrideSet::GetSymmetricalHumanoidId(hid);
				if (symmHid >= 0)
				{
					std::string symmName = BoneOverrideSet::GetBoneNameByHumanoidId(symmHid);
					effectiveOverrides.RemoveOffset(symmHid, symmName);
					motionOverrides.RemoveOffset(symmHid, symmName);
				}
			}
			effectiveOverrides.RebuildIndex();
			motionOverrides.RebuildIndex();
		}

		void ResetAllLiveOverrides()
		{
			effectiveOverrides.offsets.clear();
			motionOverrides.offsets.clear();
			effectiveOverrides.RebuildIndex();
			motionOverrides.RebuildIndex();
		}

		bool SaveLiveOverridesToMotion()
		{
			std::string targetFile = CurrentMotionOverridePath();
			if (targetFile.empty())
			{
				const MotionEntry* m = CurrentMotion();
				if (m) targetFile = m->dir + "/bone_overrides.json";
				else if (!player.currentMotionDir.empty()) targetFile = player.currentMotionDir +
					"/bone_overrides.json";
			}
			if (targetFile.empty()) return false;
			bool ok = effectiveOverrides.SaveToFile(targetFile);
			if (ok)
			{
				LogInfo("[GakuMotion] Saved bone overrides to: " + targetFile);
			}
			else
			{
				LogWarn("[GakuMotion] Failed to save bone overrides to: " + targetFile);
			}
			return ok;
		}

		bool SaveLiveOverridesToGlobal()
		{
			if (globalOverridePath.empty()) return false;
			bool ok = effectiveOverrides.SaveToFile(globalOverridePath);
			if (ok)
			{
				LogInfo("[GakuMotion] Saved global bone overrides to: " + globalOverridePath);
			}
			else
			{
				LogWarn("[GakuMotion] Failed to save global bone overrides to: " + globalOverridePath);
			}
			return ok;
		}

		bool GetLiveBoneOverride(int hid, const char* name, float* outPitch, float* outYaw, float* outRoll,
		                         bool* outWorld, bool* outEnabled) const
		{
			std::string bname = name ? name : "";
			const auto* o = effectiveOverrides.Find(hid, bname);
			if (o)
			{
				if (outPitch) *outPitch = o->eulerDeg.x;
				if (outYaw) *outYaw = o->eulerDeg.y;
				if (outRoll) *outRoll = o->eulerDeg.z;
				if (outWorld) *outWorld = o->spaceWorld;
				if (outEnabled) *outEnabled = o->enabled;
				return true;
			}
			if (outPitch) *outPitch = 0.0f;
			if (outYaw) *outYaw = 0.0f;
			if (outRoll) *outRoll = 0.0f;
			if (outWorld) *outWorld = false;
			if (outEnabled) *outEnabled = true;
			return false;
		}

		// =========================================================================
		// 面板接口（启用 ImGui 时由 GML Desktop UI capability 调用）
		// =========================================================================
#if defined(GKMOTION_ENABLE_IMGUI) || defined(HAS_IMGUI)
		void OnRenderGui(bool hostedPanel = false)
		{
			std::lock_guard<std::recursive_mutex> lock(stateMutex);
			gui.Draw(BuildGuiContext(), hostedPanel);
			guiTextInputActive.store(ImGui::GetIO().WantTextInput && !ImGui::GetIO().AppFocusLost, std::memory_order_relaxed);
		}
#endif

		GuiContext BuildGuiContext()
		{
			GuiContext ctx;
			ctx.injectionAllowed = InjectionAllowed();
			ctx.injectionNotice = injectionStatus.empty()
				? "仅偶像摄影与更衣室可以使用；其他页面保留设置但不生效。" : injectionStatus;
			ctx.player = &player;
			ctx.cameraSnapshot = camera.ReadSnapshot();
			ctx.cameraImportBusy = cameraImportBusy;
			ctx.cameraNotice = cameraNotice;
			ctx.cameraSource = player.currentMotionDir.empty() ? "" : player.currentMotionDir + "/camera.gkcamera";
			ctx.cameraFrames = static_cast<uint32_t>(camera.track.frames.size());
			ctx.cameraFps = camera.track.fps;
			ctx.fnSetCameraSettings = [](void* self, const CameraSettings& settings)
			{
				static_cast<Core*>(self)->QueueCameraSettings(settings);
			};
			ctx.fnImportCamera = [](void* self, const CameraImportRequest& request)
			{
				GuiCommand cmd{GuiAction::CameraImport};
				cmd.cameraImport = request;
				static_cast<Core*>(self)->QueueGui(std::move(cmd));
			};
			ctx.fnSaveCameraSettings = [](void* self)
			{
				static_cast<Core*>(self)->QueueGui({GuiAction::SaveCameraSettings});
			};
			ctx.fnReloadCamera = [](void* self) { static_cast<Core*>(self)->QueueGui({GuiAction::ReloadCamera}); };
			ctx.lookAt = &lookAt;
			ctx.config = &config;
			ctx.motions = &motions;
			ctx.overrides = &effectiveOverrides;
			ctx.masterOverridesEnabled = &masterOverridesEnabled;
			ctx.selectedMotionIndex = &currentMotionIndex;
			ctx.libraryRevision = libraryRevision;
			ctx.loadedMotionGeneration = loadedMotionGeneration;
			ctx.injectionSession = injectionSession;
			ctx.libraryNotice = libraryNotice;
			ctx.libraryRefreshing = libraryScan.valid();
			ctx.loadedMotionKey = loadedMotionKey;
			ctx.loadedSourceMissing = loadedSourceMissing;
			ctx.loadedSourceChanged = loadedSourceChanged;
			ctx.actorRosterEpoch = sceneActorRegistry.Epoch();
			for (const auto& item : sceneActorRegistry.Entries())
			{
				const std::string name = item.character.empty() ? "未识别角色" : FormatIdolDisplayName(item.character.c_str());
				ctx.sceneActors.push_back({item.token, name + " · " + std::to_string(ctx.sceneActors.size() + 1),
					item.actor == player.controller.targetActor});
			}
			auto actorLabel = [&](void* actor)
			{
				for (size_t i = 0; i < sceneActorRegistry.Entries().size(); ++i)
					if (sceneActorRegistry.Entries()[i].actor == actor) return ctx.sceneActors[i].label;
				return std::string("尚未绑定角色");
			};
			ctx.boundActorLabel = actorLabel(player.controller.currentBinding.actor);
			ctx.targetActorLabel = actorLabel(player.controller.targetActor);
			ctx.fnSelectTargetActor = [](void* self, uint64_t token, uint64_t epoch)
			{
				GuiCommand command{GuiAction::SelectTarget};
				command.actorToken = token; command.actorEpoch = epoch;
				static_cast<Core*>(self)->QueueGui(std::move(command));
			};
			ctx.statusLine = StatusSummary();
			ctx.physicsBackend = guiPhysicsBackend.load();
			ctx.beforeQuartzExperiment = guiBeforeQuartzExperiment.load();
			ctx.fnSetBeforeQuartzExperiment = [](void* self, bool enabled)
			{
				static_cast<Core*>(self)->QueueBeforeQuartzExperiment(enabled);
			};
			ctx.experimentalWaistSupport = guiExperimentalWaistSupport.load();
			ctx.fnSetExperimentalWaistSupport = [](void* self, bool enabled)
			{
				static_cast<Core*>(self)->QueueExperimentalWaistSupport(enabled);
			};
			ctx.playlistMode = static_cast<int>(playlist.mode);
			ctx.importBusy = importBusy;
			guiImportProgressSnapshot = importProgress;
			ctx.importProgress = &guiImportProgressSnapshot;
			ctx.playlistNotice = playlistNotice;
			ctx.initialPositionNotice = initialPositionNotice;
			ctx.soleSettings = soleSettings;
			ctx.soleSnapshot = soleSnapshot;
			ctx.soleNotice = soleNotice;
			ctx.experimentalSolePitchSettings = experimentalSolePitchSettings;
			ctx.experimentalSolePitchSnapshot = experimentalSolePitchSnapshot;
			ctx.fnSetExperimentalSolePitch = [](void* self, const sole_pitch::Settings& settings)
			{
				static_cast<Core*>(self)->QueueExperimentalSolePitch(settings);
			};
			ctx.soleCalibrationGeneration = soleCalibrationGeneration;
			ctx.fnSetSoleSettings = [](void* self, const SoleMeasurementSettings& settings)
			{
				static_cast<Core*>(self)->QueueSoleSettings(settings);
			};
			ctx.fnSaveSoleSettings = [](void* self)
			{
				static_cast<Core*>(self)->QueueGui({GuiAction::SaveSoleSettings});
			};
			ctx.fnLoadSoleSettings = [](void* self)
			{
				static_cast<Core*>(self)->QueueGui({GuiAction::LoadSoleSettings});
			};
			ctx.fnSetInitialPosition = [](void* self, const Vector3& offset)
			{
				static_cast<Core*>(self)->QueueInitialPosition(offset);
			};
			ctx.fnSaveInitialPosition = [](void* self)
			{
				static_cast<Core*>(self)->QueueGui({GuiAction::SaveInitialPosition});
			};
			ctx.fnPickFile = fnPickFile;
			ctx.fnBeginPickFile = fnBeginPickFile;
			ctx.fnPollPickFile = fnPollPickFile;
			ctx.fnPlaylistAction = [](void* self, int action, const char* key, int value)
			{
				auto& c = *static_cast<Core*>(self);
				GuiAction command;
				switch (static_cast<GuiPlaylistAction>(action))
				{
				case GuiPlaylistAction::Move: command = GuiAction::Move; break;
				case GuiPlaylistAction::SaveList: command = GuiAction::SaveList; break;
				case GuiPlaylistAction::LoadList: command = GuiAction::LoadList; break;
				case GuiPlaylistAction::SetMode: command = GuiAction::Mode; break;
				case GuiPlaylistAction::DeleteFiles: command = GuiAction::DeleteFiles; break;
				default: return;
				}
				std::string path = key ? key : "";
				// File-dialog paths already use UTF-8. Do not convert through ACP.
				GuiCommand queued{command, static_cast<float>(value), command == GuiAction::DeleteFiles && value == 1, path};
				queued.libraryRevision = c.libraryRevision;
				c.QueueGui(std::move(queued));
			};
			ctx.fnImportMotion = [](void* self, const MotionImportRequest& request)
			{
				GuiCommand cmd{GuiAction::Import};
				cmd.import = request;
				static_cast<Core*>(self)->QueueGui(std::move(cmd));
			};
			ctx.fnSetMuted = [](void* self, bool value)
			{
				static_cast<Core*>(self)->QueueGui({GuiAction::Mute, 0, value});
			};
			ctx.fnSetSpeed = [](void* self, float value)
			{
				static_cast<Core*>(self)->QueueGui({GuiAction::Speed, value});
			};
			ctx.fnSeekFrame = [](void* self, int64_t frame) { static_cast<Core*>(self)->QueueSeekFrame(frame); };
			ctx.fnStepFrame = [](void* self, int delta) { static_cast<Core*>(self)->QueueStepFrame(delta); };
			ctx.fnReloadMotion = [](void* self) { static_cast<Core*>(self)->QueueReloadMotion(); };
			ctx.knownActorCount = KnownActorCount();
			ctx.fnClearTargetActor = [](void* self)
			{
				static_cast<Core*>(self)->QueueGui({GuiAction::ClearTarget});
			};
			ctx.fnNextTargetActor = [](void* self)
			{
				static_cast<Core*>(self)->QueueGui({GuiAction::NextTarget});
			};
			ctx.fnPickFocusedActor = [](void* self)
			{
				static_cast<Core*>(self)->QueueGui({GuiAction::PickTarget});
			};
			ctx.fnSelectMotion = [](void* self, int index, bool autoplay)
			{
				static_cast<Core*>(self)->QueueSelectMotion(index, autoplay);
				return true;
			};
			ctx.fnCycleMotion = [](void* self, int delta)
			{
				static_cast<Core*>(self)->QueueCycleMotion(delta);
				return true;
			};
			ctx.fnPlay = [](void* self) { static_cast<Core*>(self)->QueuePlay(); };
			ctx.fnPause = [](void* self) { static_cast<Core*>(self)->QueuePause(); };
			ctx.fnResume = [](void* self) { static_cast<Core*>(self)->QueueResume(); };
			ctx.fnStop = [](void* self) { static_cast<Core*>(self)->QueueStop(); };
			ctx.fnSeek = [](void* self, float seconds) { static_cast<Core*>(self)->QueueSeek(seconds); };
			ctx.fnSetPhysicsBackend = [](void* self, int backend)
			{
				static_cast<Core*>(self)->QueuePhysicsBackend(backend);
			};
			ctx.fnScrubBegin = [](void* self, float seconds) { static_cast<Core*>(self)->QueueScrubBegin(seconds); };
			ctx.fnScrubUpdate = [](void* self, float seconds) { static_cast<Core*>(self)->QueueScrubUpdate(seconds); };
			ctx.fnScrubEnd = [](void* self, float seconds) { static_cast<Core*>(self)->QueueScrubEnd(seconds); };
#ifdef GKMOTION_SCRUB_TRACE
			ctx.fnProbe = [](void* self, const char* line)
			{
				static_cast<Core*>(self)->LogInfo(line ? line : "");
			};
#endif
			ctx.fnReloadOverrides = [](void* self)
			{
				static_cast<Core*>(self)->LoadOverrides(true);
			};
			ctx.fnToggleOverrides = [](void* self)
			{
				return static_cast<Core*>(self)->ToggleOverrides();
			};
			ctx.fnCycleGaze = [](void* self)
			{
				return static_cast<Core*>(self)->CycleGazeMode();
			};
			ctx.fnRefreshMotions = [](void* self)
			{
				static_cast<Core*>(self)->QueueGui({GuiAction::Refresh});
				return true;
			};
			ctx.fnSetBoneOverride = [](void* self, int hid, const char* name, float pitch, float yaw, float roll,
			                           bool world, bool enabled, bool symmetrical)
			{
				static_cast<Core*>(self)->SetLiveBoneOverride(hid, name, pitch, yaw, roll, world, enabled, symmetrical);
			};
			ctx.fnResetBoneOverride = [](void* self, int hid, const char* name, bool symmetrical)
			{
				static_cast<Core*>(self)->ResetLiveBoneOverride(hid, name, symmetrical);
			};
			ctx.fnResetAllOverrides = [](void* self)
			{
				static_cast<Core*>(self)->ResetAllLiveOverrides();
			};
			ctx.fnSaveOverridesToMotion = [](void* self)
			{
				return static_cast<Core*>(self)->SaveLiveOverridesToMotion();
			};
			ctx.fnSaveOverridesToGlobal = [](void* self)
			{
				return static_cast<Core*>(self)->SaveLiveOverridesToGlobal();
			};
			ctx.fnGetBoneOverride = [](void* self, int hid, const char* name, float* outPitch, float* outYaw,
			                           float* outRoll, bool* outWorld, bool* outEnabled)
			{
				return static_cast<Core*>(self)->GetLiveBoneOverride(hid, name, outPitch, outYaw, outRoll, outWorld,
				                                                     outEnabled);
			};
			ctx.stageSettings = stageManager.settings;
			ctx.stageCandidates = stageManager.candidates;
			ctx.stageSceneName = stageManager.currentSceneName;
			ctx.stageMountedKey = stageManager.mountedStageKey;
			ctx.stageStatusNotice = stageManager.statusNotice;
			ctx.stageLoadNotice = stageLoadNotice;
			ctx.fnSetStageLoadReplacement = [](void* self, const char* key)
			{
				static_cast<Core*>(self)->QueueStageLoadReplacement(key ? key : "");
			};
			ctx.fnSetStageSettings = [](void* self, const StageSettings& settings)
			{
				static_cast<Core*>(self)->QueueStageSettings(settings);
			};
			ctx.fnSetStagePreset = [](void* self, int presetIndex)
			{
				static_cast<Core*>(self)->QueueStagePreset(presetIndex);
			};
			ctx.fnToggleStageCandidate = [](void* self, int index, bool active)
			{
				static_cast<Core*>(self)->QueueToggleStageCandidate(index, active);
			};
			ctx.fnScanStageCandidates = [](void* self)
			{
				static_cast<Core*>(self)->QueueScanStageCandidates();
			};
			ctx.fnMountCustomStage = [](void* self, const char* key)
			{
				static_cast<Core*>(self)->QueueMountCustomStage(key ? key : "");
			};
			ctx.fnUnloadCustomStage = [](void* self)
			{
				static_cast<Core*>(self)->QueueUnloadCustomStage();
			};
			ctx.fnRestoreCameraSkybox = [](void* self)
			{
				static_cast<Core*>(self)->QueueRestoreCameraSkybox();
			};
			ctx.self = this;
			return ctx;
		}

	private:
		void NotifyMotionPlaying(bool playing) const
		{
			if (fnNotifyMotionPlaying) fnNotifyMotionPlaying(playing);
		}

		void LogInfo(const std::string& message) const
		{
			if (fnLogInfo) fnLogInfo("%s", message.c_str());
		}

		void LogWarn(const std::string& message) const
		{
			if (fnLogWarn) fnLogWarn("%s", message.c_str());
		}

		// Release builds compile this to nothing (GAKUMOTION_LOG_FLOOR), which is
		// exactly what a scene-selection trace should do.
		void LogDebug(const std::string& message) const
		{
			GM_DEBUG("%s", message.c_str());
		}

		// 相对路径一律相对 localRoot 解析；绝对路径原样返回
		std::string ResolvePath(const std::string& candidate, const std::string& fallback) const
		{
			return dataLayout.ResolveOverride(candidate, fallback);
		}

		// 生成「不启用」的全局覆盖模板，便于用户发现该能力
		bool EnsureGlobalOverrideTemplate()
		{
			std::error_code ec;
			if (std::filesystem::exists(MotionPathFromUtf8(globalOverridePath), ec)) return false;

			std::filesystem::create_directories(MotionPathFromUtf8(configRoot), ec);
			std::ofstream out(MotionPathFromUtf8(globalOverridePath), std::ios::binary);
			if (!out.is_open()) return false;
			out <<
				"// GakuMotion 全局骨骼旋转覆盖表（热加载）\n"
				"// space=local: R = Q_actor * (R_vmd * R_rest) * Q_offset（默认，骨骼局部空间）\n"
				"// space=world: R = Q_offset * Q_actor * (R_vmd * R_rest)（世界空间左乘）\n"
				"// euler_deg 顺序为 [pitch(X), yaw(Y), roll(Z)]，单位为度。\n"
				"{\n"
				"  \"enabled\": false,\n"
				"  \"overrides\": [\n"
				"    { \"bone\": \"LeftUpperArm\", \"humanoid_id\": 13, \"enabled\": true,\n"
				"      \"space\": \"local\", \"euler_deg\": [-6.0, 0.0, 4.0] },\n"
				"    { \"bone\": \"RightUpperArm\", \"humanoid_id\": 14, \"enabled\": true,\n"
				"      \"space\": \"local\", \"euler_deg\": [-6.0, 0.0, -4.0] }\n"
				"  ]\n"
				"}\n";
			out.close();
			return true;
		}
	};

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
