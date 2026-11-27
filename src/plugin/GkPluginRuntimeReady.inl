// IL2CPP became resolvable. The loader may dispatch this on a worker thread:
// resolve exports and metadata only; managed object access is deferred to the
// Unity-main-thread callbacks.
void MotionRuntimeReady(void*)
{
	std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
	if (!g_pluginInitialized || g_runtimeReady.load(std::memory_order_acquire)) return;
	if (!g_gameAssembly)
	{
		g_gameAssembly = GetModuleHandleW(L"GameAssembly.dll");
		if (!g_gameAssembly) return;
	}

	ResolveUnityIl2CppBindings();

	// 7. Bind function pointers into GakuMotion Core
	auto& ctrl = GakuMotion::Core::Instance().player.controller;
	g_getSceneNative = reinterpret_cast<decltype(g_getSceneNative)>(FindMethodPointer(
		FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject"), "get_scene_Injected", 1));
	g_getSceneValueNative = reinterpret_cast<decltype(g_getSceneValueNative)>(FindMethodPointer(
		FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject"), "get_scene", 0));
	g_getSceneNameNative = reinterpret_cast<decltype(g_getSceneNameNative)>(FindMethodPointer(
		FindClass("UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement", "Scene"), "GetNameInternal", 1));
	ctrl.fnCanDriveActor = HelperCanDriveActor;
	void* timeClass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Time");
	g_faceUnityFrame = reinterpret_cast<int (*)()>(FindMethodPointer(timeClass, "get_frameCount", 0));
	g_unityUnscaledDeltaTime = reinterpret_cast<float (*)()>(FindMethodPointer(timeClass, "get_unscaledDeltaTime", 0));
	g_unityRenderedFrameCount = reinterpret_cast<int (*)()>(FindMethodPointer(timeClass, "get_renderedFrameCount", 0));
	CameraBridge::onFrameRenderCompleted = []()
	{
		GakuMotion::Core::Instance().OnMainCameraRendered(true);
	};
	if (g_hostContext.logInfo)
		g_hostContext.logInfo(
			"[GakuMotion][Scene] API gameObject=%d injected=%d value=%d name=%d",
			!!g_getGameObjectNative, !!g_getSceneNative, !!g_getSceneValueNative, !!g_getSceneNameNative);
	// Gakumas faces are VLActorFaceModel components with GPU deformation;
	// the base Unity Mesh intentionally contains no SMR blendshape channels.
	const char* faceAsms[] = {"vl-unity.Runtime.dll", "vl-unity.Runtime", "vl-unity", nullptr};
	void* faceClass = nullptr;
	for (int i = 0; faceAsms[i] && !faceClass; ++i)
		faceClass = FindClass(faceAsms[i], "VL.FaceSystem", "VLActorFaceModel");
	if (faceClass && il2cpp_class_get_type && il2cpp_type_get_object)
	{
		g_faceModelClass = faceClass;
		// System.Type for VLActorFaceModel is materialised on the main thread;
		// HelperGetFaceModel returns null until then.
		g_pendingFaceModelTypeObj.store(true, std::memory_order_release);
		g_faceShapeCount = reinterpret_cast<int (*)(void*)>(FindMethodPointer(faceClass, "get_blendShapeLength", 0));
		g_faceShapeName = reinterpret_cast<void* (*)(void*, int)>(FindMethodPointer(faceClass, "GetBlendShapeName", 1));
		g_faceShapeWeight = reinterpret_cast<float (*)(void*, int)>(FindMethodPointer(
			faceClass, "GetBlendShapeWeight", 1));
		g_setFaceShapeWeight = reinterpret_cast<void (*)(void*, int, float)>(FindMethodPointer(
			faceClass, "SetBlendShapeWeight", 2));
	}
	if (faceClass && g_getComponentInChildrenNative && g_faceShapeCount &&
		g_faceShapeName && g_faceShapeWeight && g_setFaceShapeWeight)
	{
		g_faceUnityFrame = reinterpret_cast<int (*)()>(FindMethodPointer(
			FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Time"), "get_frameCount", 0));
		ctrl.faceApi = {
			HelperGetFaceModel, g_faceShapeCount, HelperGetFaceShapeName,
			g_faceShapeWeight, g_setFaceShapeWeight, HelperFaceQpcTicks, g_faceUnityFrame,
			HelperGetFacial, HelperReadBlink, HelperWriteBlink, HelperRefreshHighlight,
			HelperBeginFaceBinding, HelperGetActorName
		};
		ctrl.faceApi.getModelName = HelperGetFaceModelName;
		ctrl.faceApi.reportFacialFit = [](const char* component, const char* decision, const char* faceName)
		{
			GM_INFO("[GakuMotion][FacialFit] component=%s decision=%s actual_face=%s (prefab identity; asset bytes not runtime-verified)",
				component, decision, faceName);
		};
		ctrl.faceApi.reportMouthFit = [](const char* decision, const char* faceName, const char* tableSha256, uint32_t qualityGrade)
		{
			GM_INFO("[GakuMotion][MouthFit] decision=%s actual_face=%s table_sha256=%s quality_grade=%u",
				decision, faceName, tableSha256 ? tableSha256 : "", qualityGrade);
		};
		if (g_hostContext.logInfo)
			g_hostContext.logInfo(
				"[GakuMotion][Face] Named blendshapes use native percentage weights; blink metadata=%d highlight invoke=%d",
				!!g_faceRuntime.findField && !!g_faceRuntime.readField && !!g_faceRuntime.writeField,
				!!g_faceRuntime.invoke && !!g_faceRuntime.returnType && !!g_faceRuntime.methodFlags);
	}
	else
	{
		ctrl.faceApi = {};
		if (g_hostContext.logWarn) g_hostContext.logWarn(
			"[GakuMotion][Face] VLActorFaceModel API incomplete; facial playback unavailable");
	}
	// Talk ownership also applies to body-only clips and does not require the
	// optional VLActorFaceModel blendshape API.
	ctrl.faceApi.getTalk = [](void* actor, int slot) { return g_faceRuntime.Talk(actor, slot); };
	ctrl.faceApi.readTalk = [](void* talk, bool& enabled) { return g_faceRuntime.ReadTalk(talk, enabled); };
	ctrl.faceApi.writeTalk = [](void* talk, bool enabled) { return g_faceRuntime.WriteTalk(talk, enabled); };
	ctrl.fnGetBoneTransform = HelperGetBoneTransform;
	ctrl.fnGetComponentTransform = HelperGetComponentTransform;
	ctrl.fnGetTransformParent = g_getParentNative;
	ctrl.fnGetLocalPosition = HelperGetLocalPos;
	ctrl.fnSetLocalPosition = HelperSetLocalPos;
	ctrl.fnGetLocalRotation = HelperGetLocalRot;
	ctrl.fnSetLocalRotation = HelperSetLocalRot;
	ctrl.fnGetWorldPosition = HelperGetWorldPos;
	if (g_getLossyScaleNative)
	{
		ctrl.fnGetLossyScale = [](void* transform, float* outScale)
		{
			const UnityVector3 value = g_getLossyScaleNative(transform);
			outScale[0] = value.x;
			outScale[1] = value.y;
			outScale[2] = value.z;
		};
	}
	else ctrl.fnGetLossyScale = nullptr;
	ctrl.fnSetWorldPosition = HelperSetWorldPos;
	ctrl.fnGetWorldRotation = HelperGetWorldRot;
	ctrl.fnSetWorldRotation = HelperSetWorldRot;
	ctrl.fnSetAnimatorEnabled = g_setAnimatorEnabledNative ? HelperSetAnimatorEnabled : nullptr;
	ctrl.fnGetAnimatorEnabled = g_getAnimatorEnabledNative ? HelperGetAnimatorEnabled : nullptr;
	ctrl.fnAnimatorRebind = g_animatorRebindNative ? HelperAnimatorRebind : nullptr;
	ctrl.fnGetAnimatorController = g_getAnimatorControllerNative ? HelperGetAnimatorController : nullptr;
	ctrl.fnSetAnimatorController = g_setAnimatorControllerNative ? HelperSetAnimatorController : nullptr;
	ctrl.fnPausePlayableDirector = HelperPauseDirector;
	ctrl.fnGetChildCount = HelperGetChildCount;
	ctrl.fnGetChild = HelperGetChild;
	ctrl.fnGetTransformName = HelperGetTransformName;
	GakuMotion::Core::Instance().fnBeforeTargetChange = []
	{
		const bool physicsClean = RestoreSecondaryPhysics();
		if (g_capturePhysicsUnconfirmed || !physicsClean)
		{
			g_capturePhysicsUnconfirmed = true;
			return false; // Preserve failed ownership snapshots for the capture gate.
		}
		ClearSecondaryPhysics();
		return true;
	};
	ctrl.fnLogInfo = g_hostContext.logInfo;
	ctrl.fnLogWarn = g_hostContext.logWarn;
	GakuMotion::Core::Instance().fnEnsembleActorEligible = PhotographyEnsembleProbe::ActorEligible;

	auto& stageApi = GakuMotion::Core::Instance().stageManager.api;
	stageApi.fnSetActive = HelperGameObjectSetActive;
	stageApi.fnGetActiveSelf = HelperGameObjectGetActiveSelf;
	stageApi.fnTryGetActiveSelf = HelperTryGetActiveSelf;
	stageApi.fnCameraGetMain = HelperCameraGetMain;
	stageApi.fnCameraSetClearFlags = HelperCameraSetClearFlags;
	stageApi.fnCameraGetClearFlags = HelperCameraGetClearFlags;
	stageApi.fnCameraSetBgColor = HelperCameraSetBgColor;
	stageApi.fnCameraGetBgColor = HelperCameraGetBgColor;
	stageApi.fnGetParent = StageLoader::Parent;
	stageApi.fnGetChildCount = StageLoader::ChildCount;
	stageApi.fnGetChild = StageLoader::Child;
	stageApi.fnGetComponentTransform = nullptr; // Root discovery never dereferences a cached actor.
	stageApi.fnGetGameObject = HelperGetGameObject;
	stageApi.fnGetObjectName = HelperGetObjectName;
	stageApi.fnSetWorldPos = HelperSetWorldPos;
	stageApi.fnSetWorldRot = HelperSetWorldRot;
	stageApi.fnGetWorldPos = HelperGetWorldPos;
	stageApi.fnLoadBackground = HelperLoadBackground;
	stageApi.fnUnloadBackground = HelperUnloadBackground;
	stageApi.fnSetStageTransform = HelperSetStageTransform;
	stageApi.fnSetLoadedStageRoot = HelperSetLoadedStageRoot;
	stageApi.fnFindSceneRoots = HelperFindSceneRoots;
	stageApi.fnBeginAccess = StageLoader::BeginAccess;
	stageApi.fnEndAccess = StageLoader::EndAccess;
	stageApi.fnObjectId = StageLoader::ObjectId;
	stageApi.fnGeneration = []() -> uint64_t { return StageLoader::generation; };
	stageApi.fnMatches = StageLoader::Matches;
	stageApi.fnAbandonScene = StageLoader::Abandon;
	stageApi.fnSetMountedStageActive = StageLoader::SetMountedStageActive;
	stageApi.fnIsMountedStageObject = StageLoader::IsMountedStageObject;

	StageLoader::ResolveReflection(g_gameAssembly);
	g_captureAnimatorUpdateMethod = StageLoader::metadata.Find(g_animatorKlass, "Update", false, {0x01}, {{0x0c}});
	void* sceneClass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement", "Scene");
	g_activeSceneMethod = StageLoader::metadata.Find(
		FindClass("UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement", "SceneManager"),
		"GetActiveScene_Injected", true, {0x01}, {{0x11, sceneClass, true}});
	uint32_t sceneAlignment = 0;
	if (!StageLoader::valueSize || StageLoader::valueSize(sceneClass, &sceneAlignment) != sizeof(int))
		g_activeSceneMethod = nullptr;
	const GakuMotion::capture::Callbacks protection{
		CustomCaptureActive, nullptr, nullptr, ManualCaptureFault, ProtectedPageChanged,
		ObserveCapture, nullptr};
	const bool captureReady = GakuMotion::capture::Install(g_gameAssembly, FindClass, protection);
	const bool ownerSceneReady = SceneOwnerRuntime::Resolve();
	StageLoadOverride::nestedTypes = reinterpret_cast<decltype(StageLoadOverride::nestedTypes)>(
		GetProcAddress(g_gameAssembly, "il2cpp_class_get_nested_types"));
	StageLoadOverride::className = reinterpret_cast<decltype(StageLoadOverride::className)>(
		GetProcAddress(g_gameAssembly, "il2cpp_class_get_name"));
	if (ownerSceneReady && g_activeSceneMethod) StageLoadOverride::Install();
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
	PageObservation::Resolve();
#endif
	GakuMotion::Core::Instance().injectionPolicy.SetCaptureHooksReady(
		captureReady && g_activeSceneMethod && ownerSceneReady);
	if (g_hostContext.logInfo) g_hostContext.logInfo(
		"[GakuMotion][Protection] capture=%d animatorEvaluation=%d activeScene=%d ownerScene=%d",
		captureReady, !!g_captureAnimatorUpdateMethod, !!g_activeSceneMethod, ownerSceneReady);

	// Check VRify query
	GakuMotion::Core::Instance().lookAt.fnQueryVRifyGaze = g_hostContext.isVRifyLiveGazeActive;

	if (g_hostContext.logInfo)
	{
		g_hostContext.logInfo(
			"[GakuMotion] Unity runtime resolved. BoneMethod=%p, AnimatorBoneMethod=%p, AnimatorRebind=%p, GetCtrl=%p, SetCtrl=%p, CompTransform=%p, WorldPos=%p, WorldRot=%p (managed System.Type handles deferred to main thread)",
			g_getBoneTransform, g_animatorGetBoneTransform, g_animatorRebindNative, g_getAnimatorControllerNative,
			g_setAnimatorControllerNative, g_getComponentTransformNative, g_setWorldPosNative,
			g_setWorldRotNative);

		// PROBE A4: hard assertion on the IL2CPP bindings this module depends
		// on. Every helper is a silent no-op when its native pointer is null --
		// HelperSetLocalRot() checks `if (g_setRotNative && transform)` and
		// simply returns. A failed FindMethodPointer therefore produces a DLL
		// that loads cleanly, binds "successfully" at the fn-pointer level, and
		// silently writes nothing. That is the same class of silent failure as
		// the parent-chain bug, and it must be ruled out explicitly.
		struct
		{
			const char* name;
			const void* p;
		} binds[] = {
			{"get_localRotation", g_getRotNative},
			{"set_localRotation", g_setRotNative},
			{"get_rotation", g_getWorldRotNative},
			{"set_rotation", g_setWorldRotNative},
			{"get_localPosition", g_getPosNative},
			{"set_localPosition", g_setPosNative},
			{"get_position", g_getWorldPosNative},
			{"set_position", g_setWorldPosNative},
			{"get_childCount", g_getChildCountNative},
			{"GetChild", g_getChildNative},
			{"get_parent", g_getParentNative},
			{"get_boneTransform", g_getBoneTransform},
			{"Animator.Rebind", g_animatorRebindNative},
			{"get_animatorController", g_getAnimatorControllerNative},
			{"set_animatorController", g_setAnimatorControllerNative},
		};
		int missing = 0;
		for (const auto& b : binds)
		{
			if (!b.p)
			{
				++missing;
				g_hostContext.logInfo("[PROBE A4] *** MISSING BINDING: %s -> writes/reads are SILENT NO-OP", b.name);
			}
		}
		g_hostContext.logInfo("[PROBE A4] binding check: %d/%d resolved%s",
		                      static_cast<int>(sizeof(binds) / sizeof(binds[0])) - missing,
		                      static_cast<int>(sizeof(binds) / sizeof(binds[0])),
		                      missing ? " *** SOME BINDINGS MISSING ***" : " (all OK)");

		// PROBE A5: the gkclip path calls setLocalRotation unconditionally. If
		// that pointer is null it degrades to setWorldRotation, which back-
		// subtracts the parent chain and is a known-wrong path for this format.
		// State which branch will actually run, before any frame is played.
		g_hostContext.logInfo("[PROBE A5] gkclip local write path: %s",
		                      g_setRotNative
			                      ? "setLocalRotation (CORRECT)"
			                      : (g_setWorldRotNative
				                         ? "setWorldRotation (DEGRADED -- will back-subtract parent chain)"
				                         : "NEITHER -- gkclip will not write bone rotations at all"));
	}
	{
		GakuMotion::audio::AudioPluginClient::PluginAvailability audioAvail{};
		if (g_audioPlugin.TryClaimUnavailableNotice(audioAvail, true) && g_hostContext.logInfo)
		{
			g_hostContext.logInfo(
				"[GakuMotion][Audio] GakuAudio plugin unavailable via %s (state=%s); skipping %s probes and keeping MCI/offline fallback",
				GAKU_SERVICE_PLUGINS,
				GakuMotion::audio::AudioPluginClient::PluginStateName(audioAvail.state),
				GAKU_SERVICE_AUDIO_CORE);
		}
	}
	g_runtimeReady.store(true, std::memory_order_release);
	if (auto adapter = EnsureAutoToolsAdapter()) adapter->OnRuntimeReady();
}
