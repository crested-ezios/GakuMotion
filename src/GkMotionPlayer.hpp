#pragma once

#include "GkMotionData.hpp"
#include "GkMouthSource.hpp"
#include "GkAudioPlayback.hpp"
#include "GkIdolController.hpp"
#include "GkPerformanceClock.hpp"
#include "GkRuntimeConfig.hpp"   // MotionEntry：曲目库索引与「上一曲/下一曲」
#include "GkTransportTime.hpp"
#include <chrono>
#include <string>
#include <memory>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <unordered_set>

namespace GakuMotion
{
	inline bool IsCustomMotionScene(const std::string& name)
	{
		// OutGame also hosts Home. Costume requires independent page evidence.
		return name == "Photography" || name == "PhotographyScene";
	}

	class ActorFrameTracker
	{
		int frame_{-1};
		std::unordered_set<void*> previous_, current_;

	public:
		void Reset()
		{
			frame_ = -1;
			previous_.clear();
			current_.clear();
		}

		bool Note(int frame, void* actor)
		{
			if (frame != frame_)
			{
				previous_.swap(current_);
				current_.clear();
				frame_ = frame;
			}
			const bool arrived = !previous_.count(actor) && !current_.count(actor);
			current_.insert(actor);
			return arrived;
		}

		bool Contains(void* actor) const { return previous_.count(actor) || current_.count(actor); }
	};

	enum class PlaybackState
	{
		Stopped,
		Playing,
		Paused,
		// 已载入动作但未播放：静止保持当前帧姿态（默认 currentTime = 0），
		// 让「选中动作」立刻可见第 0 帧，而不是退回官方待机动画。
		Preview
	};

	class MotionPlayer
	{
	public:
		PlaybackState state{PlaybackState::Stopped};
		MotionClip clip;
		IdolController controller;

		float currentTime{0.0f};
		// Keep consecutive frame commands exact even when n/fps is rounded to a
		// float. Ordinary seeks and clock advancement invalidate this cursor.
		int64_t navigationFrame{-1};
		float navigationFrameTime{0.0f};
		bool loop{true};
		bool finished{false}; // Consumed by Core for automatic playlist advancement.
		bool muted{false};
		float musicVolume{1.0f};
		bool (*fnSetAudioVolume)(float){nullptr};
		int (*fnAudioVolumeSupport)(){nullptr}; // 0=not open, 1=available, -1=unavailable
		bool SetMusicVolume(float value)
		{
			if (!std::isfinite(value)) return false;
			musicVolume = std::clamp(value, 0.0f, 1.0f);
			return !fnSetAudioVolume || fnSetAudioVolume(musicVolume);
		}
		bool audioStarted{false};
		std::string currentMotionDir;
		int currentMotionSlot{1};
		std::string currentSlotDir;

		// The performance clock advances at this rate; each file maps that clock
		// through its own speed and offset. MCI cannot stretch audio.
		float playbackSpeedMultiplier{1.0f};
		PerformanceTiming timing;
		float cameraSourceDuration{0.0f};
		float audioSourceDuration{0.0f};
		bool audioWaitingForTiming{false};
		std::string audioFileName{"song.wav"};
		performance::ClockConfiguration clockConfig{};
		performance::Manual engineClock_{};
		performance::FixedStep fixedClock_{};
		double preciseTimeSeconds{0.0};
		double lastStepDeltaSeconds{0.0};
		uint64_t evaluatedFrameSerial{0};
		bool waitingForRenderComplete{false};
		bool gateRecordingByRenderComplete{false};
		uint64_t recordingLeaseId{0};
		std::string recordingLeaseOwner;
		// Session-owned tail supplied by additional actor tracks. Zero preserves
		// the single-actor duration contract; actor samplers never advance it.
		float additionalPerformanceDuration{0.0f};

