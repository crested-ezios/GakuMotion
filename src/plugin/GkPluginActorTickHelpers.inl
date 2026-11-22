// Foreground hotkey polling + periodic heartbeat status logging on new physical frames.
static void ProcessActorHotkeysAndHeartbeat(GakuMotion::Core& core, GakuMotion::MotionPlayer& player)
{
	// ---- 焦点门控 + 上升沿检测（替代旧的`GetAsyncKeyState(vk) & 1`）----
	// 详见上方 IsGameForeground / GkHotkey.hpp 的注释：旧写法的低位 bit 是
	// 粘滞的，会把失焦/掉帧期间的按键在帧循环恢复后一次性补发，造成
	// 「暂停一段时间后无人操作却自动 prev/next 切歌」。
	{
		const bool focused = IsGameForeground();
		g_hotkeys.SetForegroundWindowFocused(focused);
		g_hotkeys.NewFrame();
		// The former "foreground focus in -> out" line fired on every alt-tab
		// and carried no user-visible meaning; the edge baseline rebuild is
		// unconditional and needs no trace.
	}
	auto keyPressed = [&core](int vk)
	{
		const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		const bool pressed = g_hotkeys.Pressed(vk, down);
		const bool editingPanel = core.guiTextInputActive.load(std::memory_order_relaxed) &&
			(!GakuMotion::host::desktopWindow.getWindow ||
				GetForegroundWindow() == static_cast<HWND>(GakuMotion::host::desktopWindow.getWindow()));
		return pressed && core.InjectionAllowed() && !editingPanel;
	};
	// 非焦点期间仍需逐帧喂入按键态，否则焦点切回时会补发历史按键。
	// 为省开销，失焦时只喂会改动作的少数几个键。
	if (!g_hotkeys.ForegroundFocused())
	{
		keyPressed(VK_F9);
		keyPressed(VK_F10);
		keyPressed(VK_OEM_COMMA);
		keyPressed(VK_OEM_PERIOD);
		keyPressed(VK_OEM_MINUS);
		keyPressed(VK_OEM_PLUS);
		keyPressed(VK_OEM_5);
		keyPressed(VK_F8);
		keyPressed(VK_OEM_4);
		keyPressed(VK_OEM_6);
		keyPressed(VK_OEM_1);
	}

	// Hotkey F9: Toggle Play / Pause —— 契约与改动前保持一致
	if (keyPressed(VK_F9))
	{
		core.stageStoppedByTransport = false;
		if (player.state == GakuMotion::PlaybackState::Playing)
		{
			player.Pause();
			NotifyHostMotionPlaying(false);
			if (g_hostContext.logInfo) g_hostContext.logInfo("[GakuMotion] Hotkey F9: Paused | %s",
			                                                 core.StatusSummary().c_str());
		}
		else if (player.state == GakuMotion::PlaybackState::Paused)
		{
			player.Resume();
			NotifyHostMotionPlaying(true);
			if (g_hostContext.logInfo) g_hostContext.logInfo("[GakuMotion] Hotkey F9: Resumed (seek-resume) | %s",
			                                                 core.StatusSummary().c_str());
		}
		else
		{
			// Stopped: 若尚未载入曲目，则按曲目库当前下标补载
			bool startable = true;
			if (player.clip.total_frames == 0)
			{
				if (core.motions.empty()) core.RefreshMotionLibrary();
				int index = core.currentMotionIndex;
				if (index < 0 && !core.motions.empty()) index = 0;
				if (index >= 0)
				{
					core.SelectMotionByIndex(index, false);
				}
				else
				{
					// 2026-11-11: 旧兜底加载 "<motionsRoot>/test_motion"，那是研发用的
					// 固定曲目，用户安装里根本不存在，只会静默播一个空表演。曲库为空
					// 是用户状态，不是隐藏默认曲：说清楚并保持传输状态不变。
					startable = false;
					GM_WARN("[GakuMotion] F9 ignored: the motion library is empty; import a VMD or drop a gkclip first");
				}
			}
			if (startable)
			{
				player.Play();
				NotifyHostMotionPlaying(true);
				GM_INFO("[GakuMotion] Hotkey F9: Started playing | %s",
				        core.StatusSummary().c_str());
			}
		}
	}

	// Hotkey F10: Stop & Reset to start (restores Animator) —— 契约与改动前保持一致
	if (keyPressed(VK_F10))
	{
		player.Seek(0.0f);
		core.StopPerformance();
		NotifyHostMotionPlaying(false);
		if (g_hostContext.logInfo) g_hostContext.logInfo(
			"[GakuMotion] Hotkey F10: Stopped and reset to start (Animator restored & rebinded)");
	}

	// ',' / '.' : 上一曲 / 下一曲
	if (keyPressed(VK_OEM_COMMA))
	{
		core.CycleMotion(-1, "hotkey,");
	}
	if (keyPressed(VK_OEM_PERIOD))
	{
		core.CycleMotion(+1, "hotkey.");
	}

	// '-' / '=' : 进度 -5s / +5s（动作与 MCI 音频同步跳转）
	if (keyPressed(VK_OEM_MINUS))
	{
		player.SeekRelative(-5.0f);
		if (g_hostContext.logInfo) g_hostContext.logInfo(
			"[GakuMotion] Hotkey '-': Seek -5s -> %.2fs (audioOffset=%.3fs)", player.currentTime,
			player.AudioSyncOffsetSeconds());
	}
	if (keyPressed(VK_OEM_PLUS))
	{
		player.SeekRelative(+5.0f);
		if (g_hostContext.logInfo) g_hostContext.logInfo(
			"[GakuMotion] Hotkey '=': Seek +5s -> %.2fs (audioOffset=%.3fs)", player.currentTime,
			player.AudioSyncOffsetSeconds());
	}

	// '\' : 视线模式三态循环 (Auto -> ForceOff -> DesktopOn)
	if (keyPressed(VK_OEM_5))
	{
		core.CycleGazeMode();
	}

	// 'F8' : 辅助骨驱动模式循环 (NORMAL -> SILENCE -> IDENTITY -> REPLAY)
	//        —— 实机袖筒塌陷靶向对照实验：同一帧 features 同一动作，现场 A/B 肉眼裁决。
	if (keyPressed(VK_F8))
	{
		player.controller.CycleAuxDriveMode();
		if (g_hostContext.logInfo)
		{
			g_hostContext.logInfo("[GakuMotion] Hotkey F8: aux bones -> %d = %s (gt tracks=%zu)",
			                      player.controller.auxDriveMode,
			                      GakuMotion::AuxDriveModeName(player.controller.auxDriveMode),
			                      player.controller.auxGroundTruth.tracks.size());
		}
	}

	// Shift+F11/Ctrl+F11 are registered with the Loader so Localify conflicts
	// cannot bypass its registry through this actor polling path.

	// 既有热键：'[' / ']' 臂部 roll 微调；';' 切换 useRestRotations
	if (keyPressed(VK_OEM_4))
	{
		player.controller.armOffsetRoll -= 5.0f;
		if (g_hostContext.logInfo) g_hostContext.logInfo("[GakuMotion] Live Arm Roll Offset: %.1f deg",
		                                                 player.controller.armOffsetRoll);
	}
	if (keyPressed(VK_OEM_6))
	{
		player.controller.armOffsetRoll += 5.0f;
		if (g_hostContext.logInfo) g_hostContext.logInfo("[GakuMotion] Live Arm Roll Offset: %.1f deg",
		                                                 player.controller.armOffsetRoll);
	}
	if (keyPressed(VK_OEM_1))
	{
		player.controller.useRestRotations = !player.controller.useRestRotations;
		if (g_hostContext.logInfo) g_hostContext.logInfo("[GakuMotion] useRestRotations toggled to: %s",
		                                                 player.controller.useRestRotations ? "TRUE" : "FALSE");
	}

	// 状态心跳：结构变化立刻打，否则 30 s 一条。旧版固定 5 s，782 s 的会话里
	// 142 条中 130 条内容完全相同；而切曲/暂停这些真正要看的事件反而淹没在里面。
	static std::string s_lastStatusFingerprint;
	static auto s_lastStatusEmit = std::chrono::steady_clock::now();
	const auto statusNow = std::chrono::steady_clock::now();
	const float sinceStatusEmit = std::chrono::duration<float>(statusNow - s_lastStatusEmit).count();
	if (sinceStatusEmit >= 0.25f) // 去抖：最多 4 次/秒，顺带避免每帧构造摘要
	{
		const std::string summary = core.StatusSummary();
		const std::string fingerprint = StatusFingerprint(summary);
		if (fingerprint != s_lastStatusFingerprint || sinceStatusEmit >= 30.0f)
		{
			s_lastStatusFingerprint = fingerprint;
			s_lastStatusEmit = statusNow;
			// ASCII only: the host decodes log lines as ANSI, so a Chinese
			// literal here arrives as mojibake (was "本帧覆盖生效"). It also
			// adds nothing -- the count is already in StatusSummary()'s
			// "overrides=ON(n)" field.
			GM_INFO("[GakuMotion][Status] %s", summary.c_str());
		}
	}
}

