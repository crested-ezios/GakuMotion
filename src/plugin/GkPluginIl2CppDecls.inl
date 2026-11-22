	// Typedefs for IL2CPP exported APIs
	using Il2CppDomainGetFn = void* (*)();
	using Il2CppDomainAssemblyOpenFn = void* (*)(void* domain, const char* name);
	using Il2CppAssemblyGetImageFn = void* (*)(void* assembly);
	using Il2CppClassFromNameFn = void* (*)(void* image, const char* namespaze, const char* name);
	using Il2CppClassGetParentFn = void* (*)(void* klass);
	using Il2CppClassGetTypeFn = void* (*)(void* klass);
	using Il2CppTypeGetObjectFn = void* (*)(void* type);
	using Il2CppClassGetFieldsFn = void* (*)(void* klass, void** iter);
	using Il2CppFieldGetNameFn = const char* (*)(void* field);
	using Il2CppFieldGetOffsetFn = int(*)(void* field);

	struct Il2CppMethodInfo
	{
		void* methodPointer;
	};

	using Il2CppClassGetMethodFromNameFn = Il2CppMethodInfo* (*)(void* klass, const char* name, int argsCount);

	Il2CppDomainGetFn il2cpp_domain_get = nullptr;
	Il2CppDomainAssemblyOpenFn il2cpp_domain_assembly_open = nullptr;
	Il2CppAssemblyGetImageFn il2cpp_assembly_get_image = nullptr;
	Il2CppClassFromNameFn il2cpp_class_from_name = nullptr;
	Il2CppClassGetParentFn il2cpp_class_get_parent = nullptr;
	Il2CppClassGetMethodFromNameFn il2cpp_class_get_method_from_name = nullptr;
	Il2CppClassGetTypeFn il2cpp_class_get_type = nullptr;
	Il2CppTypeGetObjectFn il2cpp_type_get_object = nullptr;
	Il2CppClassGetFieldsFn il2cpp_class_get_fields = nullptr;
	Il2CppFieldGetNameFn il2cpp_field_get_name = nullptr;
	Il2CppFieldGetOffsetFn il2cpp_field_get_offset = nullptr;

	void* g_campusActorControllerKlass = nullptr;
	void* g_campusVirtualActorControllerKlass = nullptr;
	void* g_transformKlass = nullptr;
	void* g_animatorKlass = nullptr;
	void* g_componentKlass = nullptr;
	void* g_playableDirectorKlass = nullptr;
	void* g_lookAtControllerKlass = nullptr;

	void* g_animatorTypeObj = nullptr;
	// il2cpp_type_get_object() hands back a managed System.Type, which belongs to
	// the Unity main thread. Runtime-ready may arrive on a loader worker, so the
	// request is deferred to the next actor tick instead of being built here.
	std::atomic<bool> g_pendingAnimatorTypeObj{false};
	std::atomic<bool> g_pendingTransformTypeObj{false};
	std::atomic<bool> g_pendingFaceModelTypeObj{false};
	void* g_faceModelClass = nullptr;
	void* g_faceModelTypeObj = nullptr;
	int (*g_faceShapeCount)(void*) = nullptr;
	void* (*g_faceShapeName)(void*, int) = nullptr;
	float (*g_faceShapeWeight)(void*, int) = nullptr;
	void (*g_setFaceShapeWeight)(void*, int, float) = nullptr;
	GakuMotion::FaceRuntimeAccess g_faceRuntime;
	std::unordered_set<void*> g_blinkUnavailableClasses, g_highlightUnavailableClasses;
	int g_actorAnimatorFieldOffset = -1;

	// Unity Transform & Animator method pointers
	using GetBoneTransformFn = void* (*)(void*, int);
	using AnimatorGetBoneTransformFn = void* (*)(void*, int);
	using GetComponentFn = void* (*)(void*, void*);
	using GetLocalPosFn = void (*)(void*, float*);
	using SetLocalPosFn = void (*)(void*, float, float, float);
	using GetLocalRotFn = void (*)(void*, float*);
	using SetLocalRotFn = void (*)(void*, float, float, float, float);

	GetBoneTransformFn g_getBoneTransform = nullptr;
	AnimatorGetBoneTransformFn g_animatorGetBoneTransform = nullptr;
	GakuRuntime::SafeBoneLookup g_safeBones;
	GetComponentFn g_getComponentNative = nullptr;
	GetComponentFn g_getComponentInChildrenNative = nullptr;
	GetComponentFn g_getComponentInParentNative = nullptr;
	using GetParentNativeFn = void* (*)(void* transform);
	GetParentNativeFn g_getParentNative = nullptr;

	static std::unordered_map<void*, void*> g_actorAnimatorMap;

	void HelperClearActorAnimatorCache()
	{
		g_actorAnimatorMap.clear();
	}

	void* HelperGetActorAnimator(void* actor)
	{
		if (!actor) return nullptr;
		void* resolved = nullptr;

		// 1. Try Component.GetComponentInChildren(Animator)
		if (g_getComponentInChildrenNative && g_animatorTypeObj)
		{
			resolved = g_getComponentInChildrenNative(actor, g_animatorTypeObj);
		}

		// 2. Try Component.GetComponent(Animator)
		if (!resolved && g_getComponentNative && g_animatorTypeObj)
		{
			resolved = g_getComponentNative(actor, g_animatorTypeObj);
		}

		// 3. Try Component.GetComponentInParent(Animator)
		if (!resolved && g_getComponentInParentNative && g_animatorTypeObj)
		{
			resolved = g_getComponentInParentNative(actor, g_animatorTypeObj);
		}

		// 4. Try direct field offset if resolved
		if (!resolved && g_actorAnimatorFieldOffset > 0)
		{
			resolved = *reinterpret_cast<void**>(reinterpret_cast<char*>(actor) + g_actorAnimatorFieldOffset);
		}

		// 5. Try from Hips (bone 0) climbing up parents to find Animator
		if (!resolved && g_getBoneTransform)
		{
			void* hips = g_safeBones.ActorBone(actor, 0);
			if (hips && g_getComponentInParentNative && g_animatorTypeObj)
			{
				resolved = g_getComponentInParentNative(hips, g_animatorTypeObj);
			}
			void* cur = hips;
			for (int depth = 0; cur && depth < 6 && !resolved; ++depth)
			{
				if (g_getComponentNative && g_animatorTypeObj)
				{
					resolved = g_getComponentNative(cur, g_animatorTypeObj);
				}
				if (resolved) break;
				cur = g_getParentNative ? g_getParentNative(cur) : nullptr;
			}
		}

		if (resolved)
		{
			auto it = g_actorAnimatorMap.find(actor);
			if (g_hostContext.logInfo && (it == g_actorAnimatorMap.end() || it->second != resolved))
			{
				g_hostContext.logInfo("[GakuMotion] Resolved Animator=%p for Actor=%p", resolved, actor);
			}
			g_actorAnimatorMap[actor] = resolved;
		}

		return resolved;
	}

	void* HelperGetBoneTransform(void* actor, int boneId)
	{
		if (!actor) return nullptr;

		if (void* result = g_safeBones.ActorBone(actor, boneId)) return result;
		return g_safeBones.AnimatorBone(HelperGetActorAnimator(actor), boneId);
	}

	// IL2CPP struct memory layout for Vector3 and Quaternion
	struct UnityVector3
	{
		float x, y, z;
	};

	struct UnityQuaternion
	{
		float x, y, z, w;
	};

	using GetLocalPositionNativeFn = UnityVector3(*)(void* transform);
	using SetLocalPositionNativeFn = void(*)(void* transform, UnityVector3 pos);
	using GetLocalRotationNativeFn = UnityQuaternion(*)(void* transform);
	using SetLocalRotationNativeFn = void(*)(void* transform, UnityQuaternion rot);
	using GetPositionNativeFn = UnityVector3(*)(void* transform);
	using SetPositionNativeFn = void(*)(void* transform, UnityVector3 pos);
	using GetRotationNativeFn = UnityQuaternion(*)(void* transform);
	using SetRotationNativeFn = void(*)(void* transform, UnityQuaternion rot);
	using GetComponentTransformNativeFn = void* (*)(void* component);
	using SetComponentEnabledNativeFn = void(*)(void* component, bool enabled);
	using GetComponentEnabledNativeFn = bool(*)(void* component);
	using AnimatorRebindNativeFn = void(*)(void* animator);
	using GetAnimatorControllerNativeFn = void* (*)(void* animator);
	using SetAnimatorControllerNativeFn = void(*)(void* animator, void* controller);
	using PlayableDirectorPauseNativeFn = void(*)(void* director);
	using GetChildCountNativeFn = int(*)(void* transform);
	using GetChildNativeFn = void* (*)(void* transform, int index);
	using GetNameNativeFn = void* (*)(void* obj);

	GetLocalPositionNativeFn g_getPosNative = nullptr;
	SetLocalPositionNativeFn g_setPosNative = nullptr;
	GetLocalRotationNativeFn g_getRotNative = nullptr;
	SetLocalRotationNativeFn g_setRotNative = nullptr;
	GetPositionNativeFn g_getWorldPosNative = nullptr;
	GetPositionNativeFn g_getLossyScaleNative = nullptr;
	SetPositionNativeFn g_setWorldPosNative = nullptr;
	GetRotationNativeFn g_getWorldRotNative = nullptr;
	SetRotationNativeFn g_setWorldRotNative = nullptr;
	GetComponentTransformNativeFn g_getComponentTransformNative = nullptr;
	SetComponentEnabledNativeFn g_setAnimatorEnabledNative = nullptr;
	GetComponentEnabledNativeFn g_getAnimatorEnabledNative = nullptr;
	AnimatorRebindNativeFn g_animatorRebindNative = nullptr;
	GetAnimatorControllerNativeFn g_getAnimatorControllerNative = nullptr;
	SetAnimatorControllerNativeFn g_setAnimatorControllerNative = nullptr;
	PlayableDirectorPauseNativeFn g_pauseDirectorNative = nullptr;
	GetChildCountNativeFn g_getChildCountNative = nullptr;
	GetChildNativeFn g_getChildNative = nullptr;
	GetNameNativeFn g_getNameNative = nullptr;

	// Scene is an int handle; injected accessors make its struct ABI explicit.
	void* g_gameObjectKlass = nullptr;
	void* (*g_getGameObjectTransformNative)(void*) = nullptr;
	void* g_transformTypeObj = nullptr;
	void* (*g_getGameObjectNative)(void*, void*) = nullptr;
	void (*g_getSceneNative)(void*, int*, void*) = nullptr;
	int (*g_getSceneValueNative)(void*, void*) = nullptr;
	void* (*g_getSceneNameNative)(int, void*) = nullptr;
	int g_currentSceneHandle = INT_MIN;
	bool g_motionSceneAllowed = false;
	std::string g_currentSceneName;