		float MotionSourceTime() const { return TrackSourceTime(currentTime, timing.motion); }
		float FacialSourceTime() const { return TrackSourceTime(currentTime, timing.facial); }
		float CameraSourceTime() const { return TrackSourceTime(currentTime, timing.camera); }
		float AudioSourceTime() const { return TrackSourceTime(currentTime, timing.audio); }
		float TimelineDuration() const
		{
			float duration = TrackTimelineDuration(clip.duration_seconds, timing.motion);
			if (!clip.blendshape_names.empty() || std::any_of(clip.bones.begin(), clip.bones.end(),
				[](const auto& bone) { return bone.head_space_eye_delta; }))
				duration = (std::max)(duration, TrackTimelineDuration(clip.duration_seconds, timing.facial));
			if (cameraSourceDuration > 0.0f)
				duration = (std::max)(duration, TrackTimelineDuration(cameraSourceDuration, timing.camera));
			if (audioSourceDuration > 0.0f)
				duration = (std::max)(duration, TrackTimelineDuration(audioSourceDuration, timing.audio));
			return (std::max)(duration, additionalPerformanceDuration);
		}
		double EffectiveAudioPlaybackRate() const noexcept
		{
			return static_cast<double>(playbackSpeedMultiplier) * timing.audio.speed;
		}
		bool AudioRateSupported() const
		{
			if (!ValidTrackTiming(timing.audio) || !std::isfinite(playbackSpeedMultiplier))
				return false;
			const double eff = EffectiveAudioPlaybackRate();
			if (fnSupportsAudioPlaybackRate && fnSupportsAudioPlaybackRate())
				return eff >= 0.05 && eff <= 4.0;
			return std::fabs(eff - 1.0) <= 1e-6;
		}
		bool AudioEligible() const
		{
			if (clockConfig.runMode == performance::PlaybackRunMode::ForRecording)
				return false;
			return !muted && AudioRateSupported() && currentTime >= timing.audio.offsetSeconds &&
				(audioSourceDuration <= 0.0f || AudioSourceTime() < audioSourceDuration);
		}
		bool (*fnSupportsAudioPlaybackRate)(){nullptr};
		void (*fnSetAudioPlaybackRate)(float){nullptr};
		void SyncAudioPlaybackRate() const
		{
			if (fnSupportsAudioPlaybackRate && fnSupportsAudioPlaybackRate() &&
				fnSetAudioPlaybackRate && AudioRateSupported())
			{
				fnSetAudioPlaybackRate(static_cast<float>(EffectiveAudioPlaybackRate()));
			}
		}
		void SyncClockState(double seconds) noexcept
		{
			preciseTimeSeconds = std::isfinite(seconds) ? (std::max)(0.0, seconds) : 0.0;
			currentTime = static_cast<float>(preciseTimeSeconds);
			engineClock_.Seek(performance::PerformanceTime{preciseTimeSeconds});
			fixedClock_.Seek(performance::PerformanceTime{preciseTimeSeconds});
		}
		void ReconcileExternalCurrentTime() noexcept
		{
			if (std::fabs(static_cast<double>(currentTime) - preciseTimeSeconds) > 1e-5)
			{
				audioPllErrorFiltered_ = 0.0;
				SyncClockState(static_cast<double>(currentTime));
			}
		}
		void ResetAudioClock()
		{
			audioSeekOffset = 0.0f;
			audioSyncSettleFrames = fnGetAudioTime ? kAudioSettleFrames : 0;
			audioPllErrorFiltered_ = 0.0;
		}
		bool StartAudioAtCurrentTime()
		{
			if (!AudioEligible()) { audioWaitingForTiming = !muted && clockConfig.runMode == performance::PlaybackRunMode::Realtime; return true; }
			if (fnPlayAudio && fnCanOpenAudio && !fnCanOpenAudio(audioOpenGuardContext)) return false;
			audioWaitingForTiming = false;
			if (fnPlayAudio)
			{
				fnPlayAudio(AudioPath());
				SyncAudioPlaybackRate();
				audioStarted = true;
				RefreshAudioStarted();
			}
			if (fnSeekAudio && AudioSourceTime() > 0.001f) fnSeekAudio(AudioSourceTime());
			ResetAudioClock();
			return true;
		}
		void UpdateAudioTiming()
		{
			if (!AudioEligible())
			{
				if (audioStarted && fnStopAudio) fnStopAudio();
				audioStarted = false;
				audioWaitingForTiming = !muted && clockConfig.runMode == performance::PlaybackRunMode::Realtime;
				audioSyncSettleFrames = 0;
			}
			else if (audioWaitingForTiming && state == PlaybackState::Playing && !scrubbing)
			{
				if (!StartAudioAtCurrentTime()) Pause();
			}
		}
		void SetPlaybackSpeed(float speed)
		{
			if (!std::isfinite(speed)) return;
			speed = std::clamp(speed, 0.1f, 5.0f);
			if (playbackSpeedMultiplier == speed) return;
			playbackSpeedMultiplier = speed;
			engineClock_.SetPlaybackRate(speed);
			fixedClock_.SetPlaybackRate(speed);
			audioPllErrorFiltered_ = 0.0;
			if (fnSupportsAudioPlaybackRate && fnSupportsAudioPlaybackRate() && audioStarted && AudioRateSupported())
			{
				SyncAudioPlaybackRate();
				UpdateAudioTiming();
				return;
			}
			if (audioStarted && fnStopAudio) fnStopAudio();
			audioStarted = false;
			audioWaitingForTiming = true;
			ResetAudioClock();
			UpdateAudioTiming();
		}

		// Optional audio synchronization callback
		using GetAudioTimeFn = float (*)();
		GetAudioTimeFn fnGetAudioTime{nullptr};
		// The native MCI backend reports an absolute source-file position.
		// Other backends retain the legacy settle calibration unless opted in.
		bool audioPositionIsAbsolute{false};

		using PlayAudioFn = void (*)(const std::string& path);
		using PauseAudioFn = void (*)();
		using StopAudioFn = void (*)();
		// 新增：断点续播（修复「暂停后继续会从头重放」）与音频跳转（进度拖动）
		using ResumeAudioFn = void (*)(float seconds);
		using SeekAudioFn = void (*)(float seconds);
		PlayAudioFn fnPlayAudio{nullptr};
		PauseAudioFn fnPauseAudio{nullptr};
		StopAudioFn fnStopAudio{nullptr};
		bool (*fnCanPlay)(){nullptr};
		void* audioOpenGuardContext{nullptr};
		bool (*fnCanOpenAudio)(void*){nullptr};
		ResumeAudioFn fnResumeAudio{nullptr};
		SeekAudioFn fnSeekAudio{nullptr};
		SeekAudioFn fnSeekPausedAudio{nullptr}; // Position only; never starts audio.
		// Optional confirmed backend state. Legacy callbacks remain source-compatible,
		// but callback presence alone is never evidence for native-BGM suppression.
		audio::Observation (*fnObserveAudio)(){nullptr};
		// Optional combined observation + position callback so audio-clock frames do
		// not acquire the audio engine lock and query cursor position twice per tick.
		bool (*fnObserveAudioWithTime)(audio::Observation* outObs, float* outTimeSeconds){nullptr};
		audio::Observation ObserveAudio() const
		{
			return fnObserveAudio ? fnObserveAudio() : audio::Observation{};
		}
		void RefreshAudioStarted()
		{
			if (fnObserveAudio)
			{
				const auto observed = fnObserveAudio();
				audioStarted = observed.sourceOpen && observed.confirmed && observed.errorCode == 0 &&
					(observed.state == audio::PlaybackState::Playing || observed.state == audio::PlaybackState::Paused);
			}
		}
		bool HasEffectiveCustomAudio() const
		{
			return state == PlaybackState::Playing && AudioEligible() && ObserveAudio().EffectivePlaying();
		}