// Secondary physics dispatch, before-quartz burst trace, ground shadow refresh, and opt-in shoe probe.
static void RunActorPhysicsAndProbes(
	GakuMotion::Core& core,
	GakuMotion::MotionPlayer& player,
	void* activeActor,
	int sceneHandle,
	int unityFrame,
	int fallbackFrame,
	float realDeltaTime,
	std::chrono::steady_clock::time_point bodyBegin,
	uint64_t poseSerialBefore,
	const GakuMotion::MotionPlayer* ensembleProbeTrack,
	uint64_t ensembleProbePoseBefore)
{
	if (!core.formation.active && HasEnsemblePhysics())
		for (size_t i = 0; i < g_ensemblePhysics.size(); ++i) ClearEnsemblePhysicsSlot(i, true);
	const bool filterSceneForces = g_sceneOwner.AllowedContext() == GakuMotion::injection::Context::Photography;
	if (core.formation.active)
		DispatchEnsemblePhysics(core, activeActor, unityFrame >= 0 ? unityFrame : fallbackFrame, realDeltaTime, bodyBegin,
			filterSceneForces);
	else if (core.IsTargetActor(activeActor))
	{
		auto binding = player.controller.bindings.find(activeActor);
		const int frame = unityFrame >= 0 ? unityFrame : fallbackFrame;
		const bool canDrive = g_motionSceneAllowed && binding != player.controller.bindings.end() &&
			binding->second.bindPoseCaptured && player.controller.CanDriveActor(activeActor);
		if (!canDrive || player.state == GakuMotion::PlaybackState::Stopped)
		{
			player.controller.fallbackMuteAnimator = false;
			if (canDrive)
			{
				const bool fallbackClean = g_fallbackPhysics.RestoreForCapture(player.controller);
				const bool bridgeClean = g_burstBridge.ClearForCapture();
				g_capturePhysicsUnconfirmed |= !fallbackClean || !bridgeClean;
			}
			if (g_physicsActor) ClearSecondaryPhysics();
		}
		else if (frame != g_physicsFrame || g_physicsActor != activeActor)
		{
			const int requested = core.config.secondaryPhysicsBackend;
			const bool structuralChange =
				requested != g_physicsBackend || core.config.beforeQuartzExperiment != g_physicsBeforeQuartz ||
				core.config.sleeveMuscleDiagnostics != g_physicsSleeveDiagnostics ||
				activeActor != g_physicsActor || core.currentMotionIndex != g_physicsMotion;
			const bool timelineDiscontinuity =
				player.SeekCount() != g_physicsSeeks || player.scrubbing ||
				player.currentTime + .001f < g_physicsTime;
			const bool clearOnTimelineBoundary =
				timelineDiscontinuity && (g_physicsBeforeQuartz || requested != 2);
			if (structuralChange || clearOnTimelineBoundary)
			{
				if (g_physicsActor == activeActor)
				{
					const bool fallbackClean = g_fallbackPhysics.RestoreForCapture(player.controller);
					const bool bridgeClean = g_burstBridge.ClearForCapture();
					g_capturePhysicsUnconfirmed |= !fallbackClean || !bridgeClean;
				}
				ClearSecondaryPhysics();
				g_burstBridge.RetryUnavailable();
				g_physicsBackend = requested;
				g_physicsBeforeQuartz = core.config.beforeQuartzExperiment;
				g_physicsSleeveDiagnostics = core.config.sleeveMuscleDiagnostics;
				g_physicsMotion = core.currentMotionIndex;
				g_physicsActor = activeActor;
			}
			else if (timelineDiscontinuity)
			{
				g_fallbackPhysics.Clear();
			}
			g_physicsSeeks = player.SeekCount();
			g_physicsTime = player.currentTime;
			g_physicsFrame = frame;
			const auto bodyEnd = std::chrono::steady_clock::now();
			auto& d = g_physicsDiagnostic;
			++d.body_calls;
			d.body_total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(bodyEnd - bodyBegin).count();
			const auto dispatchBegin = bodyEnd;
			const float physicalDt = core.EffectiveSimulationDelta(realDeltaTime);
			g_physicsLastTick = dispatchBegin;
			bool native = requested == 2 && g_burstBridge.Publish(binding->second, player.controller,
			                                                      player.currentTime, frame,
			                                                      core.config.beforeQuartzExperiment,
			                                                      core.config.sleeveMuscleDiagnostics, filterSceneForces);
			const std::string reason = requested == 2 ? g_burstBridge.reason : requested == 1 ? "fallback" : "off";
			const bool pending = IsNativePhysicsPending(requested, reason);
			const bool waitForNative = filterSceneForces && pending;
			const int actual = requested == 0 || waitForNative ? 0 : native ? 2 : 1;
			if (g_physicsActual == 1 && actual != 1) g_fallbackPhysics.Restore(player.controller);
			g_physicsActual = actual;
			SyncFallbackAnimatorMute(player.controller, activeActor, binding->second, !native && !pending);
			if (g_physicsActual == 1)
			{
				// Original engine jobs have completed at this LateUpdate callback.
				// Override only secondary local rotations after MMD body writes.
				g_fallbackPhysics.Publish(binding->second, player.controller, physicalDt, frame);
			}
			const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - dispatchBegin).count();
			++d.dispatch_calls;
			d.dispatch_last_ns = elapsed;
			d.dispatch_total_ns += elapsed;
			d.dispatch_max_ns = (std::max)(d.dispatch_max_ns, static_cast<uint64_t>(elapsed));
			d.version = GAKU_PHYSICS_DIAGNOSTICS_VERSION;
			d.size = sizeof(d);
			d.actor = reinterpret_cast<uintptr_t>(activeActor);
			++d.sequence;
			d.unity_frame = frame;
			d.playback_state = static_cast<int>(player.state);
			d.clip_time = player.currentTime;
			d.requested_backend = requested;
			d.actual_backend = g_physicsActual;
			d.bridge_state = g_burstBridge.status.state;
			d.bridge_fault = g_burstBridge.status.injection_fault;
			const auto stats = g_fallbackPhysics.GetStats();
			d.bound_bones = static_cast<uint32_t>(stats.boundBones);
			d.solved_bones = static_cast<uint32_t>(stats.solvedBones);
			d.fallback_calls = stats.publishCalls;
			d.fallback_total_ns = stats.totalCpuNs;
			d.fallback_clamps = stats.clamps;
			d.fallback_resets = stats.resets;
			const auto& perf = g_burstBridge.perf;
			d.bridge_publish_calls = perf.publish_calls;
			d.bridge_publish_total_ns = perf.publish_total_ns;
			d.bridge_native_calls = perf.native_passes;
			d.bridge_native_total_ns = perf.native_total_ns;
			d.native_solver_calls = perf.solver_calls;
			d.native_solver_total_ns = perf.solver_total_ns;
			d.bridge_failures = perf.failed_passes;
			d.bridge_expired_skips = perf.expired_skips;
			d.bridge_identity_skips = perf.identity_skips;
			if (reason != g_physicsReason)
			{
				g_physicsReason = reason;
				// Transient by design: native_pending resolves to native within a
				// frame or two, so this is a trace, not a warning.
				GM_DEBUG("[GakuMotion][Physics] requested=%d actual=%d reason=%s", requested, g_physicsActual,
					reason.c_str());
			}
		}
	}
	if (core.burstTraceRequestedSeq > 0 && (core.IsPerformanceActor(activeActor) || core.IsTargetActor(activeActor)))
	{
		const auto seq = core.burstTraceRequestedSeq;
		core.burstTraceRequestedSeq = 0;
		const std::filesystem::path directory(core.diagnosticsRoot);
		const auto tracePath = (directory / "before_quartz_trace.json").wstring();
		int result = core.formation.active ? g_multiBurstBridge.DumpTraces(tracePath.c_str()) : GBB_E_UNAVAILABLE;
		if (result == GBB_E_UNAVAILABLE) result = g_burstBridge.DumpTraces(tracePath.c_str());
		LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc);
		char json[1024]{};
		std::snprintf(json, sizeof(json),
			"{\"command_seq\":%lld,\"pid\":%lu,\"actor\":\"0x%llx\",\"unity_frame\":%d,"
			"\"clip_time\":%.6f,\"qpc_ticks\":%lld,\"enabled\":%s,\"sleeve_diagnostics\":%s,"
			"\"requested_backend\":%d,\"actual_backend\":%d,\"result\":%d}\n",
			seq, GetCurrentProcessId(), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(activeActor)),
			unityFrame, player.currentTime, qpc.QuadPart, core.config.beforeQuartzExperiment ? "true" : "false",
			core.config.sleeveMuscleDiagnostics ? "true" : "false", core.config.secondaryPhysicsBackend,
			g_physicsActual, result);
		std::ofstream snapshot(directory / "before_quartz_snapshot.json", std::ios::binary | std::ios::trunc);
		snapshot << json;
		if (g_hostContext.logInfo) g_hostContext.logInfo("[GakuMotion][BeforeQuartz] trace seq=%lld result=%d", seq, result);
	}
	// Native foot AO may have been derived before the post-actor callback.
	// Consume the completed body + secondary pose in the same tick.
	ActorGroundShadow::RefreshAfterPose(activeActor, poseSerialBefore);
	GroundShadowProbe::AfterPose(activeActor, poseSerialBefore);
	if (unityFrame > 0)
		if (auto* appliedTrack = core.PlayerForActor(activeActor))
		{
			const auto& stamp = appliedTrack->controller.footPoseWriteStamp;
			if (stamp.actor == activeActor && stamp.serial != ensembleProbePoseBefore && stamp.mask)
				PhotographyEnsembleProbe::NoteApplied(static_cast<uint64_t>(unityFrame), activeActor);
		}
