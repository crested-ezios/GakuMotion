	// File-local view of the host services. Field names deliberately match the
	// retired host context so the existing call sites stay unchanged; there is
	// no legacy ABI struct behind it and nothing is exported.
	struct MotionHostContext
	{
		uint32_t apiVersion;
		const wchar_t* gameRoot;
		const wchar_t* localRoot;
		const wchar_t* pluginsRoot;
		void* gameAssemblyHandle;
		void (*logInfo)(const char* fmt, ...);
		void (*logWarn)(const char* fmt, ...);
		void (*logError)(const char* fmt, ...);
		bool (*isVRActive)(void);
		bool (*isVRifyLiveGazeActive)(void);
		void (*setMotionPlaying)(bool playing);
		bool (*isMotionPlaying)(void);
	};

	static void MotionLogInfo(const char* fmt, ...)
	{
		if (!GakuMotion::host::log.write) return;
		char buffer[2048]{};
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buffer, sizeof(buffer), fmt, args);
		va_end(args);
		GakuMotion::host::log.write(GakuMotion::host::log.context, GAKU_LOG_INFO, buffer);
	}

	static void MotionLogWarn(const char* fmt, ...)
	{
		if (!GakuMotion::host::log.write) return;
		char buffer[2048]{};
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buffer, sizeof(buffer), fmt, args);
		va_end(args);
		GakuMotion::host::log.write(GakuMotion::host::log.context, GAKU_LOG_WARNING, buffer);
	}

	static void MotionLogError(const char* fmt, ...)
	{
		if (!GakuMotion::host::log.write) return;
		char buffer[2048]{};
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buffer, sizeof(buffer), fmt, args);
		va_end(args);
		GakuMotion::host::log.write(GakuMotion::host::log.context, GAKU_LOG_ERROR, buffer);
	}

	static void MotionSetMotionPlaying(bool playing) { GakuMotion::host::SetPlaying(playing); }
	static bool MotionIsMotionPlaying() { return GakuMotion::host::IsPlayingFlag(); }

	// Identity-bearing fields of StatusSummary(), with the moving clock removed.
	// Comparing the whole summary would fire on every frame of playback because
	// "time=9.74/110.57s" changes continuously; comparing this fires exactly
	// when something STRUCTURAL changed (track, state, bone count, clock source,
	// override set) or when the user seeked.
	static std::string StatusFingerprint(const std::string& summary)
	{
		std::string out;
		size_t pos = 0;
		while (pos <= summary.size())
		{
			const size_t bar = summary.find(" | ", pos);
			const std::string field = summary.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
			const bool structural = field.rfind("state=", 0) == 0 || field.rfind("motion=", 0) == 0
				|| field.rfind("bones=", 0) == 0 || field.rfind("gaze=", 0) == 0
				|| field.rfind("overrides=", 0) == 0 || field.rfind("clk=", 0) == 0
				|| field.rfind("secondary=", 0) == 0;
			if (structural) out += field;
			else if (field.rfind("seeks=", 0) == 0) out += field.substr(0, field.find(' '));
			out.push_back('|');
			if (bar == std::string::npos) break;
			pos = bar + 3;
		}
		return out;
	}

	MotionHostContext g_hostContext{};
	std::mutex g_lifecycleMutex;
	bool g_pluginInitialized = false;
	std::atomic<bool> g_runtimeReady{false};
	bool g_cameraAttempted = false;
	bool g_groundShadowProbeAttempted = false;
	bool g_bodyShadowProbeAttempted = false;
	GakuMotion::BurstBridgeClient g_burstBridge;
	GakuMotion::FallbackPhysics g_fallbackPhysics;
	GakuPhysicsDiagnosticSnapshot g_physicsDiagnostic{};
	int g_physicsBackend = -1, g_physicsActual = 0, g_physicsFrame = -1, g_physicsMotion = -1;
	int g_physicsSeeks = -1;
	bool g_physicsBeforeQuartz = false, g_physicsSleeveDiagnostics = false;
	float g_physicsTime = -1.0f;
	void* g_physicsActor = nullptr;
	std::string g_physicsReason;
	std::chrono::steady_clock::time_point g_physicsLastTick{};
	bool g_capturePhysicsUnconfirmed = false;
	GakuMotion::BurstBridgeMultiClient g_multiBurstBridge;
	struct EnsemblePhysicsState
	{
		GakuMotion::FallbackPhysics fallback;
		GakuPhysicsDiagnosticSnapshot diagnostic{};
		void* actor{};
		uint64_t actorToken{};
		int backend{-1}, actual{}, frame{-1}, seeks{-1};
		uint64_t session{};
		float time{-1};
		bool beforeQuartz{}, sleeveDiagnostics{}, legacyOwner{};
		int pausedSampleBackend{-1};
		std::string motionFile, reason;
		std::chrono::steady_clock::time_point lastTick{};
	};
	std::deque<EnsemblePhysicsState> g_ensemblePhysics(3);
	uint64_t g_ensemblePhysicsFormationRevision{};

	bool HasEnsemblePhysics()
	{
		for (const auto& slot : g_ensemblePhysics) if (slot.actor || slot.fallback.HasCaptureWrites()) return true;
		return g_multiBurstBridge.HasCaptureInput();
	}
	bool HasSecondaryPhysicsWrites()
	{
		if (g_fallbackPhysics.HasCaptureWrites() || g_burstBridge.HasCaptureInput() || g_multiBurstBridge.HasCaptureInput()) return true;
		for (const auto& slot : g_ensemblePhysics) if (slot.fallback.HasCaptureWrites()) return true;
		return false;
	}
	bool ClearEnsemblePhysicsSlot(size_t index, bool abandoned = false)
	{
		auto& slot = g_ensemblePhysics[index];
		if (!abandoned && (slot.fallback.HasCaptureWrites() || g_multiBurstBridge.State(index).outstanding))
			g_capturePhysicsUnconfirmed = true;
		if (slot.legacyOwner) g_burstBridge.Clear();
		if (!g_multiBurstBridge.Clear(index))
		{
			g_capturePhysicsUnconfirmed = true;
			if (abandoned) slot.fallback.Clear();
			return false;
		}
		slot = EnsemblePhysicsState{};
		return true;
	}
	bool ReconcileEnsemblePhysicsSlots(GakuMotion::Core& core)
	{
		const auto& formation = core.formation;
		if (g_ensemblePhysicsFormationRevision == formation.revision) return true;
		bool clean = true;
		// A slot swap takes effect for the whole formation before the next
		// Actor callback. Retire every displaced input before any new owner
		// publishes so the same native rig cannot appear in two contexts.
		for (size_t i = 0; i < g_ensemblePhysics.size(); ++i)
		{
			auto& slot = g_ensemblePhysics[i];
			if (!slot.actor) continue;
			const auto* observed = core.sceneActorRegistry.Find(slot.actor);
			const bool live = observed && observed->token == slot.actorToken;
			if (live && formation.active && i < formation.slots.size() &&
				formation.slots[i].occupied && formation.slots[i].actorToken == slot.actorToken)
				continue;

			if (live)
			{
				if (slot.fallback.HasCaptureWrites())
				{
					auto* track = core.PlayerForActor(slot.actor);
					if (track && track->controller.targetActor == slot.actor && track->controller.CanDriveActor(slot.actor))
						g_capturePhysicsUnconfirmed |= !slot.fallback.RestoreForCapture(track->controller);
				}
				const bool bridgeClean = slot.legacyOwner ? g_burstBridge.ClearForCapture() : g_multiBurstBridge.ClearForCapture(i);
				g_capturePhysicsUnconfirmed |= !bridgeClean;
			}
			clean = ClearEnsemblePhysicsSlot(i, !live) && clean;
		}
		// Retry failed destruction before allowing a new context to publish.
		if (clean) g_ensemblePhysicsFormationRevision = formation.revision;
		return clean;
	}
	bool RestoreSecondaryPhysics()
	{
		auto& core = GakuMotion::Core::Instance();
		bool clean = g_fallbackPhysics.RestoreForCapture(core.player.controller);
		clean = g_burstBridge.ClearForCapture() && clean;
		for (size_t i = 0; i < g_ensemblePhysics.size(); ++i)
		{
			auto& slot = g_ensemblePhysics[i];
			if (slot.fallback.HasCaptureWrites())
			{
				auto* track = core.PlayerForActor(slot.actor);
				clean = track && track->controller.targetActor == slot.actor
					? slot.fallback.RestoreForCapture(track->controller) && clean : false;
			}
		}
		return g_multiBurstBridge.ClearForCapture() && clean;
	}

	void ClearSecondaryPhysics(bool abandonedScene = false)
	{
		if (abandonedScene) g_capturePhysicsUnconfirmed = false;
		else if (HasSecondaryPhysicsWrites())
			g_capturePhysicsUnconfirmed = true;
		g_burstBridge.Clear();
		g_fallbackPhysics.Clear();
		for (size_t i = 0; i < g_ensemblePhysics.size(); ++i) ClearEnsemblePhysicsSlot(i, abandonedScene);
		g_ensemblePhysicsFormationRevision = 0;
		g_physicsDiagnostic = {};
		g_physicsActual = 0;
		g_physicsFrame = -1;
		g_physicsActor = nullptr;
		g_physicsMotion = -1;
		g_physicsSeeks = -1;
		g_physicsBeforeQuartz = g_physicsSleeveDiagnostics = false;
		g_physicsTime = -1.0f;
		g_physicsLastTick = {};
	}
	bool IsNativePhysicsPending(int requested, const std::string& reason)
	{
		return requested == 2 && (reason == "native_pending" || reason == "native_pending_stale" ||
			reason == "body_binding_pending" || reason == "discovering");
	}
	void SyncFallbackAnimatorMute(GakuMotion::IdolController& ctrl, void* actor,
		const GakuMotion::IdolBoneBinding& binding, bool shouldMute)
	{
		const bool prevMute = ctrl.fallbackMuteAnimator;
		ctrl.fallbackMuteAnimator = shouldMute;
		if (shouldMute && !ctrl.IsAnimatorMuted(actor))
		{
			ctrl.DefensiveMute(actor, binding.cachedAnimator, binding.cachedPlayableDirector);
		}
		else if (!shouldMute && prevMute && ctrl.IsAnimatorMuted(actor))
		{
			ctrl.DefensiveMute(actor, binding.cachedAnimator, binding.cachedPlayableDirector);
		}
	}
	void DispatchEnsemblePhysics(GakuMotion::Core& core, void* actor, int frame,
		float realDeltaTime, std::chrono::steady_clock::time_point bodyBegin, bool filterSceneForces = false)
	{
		auto* track = core.PlayerForActor(actor);
		if (!track) return;
		const int slotIndex = core.SlotIndexForActor(actor);
		if (slotIndex < 0 || static_cast<size_t>(slotIndex) >= core.formation.slots.size()) return;
		const size_t index = static_cast<size_t>(slotIndex);
		const auto actorToken = core.formation.slots[index].actorToken;
		if (!ReconcileEnsemblePhysicsSlots(core) &&
			(index >= g_ensemblePhysics.size() || g_ensemblePhysics[index].actorToken != actorToken))
		{
			core.formation.slots[index].physicsBackend = "capture_destroy_unconfirmed";
			return;
		}
		if (g_ensemblePhysics.size() <= index) g_ensemblePhysics.resize(index + 1);
		auto& slot = g_ensemblePhysics[index];
		auto& ctrl = track->controller;
		auto binding = ctrl.bindings.find(actor);
		const bool canDrive = binding != ctrl.bindings.end() && binding->second.bindPoseCaptured && ctrl.CanDriveActor(actor);
		if (!canDrive || core.player.state == GakuMotion::PlaybackState::Stopped)
		{
			ctrl.fallbackMuteAnimator = false;
			if (slot.actor && canDrive)
			{
				const bool fallbackClean = slot.fallback.RestoreForCapture(ctrl);
				const bool bridgeClean = slot.legacyOwner ? g_burstBridge.ClearForCapture() : g_multiBurstBridge.ClearForCapture(index);
				g_capturePhysicsUnconfirmed |= !fallbackClean || !bridgeClean;
			}
			if (!ClearEnsemblePhysicsSlot(index, !canDrive))
			{ core.formation.slots[index].physicsBackend = "capture_destroy_unconfirmed"; return; }
			core.formation.slots[index].physicsBackend = "off";
			return;
		}
		if (slot.frame == frame && slot.actor == actor) return;
		const int requested = core.config.secondaryPhysicsBackend;
		const bool useV3 = requested == 2 && g_multiBurstBridge.Ensure();
		const bool legacyOwner = requested == 2 && !useV3 && index == 0;
		{
			static std::string s_lastMultiUnavailableReason;
			if (requested == 2 && !useV3)
			{
				if (s_lastMultiUnavailableReason != g_multiBurstBridge.reason)
				{
					s_lastMultiUnavailableReason = g_multiBurstBridge.reason;
					if (g_hostContext.logWarn)
					{
						g_hostContext.logWarn(
							"[GakuMotion][Physics] multi-actor native physics unavailable (%s); slots > 0 using fallback",
							g_multiBurstBridge.reason.c_str());
					}
				}
			}
			else if (useV3)
			{
				s_lastMultiUnavailableReason.clear();
			}
		}
		const bool structuralChange =
			slot.actor != actor || slot.session != core.injectionSession || slot.backend != requested ||
			slot.beforeQuartz != core.config.beforeQuartzExperiment ||
			slot.sleeveDiagnostics != core.config.sleeveMuscleDiagnostics ||
			slot.motionFile != track->loadedMotionFile || slot.legacyOwner != legacyOwner;
		const bool timelineDiscontinuity =
			slot.seeks != core.player.SeekCount() || core.player.scrubbing ||
			core.player.currentTime + .001f < slot.time;
		const bool clearOnTimelineBoundary =
			timelineDiscontinuity && (slot.beforeQuartz || !(useV3 || legacyOwner));
		if (structuralChange || clearOnTimelineBoundary)
		{
			if (slot.actor == actor)
			{
				const bool fallbackClean = slot.fallback.RestoreForCapture(ctrl);
				const bool bridgeClean = slot.legacyOwner ? g_burstBridge.ClearForCapture() : g_multiBurstBridge.ClearForCapture(index);
				g_capturePhysicsUnconfirmed |= !fallbackClean || !bridgeClean;
			}
			if (!ClearEnsemblePhysicsSlot(index, slot.actor != actor))
			{ core.formation.slots[index].physicsBackend = "capture_destroy_unconfirmed"; return; }
			slot.actor = actor; slot.actorToken = actorToken;
			slot.session = core.injectionSession; slot.backend = requested;
			slot.beforeQuartz = core.config.beforeQuartzExperiment; slot.sleeveDiagnostics = core.config.sleeveMuscleDiagnostics;
			slot.motionFile = track->loadedMotionFile; slot.legacyOwner = legacyOwner;
			if (legacyOwner) g_burstBridge.RetryUnavailable();
		}
		else if (timelineDiscontinuity)
		{
			slot.fallback.Clear();
		}
		slot.seeks = core.player.SeekCount(); slot.time = core.player.currentTime; slot.frame = frame;
		const auto dispatchBegin = std::chrono::steady_clock::now();
		const float dt = slot.lastTick.time_since_epoch().count() == 0 ? 0.f : core.EffectiveSimulationDelta(realDeltaTime);
		slot.lastTick = dispatchBegin;
		bool native = false;
		if (useV3) native = g_multiBurstBridge.Publish(index, binding->second, ctrl, slot.time, frame,
			slot.beforeQuartz, slot.sleeveDiagnostics, filterSceneForces);
		else if (legacyOwner) native = g_burstBridge.Publish(binding->second, ctrl, slot.time, frame,
			slot.beforeQuartz, slot.sleeveDiagnostics, filterSceneForces);
		const std::string multiUnavailableReason = g_multiBurstBridge.reason.empty()
			? std::string("fallback_legacy_bridge_master_only")
			: ("fallback_legacy_bridge_master_only (" + g_multiBurstBridge.reason + ")");
		const std::string reason = requested == 0 ? "off" : requested == 1 ? "fallback" :
			useV3 ? g_multiBurstBridge.State(index).reason : legacyOwner ? g_burstBridge.reason : multiUnavailableReason;
		const bool pending = IsNativePhysicsPending(requested, reason);
		// Photography keeps its game graph running while we await native consumption.
		const bool waitForNative = filterSceneForces && pending;
		const int actual = requested == 0 || waitForNative ? 0 : native ? 2 : 1;
		if (slot.actual == 1 && actual != 1) slot.fallback.Restore(ctrl);
		slot.actual = actual;
		SyncFallbackAnimatorMute(ctrl, actor, binding->second, !native && !pending);
		if (slot.actual == 1) slot.fallback.Publish(binding->second, ctrl, dt, frame);
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - dispatchBegin).count();
		auto& d = slot.diagnostic;
		++d.body_calls; d.body_total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(dispatchBegin - bodyBegin).count();
		++d.dispatch_calls; d.dispatch_last_ns = elapsed; d.dispatch_total_ns += elapsed;
		d.dispatch_max_ns = (std::max)(d.dispatch_max_ns, static_cast<uint64_t>(elapsed));
		d.version = GAKU_PHYSICS_DIAGNOSTICS_VERSION; d.size = sizeof(d);
		d.actor = reinterpret_cast<uintptr_t>(actor); ++d.sequence; d.unity_frame = frame;
		d.playback_state = static_cast<int>(core.player.state); d.clip_time = slot.time;
		d.requested_backend = requested; d.actual_backend = slot.actual;
		GbbStatus bridgeStatus{}; GbbPerf bridgePerf{};
		if (useV3) { bridgeStatus = g_multiBurstBridge.State(index).status; bridgePerf = g_multiBurstBridge.State(index).perf; }
		else if (legacyOwner) { bridgeStatus = g_burstBridge.status; bridgePerf = g_burstBridge.perf; }
		d.bridge_state = bridgeStatus.state; d.bridge_fault = bridgeStatus.injection_fault;
		const auto stats = slot.fallback.GetStats();
		d.bound_bones = static_cast<uint32_t>(stats.boundBones); d.solved_bones = static_cast<uint32_t>(stats.solvedBones);
		d.fallback_calls = stats.publishCalls; d.fallback_total_ns = stats.totalCpuNs;
		d.fallback_clamps = stats.clamps; d.fallback_resets = stats.resets;
		d.bridge_publish_calls = bridgePerf.publish_calls; d.bridge_publish_total_ns = bridgePerf.publish_total_ns;
		d.bridge_native_calls = bridgePerf.native_passes; d.bridge_native_total_ns = bridgePerf.native_total_ns;
		d.native_solver_calls = bridgePerf.solver_calls; d.native_solver_total_ns = bridgePerf.solver_total_ns;
		d.bridge_failures = bridgePerf.failed_passes; d.bridge_expired_skips = bridgePerf.expired_skips;
		d.bridge_identity_skips = bridgePerf.identity_skips;
		const bool paused = core.player.state == GakuMotion::PlaybackState::Paused && !core.player.scrubbing;
		if (!paused || timelineDiscontinuity) slot.pausedSampleBackend = -1;
		// The last worker pass can still refer to a playing frame when Pause arrives.
		if (paused && slot.pausedSampleBackend != slot.actual && g_hostContext.logInfo &&
			(!native || std::abs(bridgeStatus.clip_time - slot.time) < 0.001f))
		{
			g_hostContext.logInfo(
				"[GakuMotion][Physics] paused slot=%zu clip=%.3f actual=%d simulation_dt=%.6f "
				"stream_frame=%d stream_dt=%.6f animator_muted=%d filter_scene_forces=%d bridge=%.*s",
				index, slot.time, slot.actual, dt, native ? bridgeStatus.source_frame : -1,
				native ? bridgeStatus.delta_time : 0.f, ctrl.IsAnimatorMuted(actor) ? 1 : 0,
				filterSceneForces ? 1 : 0, static_cast<int>(sizeof(bridgeStatus.detail)), bridgeStatus.detail);
			slot.pausedSampleBackend = slot.actual;
		}
		core.formation.slots[index].physicsBackend = native ? "native" : waitForNative ? "native pending: " + reason :
			slot.actual == 0 ? "off" : "fallback: " + reason;
		if (slot.reason != reason)
		{
			slot.reason = reason;
			if (g_hostContext.logInfo) g_hostContext.logInfo("[GakuMotion][Physics] slot=%zu requested=%d actual=%d reason=%s",
				index, requested, slot.actual, reason.c_str());
		}
	}

	HMODULE g_gameAssembly{nullptr};
