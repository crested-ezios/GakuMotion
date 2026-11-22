#pragma once

#include "GkCameraBridgeTypes.hpp"

namespace CameraBridge
{
	inline void TrackVirtualCamera(void* vcam)
	{
		if (!vcam || !IsAlive(vcam)) return;
		for (const auto& t : trackedVcams)
		{
			if (t.vcamPtr == vcam) return;
		}
		TrackedVcam item;
		item.vcamPtr = vcam;
		if (vcamLensOffset >= 0)
		{
			float* pFov = reinterpret_cast<float*>(reinterpret_cast<char*>(vcam) + vcamLensOffset + unboxedLensFovOffset);
			if (std::isfinite(*pFov) && *pFov > 1.0f && *pFov < 179.0f)
			{
				item.originalFov = *pFov;
				item.originalFovSaved = true;
			}
		}
		item.originalStateFovSaved = ReadNativeFov(vcam,
			vcamStateOffset >= 0 ? vcamStateOffset + unboxedCameraStateLensOffset + unboxedLensFovOffset : -1,
			item.originalStateFov);
		trackedVcams.push_back(item);
		GM_DEBUG("[GakuMotion][Cinemachine] Tracked virtual camera %p (saved FOV=%.2f)", vcam, item.originalFov);
	}

	inline void ResolveAllVirtualCameras(void* camera)
	{
		if (!OnUnityThread()) return;

		// 1. Check CinemachineBrain on camera
		if (camera && IsAlive(camera) && g_getComponentNative && brainTypeObj)
		{
			void* brain = g_getComponentNative(camera, brainTypeObj);
			if (brain && IsAlive(brain) && brainGetActiveVcamMethod && g_faceRuntime.invoke)
			{
				void* ex = nullptr;
				void* activeVcam = g_faceRuntime.invoke(brainGetActiveVcamMethod, brain, nullptr, &ex);
				if (!ex && activeVcam && IsAlive(activeVcam))
				{
					TrackVirtualCamera(activeVcam);
				}
			}
		}

		// 2. Query all CinemachineVirtualCamera instances in the scene
		if (findObjectsOfTypeMethod && vcamTypeObj && g_faceRuntime.invoke && il2cppArrayLength)
		{
			void* args[1] = { vcamTypeObj };
			void* ex = nullptr;
			void* arr = g_faceRuntime.invoke(findObjectsOfTypeMethod, nullptr, args, &ex);
			if (!ex && arr)
			{
				uintptr_t len = il2cppArrayLength(arr);
				void** items = reinterpret_cast<void**>(reinterpret_cast<char*>(arr) + 32);
				for (uintptr_t i = 0; i < len; ++i)
				{
					if (items[i] && IsAlive(items[i]))
					{
						TrackVirtualCamera(items[i]);
					}
				}
			}
		}
		else if (trackedVcams.empty() && findObjectOfTypeMethod && vcamTypeObj && g_faceRuntime.invoke)
		{
			void* args[1] = { vcamTypeObj };
			void* ex = nullptr;
			void* vcam = g_faceRuntime.invoke(findObjectOfTypeMethod, nullptr, args, &ex);
			if (!ex && vcam && IsAlive(vcam))
			{
				TrackVirtualCamera(vcam);
			}
		}
	}

