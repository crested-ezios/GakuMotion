
	bool RevokeStudio()
	{
		auto& core = GakuMotion::Core::Instance();
		const bool lights = StudioLighting::Revoke(core.studioLightingNotice);
		const bool video = StudioVideo::Revoke(core.studioVideoNotice);
		return lights && video;
	}

	void TickStudio(GakuMotion::Core& core)
	{
		const bool hadObjects = StudioLighting::HasObjects() || StudioVideo::HasObjects();
		const auto context = StudioRuntime::Current();
		StudioLighting::Tick(core.studioSettings.lighting, context, core.studioLightingNotice);
		StudioVideo::Tick(core.studioSettings.video, context, core.studioVideoNotice);
		const bool hasObjects = StudioLighting::HasObjects() || StudioVideo::HasObjects();
		if (hasObjects && context.allowed) core.customWasActivated = true;
		static auto lastStatus = std::chrono::steady_clock::time_point{};
		const auto now = std::chrono::steady_clock::now();
		if (!hadObjects && !hasObjects && !core.studioSettings.lighting.enabled && !core.studioSettings.video.enabled) return;
		if (now - lastStatus < std::chrono::seconds(1) && !(hadObjects && !hasObjects)) return;
		lastStatus = now;
		const auto quote = [](const std::string& value) { return "\"" + GakuMotion::jsonlite::EscapeString(value) + "\""; };
		const std::string bytes = "{\"schema\":\"gakumotion-stage-studio-status-v1\",\"process_id\":" +
			std::to_string(GetCurrentProcessId()) + ",\"frame\":" + std::to_string(g_faceUnityFrame ? g_faceUnityFrame() : -1) +
			",\"allowed\":" + (context.allowed ? "true" : "false") + ",\"active_scene\":" + quote(context.activeName) +
			",\"lighting_owned\":" + (StudioLighting::HasObjects() ? "true" : "false") +
			",\"lighting_notice\":" + quote(core.studioLightingNotice) +
			",\"video_owned\":" + (StudioVideo::HasObjects() ? "true" : "false") +
			",\"video_decoded_frame\":" + (StudioVideo::runtime.DecodedFrameObserved() ? "true" : "false") +
			",\"video_notice\":" + quote(core.studioVideoNotice) +
			",\"video_diagnostics\":" + quote(StudioVideo::Diagnostics()) +
			",\"photography_diagnostics\":" + StudioDiagnostics::Snapshot(context) + "}\n";
		const auto target = GakuMotion::MotionPathFromUtf8(core.logsRoot) / "stage_studio_status.json";
		auto temporary = target; temporary += ".tmp";
		std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out << bytes; out.close();
		if (out) MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	}

	GakuMotion::injection::OwnerSnapshot g_captureOriginOwner;
	void LogCaptureRestore(const char* phase, const char* reason, bool allowed)
	{
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
		if (!g_hostContext.logInfo) return;
		const auto& core = GakuMotion::Core::Instance();
		const auto& r = core.captureRestoreReport;
		using GakuMotion::capture::StateName;
		g_hostContext.logInfo(
			"[GakuMotion][CaptureSafety] capture_id=%llu type=%s phase=%s scene=%s scene_handle=%d session=%llu capture_session=%llu gm_active=%d lease_depth=%llu body=%s face=%s gaze=%s camera=%s stage=%s physics=%s native_audio=%s allowed=%d reason=%s",
			GakuMotion::capture::CurrentTransactionId(), GakuMotion::capture::CurrentTransactionType(), phase,
			g_verifiedOwnerSceneName.c_str(), g_sceneOwner.sceneHandle, core.injectionSession, core.captureSession,
			core.capturePerformanceWasActive, core.injectionPolicy.Read().captureDepth,
			StateName(r.body), StateName(r.face), StateName(r.gaze), StateName(r.camera), StateName(r.stage),
			StateName(r.physics), StateName(r.nativeAudio), allowed, reason);
#else
		(void)phase; (void)reason; (void)allowed;
#endif
	}

	void ObserveCapture(const GakuMotion::capture::Event& event)
	{
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
		if (!g_hostContext.logInfo) return;
		if (!ProtectionOnUnityThread())
		{
			g_hostContext.logInfo("[GakuMotion][CaptureSafety] capture_id=%llu type=%s phase=%s scene=unavailable session=0 gm_active=%d lease_depth=%u reason=%s",
				event.id, event.type, event.phase, event.gmActive, event.depth, event.reason);
			return;
		}
		const auto& core = GakuMotion::Core::Instance();
		g_hostContext.logInfo(
			"[GakuMotion][CaptureSafety] capture_id=%llu parent_id=%llu type=%s phase=%s scene=%s scene_handle=%d session=%llu capture_session=%llu gm_active=%d guard_active=%d lease_depth=%u state=%d reason=%s",
			event.id, event.parentId, event.type, event.phase, g_currentSceneName.c_str(), g_verifiedActiveSceneHandle,
			core.injectionSession, core.captureSession, event.gmActive, event.gmActive, event.depth, event.state, event.reason);
#else
		(void)event;
#endif
	}

	void ApplyCaptureFailureInjection(GakuMotion::capture::RestoreReport& report)
	{
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
		char value[128]{};
		const auto size = GetEnvironmentVariableA("GAKUMOTION_DEVREVIEW_FAIL_RESTORE", value, sizeof(value));
		if (size && size < sizeof(value)) GakuMotion::capture::ApplyFailures(report, GakuMotion::capture::ParseFailureMask(value));
#else
		(void)report;
#endif
	}

	bool PrepareProtectedInjection()
	{
		// Only a local provenance marker for unknown-scene capture cancellation.
		// It is not a claim that any particular photo is contaminated.
		GakuMotion::Core::Instance().customWasActivated = true;
		return true;
	}

	bool CustomCaptureActive()
	{
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		return core.player.state != GakuMotion::PlaybackState::Stopped ||
			core.camera.owned || core.stageManager.injectionEnabled ||
			StudioLighting::HasObjects() || StudioVideo::HasObjects();
	}

	void ManualCaptureFault(const char* reason)
	{
		// Publish loss of the manual guard immediately. Existing Unity-thread
		// revocation performs native cleanup; this callback never starts capture.
		GakuMotion::Core::Instance().injectionPolicy.SetCaptureHooksReady(false);
		g_pendingProtectionFault.store(true, std::memory_order_release);
		if (g_hostContext.logError) g_hostContext.logError("[GakuMotion][Capture] manual guard disabled: %s", reason);
	}

	void RevokeProtectedInjection()
	{
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		g_restoringNative = true;
		const bool physicsClean = RestoreSecondaryPhysics();
		g_capturePhysicsUnconfirmed |= !physicsClean;
		ClearSecondaryPhysics();
		RevokeStudio();
		core.RevokeInjection();
		NativeAudio::Release(GakuMotion::bgm::ReleaseReason::PolicyInactive);
		g_restoringNative = false;
		g_motionSceneAllowed = false;
	}

	bool ContinueProtectedCapture(const char*);
	bool BeginProtectedCapture(const char* type)
	{
		if (!ProtectionOnUnityThread()) return false;
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		RefreshProtectedContext(true);
		core.injectionPolicy.BeginCapture();
		if (core.injectionPolicy.Read().captureDepth > 1) return ContinueProtectedCapture(type);
		g_captureOriginOwner = g_sceneOwner;
		if (!g_sceneOwner.Valid() && core.customWasActivated && CustomCaptureActive())
		{
			// An unreadable/ambiguous owner is not permission to touch cached old
			// game objects or erase their snapshots. Cancel before native work.
			core.captureRestoreReport = {};
			core.captureRestoreReport.body = GakuMotion::capture::RestoreState::Unconfirmed;
			LogCaptureRestore("restore", "owner-unconfirmed-snapshots-retained", false);
			return false;
		}
		const bool orphansClean = StageLoader::EnsureNoOrphanedStages();
		g_motionSceneAllowed = false;
		g_restoringNative = true;
		const bool physicsModified = HasSecondaryPhysicsWrites();
		const bool physicsClean = RestoreSecondaryPhysics();
		g_capturePhysicsUnconfirmed |= !physicsClean;
		// Preserve failed ownership in each checked provider; do not call the
		// old blind Clear path and erase the evidence before the capture gate.
		if (physicsClean) ClearSecondaryPhysics();
		const bool bgmModified = g_bgmLease.GetSnapshot().ownsMutation || g_bgmLease.GetSnapshot().recoveryPending ||
			g_voiceLease.GetSnapshot().ownsMutation || g_voiceLease.GetSnapshot().recoveryPending ||
			g_seLease.GetSnapshot().ownsMutation || g_seLease.GetSnapshot().recoveryPending;
		const bool studioModified = StudioLighting::HasObjects() || StudioVideo::HasObjects();
		const bool studioClean = RevokeStudio();
		core.SuspendForCapture();
		auto& report = core.captureRestoreReport;
		using GakuMotion::capture::RestoreState;
		report.physics = g_capturePhysicsUnconfirmed ? RestoreState::Unconfirmed :
			physicsModified ? RestoreState::Confirmed : RestoreState::NotModified;
		if (!orphansClean) report.stage = RestoreState::Unconfirmed;
		if (!studioClean) report.stage = RestoreState::Unconfirmed;
		else if (studioModified && report.stage == RestoreState::NotModified) report.stage = RestoreState::Confirmed;
		if (!g_sceneOwner.Valid() && core.customWasActivated) report.body = RestoreState::Unconfirmed;
		if (!NativeAudio::ReleaseForCapture()) report.nativeAudio = RestoreState::Unconfirmed;
		else if (bgmModified && report.nativeAudio != RestoreState::Unconfirmed) report.nativeAudio = RestoreState::Confirmed;
		// Rebind only restores the graph. Evaluate it now so a capture that renders
		// synchronously cannot consume the previous custom skeleton/rest pose.
		if (report.body != RestoreState::NotModified || report.gaze != RestoreState::NotModified)
		{
			auto rebindController = [&](GakuMotion::MotionPlayer* track)
			{
				if (!track) return;
				for (const auto& entry : track->controller.bindings)
				{
					void* animator = HelperGetActorAnimator(entry.first);
					if (!animator || !g_captureAnimatorUpdateMethod) { report.body = RestoreState::Unconfirmed; continue; }
					float zero = 0.0f;
					void* args[]{&zero}; void* exception = nullptr;
					g_faceRuntime.invoke(g_captureAnimatorUpdateMethod, animator, args, &exception);
					if (exception) report.body = RestoreState::Unconfirmed;
				}
			};
			if (core.formation.active)
			{
				for (auto& kv : core.formationTracks) rebindController(kv.second.get());
			}
			else
			{
				rebindController(&core.player);
			}
		}
		g_restoringNative = false;
		ApplyCaptureFailureInjection(report);
		LogCaptureRestore("restore", report.FailureReason(), report.AllConfirmed());
		return report.AllConfirmed();
	}

	bool ContinueProtectedCapture(const char*)
	{
		if (!ProtectionOnUnityThread()) return false;
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		RefreshProtectedContext(true);
		if (!core.captureSuspended || core.captureSession != core.injectionSession ||
			!g_captureOriginOwner.SameOwner(g_sceneOwner))
		{
			LogCaptureRestore("continuation_gate", "scene_or_session_changed", false);
			return false;
		}
		const bool stageDisabled = core.stageManager.SetInjectionEnabled(false);
		const bool orphanStagesClean = StageLoader::EnsureNoOrphanedStages();
		const bool studioClean = RevokeStudio();
		if (!stageDisabled || !orphanStagesClean || !studioClean)
			core.captureRestoreReport.stage = GakuMotion::capture::RestoreState::Unconfirmed;
		ApplyCaptureFailureInjection(core.captureRestoreReport);
		const bool clean = core.captureRestoreReport.AllConfirmed();
		LogCaptureRestore("continuation_gate", core.captureRestoreReport.FailureReason(), clean);
		return clean;
	}

	void EndProtectedCapture(const char*)
	{
		if (!ProtectionOnUnityThread())
		{
			GakuMotion::Core::Instance().injectionPolicy.SetCaptureHooksReady(false);
			g_pendingProtectionFault.store(true, std::memory_order_release);
			return;
		}
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		RefreshProtectedContext(true);
		if (!core.injectionPolicy.EndCapture()) return;
		if (core.injectionPolicy.Read().captureDepth != 0) return;
		g_motionSceneAllowed = core.InjectionAllowed();
		const bool restored = core.ResumeAfterCapture();
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
		if (g_hostContext.logInfo) g_hostContext.logInfo(
			"[GakuMotion][CaptureSafety] capture_id=%llu type=%s phase=resume scene=%s scene_handle=%d session=%llu capture_session=%llu gm_active=%d restored=%d reason=%s",
			GakuMotion::capture::CurrentTransactionId(), GakuMotion::capture::CurrentTransactionType(),
			g_verifiedOwnerSceneName.c_str(), g_sceneOwner.sceneHandle, core.injectionSession, core.captureSession,
			core.capturePerformanceWasActive, restored, core.captureResumeReason);
#else
		(void)restored;
#endif
	}

	void ProtectedCaptureFault(const char* reason)
	{
		auto& core = GakuMotion::Core::Instance();
		if (!ProtectionOnUnityThread())
		{
			core.injectionPolicy.SetCaptureHooksReady(false);
			g_pendingProtectionFault.store(true, std::memory_order_release);
			if (g_hostContext.logError) g_hostContext.logError("[GakuMotion][Capture] off-thread failure: %s", reason);
			return;
		}
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		RefreshProtectedContext(true);
		core.injectionPolicy.SetCaptureHooksReady(false);
		if (g_sceneOwner.Valid()) RevokeProtectedInjection();
		else
		{
			// Keep all body/camera/stage/physics evidence until a confirmed owner
			// permits checked recovery. Do not treat failed discovery as destruction.
			++core.injectionSession;
			core.captureResumeReason = "owner_unconfirmed";
			core.captureRestoreReport.body = GakuMotion::capture::RestoreState::Unconfirmed;
			if (core.player.audioStarted && core.player.fnPauseAudio) core.player.fnPauseAudio();
			core.player.state = GakuMotion::PlaybackState::Stopped;
			core.player.scrubbing = false;
			if (core.fnNotifyMotionPlaying) core.fnNotifyMotionPlaying(false);
			g_motionSceneAllowed = false;
		}
		core.injectionStatus = "截图保护未能确认原生状态；GakuMotion 已禁用，请查看日志。";
		if (g_hostContext.logError) g_hostContext.logError("[GakuMotion][Capture] fail closed: %s", reason);
	}

	void ProtectedPageChanged(bool pageStable)
	{
		if (!ProtectionOnUnityThread())
		{
			ManualCaptureFault("page transition off Unity thread");
			return;
		}
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		RefreshProtectedContext(true);
		// The latch only marks WillChange -> DidChange. Current page identity
		// comes from the fresh exact TopScreen read, including child navigation.
		if (g_sceneOwner.role == GakuMotion::injection::OwnerRole::OutGame && !pageStable)
		{
			core.injectionPolicy.SetSceneAllowed(false);
			if (CustomCaptureActive()) RevokeProtectedInjection();
		}
	}

	void RefreshProtectedContext(bool forceOwnerRead)
	{
		if (!ProtectionOnUnityThread()) return;
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		const bool wasAllowed = core.InjectionAllowed();
		int activeHandle = 0; void* exception = nullptr;
		void* args[]{&activeHandle};
		g_activeSceneVerified = g_activeSceneMethod && g_getSceneNameNative && g_faceRuntime.invoke;
		if (g_activeSceneVerified)
		{
			g_faceRuntime.invoke(g_activeSceneMethod, nullptr, args, &exception);
			g_activeSceneVerified = exception == nullptr && activeHandle != 0;
		}
		static int s_lastSceneNameHandle = 0;
		static int s_lastSceneNameFrame = INT_MIN;
		static bool s_lastSceneNameValid = false;
		static std::string s_lastSceneName;
		std::string name;
		const int unityFrame = g_faceUnityFrame ? g_faceUnityFrame() : -1;
		if (g_activeSceneVerified)
		{
			const bool reuseSceneName = !forceOwnerRead && s_lastSceneNameValid &&
				activeHandle == s_lastSceneNameHandle && unityFrame >= 0 &&
				s_lastSceneNameFrame >= 0 &&
				(unityFrame == s_lastSceneNameFrame ||
					(g_sceneOwner.Valid() && unityFrame >= s_lastSceneNameFrame &&
						unityFrame - s_lastSceneNameFrame < 30));
			if (reuseSceneName)
			{
				name = s_lastSceneName;
			}
			else
			{
				name = Il2CppStringToUtf8(g_getSceneNameNative(activeHandle, nullptr));
				s_lastSceneName = name;
				s_lastSceneNameHandle = activeHandle;
				s_lastSceneNameFrame = unityFrame;
				s_lastSceneNameValid = true;
			}
		}
		else
		{
			s_lastSceneNameValid = false;
			s_lastSceneNameHandle = 0;
			s_lastSceneNameFrame = INT_MIN;
			s_lastSceneName.clear();
		}
		g_verifiedActiveSceneHandle = g_activeSceneVerified ? activeHandle : 0;
		if (GakuMotion::stage_load::receipt.owner.Valid() &&
			SceneOwnerRuntime::ConfirmedUnloaded(GakuMotion::stage_load::receipt.owner.sceneHandle))
			GakuMotion::stage_load::receipt.Clear();
		const auto ownerObservation = SceneOwnerRuntime::Read(name, forceOwnerRead || g_pendingProtectionFault.load(std::memory_order_acquire));
		g_sceneOwner = ownerObservation.owner;
		g_verifiedOwnerSceneName = ownerObservation.sceneName;
		StageLoadOverride::ObserveActiveScene(g_verifiedActiveSceneHandle, name);
		core.injectionPolicy.SetContext(g_activeSceneVerified ? ownerObservation.context :
			GakuMotion::injection::Context::Unknown);
		g_motionSceneAllowed = core.InjectionAllowed();
		const bool oldSceneLost = g_currentSceneHandle != INT_MIN &&
			(g_boundSceneOwner.ReplacedBy(g_sceneOwner) ||
				(!g_sceneOwner.Valid() && SceneOwnerRuntime::ConfirmedUnloaded(g_boundSceneOwner.sceneHandle)));
		if (oldSceneLost)
		{
			// The old Scene may already have destroyed its actors. Never Rebind or
			// restore cached Unity instances here; orphan stages have a fresh scan.
			ClearSecondaryPhysics(true);
			core.AbandonScene();
			HelperClearActorAnimatorCache();
			NativeAudio::Release(GakuMotion::bgm::ReleaseReason::SceneLost);
			g_currentSceneHandle = INT_MIN;
			g_currentSceneName.clear();
			g_boundSceneOwner = {};
		}
		else if (g_sceneOwner.Valid() && ((wasAllowed && !g_motionSceneAllowed) || g_pendingProtectionFault.load(std::memory_order_acquire)))
			RevokeProtectedInjection();
		if (g_sceneOwner.Valid() || oldSceneLost) g_pendingProtectionFault.store(false, std::memory_order_release);
		// A retired asynchronous load can finish even with no actor callbacks.
		StageLoader::EnsureNoOrphanedStages();
		const auto state = core.injectionPolicy.Read();
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
		PageObservation::Poll(CustomCaptureActive());
#endif
		if (state.captureDepth != 0)
			core.injectionStatus = core.captureSuspended
				? "游戏截图事务尚未结束，自定义注入保持暂停；仅在同一有效会话结束后恢复。"
				: "仍有游戏截图事务未结束，GakuMotion 保持暂停；角色或场景变化后不会恢复旧状态。";
		else if (!g_motionSceneAllowed && !core.captureSuspended)
		{
			core.injectionStatus = !state.captureHooksReady
				? "手动拍照保护不可用，GakuMotion 已禁用；设置仍保留，请查看日志。"
				: "仅支持已验证的偶像摄影场景与更衣室；当前页面的自定义设置不生效。";
		}
		else if (core.captureSuspended)
			core.injectionStatus = "游戏正在截图，自定义注入已临时暂停；拍摄结束后恢复。";
		else
			core.injectionStatus.clear();
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
		// Stored scene identities are copied values only. Never dereference an
		// old actor merely to explain an admission failure after a scene change.
		struct AdmissionDiagnosticState
		{
			bool activeVerified;
			int activeHandle;
			std::string activeName, ownerName, rootClass, ownerReason;
			GakuMotion::injection::OwnerSnapshot owner;
			int actorHandle;
			bool sceneAllowed, captureHooksReady;
			uint64_t captureDepth;
			bool suspended;
		};
		const AdmissionDiagnosticState current{g_activeSceneVerified, g_verifiedActiveSceneHandle, name,
			g_verifiedOwnerSceneName, ownerObservation.rootClassName, ownerObservation.reason, g_sceneOwner,
			g_currentSceneHandle, state.sceneAllowed, state.captureHooksReady, state.captureDepth, core.captureSuspended};
		static std::string previous;
		const std::string signature = std::to_string(current.activeHandle) + ":" + current.activeName + ":" +
			std::to_string(current.owner.presenterIdentity) + ":" + std::to_string(current.owner.sceneHandle) + ":" +
			current.ownerReason + ":" + std::to_string(current.actorHandle) + ":" + std::to_string(current.sceneAllowed) + ":" +
			std::to_string(current.captureHooksReady) + ":" + std::to_string(current.captureDepth) + ":" +
			std::to_string(current.suspended) + ":" + std::to_string(current.owner.costumePage);
		if (signature != previous && g_hostContext.logInfo)
		{
			g_hostContext.logInfo(
				"[GakuMotion][Admission] active_verified=%d active_scene_handle=%d active_scene=%s owner_confirmed=%d presenter=%llu presenter_class=%s owner_scene_handle=%d owner_scene=%s actor_scene_handle=%d scene_allowed=%d capture_hooks_ready=%d capture_depth=%llu capture_suspended=%d costume_page=%d reason=%s",
				current.activeVerified, current.activeHandle, current.activeName.c_str(), current.owner.confirmed,
				static_cast<unsigned long long>(current.owner.presenterIdentity), current.rootClass.c_str(),
				current.owner.sceneHandle, current.ownerName.c_str(), current.actorHandle, current.sceneAllowed,
				current.captureHooksReady, current.captureDepth, current.suspended, current.owner.costumePage, current.ownerReason.c_str());
			previous = signature;
		}
#endif
	}

	bool HelperLoadBackground(const char* stageKey)
	{
		return StageLoader::LoadBackground(stageKey);
	}

	bool HelperUnloadBackground()
	{
		return StageLoader::UnloadBackground();
	}

	void HelperSetStageTransform(float x, float y, float z, float rotY)
	{
		StageLoader::SetStageTransform(x, y, z, rotY);
	}

	void HelperSetLoadedStageRoot(void* go)
	{
		StageLoader::SetLoadedStageRoot(go);
	}
