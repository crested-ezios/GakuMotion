#pragma once

#include "GkCameraBridgeHooks.hpp"

namespace CameraBridge
{
	inline void Shutdown()
	{
		sharedBeginEnabled.store(false, std::memory_order_release);
		if (sharedBeginSubscribed) sharedBegin.unsubscribe(sharedBegin.context, SharedCameraBegin, nullptr);
		sharedBeginSubscribed = false;
		sharedBegin = {};
		sharedWritesEnabled.store(false, std::memory_order_release);
		if (sharedWritesSubscribed && sharedWrites.unsubscribe)
		{
			const auto status = sharedWrites.unsubscribe(sharedWrites.context, SharedTransformWritten, nullptr);
			if (status != GAKU_OK && g_hostContext.logWarn) g_hostContext.logWarn(
				"[GakuMotion][Camera] Localify camera-write unsubscribe failed: status=%d", status);
		}
		sharedWritesSubscribed = false;
		sharedWrites = {};
		for (auto& target : sharedWriteTargets) target = nullptr;
		ReleaseVirtualCameras(OnUnityThread());
		Controller().Release(OnUnityThread());
		ready = false;
		// Never disable/uninitialize another module's hooks.
		for (void* target : installed)
		{
			if (GakuMotion::host::active) {
				GakuMotion::host::hooks.release(GakuMotion::host::hooks.context, target);
				continue;
			}
			MH_DisableHook(target);
			MH_RemoveHook(target);
		}
		installed.clear();
		orig_AddRenderPasses = nullptr;
		ownedNativeCamera = ownedNativeTransform = nullptr;
	}