	inline void ResolvePhotographyFocusController()
	{
		if (!OnUnityThread()) return;
		if (trackedActCtrl && IsAlive(trackedActCtrl) && trackedModel) return;
		trackedActCtrl = nullptr;
		trackedModel = nullptr;

		if (findObjectOfTypeMethod && actCtrlTypeObj && g_faceRuntime.invoke)
		{
			void* args[1] = { actCtrlTypeObj };
			void* ex = nullptr;
			void* ctrl = g_faceRuntime.invoke(findObjectOfTypeMethod, nullptr, args, &ex);
			if (!ex && ctrl && IsAlive(ctrl))
			{
				trackedActCtrl = ctrl;
				if (actCtrlModelOffset >= 0)
				{
					trackedModel = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctrl) + actCtrlModelOffset);
				}
				GM_DEBUG("[GakuMotion][DoF] Tracked PhotographyActivityController %p, Model %p", trackedActCtrl, trackedModel);
			}
		}
	}

	inline void* ResolveCameraIcall(ResolveIcall resolver, const char* signature)
	{
		if (!resolver || !signature) return nullptr;
		if (void* full = resolver(signature)) return full;
		// IL2CPP supports internal calls registered by either full signature or
		// qualified method name. Only strip arguments for the known Camera /
		// Transform methods used by this bridge; never change a method's ABI.
		const char* args = std::strchr(signature, '(');
		if (!args || (std::strncmp(signature, "UnityEngine.Camera::", 20) != 0 &&
			std::strncmp(signature, "UnityEngine.Transform::", 23) != 0))
			return nullptr;
		const std::string shortName(signature, static_cast<size_t>(args - signature));
		void* address = resolver(shortName.c_str());
		if (address && g_hostContext.logInfo)
			g_hostContext.logInfo("[GakuMotion][Camera] icall resolved by short name: %s -> %p", shortName.c_str(),
			                      address);
		return address;
	}

	inline bool ResolveNativeSelfField(void* objectClass)
	{
		if (!RequireDependency("UnityEngine.Object class", objectClass)) return false;
		const auto& api = g_faceRuntime;
		if (!RequireDependency("il2cpp_class_get_field_from_name", reinterpret_cast<void*>(api.findField)) ||
			!RequireDependency("il2cpp_field_get_type", reinterpret_cast<void*>(api.fieldType)) ||
			!RequireDependency("il2cpp_field_get_flags", reinterpret_cast<void*>(api.fieldFlags)) ||
			!RequireDependency("il2cpp_type_get_type", reinterpret_cast<void*>(api.typeCode)) ||
			!RequireDependency("il2cpp_field_get_value", reinterpret_cast<void*>(api.readField)) ||
			!RequireDependency("il2cpp_field_get_offset", reinterpret_cast<void*>(il2cpp_field_get_offset)))
			return false;
		cachedPtrField = api.findField(objectClass, "m_CachedPtr");
		if (!RequireDependency("UnityEngine.Object.m_CachedPtr field", cachedPtrField)) return false;
		void* type = api.fieldType(cachedPtrField);
		const int code = type ? api.typeCode(type) : -1;
		const int offset = il2cpp_field_get_offset(cachedPtrField);
		// IL2CPP_TYPE_I is System.IntPtr. Read through the field API, not an
		// assumed managed-object offset; validate offset metadata as a guard.
		if (!type || code != 0x18 || (api.fieldFlags(cachedPtrField) & 0x10) || offset < 0 ||
			offset % alignof(void*) != 0)
		{
			if (g_hostContext.logWarn)
				g_hostContext.logWarn(
					"[GakuMotion][Camera] invalid Object.m_CachedPtr metadata: type=%d offset=%d flags=%u",
					code, offset, api.fieldFlags(cachedPtrField));
			cachedPtrField = nullptr;
			return false;
		}
		if (g_hostContext.logInfo)
			g_hostContext.logInfo(
				"[GakuMotion][Camera] Unity native-self ABI: m_CachedPtr metadata offset=%d type=System.IntPtr",
				offset);
		return true;
	}

	inline bool RequireNativeSelfMethod(void* klass, const char* name, int count, MethodParam paramType)
	{
		const auto* method = klass && il2cpp_class_get_method_from_name
			                     ? il2cpp_class_get_method_from_name(klass, name, count)
			                     : nullptr;
		if (!RequireDependency(name, method) || !paramType || !g_faceRuntime.methodFlags || !g_faceRuntime.typeCode)
			return false;
		void* selfType = paramType(method, 0);
		uint32_t implementation = 0;
		const uint32_t flags = g_faceRuntime.methodFlags(method, &implementation);
		const int code = selfType ? g_faceRuntime.typeCode(selfType) : -1;
		if (!method->methodPointer || code != 0x18 || !(flags & 0x10))
		{
			if (g_hostContext.logWarn)
				g_hostContext.logWarn(
					"[GakuMotion][Camera] injected method ABI mismatch: %s selfType=%d flags=%u pointer=%p",
					name, code, flags, method->methodPointer);
			return false;
		}
		return true;
	}

	struct ObjectFinderMetadata
	{
		const void* (*nextMethod)(void*, void**){nullptr};
		const char* (*methodName)(const void*){nullptr};
		uint32_t (*parameterCount)(const void*){nullptr};
		bool (*isGeneric)(const void*){nullptr};
		MethodParam parameterType{nullptr};
		void* (*classFromType)(const void*){nullptr};
		void* (*elementClass)(void*){nullptr};
	};

	// Object also exposes FindObjectOfType<T>(bool). Name/arity lookup can
	// select that generic overload before FindObjectOfType(System.Type).
	inline const void* FindObjectFinder(void* objectClass, void* systemTypeClass, const char* name,
		bool arrayResult, const ObjectFinderMetadata& metadata)
	{
		if (!objectClass || !systemTypeClass || !name || !metadata.nextMethod || !metadata.methodName ||
			!metadata.parameterCount || !metadata.isGeneric || !metadata.parameterType ||
			!metadata.classFromType || (arrayResult && !metadata.elementClass) ||
			!g_faceRuntime.methodFlags || !g_faceRuntime.returnType || !g_faceRuntime.typeCode)
			return nullptr;
		const void* match = nullptr;
		void* iterator = nullptr;
		while (const void* method = metadata.nextMethod(objectClass, &iterator))
		{
			const char* methodName = metadata.methodName(method);
			if (!methodName || std::strcmp(methodName, name) != 0 || metadata.isGeneric(method) ||
				metadata.parameterCount(method) != 1)
				continue;
			uint32_t implementation = 0;
			if (!(g_faceRuntime.methodFlags(method, &implementation) & 0x10)) continue;
			void* parameter = metadata.parameterType(method, 0);
			void* result = g_faceRuntime.returnType(method);
			if (!parameter || !result || g_faceRuntime.typeCode(parameter) != 0x12 ||
				metadata.classFromType(parameter) != systemTypeClass)
				continue;
			const int expectedResult = arrayResult ? 0x1d : 0x12; // SZARRAY / CLASS
			if (g_faceRuntime.typeCode(result) != expectedResult) continue;
			void* resultClass = metadata.classFromType(result);
			if (!resultClass || (arrayResult ? metadata.elementClass(resultClass) : resultClass) != objectClass)
				continue;
			if (match) return nullptr; // An ambiguous signature must not be invoked.
			match = method;
		}
		return match;
	}
} // namespace CameraBridge