		// -------------------------------------------------------------------------
		// 音频主时钟对齐状态
		// -------------------------------------------------------------------------
		// 问题：MCI 在「play <alias> from <ms>」之后的 position 语义并不保证与文件
		//       时间轴一致，若跳转后立即采信音频时钟，动作会被拉回原位或整体错位。
		// 方案：跳转当帧起「settle」若干帧不采信音频时钟（本地按 deltaTime 前进），
		//       settle 结束时采样一次音频位置，算出偏移量 audioSeekOffset，
		//       此后统一使用 currentTime = audioPosition + audioSeekOffset，
		//       从而对两种 position 语义都自动正确，且不产生音画漂移。
		static constexpr int kAudioSettleFrames = 3;
		// 音频时钟回退容差（方案 B）：MCI position 偶发回退（毫秒量化抖动）时，
		// 低于当前时间减去本容差的采样被视为抖动而不采信，避免动作瞬间倒退。
		static constexpr float kAudioBackwardToleranceSeconds = 0.01f;
		// Phase-locked audio clock parameters for absolute audio backends.
		// Audio mixer cursors advance in discrete ~10-25ms WASAPI buffer chunks;
		// tracking smoothed phase error with bounded rate nudging eliminates
		// per-frame quantization stutter while staying locked to audio time.
		static constexpr double kAudioPllHardSnapSeconds = 0.080;
		static constexpr double kAudioPllBackwardHoldSeconds = 0.080;
		static constexpr double kAudioPllFilterTauSeconds = 0.15;
		static constexpr double kAudioPllGainPerSecond = 4.0;
		static constexpr double kAudioPllMaxRateAdjust = 0.08;
		float audioSeekOffset{0.0f};
		int audioSyncSettleFrames{0};
		double audioPllErrorFiltered_{0.0};
		float lastSyncCorrectionSeconds{0.0f}; // 最近一次跳转的校正量（诊断用）
		int seekCount{0}; // 累计跳转次数（诊断用）

		std::string AudioPath() const
		{
			return currentMotionDir + "/" + (audioFileName.empty() ? std::string("song.wav") : audioFileName);
		}

		const std::string& CurrentSlotDirectory() const
		{
			return currentSlotDir.empty() ? currentMotionDir : currentSlotDir;
		}

		std::string SlotBoneOverridesPath() const
		{
			const auto& dir = CurrentSlotDirectory();
			return dir.empty() ? std::string() : dir + "/bone_overrides.json";
		}

		std::string SlotAuxReplayPath() const
		{
			const auto& dir = CurrentSlotDirectory();
			return dir.empty() ? std::string() : dir + "/aux_replay.json";
		}

		std::string SlotRootHeightCalibrationPath() const
		{
			const auto& dir = CurrentSlotDirectory();
			return dir.empty() ? std::string() : dir + "/root_height_calibration.json";
		}

		// Absolute path of the clip actually loaded (diagnostics / panel display)
		std::string loadedMotionFile{""};
		std::string mouthSourceStatus;

		bool LoadMotion(const std::string& motionDirectory)
		{
			return LoadMotionSlot(motionDirectory, 1);
		}

		bool LoadMotionSlot(const std::string& trackDirectory, int slot)
		{
			const int normalizedSlot = slot < 1 ? 1 : slot;
			const std::string slotDir = ResolveMotionSlotDirectory(trackDirectory, normalizedSlot);
			// Format-agnostic discovery: gkclip (role-agnostic) wins, then
			// gk_world_dump, then the legacy .gkmotion.
			std::string motionFile = FindMotionFile(slotDir);
			if (motionFile.empty())
			{
				return false;
			}
			MotionClip nextClip;
			if (!nextClip.LoadFromFile(motionFile))
			{
				return false;
			}
			AdoptLoadedMotionSlot(trackDirectory, slotDir, normalizedSlot, motionFile, std::move(nextClip));
			return true;
		}

		// Commit a fully preflighted clip without opening its source a second time.
		// Core uses this when atomically reloading body and camera from one version.
		void AdoptLoadedMotion(const std::string& motionDirectory, const std::string& motionFile, MotionClip nextClip)
		{
			AdoptLoadedMotionSlot(motionDirectory, motionDirectory, 1, motionFile, std::move(nextClip));
		}

		void AdoptLoadedMotionSlot(const std::string& trackDirectory, const std::string& slotDirectory, int slot,
			const std::string& motionFile, MotionClip nextClip)
		{
			const int normalizedSlot = slot < 1 ? 1 : slot;
			const std::string resolvedSlotDir = slotDirectory.empty()
				? ResolveMotionSlotDirectory(trackDirectory, normalizedSlot)
				: slotDirectory;
			jsonlite::Value metadata;
			std::string metadataError;
			const bool hasMetadata = ParseTrackInfoJsonFile(MotionPathFromUtf8(trackDirectory) / "info.json", metadata, metadataError);
			const jsonlite::Value* mouthMetadata = nullptr;
			const jsonlite::Value* slotMeta = nullptr;
			if (hasMetadata)
			{
				mouthMetadata = &metadata;
				if (const auto* positions = metadata.Find("positions"); positions && positions->IsArray())
				{
					for (const auto& item : positions->arrayItems)
					{
						if (!item.IsObject()) continue;
						const auto* slotField = item.Find("slot");
						if (slotField && slotField->IsNumber() &&
							static_cast<int>(std::llround(slotField->numberValue)) == normalizedSlot)
						{
							slotMeta = &item;
							break;
						}
					}
					if (!slotMeta && static_cast<size_t>(normalizedSlot - 1) < positions->arrayItems.size())
					{
						const auto& candidate = positions->arrayItems[static_cast<size_t>(normalizedSlot - 1)];
						if (candidate.IsObject() && candidate.Find("slot") == nullptr)
							slotMeta = &candidate;
					}
					if (normalizedSlot >= 2 && slotMeta &&
						(slotMeta->Find("source_fps") || slotMeta->Find("source_morph_fps")))
						mouthMetadata = slotMeta;
				}
			}
			mouthSourceStatus = mouth_source::Restore(
				nextClip,
				resolvedSlotDir,
				mouthMetadata);
			if (mouth_source::report) mouth_source::report(resolvedSlotDir.c_str(), mouthSourceStatus.c_str());
			// Release the old pose before replacing its clip, including callers
			// that bypass Core::SelectMotionByIndex. Keep the original actor rest
			// snapshot; recapturing immediately after Rebind samples old animation.
			if (state != PlaybackState::Stopped) Stop();
			currentMotionDir = trackDirectory;
			currentSlotDir = resolvedSlotDir;
			currentMotionSlot = normalizedSlot;
			additionalPerformanceDuration = 0.0f;
			clip = std::move(nextClip);
			loadedMotionFile = motionFile;
			// Reload even when absent/invalid: another song's trim must not leak.
			controller.LoadRootHeightCalibration(resolvedSlotDir + "/root_height_calibration.json", clip);
			audioStarted = false;
			audioWaitingForTiming = false;
			timing = hasMetadata ? ReadPerformanceTiming(metadata) : PerformanceTiming{};
			if (hasMetadata && slotMeta)
			{
				ReadSlotTrackTimings(*slotMeta, timing, timing.motion, timing.facial);
			}
			cameraSourceDuration = 0.0f;
			const std::string hintedAudio = hasMetadata ? metadata.StringPath("audio_file", "") : std::string();
			const std::string resolvedAudio = audio::ResolveTrackAudioFileName(
				MotionPathFromUtf8(trackDirectory), hintedAudio);
			audioFileName = resolvedAudio.empty() ? std::string("song.wav") : resolvedAudio;
			audioSourceDuration = audio::AudioFileDuration(MotionPathFromUtf8(AudioPath()));
			if (audioSourceDuration <= 0.0f && hasMetadata)
			{
				const float hintedDur = metadata.FloatPath("audio_duration", 0.0f);
				if (std::isfinite(hintedDur) && hintedDur > 0.0f) audioSourceDuration = hintedDur;
			}
			scrubbing = false;
			finished = false;
			waitingForRenderComplete = false;
			lastStepDeltaSeconds = 0.0;

			SyncClockState(0.0);
			navigationFrame = -1;
			// 载入后进入 Preview：静止保持第 0 帧姿态（不推进时钟、不播音频），
			// 这样「选中动作」立刻看到首帧造型，而不是官方待机动画。
			state = (!fnCanPlay || fnCanPlay()) ? PlaybackState::Preview : PlaybackState::Stopped;
			audioSeekOffset = 0.0f;
			audioSyncSettleFrames = 0;
			audioPllErrorFiltered_ = 0.0;
			lastSyncCorrectionSeconds = 0.0f;
			seekCount = 0;

			// Remap the new clip by humanoid id, preserving the original actor rest
			// snapshot and avoiding recapture of the previous animation frame.
			controller.InvalidateClipBindings();
		}

