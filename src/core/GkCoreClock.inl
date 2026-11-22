namespace GakuMotion
{
	inline void Core::LoadClockSettings()
	{
		if (configRoot.empty()) return;
		const auto path = configRoot + "/clock.json";
		std::error_code ec;
		if (!std::filesystem::exists(MotionPathFromUtf8(path), ec)) return;
		jsonlite::Value root;
		std::string error;
		if (!jsonlite::ParseFile(path, root, error) || !root.IsObject()) return;

		performance::ClockConfiguration next = player.clockConfig;
		const std::string rm = root.StringPath("runMode", "realtime");
		next.runMode = (rm == "recording") ? performance::PlaybackRunMode::ForRecording : performance::PlaybackRunMode::Realtime;
		const std::string src = root.StringPath("source", "audio");
		if (src == "engine_delta") next.source = performance::ClockSource::EngineDelta;
		else if (src == "fixed_step") next.source = performance::ClockSource::FixedStep;
		else if (src == "external") next.source = performance::ClockSource::External;
		else next.source = performance::ClockSource::Audio;

		const int num = root.IntPath("fpsNumerator", static_cast<int>(next.fixedFrameRate.numerator));
		const int den = root.IntPath("fpsDenominator", static_cast<int>(next.fixedFrameRate.denominator));
		if (num > 0 && den > 0)
			next.fixedFrameRate = { static_cast<uint32_t>(num), static_cast<uint32_t>(den) };

		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		player.SetClockConfiguration(next);
	}

	inline bool Core::SaveClockSettingsLocked() const
	{
		if (configRoot.empty()) return false;
		const auto path = MotionPathFromUtf8(configRoot + "/clock.json");
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) return false;
		out << "{\n"
		    << "  \"runMode\": \"" << performance::RunModeName(player.clockConfig.runMode) << "\",\n"
		    << "  \"source\": \"" << performance::ClockSourceName(player.clockConfig.source) << "\",\n"
		    << "  \"fpsNumerator\": " << player.clockConfig.fixedFrameRate.numerator << ",\n"
		    << "  \"fpsDenominator\": " << player.clockConfig.fixedFrameRate.denominator << "\n"
		    << "}\n";
		return out.good();
	}

	inline GakuStatus Core::SetClockRunMode(uint32_t rawMode)
	{
		if (rawMode != GAKUMOTION_RUN_REALTIME && rawMode != GAKUMOTION_RUN_RECORDING)
			return GAKU_E_INVALID_ARGUMENT;
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		auto next = player.clockConfig;
		next.runMode = (rawMode == GAKUMOTION_RUN_RECORDING)
			? performance::PlaybackRunMode::ForRecording
			: performance::PlaybackRunMode::Realtime;
		player.SetClockConfiguration(next);
		SaveClockSettingsLocked();
		return GAKU_OK;
	}

	inline GakuStatus Core::ConfigureClock(uint32_t clockSource, uint32_t fpsNumerator, uint32_t fpsDenominator)
	{
		if (clockSource > GAKUMOTION_CLOCK_EXTERNAL)
			return GAKU_E_INVALID_ARGUMENT;
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		auto next = player.clockConfig;
		next.source = static_cast<performance::ClockSource>(clockSource);
		if (fpsNumerator > 0 && fpsDenominator > 0)
			next.fixedFrameRate = { fpsNumerator, fpsDenominator };
		player.SetClockConfiguration(next);
		SaveClockSettingsLocked();
		return GAKU_OK;
	}

	inline GakuStatus Core::AcquireRecordingLease(const char* ownerName, uint32_t fpsNumerator, uint32_t fpsDenominator, uint64_t* outLeaseId)
	{
		if (!outLeaseId || fpsNumerator == 0 || fpsDenominator == 0)
			return GAKU_E_INVALID_ARGUMENT;
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		if (player.recordingLeaseId != 0)
			return GAKU_E_CONFLICT;
		const uint64_t lease = nextRecordingLeaseId++;
		if (nextRecordingLeaseId == 0) nextRecordingLeaseId = 1;
		savedClockBeforeLease = player.clockConfig;
		savedLoopBeforeLease = player.loop;
		hasSavedLeaseState = true;

		player.recordingLeaseId = lease;
		player.recordingLeaseOwner = ownerName ? ownerName : "VideoExporter";
		auto next = player.clockConfig;
		next.runMode = performance::PlaybackRunMode::ForRecording;
		next.source = performance::ClockSource::External;
		next.fixedFrameRate = { fpsNumerator, fpsDenominator };
		player.SetClockConfiguration(next);
		player.loop = false;
		*outLeaseId = lease;
		return GAKU_OK;
	}

	inline GakuStatus Core::ReleaseRecordingLease(uint64_t leaseId)
	{
		if (leaseId == 0) return GAKU_E_INVALID_ARGUMENT;
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		if (player.recordingLeaseId != leaseId) return GAKU_E_CONFLICT;
		player.recordingLeaseId = 0;
		player.recordingLeaseOwner.clear();
		if (hasSavedLeaseState)
		{
			player.SetClockConfiguration(savedClockBeforeLease);
			player.loop = savedLoopBeforeLease;
			hasSavedLeaseState = false;
		}
		else
		{
			auto next = player.clockConfig;
			next.runMode = performance::PlaybackRunMode::Realtime;
			if (next.source == performance::ClockSource::External)
				next.source = performance::ClockSource::Audio;
			player.SetClockConfiguration(next);
		}
		return GAKU_OK;
	}

	inline GakuStatus Core::RecordingBegin(uint64_t leaseId, double startSeconds)
	{
		if (leaseId == 0 || !std::isfinite(startSeconds) || startSeconds < 0.0)
			return GAKU_E_INVALID_ARGUMENT;
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		if (player.recordingLeaseId != leaseId) return GAKU_E_CONFLICT;
		if (player.clip.total_frames == 0) return GAKU_E_UNAVAILABLE;
		const double maxDur = static_cast<double>(player.TimelineDuration());
		const double clamped = maxDur > 0.0 ? (std::min)(startSeconds, maxDur) : startSeconds;
		player.loop = false;
		player.finished = false;
		player.scrubbing = false;
		player.waitingForRenderComplete = false;
		player.lastStepDeltaSeconds = 0.0;
		if (player.audioStarted && player.fnStopAudio)
		{
			player.fnStopAudio();
			player.audioStarted = false;
		}
		player.SyncClockState(clamped);
		player.fixedClock_.Resume();
		player.state = PlaybackState::Playing;
		return GAKU_OK;
	}

	inline GakuStatus Core::RecordingStop(uint64_t leaseId)
	{
		if (leaseId == 0) return GAKU_E_INVALID_ARGUMENT;
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		if (player.recordingLeaseId != leaseId) return GAKU_E_CONFLICT;
		// Safe on non-Unity threads: move to Paused rather than invoking native Animator restore off-thread.
		if (player.state == PlaybackState::Playing)
			player.state = PlaybackState::Paused;
		player.waitingForRenderComplete = false;
		player.lastStepDeltaSeconds = 0.0;
		return GAKU_OK;
	}

	inline GakuStatus Core::NotifyFrameRendered(uint64_t leaseId, uint64_t evaluatedFrameSerial, uint32_t advanceFrames)
	{
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		if (leaseId != 0)
		{
			if (player.recordingLeaseId != leaseId) return GAKU_E_CONFLICT;
			if (evaluatedFrameSerial != 0 && evaluatedFrameSerial != player.evaluatedFrameSerial)
				return GAKU_E_CONFLICT;
			player.StepRecordingFrames(advanceFrames);
			return GAKU_OK;
		}
		player.MarkCurrentFrameRendered();
		if (advanceFrames > 0)
			player.StepRecordingFrames(advanceFrames);
		return GAKU_OK;
	}

	inline bool Core::IsInOffscreenCapture() const noexcept
	{
		return offscreenCaptureDepth.load(std::memory_order_acquire) > 0;
	}

	inline GakuStatus Core::BeginOffscreenCapture() noexcept
	{
		offscreenCaptureDepth.fetch_add(1, std::memory_order_acq_rel);
		return GAKU_OK;
	}

	inline GakuStatus Core::EndOffscreenCapture() noexcept
	{
		uint32_t cur = offscreenCaptureDepth.load(std::memory_order_acquire);
		while (cur > 0 &&
		       !offscreenCaptureDepth.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel))
		{
		}
		return GAKU_OK;
	}

	inline void Core::OnMainCameraRendered(bool fromEndFrameHook)
	{
		if (IsInOffscreenCapture()) return;
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		if (fromEndFrameHook)
		{
			renderCompletionHookObserved = true;
		}
		player.gateRecordingByRenderComplete = true;
		player.MarkCurrentFrameRendered();
	}

	inline float Core::EffectiveSimulationDelta(float engineDeltaSeconds)
	{
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		if (player.state == PlaybackState::Playing)
		{
			if (player.clockConfig.runMode == performance::PlaybackRunMode::ForRecording)
				return static_cast<float>(player.lastStepDeltaSeconds);
			if (player.clockConfig.source == performance::ClockSource::FixedStep ||
				player.clockConfig.source == performance::ClockSource::External)
				return static_cast<float>(player.lastStepDeltaSeconds);
		}
		return engineDeltaSeconds;
	}

	inline void Core::InvalidateClockAudioProbeCache() const noexcept
	{
		cachedClockAudioPath_.clear();
		cachedClockAudioExists_ = false;
		cachedClockAudioProbeTime_ = {};
	}

	inline GakuStatus Core::ReadClockSnapshot(GakuMotionClockSnapshotV1* output, bool probeAudioFile)
	{
		if (!output) return GAKU_E_INVALID_ARGUMENT;
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		GakuMotionClockSnapshotV1 snap{};
		snap.abiVersion = GAKU_MOTION_CLOCK_VERSION;
		snap.structSize = sizeof(snap);
		snap.runMode = static_cast<uint32_t>(player.clockConfig.runMode);
		snap.configuredSource = static_cast<uint32_t>(player.clockConfig.source);
		if (player.recordingLeaseId != 0 ||
			(player.clockConfig.runMode == performance::PlaybackRunMode::ForRecording &&
			 player.clockConfig.source == performance::ClockSource::External))
		{
			snap.effectiveSource = GAKUMOTION_CLOCK_EXTERNAL;
		}
		else if (player.clockConfig.runMode == performance::PlaybackRunMode::ForRecording)
		{
			snap.effectiveSource = GAKUMOTION_CLOCK_FIXED_STEP;
		}
		else
		{
			snap.effectiveSource = snap.configuredSource;
		}
		snap.playbackState = static_cast<uint32_t>(player.state);
		snap.fpsNumerator = player.clockConfig.fixedFrameRate.numerator;
		snap.fpsDenominator = player.clockConfig.fixedFrameRate.denominator;
		snap.waitingForRenderComplete = player.waitingForRenderComplete ? 1u : 0u;
		const std::string audioPath = (!player.currentMotionDir.empty()) ? player.AudioPath() : std::string();
		bool audioExists = false;
		if (!audioPath.empty())
		{
			if (probeAudioFile)
			{
				const auto now = std::chrono::steady_clock::now();
				if (audioPath != cachedClockAudioPath_ ||
					cachedClockAudioProbeTime_ == std::chrono::steady_clock::time_point{} ||
					now - cachedClockAudioProbeTime_ >= std::chrono::seconds(1))
				{
					std::error_code ec;
					cachedClockAudioExists_ = std::filesystem::is_regular_file(MotionPathFromUtf8(audioPath), ec);
					cachedClockAudioPath_ = audioPath;
					cachedClockAudioProbeTime_ = now;
				}
				audioExists = cachedClockAudioExists_;
			}
			else if (audioPath == cachedClockAudioPath_)
			{
				audioExists = cachedClockAudioExists_;
			}
		}
		snap.hasAudio = audioExists ? 1u : 0u;
		snap.currentTimeSeconds = player.preciseTimeSeconds;
		snap.timelineDurationSeconds = static_cast<double>(player.TimelineDuration());
		snap.lastStepDeltaSeconds = player.lastStepDeltaSeconds;
		snap.fixedFrameIndex = player.fixedClock_.FrameIndex();
		snap.leaseId = player.recordingLeaseId;
		snap.evaluatedFrameSerial = player.evaluatedFrameSerial;
		snap.audioOffsetSeconds = player.timing.audio.offsetSeconds;
		snap.audioSpeed = player.timing.audio.speed;
		snap.playbackSpeed = player.playbackSpeedMultiplier;
		snap.musicVolume = player.musicVolume;
		std::snprintf(snap.leaseOwner, sizeof(snap.leaseOwner), "%s", player.recordingLeaseOwner.c_str());
		std::string title;
		if (currentMotionIndex >= 0 && currentMotionIndex < static_cast<int>(motions.size()))
			title = motions[currentMotionIndex].title;
		std::snprintf(snap.trackTitle, sizeof(snap.trackTitle), "%s", title.c_str());
		if (audioExists)
			std::snprintf(snap.audioFilePath, sizeof(snap.audioFilePath), "%s", audioPath.c_str());
		const std::string effId = fnActiveAudioEffectId ? fnActiveAudioEffectId() : std::string("flat");
		std::snprintf(snap.activeAudioEffectId, sizeof(snap.activeAudioEffectId), "%s", effId.c_str());
		*output = snap;
		return GAKU_OK;
	}

	inline GakuStatus Core::ReadStageState(GakuMotionStageStateV1* output)
	{
		if (!output) return GAKU_E_INVALID_ARGUMENT;
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		GakuMotionStageStateV1 st{};
		st.abiVersion = GAKU_MOTION_STAGE_STATE_VERSION;
		st.structSize = sizeof(st);
		st.sceneAllowed = (controlActive && initialized && currentActor && !captureSuspended && InjectionAllowed()) ? 1u : 0u;
		st.injectionActive = (stageManager.HasCustomizations() && SceneNodesAllowed()) ? 1u : 0u;
		st.greenScreenActive = stageManager.settings.greenScreenActive ? 1u : 0u;
		st.overrideCameraClear = stageManager.settings.overrideCameraClear ? 1u : 0u;
		st.hideBackground = stageManager.settings.hideBackground ? 1u : 0u;
		for (int i = 0; i < 4; ++i)
			st.greenScreenColor[i] = stageManager.settings.greenScreenColor[i];
		*output = st;
		return GAKU_OK;
	}
} // namespace GakuMotion