	inline bool Install()
	{
		if (ready) return true;
		unityThreadId = GetCurrentThreadId();
		auto resolve = reinterpret_cast<ResolveIcall>(GetProcAddress(g_gameAssembly, "il2cpp_resolve_icall"));
		if (!RequireDependency("GameAssembly.il2cpp_resolve_icall", reinterpret_cast<void*>(resolve))) return false;
		void* klass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
		void* object = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Object");
		void* transformClass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
		getMain = reinterpret_cast<GetMain>(FindMethodPointer(klass, "get_main", 0));
		alive = reinterpret_cast<Alive>(FindMethodPointer(object, "op_Implicit", 1));
		nativeSelf = FindMethodPointer(klass, "get_fieldOfView_Injected", 1) != nullptr;
		if (nativeSelf)
		{
			bool nativeComplete = ResolveNativeSelfField(object);
			auto paramType = reinterpret_cast<MethodParam>(GetProcAddress(g_gameAssembly, "il2cpp_method_get_param"));
			nativeComplete &= RequireDependency("il2cpp_method_get_param", reinterpret_cast<void*>(paramType));
			nativeComplete &= RequireDependency("il2cpp_method_get_flags",
			                                    reinterpret_cast<void*>(g_faceRuntime.methodFlags));
			const char* transformSetters[] = {
				"set_position_Injected", "set_localPosition_Injected", "set_rotation_Injected",
				"set_localRotation_Injected"
			};
			for (const auto* name : transformSetters) nativeComplete &= RequireNativeSelfMethod(
				transformClass, name, 2, paramType);
			for (const auto* name : {
				     "SetPositionAndRotation_Injected", "SetLocalPositionAndRotation_Injected",
				     "InverseTransformPoint_Injected"
			     })
				nativeComplete &= RequireNativeSelfMethod(transformClass, name, 3, paramType);
			for (const auto* name : {
				     "get_fieldOfView_Injected", "get_orthographicSize_Injected", "get_orthographic_Injected"
			     })
				nativeComplete &= RequireNativeSelfMethod(klass, name, 1, paramType);
			for (const auto* name : {
				     "set_fieldOfView_Injected", "set_orthographicSize_Injected", "set_orthographic_Injected"
			     })
				nativeComplete &= RequireNativeSelfMethod(klass, name, 2, paramType);
			if (!nativeComplete) return false;
		}
		// The old ABI is accepted only by its exact signature. New Unity versions
		// expose static _Injected calls taking the native m_CachedPtr IntPtr.
		auto resolveCall = [&](const char* legacy, const char* current)
		{
			return nativeSelf ? ResolveCameraIcall(resolve, current) : resolve(legacy);
		};
		inversePoint = reinterpret_cast<InversePoint>(resolveCall(
			"UnityEngine.Transform::InverseTransformPoint_Injected(UnityEngine.Vector3&,UnityEngine.Vector3&)",
			"UnityEngine.Transform::InverseTransformPoint_Injected(System.IntPtr,UnityEngine.Vector3&,UnityEngine.Vector3&)"));
		getFov = reinterpret_cast<GetFloat>(resolveCall("UnityEngine.Camera::get_fieldOfView()",
		                                                "UnityEngine.Camera::get_fieldOfView_Injected(System.IntPtr)"));
		getSize = reinterpret_cast<GetFloat>(resolveCall("UnityEngine.Camera::get_orthographicSize()",
		                                                 "UnityEngine.Camera::get_orthographicSize_Injected(System.IntPtr)"));
		getOrthographic = reinterpret_cast<GetBool>(resolveCall("UnityEngine.Camera::get_orthographic()",
		                                                        "UnityEngine.Camera::get_orthographic_Injected(System.IntPtr)"));
		bool complete = true;
		complete &= RequireDependency("UnityEngine.Camera class", klass);
		complete &= RequireDependency("UnityEngine.Object class", object);
		complete &= RequireDependency("Camera.get_main method", reinterpret_cast<void*>(getMain));
		complete &= RequireDependency("Object.op_Implicit method", reinterpret_cast<void*>(alive));
		complete &= RequireDependency("Transform.InverseTransformPoint_Injected icall",
		                              reinterpret_cast<void*>(inversePoint));
		complete &= RequireDependency("Camera.get_fieldOfView icall", reinterpret_cast<void*>(getFov));
		complete &= RequireDependency("Camera.get_orthographicSize icall", reinterpret_cast<void*>(getSize));
		complete &= RequireDependency("Camera.get_orthographic icall", reinterpret_cast<void*>(getOrthographic));
		complete &= RequireDependency("Transform.get_parent method", reinterpret_cast<void*>(g_getParentNative));
		complete &= RequireDependency("Transform.get_position method", reinterpret_cast<void*>(g_getWorldPosNative));
		complete &= RequireDependency("Transform.get_rotation method", reinterpret_cast<void*>(g_getWorldRotNative));
		complete &= RequireDependency("Component.get_transform method",
		                              reinterpret_cast<void*>(g_getComponentTransformNative));
		if (!complete) return false;
		MH_STATUS status = GakuMotion::host::active ? MH_OK : MH_Initialize();
		if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
		{
			if (g_hostContext.logWarn) g_hostContext.logWarn("[GakuMotion][Camera] MH_Initialize failed: %s",
			                                                 MH_StatusToString(status));
			return false;
		}
		struct Entry
		{
			const char* name;
			const char* current;
			void* detour;
			void** original;
			int sharedKind{-1};
			void* target{nullptr};
		};
		Entry entries[] = {
			{
				"UnityEngine.Transform::set_position_Injected(UnityEngine.Vector3&)",
				"UnityEngine.Transform::set_position_Injected(System.IntPtr,UnityEngine.Vector3&)",
				(void*)WorldPosition, (void**)&worldPosition, GAKULOCALIFY_CAMERA_WORLD_POSITION
			},
			{
				"UnityEngine.Transform::set_localPosition_Injected(UnityEngine.Vector3&)",
				"UnityEngine.Transform::set_localPosition_Injected(System.IntPtr,UnityEngine.Vector3&)",
				(void*)LocalPosition, (void**)&localPosition, GAKULOCALIFY_CAMERA_LOCAL_POSITION
			},
			{
				"UnityEngine.Transform::set_rotation_Injected(UnityEngine.Quaternion&)",
				"UnityEngine.Transform::set_rotation_Injected(System.IntPtr,UnityEngine.Quaternion&)",
				(void*)WorldRotation, (void**)&worldRotation, GAKULOCALIFY_CAMERA_WORLD_ROTATION
			},
			{
				"UnityEngine.Transform::set_localRotation_Injected(UnityEngine.Quaternion&)",
				"UnityEngine.Transform::set_localRotation_Injected(System.IntPtr,UnityEngine.Quaternion&)",
				(void*)LocalRotation, (void**)&localRotation
			},
			{
				"UnityEngine.Transform::SetPositionAndRotation_Injected(UnityEngine.Vector3&,UnityEngine.Quaternion&)",
				"UnityEngine.Transform::SetPositionAndRotation_Injected(System.IntPtr,UnityEngine.Vector3&,UnityEngine.Quaternion&)",
				(void*)WorldPose, (void**)&worldPose, GAKULOCALIFY_CAMERA_WORLD_POSE
			},
			{
				"UnityEngine.Transform::SetLocalPositionAndRotation_Injected(UnityEngine.Vector3&,UnityEngine.Quaternion&)",
				"UnityEngine.Transform::SetLocalPositionAndRotation_Injected(System.IntPtr,UnityEngine.Vector3&,UnityEngine.Quaternion&)",
				(void*)LocalPose, (void**)&localPose
			},
			{
				"UnityEngine.Camera::set_fieldOfView(System.Single)",
				"UnityEngine.Camera::set_fieldOfView_Injected(System.IntPtr,System.Single)", (void*)Fov, (void**)&setFov
			},
			{
				"UnityEngine.Camera::set_orthographicSize(System.Single)",
				"UnityEngine.Camera::set_orthographicSize_Injected(System.IntPtr,System.Single)", (void*)Size,
				(void**)&setSize
			},
			{
				"UnityEngine.Camera::set_orthographic(System.Boolean)",
				"UnityEngine.Camera::set_orthographic_Injected(System.IntPtr,System.Boolean)", (void*)Orthographic,
				(void**)&setOrthographic
			},
		};
		void* sharedTargets[GAKULOCALIFY_CAMERA_WRITE_COUNT]{};
		for (auto& e : entries)
		{
			e.target = resolveCall(e.name, e.current);
			if (!RequireDependency(nativeSelf ? e.current : e.name, e.target))
			{
				Shutdown();
				return false;
			}
			if (e.sharedKind >= 0) sharedTargets[e.sharedKind] = e.target;
		}
		if (!SubscribeSharedTransformWrites(sharedTargets))
		{
			Shutdown();
			return false;
		}
		for (const auto& e : entries)
		{
			if (!InstallHook(nativeSelf ? e.current : e.name, e.target, e.detour, e.original, e.sharedKind))
			{
				if (g_hostContext.logWarn) g_hostContext.logWarn("[GakuMotion][Camera] hook unavailable: %s", e.name);
				Shutdown();
				return false;
			}
		}
		void* pipeline = FindClass("UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "RenderPipeline");
		void* manager = FindClass("UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "RenderPipelineManager");
		void* begin = FindMethodPointer(pipeline, "BeginCameraRendering", 2);
		void* managerBegin = FindMethodPointer(manager, "BeginCameraRendering", 2);
		if (g_hostContext.logInfo)
			g_hostContext.logInfo(
				"[GakuMotion][Camera] render boundary: pipelineClass=%p begin=%p managerClass=%p begin=%p",
				pipeline, begin, manager, managerBegin);
		if (!begin && !managerBegin && g_hostContext.logWarn)
			g_hostContext.logWarn(
				"[GakuMotion][Camera] missing required dependency: RenderPipeline/Manager.BeginCameraRendering method");
		bool sharedRenderSelected = false;
		if (!ConnectSharedCameraBegin(begin, managerBegin, sharedRenderSelected) ||
			(!sharedRenderSelected && ((!begin && !managerBegin) ||
			(begin && !InstallHook("RenderPipeline.BeginCameraRendering", begin, (void*)BeforeCameraRendering,
			                (void**)&render)) ||
			(managerBegin && managerBegin != begin && !InstallHook("RenderPipelineManager.BeginCameraRendering", managerBegin,
			                                                (void*)BeforeCameraRenderingManager,
			                                                (void**)&managerRender)))))
		{
			Shutdown();
			return false;
		}
		il2cppArrayLength = reinterpret_cast<Il2CppArrayLengthFn>(GetProcAddress(g_gameAssembly, "il2cpp_array_length"));

		// Optional render-complete hooks for standalone ForRecording pacing. Non-fatal if
		// unavailable or owned by another plugin; Unity Time.renderedFrameCount acts as fallback.
		if (void* endFrame = FindMethodPointer(pipeline, "EndFrameRendering", 2))
		{
			InstallHook("RenderPipeline.EndFrameRendering", endFrame, (void*)AfterFrameRendering,
				(void**)&endFrameRender);
		}
		if (void* managerEndFrame = FindMethodPointer(manager, "EndFrameRendering", 2);
			managerEndFrame && managerEndFrame != FindMethodPointer(pipeline, "EndFrameRendering", 2))
		{
			InstallHook("RenderPipelineManager.EndFrameRendering", managerEndFrame,
				(void*)AfterFrameRenderingManager, (void**)&managerEndFrameRender);
		}

		// Hook DrawActorShadowPass.AddRenderPasses for actor shadow experiments (anchor / bypass)
		void* drawShadowPassKlass = FindClass("vl-unity.Runtime.dll", "VL.Rendering", "DrawActorShadowPass");
		if (drawShadowPassKlass)
		{
			const auto metadata = GroundShadowProbeBootstrap::Resolve(g_gameAssembly).method;
			void* rendererClass = FindClass("Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal", "ScriptableRenderer");
			void* dataClass = FindClass("Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal", "RenderingData");
			const void* method = rendererClass && dataClass ? metadata.Find(drawShadowPassKlass, "AddRenderPasses", false,
				{0x01}, {{0x12, rendererClass}, {0x11, dataClass, true}}) : nullptr;
			void* pAddPasses = GroundShadowProbeBootstrap::MethodPointer(method);
			if (pAddPasses)
			{
				InstallHook("DrawActorShadowPass.AddRenderPasses", pAddPasses,
					 reinterpret_cast<void*>(Detour_AddRenderPasses),
					 reinterpret_cast<void**>(&orig_AddRenderPasses));
			}
		}

		// Resolve Cinemachine classes
		brainKlass = FindCinemachineClass("CinemachineBrain");
		if (brainKlass && il2cpp_class_get_type && il2cpp_type_get_object)
		{
			brainTypeObj = il2cpp_type_get_object(il2cpp_class_get_type(brainKlass));
			brainGetActiveVcamMethod = il2cpp_class_get_method_from_name(brainKlass, "get_ActiveVirtualCamera", 0);
			void* csField = g_faceRuntime.findField ? g_faceRuntime.findField(brainKlass, "<CurrentCameraState>k__BackingField") : nullptr;
			if (csField && il2cpp_field_get_offset) brainCurrentStateOffset = il2cpp_field_get_offset(csField);
		}

		vcamKlass = FindCinemachineClass("CinemachineVirtualCamera");
		if (vcamKlass && il2cpp_class_get_type && il2cpp_type_get_object)
		{
			vcamTypeObj = il2cpp_type_get_object(il2cpp_class_get_type(vcamKlass));
			vcamLensField = g_faceRuntime.findField ? g_faceRuntime.findField(vcamKlass, "m_Lens") : nullptr;
			if (vcamLensField && il2cpp_field_get_offset) vcamLensOffset = il2cpp_field_get_offset(vcamLensField);
			vcamStateField = g_faceRuntime.findField ? g_faceRuntime.findField(vcamKlass, "m_State") : nullptr;
			if (vcamStateField && il2cpp_field_get_offset) vcamStateOffset = il2cpp_field_get_offset(vcamStateField);
		}

		lensSettingsKlass = FindCinemachineClass("LensSettings");
		if (lensSettingsKlass && g_faceRuntime.findField)
		{
			lensFovField = g_faceRuntime.findField(lensSettingsKlass, "FieldOfView");
			if (lensFovField && il2cpp_field_get_offset)
			{
				lensFovOffset = il2cpp_field_get_offset(lensFovField);
				unboxedLensFovOffset = GetUnboxedFieldOffset(lensFovField);
			}
		}

		cameraStateKlass = FindCinemachineClass("CameraState");
		if (cameraStateKlass && g_faceRuntime.findField)
		{
			cameraStateLensField = g_faceRuntime.findField(cameraStateKlass, "<Lens>k__BackingField");
			if (cameraStateLensField && il2cpp_field_get_offset)
			{
				cameraStateLensOffset = il2cpp_field_get_offset(cameraStateLensField);
				unboxedCameraStateLensOffset = GetUnboxedFieldOffset(cameraStateLensField);
			}
		}

		void* systemTypeClass = FindClass("mscorlib.dll", "System", "Type");
		if (!systemTypeClass) systemTypeClass = FindClass("mscorlib", "System", "Type");
		ObjectFinderMetadata finderMetadata;
		finderMetadata.nextMethod = reinterpret_cast<decltype(finderMetadata.nextMethod)>(
			GetProcAddress(g_gameAssembly, "il2cpp_class_get_methods"));
		finderMetadata.methodName = reinterpret_cast<decltype(finderMetadata.methodName)>(
			GetProcAddress(g_gameAssembly, "il2cpp_method_get_name"));
		finderMetadata.parameterCount = reinterpret_cast<decltype(finderMetadata.parameterCount)>(
			GetProcAddress(g_gameAssembly, "il2cpp_method_get_param_count"));
		finderMetadata.isGeneric = reinterpret_cast<decltype(finderMetadata.isGeneric)>(
			GetProcAddress(g_gameAssembly, "il2cpp_method_is_generic"));
		finderMetadata.parameterType = reinterpret_cast<MethodParam>(
			GetProcAddress(g_gameAssembly, "il2cpp_method_get_param"));
		finderMetadata.classFromType = reinterpret_cast<decltype(finderMetadata.classFromType)>(
			GetProcAddress(g_gameAssembly, "il2cpp_class_from_type"));
		finderMetadata.elementClass = reinterpret_cast<decltype(finderMetadata.elementClass)>(
			GetProcAddress(g_gameAssembly, "il2cpp_class_get_element_class"));
		findObjectOfTypeMethod = const_cast<void*>(FindObjectFinder(object, systemTypeClass,
			"FindObjectOfType", false, finderMetadata));
		findObjectsOfTypeMethod = const_cast<void*>(FindObjectFinder(object, systemTypeClass,
			"FindObjectsOfType", true, finderMetadata));
		if (g_hostContext.logInfo)
			g_hostContext.logInfo("[GakuMotion][Camera] exact Object finders: single=%p array=%p",
				findObjectOfTypeMethod, findObjectsOfTypeMethod);

		// Resolve Photography Focus & DoF classes
		actCtrlKlass = FindPhotographyClass("PhotographyActivityController");
		if (actCtrlKlass)
		{
			if (il2cpp_class_get_type && il2cpp_type_get_object)
				actCtrlTypeObj = il2cpp_type_get_object(il2cpp_class_get_type(actCtrlKlass));
			if (g_faceRuntime.findField)
				actCtrlModelField = g_faceRuntime.findField(actCtrlKlass, "_model");
			if (actCtrlModelField && il2cpp_field_get_offset)
				actCtrlModelOffset = il2cpp_field_get_offset(actCtrlModelField);
		}
		modelKlass = FindPhotographyClass("PhotographyActivityControllerModel");
		if (modelKlass && g_faceRuntime.findMethod)
		{
			modelSetFocalPointMethod = g_faceRuntime.findMethod(modelKlass, "SetFocalPoint", 1);
		}

		if (g_hostContext.logInfo)
		{
			g_hostContext.logInfo("[GakuMotion][Cinemachine] vcamKlass=%p lensOffset=%d unboxedFovOffset=%d brain=%p activeMethod=%p",
			                      vcamKlass, vcamLensOffset, unboxedLensFovOffset, brainKlass, brainGetActiveVcamMethod);
			g_hostContext.logInfo("[GakuMotion][DoF] actCtrlKlass=%p modelField=%p modelOffset=%d modelKlass=%p setFocalPoint=%p",
			                      actCtrlKlass, actCtrlModelField, actCtrlModelOffset, modelKlass, modelSetFocalPointMethod);
		}

		ready = true;
		sharedWritesEnabled.store(sharedWritesSubscribed, std::memory_order_release);
		auto& c = Controller();
		c.fnGetCamera = []() -> void* { return OnUnityThread() ? getMain(nullptr) : nullptr; };
		c.fnGetTransform = HelperGetComponentTransform;
		c.fnAlive = IsAlive;
		c.fnWrite = Write;
		c.fnRead = Read;
		c.fnReleaseVcam = ReleaseVirtualCameras;
		c.fnReleaseVcamForCapture = ReleaseVirtualCamerasForCapture;
		c.fnWriteFovFallback = [](void* camera, float fov)
		{
			if (setFov && camera)
			{
				void* cameraSelf = Self(camera);
				if (cameraSelf) setFov(cameraSelf, fov);
			}
		};
		c.fnActorPose = [](void* actor, GakuMotion::Vector3& origin, GakuMotion::Quaternion& rotation,
		                   GakuMotion::Vector3& hips)
		{
			if (!IsAlive(actor)) return false;
			void* root = HelperGetComponentTransform(actor);
			void* hip = HelperGetBoneTransform(actor, 0);
			if (!IsAlive(root) || !IsAlive(hip)) return false;
			auto p = g_getWorldPosNative(root);
			auto h = g_getWorldPosNative(hip);
			auto q = g_getWorldRotNative(root);
			// Main playback applies XYZ to the hips world target rather than the
			// actor component. Include it in the camera's logical scene anchor.
			auto* selected = GakuMotion::Core::Instance().PlayerForActor(actor);
			const auto& controller = selected ? selected->controller : GakuMotion::Core::Instance().player.controller;
			const auto placement = controller.initialPositionOffset;
			origin = GakuMotion::Vector3{p.x, p.y, p.z} + placement;
			hips = {h.x, h.y, h.z};
			rotation = {q.x, q.y, q.z, q.w};
			if (controller.placementYawDegrees != 0)
				rotation = GakuMotion::Quaternion::FromEulerDegreesXYZ(0, controller.placementYawDegrees, 0) * rotation;
			float norm = GakuMotion::Quaternion::Dot(rotation, rotation);
			if (!std::isfinite(norm) || norm < 1e-8f) return false;
			rotation = rotation.Normalized();
			return GakuMotion::CameraFinite(origin) && GakuMotion::CameraFinite(hips);
		};
		c.fnFollowTarget = [](void* actor, GakuMotion::CameraFollowTarget target, GakuMotion::Vector3& position)
		{
			const int hid = GakuMotion::CameraFollowTargetBoneId(target);
			if (hid < 0 || !IsAlive(actor)) return false;
			void* bone = HelperGetBoneTransform(actor, hid);
			if (!IsAlive(bone)) return false;
			const auto p = g_getWorldPosNative(bone);
			position = {p.x, p.y, p.z};
			return GakuMotion::CameraFinite(position);
		};
		c.Publish();
		return true;
	}
} // namespace CameraBridge