		// 按曲目库下标载入（供面板/快捷键的「上一曲/下一曲」使用）
		bool LoadMotionByIndex(const std::vector<MotionEntry>& library, int index)
		{
			if (index < 0 || index >= static_cast<int>(library.size())) return false;
			if (!LoadMotion(library[index].dir)) return false;
			timing = ResolveSlotPerformanceTiming(&library[index], 1, library[index].timing);
			return true;
		}

		void Play()
		{
			if (clip.total_frames == 0 || (fnCanPlay && !fnCanPlay())) return;
			finished = false;
			scrubbing = false;
			waitingForRenderComplete = false;
			lastStepDeltaSeconds = 0.0;
			if (!StartAudioAtCurrentTime()) return;
			state = PlaybackState::Playing;
			engineClock_.Resume();
			fixedClock_.Resume();
		}

		void Pause()
		{
			if (state == PlaybackState::Playing)
			{
				state = PlaybackState::Paused;
				waitingForRenderComplete = false;
				lastStepDeltaSeconds = 0.0;
				audioPllErrorFiltered_ = 0.0;
				engineClock_.Pause();
				fixedClock_.Pause();
				if (fnPauseAudio) fnPauseAudio();
			}
		}

		void Resume()
		{
			if (state != PlaybackState::Paused || (fnCanPlay && !fnCanPlay())) return;
			state = PlaybackState::Playing;
			engineClock_.Resume();
			fixedClock_.Resume();
			waitingForRenderComplete = false;
			lastStepDeltaSeconds = 0.0;

			// 修复既有缺陷：原实现调用 fnPlayAudio（内部为 "play ... from 0"），
			// 导致「暂停后继续」实际是从曲子开头重放，音画立即失步。
			scrubbing = false;
			if (!AudioEligible()) { UpdateAudioTiming(); return; }
			if (!audioStarted)
			{
				Play();
				return;
			}
			if (fnResumeAudio)
			{
				fnResumeAudio(AudioSourceTime());
			}
			else if (fnPlayAudio)
			{
				if (fnCanOpenAudio && !fnCanOpenAudio(audioOpenGuardContext)) { Pause(); return; }
				fnPlayAudio(AudioPath());
				if (AudioSourceTime() > 0.001f && fnSeekAudio) fnSeekAudio(AudioSourceTime());
			}
			SyncAudioPlaybackRate();
			RefreshAudioStarted();
			audioSyncSettleFrames = (fnGetAudioTime != nullptr) ? kAudioSettleFrames : 0;
			audioPllErrorFiltered_ = 0.0;
		}

		void Stop()
		{
			audioWaitingForTiming = false;
			navigationFrame = -1;
			scrubbing = false;
			finished = false;
			audioStarted = false;
			waitingForRenderComplete = false;
			lastStepDeltaSeconds = 0.0;
			state = PlaybackState::Stopped;
			engineClock_.Pause();
			fixedClock_.Pause();
			SyncClockState(0.0);
			audioSeekOffset = 0.0f;
			audioSyncSettleFrames = 0;
			audioPllErrorFiltered_ = 0.0;
			if (fnStopAudio) fnStopAudio();
			// 保留 clip 剪辑缓存，杜绝下次播放必须重新读盘
			controller.RestoreMute();
			controller.fallbackMuteAnimator = false;
		}

		void OnActorChanged(void* newActor)
		{
			controller.OnActorChanged(newActor);
		}

		void AbandonScene()
		{
			audioWaitingForTiming = false;
			navigationFrame = -1;
			state = PlaybackState::Stopped;
			engineClock_.Pause();
			fixedClock_.Pause();
			SyncClockState(0.0);
			scrubbing = false;
			audioStarted = false;
			finished = false;
			waitingForRenderComplete = false;
			lastStepDeltaSeconds = 0.0;
			audioSeekOffset = 0.0f;
			audioSyncSettleFrames = 0;
			audioPllErrorFiltered_ = 0.0;
			if (fnStopAudio) fnStopAudio();
			controller.DiscardBindings();
			controller.SetTargetActor(nullptr);
		}

		// 进度拖动：动作与音频必须同时跳转。
		// 若只改 currentTime，播放中的下一帧就会被音频主时钟覆盖回原处。
		void Seek(float timeSeconds)
		{
			SeekPosition(timeSeconds, TimelineDuration());
		}