#ifdef GKMOTION_ENABLE_SHOE_MESH_PROBE
	// Obtained from the actual scene-name API when this actor's scene changes.
	// Kept separate from the custom-motion write/takeover policy.
	std::string g_shoeProbeSceneName;
#endif
	bool g_restoringNative = false;
	const void* g_captureAnimatorUpdateMethod = nullptr;
	const void* g_activeSceneMethod = nullptr;
	bool g_activeSceneVerified = false;
	std::atomic<bool> g_pendingProtectionFault{false};
	int g_verifiedActiveSceneHandle = 0;
	GakuMotion::injection::OwnerSnapshot g_sceneOwner, g_boundSceneOwner;
	std::string g_verifiedOwnerSceneName;

	bool HelperCanDriveActor(void* actor)
	{
		if (g_restoringNative) return true;
		if (!actor || !g_activeSceneVerified || !g_motionSceneAllowed ||
			!GakuMotion::Core::Instance().InjectionAllowed() || !g_getGameObjectNative) return false;
		void* go = g_getGameObjectNative(actor, nullptr);
		int scene = 0;
		if (go && g_getSceneNative) g_getSceneNative(go, &scene, nullptr);
		else if (go && g_getSceneValueNative) scene = g_getSceneValueNative(go, nullptr);
		return g_sceneOwner.OwnsActor(scene);
	}
	bool HelperCanPlay() { return GakuMotion::Core::Instance().PrepareInjection(); }

	struct Il2CppString
	{
		void* klass;
		void* monitor;
		int32_t length;
		wchar_t chars[1];
	};
