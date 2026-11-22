namespace GakuMotion
{
	inline GakuStatus Core::ReadSelectedActor(uint32_t size, GakuMotionActorSnapshot* output)
	{
		if (!output) return GAKU_E_INVALID_ARGUMENT;
		if (size != sizeof(*output)) return GAKU_E_SIZE;
		std::lock_guard lock(stateMutex);
		if (!controlActive) return GAKU_E_UNAVAILABLE;
		GakuMotionActorSnapshot snapshot{};
		snapshot.abiVersion = GAKU_MOTION_ACTOR_VERSION;
		snapshot.structSize = sizeof(snapshot);
		snapshot.rosterEpoch = sceneActorRegistry.Epoch();
		if (const auto* actor = sceneActorRegistry.Find(player.controller.targetActor))
		{
			if (actor->character.size() >= sizeof(snapshot.character) ||
				actor->displayName.size() >= sizeof(snapshot.displayName) ||
				actor->faceName.size() >= sizeof(snapshot.faceName)) return GAKU_E_SIZE;
			snapshot.actorToken = actor->token;
			std::snprintf(snapshot.character, sizeof(snapshot.character), "%s", actor->character.c_str());
			const char* displayName = actor->displayName.empty() ? LookupIdolChineseName(actor->character.c_str()) :
				actor->displayName.c_str();
			if (displayName) std::snprintf(snapshot.displayName, sizeof(snapshot.displayName), "%s", displayName);
			std::snprintf(snapshot.faceName, sizeof(snapshot.faceName), "%s", actor->faceName.c_str());
		}
		*output = snapshot;
		return GAKU_OK;
	}

	inline bool Core::SceneNodesAllowed() const
	{
		return controlActive && initialized && currentActor && !captureSuspended &&
			InjectionAllowed() && !stageStoppedByTransport && player.state != PlaybackState::Stopped;
	}

	inline void Core::RefreshSceneNodes()
	{
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		stageManager.RefreshSceneNodes(SceneNodesAllowed());
	}

	inline scene_nodes::v1::Status Core::AcquireSceneNodes(uint64_t revision, uint64_t* lease)
	{
		if (lease) *lease = 0;
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		if (!controlActive) return scene_nodes::v1::Status::Unavailable;
		if (captureSuspended || !InjectionAllowed()) return scene_nodes::v1::Status::Protected;
		if (!SceneNodesAllowed()) return scene_nodes::v1::Status::NotReady;
		if (!PrepareInjection()) return scene_nodes::v1::Status::Protected;
		return stageManager.AcquireSceneNodes(revision, lease);
	}

	inline scene_nodes::v1::Status Core::ApplySceneNodes(uint64_t lease, uint64_t revision, uint32_t count, const scene_nodes::v1::Visibility* values)
	{
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		if (!controlActive) return scene_nodes::v1::Status::Unavailable;
		if (captureSuspended || !InjectionAllowed()) return scene_nodes::v1::Status::Protected;
		if (!SceneNodesAllowed()) return scene_nodes::v1::Status::NotReady;
		return stageManager.ApplySceneNodes(lease, revision, count, values);
	}

	inline bool Core::PrepareInjection()
	{
		return InjectionAllowed() && (!fnBeforeInjection || fnBeforeInjection());
	}

	inline bool Core::SuspendForCapture()
	{
		if (captureSuspended) return captureRestoreReport.AllConfirmed();
		InvalidateControlSession();
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
			if (player.fnObserveAudio)
			{
				const auto audioState = player.ObserveAudio();
				captureRestoreReport.nativeAudio = audioState.confirmed && audioState.errorCode == 0 &&
					(audioState.state == audio::PlaybackState::Paused || audioState.state == audio::PlaybackState::Stopped)
					? capture::RestoreState::Confirmed : capture::RestoreState::Unconfirmed;
			}
			else
			{
				captureRestoreReport.nativeAudio = (!enforceInjectionPolicy && player.fnPauseAudio)
					? capture::RestoreState::Confirmed : capture::RestoreState::Unconfirmed;
			}
		}
		// Disabled GUI cancels its drag; retain the sampled time, not a scrub
		// ownership flag whose release event can no longer arrive.
		player.scrubbing = false;
		player.audioStarted = false;
		player.state = PlaybackState::Stopped;
		const auto actorRestore = !formation.active ? player.controller.RestoreForCapture() : BodyFaceRestoreResult{};
		bool ensembleBody = true, ensembleFace = true, ensembleGaze = true;
		bool ensembleBodyModified = false, ensembleFaceModified = false, ensembleGazeModified = false;
		if (formation.active)
		{
			for (auto& kv : formationTracks)
			{
				if (!kv.second) continue;
				const auto r = kv.second->controller.RestoreForCapture();
				kv.second->state = PlaybackState::Stopped;
				ensembleBody &= r.bodyConfirmed;
				ensembleFace &= r.faceConfirmed;
				ensembleGaze &= r.gazeConfirmed;
				ensembleBodyModified |= r.bodyModified;
				ensembleFaceModified |= r.faceModified;
				ensembleGazeModified |= r.gazeModified;
			}
		}
		else
		{
			for (auto& kv : formationTracks)
			{
				if (kv.second) kv.second->state = PlaybackState::Stopped;
			}
		}
		auto status = [](bool modified, bool confirmed) {
			return !confirmed ? capture::RestoreState::Unconfirmed :
				modified ? capture::RestoreState::Confirmed : capture::RestoreState::NotModified;
		};
		captureRestoreReport.body = status(actorRestore.bodyModified || ensembleBodyModified, actorRestore.bodyConfirmed && ensembleBody);
		captureRestoreReport.face = status(actorRestore.faceModified || ensembleFaceModified, actorRestore.faceConfirmed && ensembleFace);
		captureRestoreReport.gaze = status(actorRestore.gazeModified || ensembleGazeModified, actorRestore.gazeConfirmed && ensembleGaze);
		const auto cameraRestore = camera.ReleaseForCapture();
		captureRestoreReport.camera = status(cameraRestore.modified, cameraRestore.confirmed);
		const bool stageModified = stageManager.HasCustomizations();
		captureRestoreReport.stage = status(stageModified, stageManager.SetInjectionEnabled(false));
		NotifyMotionPlaying(false);
		return captureRestoreReport.AllConfirmed();
	}

	inline bool Core::ResumeAfterCapture()
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
		for (auto& kv : formationTracks)
		{
			if (!kv.second) continue;
			kv.second->SyncClockState(player.currentTime);
			kv.second->state = player.state;
		}
		formationFrameWrittenActors.clear();
		formationCameraTicked = false;
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

	inline void Core::RevokeInjection()
	{
		captureResumeReason = "gm_revoked";
		++injectionSession;
		captureSuspended = false;
		if (player.state != PlaybackState::Stopped) player.Stop();
		for (auto& kv : formationTracks) if (kv.second && kv.second->state != PlaybackState::Stopped) kv.second->Stop();
		formationFrameWrittenActors.clear();
		formationCameraTicked = false;
		camera.Release();
		stageManager.SetInjectionEnabled(false);
		NotifyMotionPlaying(false);
	}

	inline void Core::StopPerformance()
	{
		++injectionSession;
		captureSuspended = false;
		captureResumeReason = "gm_stopped";
		stageStoppedByTransport = true;
		player.Stop();
		for (auto& kv : formationTracks) if (kv.second) kv.second->Stop();
		formationFrameWrittenActors.clear();
		formationCameraTicked = false;
		camera.Release();
		stageManager.SetInjectionEnabled(false);
		NotifyMotionPlaying(false);
	}
} // namespace GakuMotion