		void SeekFrame(int64_t frame)
		{
			if (!ValidTransportTimeline(clip.total_frames, clip.fps) || timing.motion.speed <= 0.0f) return;
			frame = std::clamp(frame, int64_t{0}, static_cast<int64_t>(clip.total_frames - 1));
			const float time = static_cast<float>((std::max)(0.0, static_cast<double>(frame) /
				clip.fps / timing.motion.speed + timing.motion.offsetSeconds));
			Pause(); // A frame inspection never starts or resumes audio.
			// Legacy files may have a shorter header duration than their samples.
			// Frame inspection still reaches every stored frame; ordinary time seek
			// and playback keep the existing header-duration contract.
			SeekPosition(time, (std::max)(TimelineDuration(), time));
			navigationFrame = static_cast<int64_t>(TransportFramePosition(MotionSourceTime(), clip.fps, clip.total_frames));
			navigationFrameTime = currentTime;
		}

		void StepFrame(int delta)
		{
			if (delta == 0 || !ValidTransportTimeline(clip.total_frames, clip.fps)) return;
			const int64_t target = navigationFrame >= 0 && currentTime == navigationFrameTime
				? std::clamp(navigationFrame + static_cast<int64_t>(delta), int64_t{0},
					static_cast<int64_t>(clip.total_frames - 1))
				: TransportSteppedFrame(MotionSourceTime(), clip.fps, clip.total_frames, delta);
			SeekFrame(target);
		}

		// Shared seek transport, including paused MCI positioning and settle reset.
		// Only explicit frame inspection may use an upper bound past a malformed
		// legacy header's duration; normal Seek retains its original range.
		void SeekPosition(float timeSeconds, float upperBound)
		{
			if (!std::isfinite(timeSeconds) || !std::isfinite(upperBound) || upperBound < 0.0f) return;
			navigationFrame = -1;
			scrubbing = false;
			waitingForRenderComplete = false;
			lastStepDeltaSeconds = 0.0;
			if (clip.total_frames == 0)
			{
				SyncClockState(0.0);
				return;
			}
			const float clamped = std::max(0.0f, std::min(upperBound, timeSeconds));
			SyncClockState(clamped);
			audioSeekOffset = 0.0f;
			audioSyncSettleFrames = (fnGetAudioTime != nullptr) ? kAudioSettleFrames : 0;
			audioPllErrorFiltered_ = 0.0;
			++seekCount;

			// Playing scrub temporarily pauses the backend, so seek mode follows
			// the logical player state, not the transient MCI mode.
			Probe("[GakuMotion][ProbeS1][PLY] SEEK req=%.3f -> %.3f state=%d scrubbing=%d "
			      "hasSeekAudio=%d hasGetAudioTime=%d settle=%d",
			      timeSeconds, clamped, static_cast<int>(state), scrubbing ? 1 : 0,
			      fnSeekAudio ? 1 : 0, fnGetAudioTime ? 1 : 0, audioSyncSettleFrames);

			if (state == PlaybackState::Stopped) state = PlaybackState::Preview;
			const bool hadAudio = audioStarted;
			UpdateAudioTiming();
			if (AudioEligible() && (hadAudio || !audioStarted))
			{
				if (state == PlaybackState::Playing && fnSeekAudio) fnSeekAudio(AudioSourceTime());
				if (state == PlaybackState::Paused && fnSeekPausedAudio) fnSeekPausedAudio(AudioSourceTime());
			}
		}

		void SeekRelative(float deltaSeconds) { Seek(currentTime + deltaSeconds); }

		// -------------------------------------------------------------------------
		// 进度拖动（scrub）三段式
		// -------------------------------------------------------------------------
		// 背景：ImGui 的 SliderFloat 每帧都把 player.currentTime 回写进滑条变量，
		// 于是「鼠标按住拖动」这件事在数值上不断被播放时钟覆盖回去，表现为
		// 滑条能拖、但松手后回弹到原位置——拖动从未真正提交。
		//
		// 契约：
		//   BeginScrub  —— 进入拖动：冻结主时钟（音频时钟不得把位置拉回）
		//   UpdateScrub —— 拖动中：只改姿态时间，不下发音频跳转（避免每帧 seek 音频）
		//   EndScrub    —— 松手：一次性提交，走完整 Seek（含音频跳转 + settle）
		bool scrubbing{false};

		// ---- PROBE S1：scrub 链路探针 ----
		// 上一轮修复被判定「无效」，但单测只覆盖了本类的方法，没有覆盖
		// 「GUI -> pendingGuiCmd -> Unity 主线程 -> ProcessPendingGuiCommands -> 本类」
		// 这条真实链路。这里把本类实际收到的入参、施加后的即时状态、以及随后若被
		// 拉回时的拉回者一起打出，用于区分「命令根本没到」与「到了但被打回」。
		using ProbeFn = void (*)(const char* line);
		ProbeFn fnProbe{nullptr};

		void Probe(const char* fmt, ...) const
		{
#ifndef GKMOTION_SCRUB_TRACE
			return;
#else
			if (!fnProbe) return;
			char buf[512]{};
			va_list args;
			va_start(args, fmt);
			std::vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);
			fnProbe(buf);
#endif
		}

		// 被外部时钟拉回时的「凶手」记录：拉回发生的那一刻，AdvanceClock 各分支
		// 会写入不同的 source，用于区分是音频主时钟、settle 校正还是本地推进。
		const char* lastClockSource{"none"};

		void BeginScrub(float timeSeconds)
		{
			if (!std::isfinite(timeSeconds)) return;
			navigationFrame = -1;
			if (clip.total_frames == 0)
			{
				Probe("[GakuMotion][ProbeS1][PLY] BEGIN ignored: clip empty (total_frames=0)");
				return;
			}
			scrubbing = true;
			if (state == PlaybackState::Stopped) state = PlaybackState::Preview;
			if (!muted && state == PlaybackState::Playing && fnPauseAudio) fnPauseAudio();
			const float clamped = std::max(0.0f, std::min(TimelineDuration(), timeSeconds));
			SyncClockState(clamped);
			waitingForRenderComplete = false;
			lastStepDeltaSeconds = 0.0;
			audioSeekOffset = 0.0f;
			audioSyncSettleFrames = 0;
			audioPllErrorFiltered_ = 0.0;
			Probe("[GakuMotion][ProbeS1][PLY] BEGIN req=%.3f -> set=%.3f dur=%.3f state=%d scrubbing=1",
			      timeSeconds, clamped, clip.duration_seconds, static_cast<int>(state));
		}

