// Resolve IL2CPP exports, Unity Animation/Transform/Component/Director metadata, and field offsets.
static void ResolveUnityIl2CppBindings()
{
	il2cpp_domain_get = reinterpret_cast<Il2CppDomainGetFn>(GetProcAddress(g_gameAssembly, "il2cpp_domain_get"));
	il2cpp_domain_assembly_open = reinterpret_cast<Il2CppDomainAssemblyOpenFn>(GetProcAddress(
		g_gameAssembly, "il2cpp_domain_assembly_open"));
	il2cpp_assembly_get_image = reinterpret_cast<Il2CppAssemblyGetImageFn>(GetProcAddress(
		g_gameAssembly, "il2cpp_assembly_get_image"));
	il2cpp_class_from_name = reinterpret_cast<Il2CppClassFromNameFn>(GetProcAddress(
		g_gameAssembly, "il2cpp_class_from_name"));
	il2cpp_class_get_parent = reinterpret_cast<Il2CppClassGetParentFn>(GetProcAddress(
		g_gameAssembly, "il2cpp_class_get_parent"));
	il2cpp_class_get_method_from_name = reinterpret_cast<Il2CppClassGetMethodFromNameFn>(GetProcAddress(
		g_gameAssembly, "il2cpp_class_get_method_from_name"));
	il2cpp_class_get_type = reinterpret_cast<Il2CppClassGetTypeFn>(GetProcAddress(
		g_gameAssembly, "il2cpp_class_get_type"));
	il2cpp_type_get_object = reinterpret_cast<Il2CppTypeGetObjectFn>(GetProcAddress(
		g_gameAssembly, "il2cpp_type_get_object"));
	il2cpp_class_get_fields = reinterpret_cast<Il2CppClassGetFieldsFn>(GetProcAddress(
		g_gameAssembly, "il2cpp_class_get_fields"));
	il2cpp_field_get_name = reinterpret_cast<Il2CppFieldGetNameFn>(GetProcAddress(
		g_gameAssembly, "il2cpp_field_get_name"));
	il2cpp_field_get_offset = reinterpret_cast<Il2CppFieldGetOffsetFn>(GetProcAddress(
		g_gameAssembly, "il2cpp_field_get_offset"));
	g_safeBones.invoke = reinterpret_cast<GakuRuntime::SafeBoneLookup::RuntimeInvoke>(
		GetProcAddress(g_gameAssembly, "il2cpp_runtime_invoke"));
	g_safeBones.unbox = reinterpret_cast<GakuRuntime::SafeBoneLookup::ObjectUnbox>(
		GetProcAddress(g_gameAssembly, "il2cpp_object_unbox"));
	g_faceRuntime.objectClass = reinterpret_cast<decltype(g_faceRuntime.objectClass)>(
		GetProcAddress(g_gameAssembly, "il2cpp_object_get_class"));
	g_faceRuntime.parentClass = il2cpp_class_get_parent;
	g_faceRuntime.findField = reinterpret_cast<decltype(g_faceRuntime.findField)>(
		GetProcAddress(g_gameAssembly, "il2cpp_class_get_field_from_name"));
	g_faceRuntime.fieldType = reinterpret_cast<decltype(g_faceRuntime.fieldType)>(
		GetProcAddress(g_gameAssembly, "il2cpp_field_get_type"));
	g_faceRuntime.fieldFlags = reinterpret_cast<decltype(g_faceRuntime.fieldFlags)>(
		GetProcAddress(g_gameAssembly, "il2cpp_field_get_flags"));
	g_faceRuntime.typeCode = reinterpret_cast<decltype(g_faceRuntime.typeCode)>(
		GetProcAddress(g_gameAssembly, "il2cpp_type_get_type"));
	g_faceRuntime.readField = reinterpret_cast<decltype(g_faceRuntime.readField)>(
		GetProcAddress(g_gameAssembly, "il2cpp_field_get_value"));
	g_faceRuntime.writeField = reinterpret_cast<decltype(g_faceRuntime.writeField)>(
		GetProcAddress(g_gameAssembly, "il2cpp_field_set_value"));
	g_faceRuntime.findMethod = reinterpret_cast<decltype(g_faceRuntime.findMethod)>(il2cpp_class_get_method_from_name);
	g_faceRuntime.returnType = reinterpret_cast<decltype(g_faceRuntime.returnType)>(
		GetProcAddress(g_gameAssembly, "il2cpp_method_get_return_type"));
	g_faceRuntime.methodFlags = reinterpret_cast<decltype(g_faceRuntime.methodFlags)>(
		GetProcAddress(g_gameAssembly, "il2cpp_method_get_flags"));
	g_faceRuntime.invoke = g_safeBones.invoke;
	g_blinkUnavailableClasses.clear();
	g_highlightUnavailableClasses.clear();

	// 1. Resolve CampusActorController & GetHumanBodyBoneTransform
	const char* campusAsms[] = {
		"campus-submodule.Runtime.dll", "campus-submodule.Runtime", "campus-submodule", nullptr
	};
	for (int i = 0; campusAsms[i] && !g_campusActorControllerKlass; ++i)
	{
		g_campusActorControllerKlass = FindClass(campusAsms[i], "Campus.Common", "CampusActorController");
	}
	if (g_campusActorControllerKlass && il2cpp_class_get_parent)
	{
		g_campusVirtualActorControllerKlass = il2cpp_class_get_parent(g_campusActorControllerKlass);
		if (g_campusVirtualActorControllerKlass)
		{
			g_getBoneTransform = reinterpret_cast<GetBoneTransformFn>(
				FindMethodPointer(g_campusVirtualActorControllerKlass, "GetHumanBodyBoneTransform", 1)
			);
		}
	}
	if (!g_getBoneTransform && g_campusActorControllerKlass)
	{
		g_getBoneTransform = reinterpret_cast<GetBoneTransformFn>(
			FindMethodPointer(g_campusActorControllerKlass, "GetHumanBodyBoneTransform", 1)
		);
	}

	// 2. Resolve Animator methods & type
	g_animatorKlass = FindClass("UnityEngine.AnimationModule.dll", "UnityEngine", "Animator");
	if (g_animatorKlass)
	{
		g_setAnimatorEnabledNative = reinterpret_cast<SetComponentEnabledNativeFn>(FindMethodPointer(
			g_animatorKlass, "set_enabled", 1));
		g_getAnimatorEnabledNative = reinterpret_cast<GetComponentEnabledNativeFn>(FindMethodPointer(
			g_animatorKlass, "get_enabled", 0));
		g_animatorRebindNative = reinterpret_cast<AnimatorRebindNativeFn>(FindMethodPointer(
			g_animatorKlass, "Rebind", 0));
		g_getAnimatorControllerNative = reinterpret_cast<GetAnimatorControllerNativeFn>(FindMethodPointer(
			g_animatorKlass, "get_runtimeAnimatorController", 0));
		g_setAnimatorControllerNative = reinterpret_cast<SetAnimatorControllerNativeFn>(FindMethodPointer(
			g_animatorKlass, "set_runtimeAnimatorController", 1));
		g_animatorGetBoneTransform = reinterpret_cast<AnimatorGetBoneTransformFn>(FindMethodPointer(
			g_animatorKlass, "GetBoneTransform", 1));
		if (il2cpp_class_get_type && il2cpp_type_get_object)
		{
			g_pendingAnimatorTypeObj.store(true, std::memory_order_release);
		}
	}

	// 3. Resolve Component GetComponent methods
	if (il2cpp_class_get_method_from_name)
	{
		g_safeBones.actorBone = g_campusVirtualActorControllerKlass
			                        ? il2cpp_class_get_method_from_name(g_campusVirtualActorControllerKlass,
			                                                            "GetHumanBodyBoneTransform", 1)
			                        : nullptr;
		if (!g_safeBones.actorBone && g_campusActorControllerKlass)
			g_safeBones.actorBone = il2cpp_class_get_method_from_name(g_campusActorControllerKlass,
			                                                          "GetHumanBodyBoneTransform", 1);
		if (g_animatorKlass)
		{
			g_safeBones.animatorBone = il2cpp_class_get_method_from_name(g_animatorKlass, "GetBoneTransform", 1);
			g_safeBones.animatorIsHuman = il2cpp_class_get_method_from_name(g_animatorKlass, "get_isHuman", 0);
			g_safeBones.animatorIsInitialized = il2cpp_class_get_method_from_name(
				g_animatorKlass, "get_isInitialized", 0);
		}
	}
	if (g_hostContext.logInfo)
		g_hostContext.logInfo(
			"[GakuMotion] Guarded bone lookup: invoke=%p actor=%p animator=%p initialized=%p human=%p",
			g_safeBones.invoke, g_safeBones.actorBone, g_safeBones.animatorBone,
			g_safeBones.animatorIsInitialized, g_safeBones.animatorIsHuman);
	g_componentKlass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Component");
	if (g_componentKlass)
	{
		g_getComponentNative = reinterpret_cast<GetComponentFn>(FindMethodPointer(g_componentKlass, "GetComponent", 1));
		g_getComponentInChildrenNative = reinterpret_cast<GetComponentFn>(FindMethodPointer(
			g_componentKlass, "GetComponentInChildren", 1));
		g_getComponentInParentNative = reinterpret_cast<GetComponentFn>(FindMethodPointer(
			g_componentKlass, "GetComponentInParent", 1));
		g_getComponentTransformNative = reinterpret_cast<GetComponentTransformNativeFn>(FindMethodPointer(
			g_componentKlass, "get_transform", 0));
		g_getGameObjectNative = reinterpret_cast<decltype(g_getGameObjectNative)>(FindMethodPointer(
			g_componentKlass, "get_gameObject", 0));
	}

	// 4. Scan actor fields (and parent classes) for Animator caching
	void* curKlass = g_campusActorControllerKlass;
	while (curKlass && g_actorAnimatorFieldOffset < 0)
	{
		if (il2cpp_class_get_fields && il2cpp_field_get_name && il2cpp_field_get_offset)
		{
			void* iter = nullptr;
			while (void* f = il2cpp_class_get_fields(curKlass, &iter))
			{
				const char* fname = il2cpp_field_get_name(f);
				if (fname && (strcmp(fname, "_animator") == 0 || strcmp(fname, "animator") == 0 ||
					strstr(fname, "_animator") || strstr(fname, "Animator")))
				{
					g_actorAnimatorFieldOffset = il2cpp_field_get_offset(f);
					break;
				}
			}
		}
		curKlass = il2cpp_class_get_parent ? il2cpp_class_get_parent(curKlass) : nullptr;
	}

	// 5. Resolve Transform methods (Local & World & Hierarchy)
	g_transformKlass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
	if (g_transformKlass)
	{
		if (il2cpp_class_get_type && il2cpp_type_get_object)
		{
			// Managed System.Type creation is deferred to the Unity main thread.
			g_pendingTransformTypeObj.store(true, std::memory_order_release);
		}
		g_getPosNative = reinterpret_cast<GetLocalPositionNativeFn>(FindMethodPointer(
			g_transformKlass, "get_localPosition", 0));
		g_setPosNative = reinterpret_cast<SetLocalPositionNativeFn>(FindMethodPointer(
			g_transformKlass, "set_localPosition", 1));
		g_getRotNative = reinterpret_cast<GetLocalRotationNativeFn>(FindMethodPointer(
			g_transformKlass, "get_localRotation", 0));
		g_setRotNative = reinterpret_cast<SetLocalRotationNativeFn>(FindMethodPointer(
			g_transformKlass, "set_localRotation", 1));
		g_getWorldPosNative = reinterpret_cast<GetPositionNativeFn>(FindMethodPointer(
			g_transformKlass, "get_position", 0));
		g_getLossyScaleNative = reinterpret_cast<GetPositionNativeFn>(FindMethodPointer(
			g_transformKlass, "get_lossyScale", 0));
		g_setWorldPosNative = reinterpret_cast<SetPositionNativeFn>(FindMethodPointer(
			g_transformKlass, "set_position", 1));
		g_getWorldRotNative = reinterpret_cast<GetRotationNativeFn>(FindMethodPointer(
			g_transformKlass, "get_rotation", 0));
		g_setWorldRotNative = reinterpret_cast<SetRotationNativeFn>(FindMethodPointer(
			g_transformKlass, "set_rotation", 1));
		g_getChildCountNative = reinterpret_cast<GetChildCountNativeFn>(FindMethodPointer(
			g_transformKlass, "get_childCount", 0));
		g_getChildNative = reinterpret_cast<GetChildNativeFn>(FindMethodPointer(g_transformKlass, "GetChild", 1));
		g_getParentNative = reinterpret_cast<GetParentNativeFn>(FindMethodPointer(g_transformKlass, "get_parent", 0));
	}

	void* objKlass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Object");
	if (objKlass)
	{
		g_getNameNative = reinterpret_cast<GetNameNativeFn>(FindMethodPointer(objKlass, "get_name", 0));
	}

	g_gameObjectKlass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
	if (g_gameObjectKlass)
	{
		g_getGameObjectTransformNative = reinterpret_cast<decltype(g_getGameObjectTransformNative)>(
			FindMethodPointer(g_gameObjectKlass, "get_transform", 0));
	}

	// 6. Resolve PlayableDirector methods
	g_playableDirectorKlass = FindClass("UnityEngine.DirectorModule.dll", "UnityEngine.Playables", "PlayableDirector");
	if (g_playableDirectorKlass)
	{
		g_pauseDirectorNative = reinterpret_cast<PlayableDirectorPauseNativeFn>(FindMethodPointer(
			g_playableDirectorKlass, "Pause", 0));
	}
}
