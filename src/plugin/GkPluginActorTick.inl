void MotionActorTick(void*, void* activeActor, float deltaTime)
{
	if (!EnsureRuntimeReadyForActor(activeActor, MotionRuntimeReady)) return;
	if (activeActor) ActorGroundShadow::BindUnityThread();
	ResolveDeferredManagedTypes();
	StageLoader::ResolveDeferredTypeObjects();
	auto& core = GakuMotion::Core::Instance();
	std::lock_guard<std::recursive_mutex> stateLock(core.stateMutex);
	RefreshProtectedContext();
	auto& player = core.player;
	if (activeActor && !g_bodyShadowProbeAttempted)
	{
		// Metadata/native windows precede this module's shadow hook. They do
		// not require an active motion or call any generated game method.
		try { BodyShadowBootstrap::Capture(g_gameAssembly, core.diagnosticsRoot); }
		catch (...) { if (g_hostContext.logWarn) g_hostContext.logWarn("[GakuMotion][BodyShadow] bootstrap failed; native rendering preserved"); }
	}
	// UnityReady may be dispatched by a loader worker. Install on the first
	// actor callback so camera reads/writes and teardown know the Unity thread.
	if (activeActor && !g_cameraAttempted)
	{
		g_cameraAttempted = true;
		if (CameraBridge::Install())
		{
			if (g_hostContext.logInfo) g_hostContext.logInfo(
				"[GakuMotion][Camera] runtime bridge and camera write interception ready");
		}
		else if (g_hostContext.logWarn)
		{
			g_hostContext.logWarn("[GakuMotion][Camera] runtime bridge unavailable; camera control disabled");
		}
	}
	if (activeActor && !g_groundShadowProbeAttempted)
	{
		g_groundShadowProbeAttempted = true;
		const bool probeReady = GroundShadowProbe::Install();
		if (g_hostContext.logInfo) g_hostContext.logInfo(
			"[GakuMotion][GroundShadowProbe] scoped AO bounds adaptation %s (native cache preserved; diagnostic capture is request-driven)",
			probeReady ? "ready" : "unavailable");
	}
	if (activeActor && !g_bodyShadowProbeAttempted)
	{
		g_bodyShadowProbeAttempted = true;
		BodyShadowProbe::Install();
	}

	// Never restore old bindings before scene/actor invalidation. Import polling
	// alone is safe without a dancer; transport commands wait for a valid actor.
	if (!activeActor)
	{
		NativeAudio::Release(GakuMotion::bgm::ReleaseReason::SceneLost);
		core.camera.Release(false);
		core.PollImport();
		core.PollCameraImport();
		return;
	}
	int sceneHandle = 0;
	if (g_getGameObjectNative && g_getSceneNameNative)
	{
		if (void* go = g_getGameObjectNative(activeActor, nullptr))
		{
			if (g_getSceneNative) g_getSceneNative(go, &sceneHandle, nullptr);
			else if (g_getSceneValueNative) sceneHandle = g_getSceneValueNative(go, nullptr);
		}
	}
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
	{
		struct FreshActorSceneState
		{
			int actorScene, unityActive, ownerScene;
			bool activeVerified, matchesOwner;
			std::string actorSceneName;
			bool operator==(const FreshActorSceneState&) const = default;
		};
		const FreshActorSceneState current{sceneHandle, g_verifiedActiveSceneHandle, g_sceneOwner.sceneHandle,
			g_activeSceneVerified, g_sceneOwner.OwnsActor(sceneHandle),
			sceneHandle && g_getSceneNameNative ? Il2CppStringToUtf8(g_getSceneNameNative(sceneHandle, nullptr)) : ""};
		static FreshActorSceneState previous{};
		static bool observed = false;
		if ((!observed || !(current == previous)) && g_hostContext.logInfo)
		{
			g_hostContext.logInfo("[GakuMotion][FreshActorScene] actor_scene_handle=%d actor_scene=%s unity_active_handle=%d active_verified=%d matches_unity_active=%d owner_scene_handle=%d matches_owner=%d",
				current.actorScene, current.actorSceneName.c_str(), current.unityActive, current.activeVerified,
				current.actorScene != 0 && current.actorScene == current.unityActive, current.ownerScene, current.matchesOwner);
			previous = current;
			observed = true;
		}
	}
#endif
	// Match the current logical presenter's owning Scene, never the active
	// additive background. Other loaded actors cannot borrow this ownership.
	if (!g_sceneOwner.Valid() || sceneHandle != g_sceneOwner.sceneHandle) return;
	const bool sceneChanged = sceneHandle != g_currentSceneHandle || !g_boundSceneOwner.SameOwner(g_sceneOwner);
	if (sceneChanged)
	{
		NativeAudio::Release(GakuMotion::bgm::ReleaseReason::SceneLost);
		ClearSecondaryPhysics(true);
		core.stageManager.SetInjectionEnabled(false);
		core.AbandonScene();
		HelperClearActorAnimatorCache();
		g_currentSceneHandle = sceneHandle;
		g_boundSceneOwner = g_sceneOwner;
		const std::string name = g_getSceneNameNative
			                         ? Il2CppStringToUtf8(g_getSceneNameNative(sceneHandle, nullptr))
			                         : "<unavailable>";
		g_currentSceneName = name;
#ifdef GKMOTION_ENABLE_SHOE_MESH_PROBE
		g_shoeProbeSceneName = name;
#endif
		g_motionSceneAllowed = core.InjectionAllowed();
		GM_DEBUG("[GakuMotion][Scene] handle=%d name=%s custom_motion=%s (bindings discarded)",
			sceneHandle, name.c_str(), g_motionSceneAllowed ? "allowed" : "blocked");
	}

	// All actors share one performance clock. Target identity and roster lifetimes are
	// managed by the fresh typed snapshot below, decoupled from Windows wall-clock.
	static int s_lastUnityFrame = -1, s_lastRenderedFrame = -1, s_fallbackFrame = 0;
	static float s_cameraFrameDelta = 0.0333f;
	static float s_unscaledFrameDelta = 0.0333f;
	static uint64_t s_cameraPhysicalFrame = 0;
	const int renderedFrame = g_unityRenderedFrameCount ? g_unityRenderedFrameCount() : -1;
	if (renderedFrame >= 0 && renderedFrame != s_lastRenderedFrame)
	{
		s_lastRenderedFrame = renderedFrame;
		if (!core.IsInOffscreenCapture())
			core.OnMainCameraRendered(false);
	}
	const int unityFrame = g_faceUnityFrame ? g_faceUnityFrame() : -1;
	const bool isNewPhysicalFrame = unityFrame >= 0 ? unityFrame != s_lastUnityFrame : true;
	float realDeltaTime = (std::isfinite(deltaTime) && deltaTime > 0.0f) ? (std::min)(deltaTime, 0.1f) : 0.0f;
	if (isNewPhysicalFrame)
	{
		s_lastUnityFrame = unityFrame;
		++s_fallbackFrame;
		const float unscaledDt = g_unityUnscaledDeltaTime ? g_unityUnscaledDeltaTime() : deltaTime;
		s_unscaledFrameDelta = (std::isfinite(unscaledDt) && unscaledDt > 0.0f)
			? (std::min)(unscaledDt, 0.1f)
			: (realDeltaTime > 0.0f ? realDeltaTime : 0.0333f);
		realDeltaTime = s_unscaledFrameDelta;
		const float simDt = core.EffectiveSimulationDelta(realDeltaTime);
		s_cameraFrameDelta = s_cameraPhysicalFrame == 0 ? 0.0333f : (std::max)(0.0f, simDt);
		// Zero is reserved by CameraController for callers without a frame ID.
		if (++s_cameraPhysicalFrame == 0) ++s_cameraPhysicalFrame;
	}
	if (s_cameraPhysicalFrame == 0) s_cameraPhysicalFrame = 1;
	// Fresh typed snapshots remove disabled/destroyed actors even when no actor
	// callback remains. Never retain an accumulating list of seen scene actors.
	g_lastActorCallbackFrame = unityFrame;
	StageLoader::BindContext(sceneHandle, activeActor);
	// Refresh Photography ensemble observation BEFORE RefreshSceneActors so
	// ActorEligible sees the current Unity frame number instead of lagging by one frame.
	TickPhotographyEnsemble(core, unityFrame);
	RefreshSceneActors(core, sceneChanged, activeActor);
	if (isNewPhysicalFrame) TickPortraitExtractionOnMainThread();
	if (isNewPhysicalFrame) core.ProcessPendingGuiCommands();
	TickPhotographyEnsemble(core, unityFrame, activeActor);
	void* anim = g_motionSceneAllowed && player.state != GakuMotion::PlaybackState::Stopped
		             ? HelperGetActorAnimator(activeActor)
		             : nullptr;

	// 一次性骨骼树探查：首次见到包含上肢骨骼的角色时，探测并打印肩胛骨、大臂、胸腔完整子树
	static std::unordered_set<void*> s_probedActors;
	if (sceneChanged) s_probedActors.clear();
#if defined(GKMOTION_ENABLE_SKELETON_PROBE)
	if (g_motionSceneAllowed && player.state != GakuMotion::PlaybackState::Stopped &&
		s_probedActors.find(activeActor) == s_probedActors.end())
	{
		void* rArm = HelperGetBoneTransform(activeActor, 14); // RightUpperArm
		void* rSh = HelperGetBoneTransform(activeActor, 12); // RightShoulder
		if (rArm || rSh)
		{
			s_probedActors.insert(activeActor);
			if (g_hostContext.logInfo)
			{
				g_hostContext.logInfo("[GakuMotion] ========================================================");
				g_hostContext.logInfo("[GakuMotion] === INITIATING SKELETON TWIST BONE PROBE: ACTOR %p ===",
				                      activeActor);
				g_hostContext.logInfo("[GakuMotion] ========================================================");
			}
			void* uChest = HelperGetBoneTransform(activeActor, 54);
			void* lSh = HelperGetBoneTransform(activeActor, 11);
			void* lArm = HelperGetBoneTransform(activeActor, 13);
			if (uChest) ProbeTransformHierarchy(uChest, "UpperChest", 0, 2);
			if (rSh) ProbeTransformHierarchy(rSh, "RightShoulder", 0, 2);
			if (rArm) ProbeTransformHierarchy(rArm, "RightUpperArm", 0, 2);
			if (lSh) ProbeTransformHierarchy(lSh, "LeftShoulder", 0, 2);
			if (lArm) ProbeTransformHierarchy(lArm, "LeftUpperArm", 0, 2);
			if (g_hostContext.logInfo)
			{
				g_hostContext.logInfo("[GakuMotion] === SKELETON PROBE COMPLETE ===");
			}
		}
	}
#endif

	// 仅在每一帧首个 Actor 到达时检测键盘热键，避免同帧被多次触发
	// 仅在每一帧首个 Actor 到达时检测键盘热键，避免同帧被多次触发
	if (isNewPhysicalFrame) ProcessActorHotkeysAndHeartbeat(core, player);

	// 播放速度修复（方案 A）：动作时钟一律以 realDeltaTime 为基准。
	// - Unity deltaTime 可能被游戏 timeScale 缩放（>1 时动作快于 1.0x）；
	// - 同帧多 Actor 依次进入本函数时，若相邻间隔超过 2ms 会被误判为
	//   「新物理帧」，旧逻辑每 Actor 各推一次满帧 deltaTime，帧内累计
	//   远超真实帧长（实测表现为约 1.2~1.5x 加速播放）。
	// realDeltaTime 由 steady_clock 实测得出，跨帧累计恒等于真实墙钟，
	// 与 MCI 音频时钟同速，从根本上消除音画漂移。
	float dtToPass = realDeltaTime;
	const auto bodyBegin = std::chrono::steady_clock::now();
	const uint64_t poseSerialBefore = player.controller.footPoseWriteStamp.serial;
	const auto* ensembleProbeTrack = core.PlayerForActor(activeActor);
	const uint64_t ensembleProbePoseBefore = ensembleProbeTrack ? ensembleProbeTrack->controller.footPoseWriteStamp.serial : 0;
	EyeGazeProbe::Before(activeActor);
	if (player.controller.targetActor || core.formation.active)
		GakuMotion::OnLateUpdate(activeActor, anim, nullptr, dtToPass, isNewPhysicalFrame,
		                        s_cameraFrameDelta, s_cameraPhysicalFrame);
	EyeGazeProbe::After(activeActor);
	NativeAudio::Sync();
	if (isNewPhysicalFrame)
	{
		void* mainCam = HelperCameraGetMain();
		const bool applyStage = core.InjectionAllowed() && !core.stageStoppedByTransport &&
			core.stageManager.HasCustomizations() && core.PrepareInjection();
		core.stageManager.SetInjectionEnabled(applyStage, mainCam);
		core.stageManager.OnLateUpdate(activeActor, dtToPass, g_currentSceneName, mainCam);
		if (core.InjectionAllowed() && !core.stageManager.candidatesDiscovered)
			core.stageManager.RescanCandidates(activeActor); // Read-only discovery while GM stage controls are inactive.
		core.RefreshSceneNodes();
	}
	// Every actor in the frame must integrate its fallback springs with the same
	// delta, even when later callbacks receive a scaled or zero engine delta.
	RunActorPhysicsAndProbes(core, player, activeActor, sceneHandle, unityFrame, s_fallbackFrame,
	                         s_unscaledFrameDelta, bodyBegin, poseSerialBefore,
	                         ensembleProbeTrack, ensembleProbePoseBefore);
}