		void UpdateScrub(float timeSeconds)
		{
			if (!std::isfinite(timeSeconds)) return;
			if (!scrubbing)
			{
				// 到达这里说明 GUI 认为在拖动，但主线程侧状态已经不在拖动态——
				// 典型的「begin/end 被中间帧覆盖」或「跨线程时序丢帧」。
				Probe("[GakuMotion][ProbeS1][PLY] UPDATE req=%.3f DROPPED (not scrubbing)", timeSeconds);
				return;
			}
			const float clamped = std::max(0.0f, std::min(TimelineDuration(), timeSeconds));
			SyncClockState(clamped);
		}

		void EndScrub(float timeSeconds)
		{
			const bool wasScrubbing = scrubbing;
			scrubbing = false;
			Seek(timeSeconds);
			Probe("[GakuMotion][ProbeS1][PLY] END req=%.3f wasScrubbing=%d -> now=%.3f seeks=%d "
			      "audioSeekOffset=%.3f settle=%d hasSeekAudio=%d state=%d",
			      timeSeconds, wasScrubbing ? 1 : 0, currentTime, seekCount,
			      audioSeekOffset, audioSyncSettleFrames,
			      fnSeekAudio ? 1 : 0, static_cast<int>(state));
		}

		void SetMuted(bool value)
		{
			if (muted == value) return;
			muted = value;
			audioSeekOffset = 0;
			audioSyncSettleFrames = kAudioSettleFrames;
			audioPllErrorFiltered_ = 0.0;
			if (value)
			{
				audioStarted = false;
				if (fnStopAudio) fnStopAudio();
			}
			else if (state == PlaybackState::Playing && !StartAudioAtCurrentTime()) muted = true;
		}

		void SetClockConfiguration(const performance::ClockConfiguration& next)
		{
			clockConfig = next;
			if (!clockConfig.fixedFrameRate.Valid())
				clockConfig.fixedFrameRate = {60, 1};
			fixedClock_.SetFrameRate(clockConfig.fixedFrameRate);
			audioPllErrorFiltered_ = 0.0;
			if (clockConfig.runMode == performance::PlaybackRunMode::ForRecording)
			{
				if (audioStarted && fnStopAudio) fnStopAudio();
				audioStarted = false;
				audioWaitingForTiming = false;
			}
			else
			{
				waitingForRenderComplete = false;
				UpdateAudioTiming();
			}
		}

		bool StepRecordingFrames(uint32_t advanceFrames, double* outNewTimeSeconds = nullptr, bool* outFinished = nullptr)
		{
			if (clip.total_frames == 0 || state != PlaybackState::Playing)
			{
				if (outNewTimeSeconds) *outNewTimeSeconds = preciseTimeSeconds;
				if (outFinished) *outFinished = finished;
				return false;
			}
			ReconcileExternalCurrentTime();
			fixedClock_.SetPlaybackRate(playbackSpeedMultiplier);
			fixedClock_.SetFrameRate(clockConfig.fixedFrameRate);
			fixedClock_.Resume();
			const double prev = preciseTimeSeconds;
			if (advanceFrames > 0)
			{
				fixedClock_.AdvanceFrames(static_cast<int64_t>(advanceFrames));
				SyncClockState(fixedClock_.Time().seconds);
				lastStepDeltaSeconds = (std::max)(0.0, preciseTimeSeconds - prev);
			}
			else
			{
				lastStepDeltaSeconds = 0.0;
			}
			waitingForRenderComplete = false;

			const double dur = static_cast<double>(TimelineDuration());
			if (dur > 0.0 && preciseTimeSeconds >= dur)
			{
				if (loop)
				{
					SyncClockState(0.0);
					waitingForRenderComplete = false;
				}
				else
				{
					Stop();
					finished = true;
				}
			}
			if (outNewTimeSeconds) *outNewTimeSeconds = preciseTimeSeconds;
			if (outFinished) *outFinished = finished;
			return true;
		}

		void MarkCurrentFrameRendered() noexcept
		{
			waitingForRenderComplete = false;
		}

		// Called every frame during LateUpdate / Pre-Physics Hook
		// 注意：只有 Stopped 会直接返回（角色交还给官方待机动画）。
		// Preview 必须继续走绑定/静音/灌帧，否则选中动作后角色会退回待机造型。
		void Tick(float deltaTime, void* actor, void* animator = nullptr, void* playableDirector = nullptr,
		          bool advanceClock = true)
		{
			if (!actor || state == PlaybackState::Stopped ||
				!controller.CanDriveActor(actor))
				return;

			// Ensure bind pose is captured
			controller.BindActor(actor, clip, animator);
			auto binding = controller.bindings.find(actor);
			const bool actorBound = binding != controller.bindings.end() &&
				binding->second.bindPoseCaptured && binding->second.boundBoneCount > 0;
			if (actorBound)
				for (int slot = 0; slot < 2; ++slot)
					binding->second.talks[slot].Suppress(actor, slot, controller.faceApi);

			// Defensive muting of official animation
			if (actorBound && (animator || playableDirector))
			{
				controller.DefensiveMute(actor, animator, playableDirector);
			}

			if (advanceClock) AdvancePerformance(deltaTime);
			if (state == PlaybackState::Stopped) return;

			// Apply pose
			if (actorBound)
			{
				controller.ApplyMotion(clip, MotionSourceTime(), actor, FacialSourceTime());
				++evaluatedFrameSerial;
				if (advanceClock && state == PlaybackState::Playing &&
					clockConfig.runMode == performance::PlaybackRunMode::ForRecording &&
					recordingLeaseId == 0 &&
					clockConfig.source != performance::ClockSource::External &&
					gateRecordingByRenderComplete)
				{
					waitingForRenderComplete = true;
				}
			}
		}