#ifdef GKMOTION_ENABLE_SHOE_MESH_PROBE
	// Independent one-shot geometry evidence. Keep #054's observer and playback
	// untouched; only the explicit request file enables this detached snapshot.
	// Resolve here, not UnityReady: the loader can dispatch UnityReady on a worker.
	static bool shoeProbeResolved = false;
	if (g_motionSceneAllowed && core.IsTargetActor(activeActor) &&
		(player.controller.targetActor || player.controller.currentBinding.actor == activeActor))
	{
		if (!shoeProbeResolved)
		{
			ShoeMeshProbe::Resolve(g_gameAssembly, core.diagnosticsRoot);
			shoeProbeResolved = true;
		}
		ShoeMeshProbe::Tick(activeActor, unityFrame, player.currentTime);
	}
	else if (g_shoeProbeSceneName == "Live")
	{
		// Official Live evidence is independent of custom playback ownership.
		// Exact PID/scene/actor request matching precedes Resolve; no matching
		// request means no probe metadata resolution or managed getter calls.
		ShoeMeshProbe::SnapshotContext context;
		context.source = ShoeMeshProbe::Source::OfficialLive;
		context.processId = GetCurrentProcessId();
		context.sceneHandle = sceneHandle;
		context.sceneName = g_shoeProbeSceneName;
		if (ShoeMeshProbe::OfficialRequestReady(core.diagnosticsRoot, activeActor, context))
		{
			if (!shoeProbeResolved)
			{
				ShoeMeshProbe::Resolve(g_gameAssembly, core.diagnosticsRoot);
				shoeProbeResolved = true;
			}
			// The custom player's time cannot describe an official animation.
			// The context serializes clip_time=null and an unavailable clock.
			ShoeMeshProbe::Tick(activeActor, unityFrame, 0.0f, &context);
		}
	}
#endif
}