		// A session calls this once before ANY actor sampling. It intentionally
		// does not depend on which (possibly non-participating) actor ticks first.
		void AdvancePerformance(float deltaTime)
		{
			if (state == PlaybackState::Playing && !scrubbing)
			{
				AdvanceClock(deltaTime);

				// Loop handling
				if (currentTime >= TimelineDuration())
				{
					if (loop)
					{
						SyncClockState(0.0);
						audioSeekOffset = 0.0f;
						audioSyncSettleFrames = (fnGetAudioTime != nullptr) ? kAudioSettleFrames : 0;
						audioPllErrorFiltered_ = 0.0;
						if (fnStopAudio) fnStopAudio();
						audioStarted = false;
						if (!StartAudioAtCurrentTime()) { Pause(); return; }
					}
					else
					{
						Stop();
						finished = true;
						return;
					}
				}
			}
		}

		// No audio callbacks, local clock advancement, loop or playlist ownership.
		void SampleAtPerformanceTime(float time, PlaybackState sessionState, bool sessionScrubbing,
			void* actor, void* animator = nullptr, void* director = nullptr)
		{
			SyncClockState(time);
			state = sessionState;
			scrubbing = sessionScrubbing;
			Tick(0.0f, actor, animator, director, false);
		}

		// 主时钟推进：音频可用时以音频为基准，并处理跳转后的 settle 校正。
		// 播放速度修复（方案 B）：
		//   1) settle 结束对 MCI position 连续采样 3 次取平均，抑制毫秒级抖动；
		//   2) 常规帧引入回退容差：音频时间回退时保持 currentTime 不变（等待音频
		//      追上），而不是把动作拉回过去，消除动作偶发倒退。
		//
		// PROBE S1：每次改写 currentTime 都记录「是谁改的」。若拖动提交后
		// currentTime 被拉回，日志里紧跟 END 的那一行会直接指出凶手分支。
		void AdvanceAbsoluteAudioClock(double deltaTime, double rawAudioTime, double prevTime) noexcept
		{
			const double speed = static_cast<double>(playbackSpeedMultiplier);
			const double audioSpeed = static_cast<double>(timing.audio.speed);
			const double audioTime = rawAudioTime / audioSpeed + static_cast<double>(timing.audio.offsetSeconds);
			const double nominalStep = (std::max)(0.0, engineClock_.Time().seconds - prevTime);
			const double predictedTime = prevTime + nominalStep;
			const double rawError = audioTime - predictedTime;

			// Scale snap/hold thresholds with timeline speed so high playback rates
			// absorb ~10-40ms wall-clock mixer steps smoothly without false snaps.
			const double rateScale = (std::max)(0.25, speed);
			const double snapThreshold = kAudioPllHardSnapSeconds * rateScale;
			const double holdThreshold = kAudioPllBackwardHoldSeconds * rateScale;

			if (rawError > snapThreshold)
			{
				audioPllErrorFiltered_ = 0.0;
				SyncClockState(audioTime);
				lastClockSource = "audio";
			}
			else if (rawError < -holdThreshold)
			{
				audioPllErrorFiltered_ = 0.0;
				lastClockSource = "audio(backward-held)";
			}
			else
			{
				if (deltaTime > 0.0 && nominalStep > 0.0)
				{
					const double alpha = 1.0 - std::exp(-deltaTime / kAudioPllFilterTauSeconds);
					audioPllErrorFiltered_ += alpha * (rawError - audioPllErrorFiltered_);
					const double maxAdjust = kAudioPllMaxRateAdjust * nominalStep;
					const double gain = (std::min)(0.5, deltaTime * kAudioPllGainPerSecond);
					const double correction = std::clamp(audioPllErrorFiltered_ * gain, -maxAdjust, maxAdjust);
					const double nextTime = (std::max)(prevTime, predictedTime + correction);
					SyncClockState(nextTime);
				}
				else
				{
					SyncClockState(prevTime);
				}
				lastClockSource = "audio";
			}

			audioSeekOffset = 0.0f;
			audioSyncSettleFrames = 0;
			lastSyncCorrectionSeconds = 0.0f;
		}

		void AdvanceClock(float deltaTime)
		{
			if (!std::isfinite(deltaTime) || deltaTime < 0.0f) return;
			ReconcileExternalCurrentTime();
			engineClock_.SetPlaybackRate(playbackSpeedMultiplier);
			fixedClock_.SetPlaybackRate(playbackSpeedMultiplier);
			fixedClock_.SetFrameRate(clockConfig.fixedFrameRate);
			const double prevTime = preciseTimeSeconds;

			if (clockConfig.runMode == performance::PlaybackRunMode::ForRecording)
			{
				if (audioStarted && fnStopAudio)
				{
					fnStopAudio();
					audioStarted = false;
				}
				audioWaitingForTiming = false;
				if (recordingLeaseId != 0 || clockConfig.source == performance::ClockSource::External)
				{
					lastStepDeltaSeconds = 0.0;
					lastClockSource = "recording(external-lease)";
					return;
				}
				if (waitingForRenderComplete)
				{
					lastStepDeltaSeconds = 0.0;
					lastClockSource = "recording(waiting-render)";
					return;
				}
				fixedClock_.Resume();
				fixedClock_.AdvanceFrames(1);
				SyncClockState(fixedClock_.Time().seconds);
				lastStepDeltaSeconds = (std::max)(0.0, preciseTimeSeconds - prevTime);
				lastClockSource = "recording(fixed-step)";
				return;
			}

			if (clockConfig.source == performance::ClockSource::External)
			{
				lastStepDeltaSeconds = 0.0;
				lastClockSource = "realtime(external-hold)";
				return;
			}

			if (clockConfig.source == performance::ClockSource::FixedStep)
			{
				if (deltaTime > 0.0f)
				{
					fixedClock_.Resume();
					fixedClock_.AdvanceFrames(1);
					SyncClockState(fixedClock_.Time().seconds);
				}
				lastStepDeltaSeconds = (std::max)(0.0, preciseTimeSeconds - prevTime);
				UpdateAudioTiming();
				lastClockSource = "realtime(fixed-step)";
				return;
			}

			engineClock_.Seek(performance::PerformanceTime{preciseTimeSeconds});
			engineClock_.Resume();
			engineClock_.Advance(static_cast<double>(deltaTime));
			const float advanced = static_cast<float>(engineClock_.Time().seconds);

			if (clockConfig.source == performance::ClockSource::EngineDelta)
			{
				SyncClockState(advanced);
				UpdateAudioTiming();
				lastStepDeltaSeconds = (std::max)(0.0, preciseTimeSeconds - prevTime);
				lastClockSource = "realtime(engine-delta)";
				return;
			}

			UpdateAudioTiming();

			const auto fallbackLocalNoAudio = [&]() {
				audioPllErrorFiltered_ = 0.0;
				SyncClockState(advanced);
				UpdateAudioTiming();
				lastStepDeltaSeconds = (std::max)(0.0, preciseTimeSeconds - prevTime);
				lastClockSource = "local(no-audio)";
			};

			float combinedAudioTime = -1.0f;
			bool hasCombinedAudioTime = false;
			if (!fnGetAudioTime || !AudioEligible())
			{
				fallbackLocalNoAudio();
				return;
			}
			if (fnObserveAudioWithTime)
			{
				audio::Observation obs{};
				float obsTime = -1.0f;
				if (fnObserveAudioWithTime(&obs, &obsTime))
				{
					if (!obs.EffectivePlaying())
					{
						fallbackLocalNoAudio();
						return;
					}
					combinedAudioTime = obsTime;
					hasCombinedAudioTime = true;
				}
				else if (fnObserveAudio && !ObserveAudio().EffectivePlaying())
				{
					fallbackLocalNoAudio();
					return;
				}
			}
			else if (fnObserveAudio && !ObserveAudio().EffectivePlaying())
			{
				fallbackLocalNoAudio();
				return;
			}

			// Absolute backends (GakuAudio / MCI) report file position in discrete
			// mixer/device chunks. Phase-locking a continuous local integrator to the
			// smoothed audio error removes frame-to-frame stair-step judder while
			// catching up immediately on startup/seek stalls and holding on backsteps.
			if (audioPositionIsAbsolute)
			{
				const float rawAudioTime = hasCombinedAudioTime ? combinedAudioTime : fnGetAudioTime();
				if (std::isfinite(rawAudioTime) && rawAudioTime >= 0.0f)
				{
					AdvanceAbsoluteAudioClock(static_cast<double>(deltaTime), static_cast<double>(rawAudioTime), prevTime);
				}
				else
				{
					audioPllErrorFiltered_ = 0.0;
					SyncClockState(advanced);
					lastClockSource = "local(audio-query-fail)";
				}
				lastStepDeltaSeconds = (std::max)(0.0, preciseTimeSeconds - prevTime);
				return;
			}

			if (audioSyncSettleFrames > 0)
			{
				--audioSyncSettleFrames;
				SyncClockState(advanced);
				lastStepDeltaSeconds = (std::max)(0.0, preciseTimeSeconds - prevTime);
				lastClockSource = "local(settle)";
				if (audioSyncSettleFrames == 0)
				{
					const float audioT = fnGetAudioTime();
					if (audioT >= 0.0f)
					{
						// 更稳健的 settle：连续采样 3 次取平均，减少 MCI position 抖动
						float sum = audioT;
						int samples = 1;
						const float audioT2 = fnGetAudioTime();
						if (audioT2 >= 0.0f)
						{
							sum += audioT2;
							++samples;
						}
						const float audioT3 = fnGetAudioTime();
						if (audioT3 >= 0.0f)
						{
							sum += audioT3;
							++samples;
						}
						audioSeekOffset = currentTime - ((sum / static_cast<float>(samples)) /
							timing.audio.speed + timing.audio.offsetSeconds);
						lastSyncCorrectionSeconds = audioSeekOffset;
						Probe("[GakuMotion][ProbeS1][PLY] settle-done audioT=%.3f -> offset=%.3f time=%.3f",
						      sum / static_cast<float>(samples), audioSeekOffset, currentTime);
					}
				}
				return;
			}

			// 音频主时钟 + 回退容差：
			//   - 采样时间不早于「当前时间 - 容差」才采信，随音频前进；
			//   - 采样时间回退（MCI 抖动）时保持 currentTime 不变，等待音频追上；
			//   - 音频查询失败（未打开/设备忙）时退回本地推进。
			const float rawAudioT = fnGetAudioTime();
			const float audioT = rawAudioT >= 0.0f ? rawAudioT / timing.audio.speed + timing.audio.offsetSeconds : -1.0f;
			if (audioT >= 0.0f)
			{
				if (audioT + audioSeekOffset >= currentTime - kAudioBackwardToleranceSeconds)
				{
					// PROBE：这一行是「回弹」最可能的凶手。若它出现在 END 之后且
					// 目标值与 END 的 req 不一致，说明跳转根本没落到音频上
					// （fnSeekAudio 缺失或 MCI seek 失败），音频主时钟把动作拽回去了。
					if (std::fabs((audioT + audioSeekOffset) - currentTime) > 0.05f)
					{
						Probe("[GakuMotion][ProbeS1][PLY] audio-clock OVERRIDE audioT=%.3f offset=%.3f "
						      "-> newTime=%.3f (was=%.3f) seeks=%d hasSeekAudio=%d",
						      audioT, audioSeekOffset, audioT + audioSeekOffset, currentTime,
						      seekCount, fnSeekAudio ? 1 : 0);
					}
					SyncClockState(audioT + audioSeekOffset);
					lastClockSource = "audio";
				}
				else
				{
					lastClockSource = "audio(backward-held)";
				}
			}
			else
			{
				SyncClockState(advanced);
				lastClockSource = "local(audio-query-fail)";
			}
			lastStepDeltaSeconds = (std::max)(0.0, preciseTimeSeconds - prevTime);
		}

		// ---- 诊断/面板用的只读访问 ----
		float AudioSyncOffsetSeconds() const noexcept { return audioSeekOffset; }
		int SeekCount() const noexcept { return seekCount; }
		bool IsScrubbing() const noexcept { return scrubbing; }
		// 是否处于「已载入但未播放」的静止预览态（面板需要显式区分它与真正 Stopped，
		// 因为前者仍在逐帧灌姿态，后者已交还给官方待机动画）。
		bool IsPreviewing() const noexcept { return state == PlaybackState::Preview; }

		float ProgressRatio() const noexcept
		{
			if (TimelineDuration() <= 0.0f) return 0.0f;
			float r = currentTime / TimelineDuration();
			return (r < 0.0f) ? 0.0f : ((r > 1.0f) ? 1.0f : r);
		}
	};
} // namespace GakuMotion
