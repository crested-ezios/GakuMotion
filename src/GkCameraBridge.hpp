#pragma once
#include "GkLog.hpp"

#include "minhook/include/MinHook.h"
#include "GakuLocalifyCameraWrite.h"
#include "GakuVrifyCameraBegin.h"

// Included inside the plugin's anonymous namespace, after IL2CPP resolution helpers.
// No game ABI offsets are used. All methods/icalls are resolved on Unity's thread.
namespace CameraBridge
{
	using ResolveIcall = void* (*)(const char*);
	using GetMain = void* (*)(void*);
	using Alive = bool (*)(void*, void*);
	using SetPosition = void (*)(void*, UnityVector3*);
	using SetRotation = void (*)(void*, UnityQuaternion*);
	using SetPose = void (*)(void*, UnityVector3*, UnityQuaternion*);
	using InversePoint = void (*)(void*, UnityVector3*, UnityVector3*);
	using GetFloat = float (*)(void*);
	using SetFloat = void (*)(void*, float);
	using GetBool = bool (*)(void*);
	using SetBool = void (*)(void*, bool);
	using Render = void (*)(void*, void*, void*);
	using MethodParam = void* (*)(const void*, uint32_t);

	inline GetMain getMain = nullptr;
	inline Alive alive = nullptr;
	inline InversePoint inversePoint = nullptr;
	inline SetPosition worldPosition = nullptr, localPosition = nullptr;
	inline SetRotation worldRotation = nullptr, localRotation = nullptr;
	inline SetPose worldPose = nullptr, localPose = nullptr;
	inline GetFloat getFov = nullptr, getSize = nullptr;
	inline SetFloat setFov = nullptr, setSize = nullptr;
	inline GetBool getOrthographic = nullptr;
	inline SetBool setOrthographic = nullptr;
	inline Render render = nullptr, managerRender = nullptr;
	inline std::vector<void*> installed;
	inline thread_local bool writing = false;
	inline bool ready = false;
	inline GakuVrifyCameraBeginService sharedBegin{};
	inline std::atomic<bool> sharedBeginEnabled{false};
	inline bool sharedBeginSubscribed = false;
	inline std::atomic<DWORD> unityThreadId{0};
	inline bool nativeSelf = false;
	inline void* cachedPtrField = nullptr;
	inline void* ownedNativeCamera = nullptr;
	inline void* ownedNativeTransform = nullptr;
	inline GakuLocalifyCameraWriteService sharedWrites{};
	inline bool sharedWritesSubscribed = false;
	inline void* sharedWriteTargets[GAKULOCALIFY_CAMERA_WRITE_COUNT]{};
	inline std::atomic<bool> sharedWritesEnabled{false};

	// Cinemachine virtual camera & brain reflection
	inline void* brainKlass = nullptr;
	inline void* brainTypeObj = nullptr;
	inline void* brainGetActiveVcamMethod = nullptr;
	inline void* brainCurrentStateField = nullptr;
	inline int brainCurrentStateOffset = -1;

	inline void* vcamKlass = nullptr;
	inline void* vcamTypeObj = nullptr;
	inline void* vcamLensField = nullptr;
	inline int vcamLensOffset = -1;
	inline void* vcamStateField = nullptr;
	inline int vcamStateOffset = -1;

	inline void* lensSettingsKlass = nullptr;
	inline void* lensFovField = nullptr;
	inline int lensFovOffset = -1;
	inline int unboxedLensFovOffset = 0;

	inline void* cameraStateKlass = nullptr;
	inline void* cameraStateLensField = nullptr;
	inline int cameraStateLensOffset = -1;
	inline int unboxedCameraStateLensOffset = 0;

	// Photography Focus & DoF reflection
	inline void* actCtrlKlass = nullptr;
	inline void* actCtrlTypeObj = nullptr;
	inline void* actCtrlModelField = nullptr;
	inline int actCtrlModelOffset = -1;
	inline void* modelKlass = nullptr;
	inline const void* modelSetFocalPointMethod = nullptr;
	inline void* trackedActCtrl = nullptr;
	inline void* trackedModel = nullptr;

	inline void* findObjectOfTypeMethod = nullptr;
	inline void* findObjectsOfTypeMethod = nullptr;
	using Il2CppArrayLengthFn = uintptr_t (*)(void*);
	inline Il2CppArrayLengthFn il2cppArrayLength = nullptr;

	struct TrackedVcam
	{
		void* vcamPtr{nullptr};
		float originalFov{0.0f};
		bool originalFovSaved{false};
		float originalStateFov{0.0f};
		bool originalStateFovSaved{false}, lensModified{false}, stateModified{false};
	};
	inline std::vector<TrackedVcam> trackedVcams;
	struct TrackedBrain { void* object{}; float fov{}; bool saved{}, modified{}; };
	inline std::vector<TrackedBrain> trackedBrains;
	inline const void* modelGetFocalPointMethod = nullptr;
	inline void* focalOwner = nullptr;
	inline void* focalModel = nullptr;
	inline float originalFocalPoint = 0.0f;
	inline bool originalFocalPointSaved = false, focalPointModified = false;

	inline bool ReadNativeFov(void* object, int offset, float& value)
	{
		if (!object || offset < 0) return false;
		__try { value = *reinterpret_cast<float*>(static_cast<char*>(object)+offset); }
		__except(EXCEPTION_EXECUTE_HANDLER) { return false; }
		return std::isfinite(value) && value > 1.0f && value < 179.0f;
	}
	inline bool RestoreNativeFov(void* object, int offset, float value)
	{
		if (!object || offset < 0 || !std::isfinite(value)) return false;
		__try { *reinterpret_cast<float*>(static_cast<char*>(object)+offset) = value; }
		__except(EXCEPTION_EXECUTE_HANDLER) { return false; }
		float actual=0; return ReadNativeFov(object,offset,actual) && std::fabs(actual-value)<=1e-4f;
	}

	inline int GetUnboxedFieldOffset(void* field)
	{
		if (!field || !il2cpp_field_get_offset) return 0;
		int off = il2cpp_field_get_offset(field);
		if (off >= 16) return off - 16;
		return off >= 0 ? off : 0;
	}

	inline void* FindCinemachineClass(const char* className)
	{
		void* k = FindClass("Cinemachine.dll", "Cinemachine", className);
		if (!k) k = FindClass("Cinemachine", "Cinemachine", className);
		return k;
	}

	inline void* FindPhotographyClass(const char* className)
	{
		void* k = FindClass("campus-submodule.Runtime.dll", "Campus.Photography", className);
		if (!k) k = FindClass("campus-submodule.Runtime", "Campus.Photography", className);
		if (!k) k = FindClass("Assembly-CSharp.dll", "Campus.Photography", className);
		return k;
	}

	inline auto& Controller() { return GakuMotion::Core::Instance().camera; }

	inline bool OnUnityThread()
	{
		return GetCurrentThreadId() == unityThreadId.load(std::memory_order_acquire);
	}

	inline bool IsAlive(void* object)
	{
		return OnUnityThread() && object && alive && alive(object, nullptr);
	}

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

	inline void WriteVirtualCamerasFov(void* camera, float fov)
	{
		if (!std::isfinite(fov) || fov < 1.0f || fov > 179.0f) return;

		if (trackedVcams.empty())
		{
			ResolveAllVirtualCameras(camera);
		}

		for (auto& item : trackedVcams)
		{
			void* v = item.vcamPtr;
			if (!v || !IsAlive(v)) continue;

			// Write m_Lens.FieldOfView
			if (vcamLensOffset >= 0)
			{
				item.lensModified = true;
				float* pFov = reinterpret_cast<float*>(reinterpret_cast<char*>(v) + vcamLensOffset + unboxedLensFovOffset);
				if (!item.originalFovSaved && std::isfinite(*pFov) && *pFov > 1.0f && *pFov < 179.0f)
				{
					item.originalFov = *pFov;
					item.originalFovSaved = true;
				}
				*pFov = fov;

				if (vcamLensField && g_faceRuntime.readField && g_faceRuntime.writeField)
				{
					char lensBuffer[128]{};
					g_faceRuntime.readField(v, vcamLensField, lensBuffer);
					*reinterpret_cast<float*>(lensBuffer + unboxedLensFovOffset) = fov;
					g_faceRuntime.writeField(v, vcamLensField, lensBuffer);
				}
			}

			// Write m_State.Lens.FieldOfView
			if (vcamStateOffset >= 0)
			{
				item.stateModified = true;
				float* pStateFov = reinterpret_cast<float*>(
					reinterpret_cast<char*>(v) + vcamStateOffset + unboxedCameraStateLensOffset + unboxedLensFovOffset);
				*pStateFov = fov;
			}
		}

		// Also update CinemachineBrain CurrentCameraState if brain is present
		if (camera && g_getComponentNative && brainTypeObj && brainCurrentStateOffset >= 0)
		{
			void* brain = g_getComponentNative(camera, brainTypeObj);
			if (brain && IsAlive(brain))
			{
				auto found = std::find_if(trackedBrains.begin(),trackedBrains.end(),[&](const TrackedBrain& item) { return item.object==brain; });
				if (found==trackedBrains.end())
				{
					TrackedBrain item; item.object=brain;
					item.saved=ReadNativeFov(brain,brainCurrentStateOffset + unboxedCameraStateLensOffset + unboxedLensFovOffset,item.fov);
					trackedBrains.push_back(item); found=std::prev(trackedBrains.end());
				}
				found->modified=true;
				float* pBrainFov = reinterpret_cast<float*>(
					reinterpret_cast<char*>(brain) + brainCurrentStateOffset + unboxedCameraStateLensOffset + unboxedLensFovOffset);
				*pBrainFov = fov;
			}
		}
	}

	inline bool ReadFocalPoint(void* model, float& value)
	{
		if (!model || !modelKlass || !g_faceRuntime.findMethod || !g_faceRuntime.returnType ||
			!g_faceRuntime.typeCode || !g_faceRuntime.methodFlags || !g_faceRuntime.invoke || !g_safeBones.unbox) return false;
		if (!modelGetFocalPointMethod)
		{
			const void* getter=g_faceRuntime.findMethod(modelKlass,"get_FocalPoint",0);
			uint32_t impl=0;
			void* returns=getter ? g_faceRuntime.returnType(getter):nullptr;
			if (!getter || !returns || (g_faceRuntime.methodFlags(getter,&impl)&0x10) ||
				g_faceRuntime.typeCode(returns)!=0x0c) return false;
			modelGetFocalPointMethod=getter;
		}
		void* exception=nullptr;
		void* boxed=g_faceRuntime.invoke(modelGetFocalPointMethod,model,nullptr,&exception);
		void* raw=boxed && !exception ? g_safeBones.unbox(boxed):nullptr;
		if (!raw) return false;
		value=*static_cast<float*>(raw);
		return std::isfinite(value);
	}

	inline GakuMotion::CaptureRestoreStatus ReleaseVirtualCamerasForCapture()
	{
		GakuMotion::CaptureRestoreStatus result;
		for (const auto& item:trackedVcams)
		{
			if (!item.lensModified && !item.stateModified) continue;
			result.modified=true;
			if (!IsAlive(item.vcamPtr)) { result.confirmed=false; continue; }
			if (item.lensModified && (!item.originalFovSaved || !RestoreNativeFov(item.vcamPtr,
				vcamLensOffset + unboxedLensFovOffset,item.originalFov))) result.confirmed=false;
			if (item.stateModified && (!item.originalStateFovSaved || !RestoreNativeFov(item.vcamPtr,
				vcamStateOffset + unboxedCameraStateLensOffset + unboxedLensFovOffset,item.originalStateFov))) result.confirmed=false;
		}
		for (const auto& item:trackedBrains)
		{
			if (!item.modified) continue;
			result.modified=true;
			if (!item.saved || !IsAlive(item.object) || !RestoreNativeFov(item.object,
				brainCurrentStateOffset + unboxedCameraStateLensOffset + unboxedLensFovOffset,item.fov)) result.confirmed=false;
		}
		if (focalPointModified)
		{
			result.modified=true;
			void* currentModel=nullptr;
			if (IsAlive(focalOwner) && actCtrlModelField && g_faceRuntime.readField)
				g_faceRuntime.readField(focalOwner,actCtrlModelField,&currentModel);
			if (!originalFocalPointSaved || currentModel!=focalModel || !modelSetFocalPointMethod || !g_faceRuntime.invoke)
				result.confirmed=false;
			else
			{
				void* args[]{&originalFocalPoint}; void* exception=nullptr;
				g_faceRuntime.invoke(modelSetFocalPointMethod,focalModel,args,&exception);
				float actual=0;
				if (exception || !ReadFocalPoint(focalModel,actual) || std::fabs(actual-originalFocalPoint)>1e-4f) result.confirmed=false;
			}
		}
		if (result.confirmed)
		{
			trackedVcams.clear(); trackedBrains.clear(); focalOwner=focalModel=nullptr;
			originalFocalPointSaved=focalPointModified=false;
			trackedActCtrl=nullptr; trackedModel=nullptr;
		}
		return result;
	}

	inline void ReleaseVirtualCameras(bool restore)
	{
		if (restore) { ReleaseVirtualCamerasForCapture(); return; }
		trackedVcams.clear(); trackedBrains.clear(); focalOwner=focalModel=nullptr;
		originalFocalPointSaved=focalPointModified=false;
		trackedActCtrl=nullptr; trackedModel=nullptr;
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

	inline void WritePhotographyDoFFocalPoint(float focalDistance)
	{
		if (!std::isfinite(focalDistance) || focalDistance <= 0.05f) return;
		ResolvePhotographyFocusController();

		if (trackedModel && modelSetFocalPointMethod && g_faceRuntime.invoke)
		{
			// Do not extend ownership to a replacement model while the old focus
			// mutation still needs restoration.
			if (focalPointModified && trackedModel!=focalModel) return;
			if (!focalPointModified)
			{
				focalOwner=trackedActCtrl; focalModel=trackedModel;
				originalFocalPointSaved=ReadFocalPoint(focalModel,originalFocalPoint);
			}
			focalPointModified=true;
			void* args[1] = { &focalDistance };
			void* ex = nullptr;
			g_faceRuntime.invoke(modelSetFocalPointMethod, trackedModel, args, &ex);
		}
	}

	inline bool RequireDependency(const char* name, const void* value)
	{
		if (value) return true;
		if (g_hostContext.logWarn) g_hostContext.logWarn("[GakuMotion][Camera] missing required dependency: %s", name);
		return false;
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

	inline void* Self(void* managed)
	{
		if (!OnUnityThread() || !managed) return nullptr;
		if (!nativeSelf) return managed;
		if (!cachedPtrField || !g_faceRuntime.readField) return nullptr;
		void* native = nullptr;
		g_faceRuntime.readField(managed, cachedPtrField, &native);
		return native;
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

	inline void Write(void* camera, void* transform, const GakuMotion::CameraFrame& frame)
	{
		if (!OnUnityThread() || !ready || !IsAlive(camera) || !IsAlive(transform)) return;
		void* cameraSelf = Self(camera);
		void* transformSelf = Self(transform);
		if (!cameraSelf || !transformSelf) return;
		ownedNativeCamera = cameraSelf;
		ownedNativeTransform = transformSelf;
		writing = true;
		UnityVector3 position{frame.position.x, frame.position.y, frame.position.z};
		auto q = frame.rotation;
		// The host already hooks world setters. Its free-camera detour can replace
		// our arguments, so apply through the native LOCAL pair setter instead.
		// Parent conversion makes this work for camera rigs as well as scene roots.
		if (void* parent = g_getParentNative(transform); IsAlive(parent))
		{
			UnityVector3 local{};
			void* parentSelf = Self(parent);
			if (!parentSelf)
			{
				writing = false;
				return;
			}
			inversePoint(parentSelf, &position, &local);
			position = local;
			UnityQuaternion p = g_getWorldRotNative(parent);
			q = GakuMotion::Quaternion(p.x, p.y, p.z, p.w).Inverse() * q;
		}
		UnityQuaternion rotation{q.x, q.y, q.z, q.w};
		localPose(transformSelf, &position, &rotation);
		// Drive active Cinemachine Virtual Camera Lens with VMD FOV
		WriteVirtualCamerasFov(camera, frame.fov);

		// Drive Photography DoF Focal Point from VMD camera distance
		float focalDistance = 0.0f;
		if ((frame.flags & 1u) == 0 && std::isfinite(frame.fov) && frame.fov > 1.0f && frame.fov < 179.0f)
		{
			constexpr float kPi = 3.14159265358979323846f;
			float tanHalfFov = std::tan(frame.fov * (kPi / 360.0f));
			if (tanHalfFov > 1e-4f && frame.orthographicSize > 0.0015f)
			{
				focalDistance = frame.orthographicSize / tanHalfFov;
			}
		}

		// Fallback to Euclidean distance to actor if distance is ~0.0 or unavailable (e.g. SOS camera)
		if (focalDistance < 0.2f)
		{
			auto& ctrl = Controller();
			if (ctrl.actor && ctrl.fnActorPose)
			{
				GakuMotion::Vector3 actorPos, hips;
				GakuMotion::Quaternion actorRot;
				if (ctrl.fnActorPose(ctrl.actor, actorPos, actorRot, hips))
				{
					GakuMotion::Vector3 delta = frame.position - actorPos;
					focalDistance = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
				}
			}
		}

		if (focalDistance >= 0.2f && focalDistance <= 50.0f)
		{
			WritePhotographyDoFFocalPoint(focalDistance);
		}

		// Use icalls rather than the host's managed FOV detour.
		setOrthographic(cameraSelf, (frame.flags & 1u) != 0);
		setSize(cameraSelf, frame.orthographicSize);
		setFov(cameraSelf, frame.fov);
		writing = false;
	}

	inline bool Read(void* camera, void* transform, GakuMotion::CameraFrame& frame)
	{
		if (!OnUnityThread() || !IsAlive(camera) || !IsAlive(transform)) return false;
		void* cameraSelf = Self(camera);
		if (!cameraSelf) return false;
		UnityVector3 p = g_getWorldPosNative(transform);
		UnityQuaternion q = g_getWorldRotNative(transform);
		frame.position = {p.x, p.y, p.z};
		frame.rotation = {q.x, q.y, q.z, q.w};
		frame.fov = getFov(cameraSelf);
		frame.orthographicSize = getSize(cameraSelf);
		frame.flags = getOrthographic(cameraSelf) ? 1u : 0u;
		float norm = GakuMotion::Quaternion::Dot(frame.rotation, frame.rotation);
		return GakuMotion::CameraFinite(frame.position) && std::isfinite(frame.fov) &&
			frame.fov >= 1 && frame.fov <= 179 && std::isfinite(frame.orthographicSize) &&
			frame.orthographicSize > 0 && std::isfinite(norm) && norm > 1e-8f;
	}

	inline void ReapplyTransform(void* tr)
	{
		if (!OnUnityThread()) return;
		if (!writing && Controller().owned &&
			(nativeSelf ? tr == ownedNativeTransform : Controller().BlocksTransform(tr)))
			Controller().BeforeRender(Controller().camera);
	}

	inline void SharedTransformWritten(void*, uint32_t kind, void* transformSelf)
	{
		// Provider notifications can also originate from non-Unity writers. Do
		// not inspect controller state on those threads or after retirement.
		if (!OnUnityThread() || kind >= GAKULOCALIFY_CAMERA_WRITE_COUNT ||
			!sharedWritesEnabled.load(std::memory_order_acquire)) return;
		ReapplyTransform(transformSelf);
	}

	inline bool SubscribeSharedTransformWrites(void* const (&targets)[GAKULOCALIFY_CAMERA_WRITE_COUNT])
	{
		if (!GakuMotion::host::active) return true;
		GakuLocalifyCameraWriteService candidate{};
		const auto& api = GakuMotion::host::api;
		const auto queried = api.queryService
			? api.queryService(api.hostContext, GAKULOCALIFY_SERVICE_CAMERA_WRITE,
				GAKULOCALIFY_CAMERA_WRITE_VERSION, sizeof(candidate), &candidate)
			: GAKU_E_INVALID_ARGUMENT;
		// An absent provider keeps the existing exclusive-lease path. A present
		// but incompatible provider must never waive any interception lease.
		if (queried == GAKU_E_UNAVAILABLE) return true;
		if (queried != GAKU_OK || candidate.abiVersion != GAKULOCALIFY_CAMERA_WRITE_VERSION ||
			candidate.structSize < sizeof(candidate) || !candidate.getTarget ||
			!candidate.subscribe || !candidate.unsubscribe)
		{
			if (g_hostContext.logWarn) g_hostContext.logWarn(
				"[GakuMotion][Camera] Localify camera-write service incompatible: status=%d version=%u size=%u",
				queried, candidate.abiVersion, candidate.structSize);
			return false;
		}
		void* providerTargets[GAKULOCALIFY_CAMERA_WRITE_COUNT]{};
		bool anyProvided = false;
		for (uint32_t kind = 0; kind < GAKULOCALIFY_CAMERA_WRITE_COUNT; ++kind)
		{
			void* target = candidate.getTarget(candidate.context, kind);
			// A disabled or partially initialized provider owns only its non-null
			// targets. Unprovided entries still require our exclusive hook lease.
			if (target && (!targets[kind] || target != targets[kind]))
			{
				if (g_hostContext.logWarn) g_hostContext.logWarn(
					"[GakuMotion][Camera] Localify camera-write target mismatch: kind=%u provider=%p expected=%p",
					kind, target, targets[kind]);
				return false;
			}
			providerTargets[kind] = target;
			anyProvided |= target != nullptr;
		}
		if (!anyProvided) return true;
		const auto subscribed = candidate.subscribe(candidate.context, SharedTransformWritten, nullptr);
		if (subscribed != GAKU_OK)
		{
			if (g_hostContext.logWarn) g_hostContext.logWarn(
				"[GakuMotion][Camera] Localify camera-write subscription refused: status=%d", subscribed);
			return false;
		}
		sharedWrites = candidate;
		sharedWritesSubscribed = true;
		for (uint32_t kind = 0; kind < GAKULOCALIFY_CAMERA_WRITE_COUNT; ++kind)
			sharedWriteTargets[kind] = providerTargets[kind];
		if (g_hostContext.logInfo) g_hostContext.logInfo(
			"[GakuMotion][Camera] Localify camera-write observer connected: exact Transform targets validated");
		return true;
	}

	inline bool InstallHook(const char* name, void* target, void* detour, void** original, int sharedKind = -1)
	{
		if (!RequireDependency(name, target)) return false;
		if (sharedWritesSubscribed && sharedKind >= 0 && sharedKind < GAKULOCALIFY_CAMERA_WRITE_COUNT &&
			sharedWriteTargets[sharedKind] == target) return true;
		if (GakuMotion::host::active)
		{
			const auto acquired = GakuMotion::host::hooks.acquire(
				GakuMotion::host::hooks.context, target, detour, original);
			if (acquired == GAKU_OK) installed.push_back(target);
			else if (g_hostContext.logWarn) g_hostContext.logWarn(
				"[GakuMotion][Camera] GakuLoader hook lease refused: %s status=%d", name, acquired);
			return acquired == GAKU_OK;
		}
		const auto created = MH_CreateHook(target, detour, original);
		if (created != MH_OK)
		{
			if (g_hostContext.logWarn) g_hostContext.logWarn(
				"[GakuMotion][Camera] MH_CreateHook failed: %s target=%p status=%s", name, target,
				MH_StatusToString(created));
			return false;
		}
		installed.push_back(target);
		const auto enabled = MH_EnableHook(target);
		if (enabled != MH_OK && g_hostContext.logWarn)
			g_hostContext.logWarn("[GakuMotion][Camera] MH_EnableHook failed: %s target=%p status=%s", name, target,
				MH_StatusToString(enabled));
		return enabled == MH_OK;
	}

	inline void ReapplyLens(void* c)
	{
		if (!OnUnityThread()) return;
		if (!writing && Controller().owned &&
			(nativeSelf ? c == ownedNativeCamera : Controller().BlocksCamera(c)))
			Controller().BeforeRender(Controller().camera);
	}

	inline void WorldPosition(void* tr, UnityVector3* p)
	{
		worldPosition(tr, p);
		ReapplyTransform(tr);
	}

	inline void LocalPosition(void* tr, UnityVector3* p)
	{
		localPosition(tr, p);
		ReapplyTransform(tr);
	}

	inline void WorldRotation(void* tr, UnityQuaternion* q)
	{
		worldRotation(tr, q);
		ReapplyTransform(tr);
	}

	inline void LocalRotation(void* tr, UnityQuaternion* q)
	{
		localRotation(tr, q);
		ReapplyTransform(tr);
	}

	inline void WorldPose(void* tr, UnityVector3* p, UnityQuaternion* q)
	{
		worldPose(tr, p, q);
		ReapplyTransform(tr);
	}

	inline void LocalPose(void* tr, UnityVector3* p, UnityQuaternion* q)
	{
		localPose(tr, p, q);
		ReapplyTransform(tr);
	}

	inline void Fov(void* c, float v)
	{
		setFov(c, v);
		ReapplyLens(c);
	}

	inline void Size(void* c, float v)
	{
		setSize(c, v);
		ReapplyLens(c);
	}

	inline void Orthographic(void* c, bool v)
	{
		setOrthographic(c, v);
		ReapplyLens(c);
	}

	inline void SharedCameraBegin(void*, void* camera)
	{
		if (sharedBeginEnabled.load(std::memory_order_acquire) && OnUnityThread())
			Controller().BeforeRender(camera);
	}
	// selected=false keeps independent leases. A present but invalid provider
	// fails visibly rather than requesting the same exclusive leases.
	inline bool ConnectSharedCameraBegin(void* begin, void* manager, bool& selected)
	{
		selected = false;
		if (!GakuMotion::host::active) return true;
		GakuVrifyCameraBeginService service{};
		const auto status = GakuMotion::host::api.queryService(GakuMotion::host::api.hostContext,
			GAKU_VRIFY_CAMERA_BEGIN_SERVICE, 1, sizeof(service), &service);
		if (status == GAKU_E_UNAVAILABLE) return true;
		selected = true;
		if (status != GAKU_OK || service.abiVersion != 1 || service.structSize < sizeof(service) ||
			!service.context || !service.getTarget || !service.subscribe || !service.unsubscribe ||
			(!begin && !manager))
		{
			if (g_hostContext.logWarn) g_hostContext.logWarn("[GakuMotion][Camera] VRify camera-begin service invalid: status=%d", status);
			return false;
		}
		// No installed VR begin hooks (VR disabled/native-only) leaves Motion sole owner.
		if (!service.getTarget(service.context, 0) && !service.getTarget(service.context, 1))
		{
			selected = false;
			return true;
		}
		if (service.getTarget(service.context, 0) != begin ||
			service.getTarget(service.context, 1) != manager)
		{
			if (g_hostContext.logWarn) g_hostContext.logWarn("[GakuMotion][Camera] VRify camera-begin provider incompatible or target mismatch: status=%d", status);
			return false;
		}
		const auto subscribed = service.subscribe(service.context, SharedCameraBegin, nullptr);
		if (subscribed != GAKU_OK) return false;
		sharedBegin = service;
		sharedBeginSubscribed = true;
		sharedBeginEnabled.store(true, std::memory_order_release);
		if (g_hostContext.logInfo) g_hostContext.logInfo("[GakuMotion][Camera] VRify camera-begin observer connected: exact render targets validated");
		return true;
	}

	inline void BeforeCameraRendering(void* context, void* camera, void* method)
	{
		if (render) render(context, camera, method);
		// Event handlers and the host run first, then the MMD pose wins at render.
		if (OnUnityThread()) Controller().BeforeRender(camera);
	}

	inline void BeforeCameraRenderingManager(void* context, void* camera, void* method)
	{
		if (managerRender) managerRender(context, camera, method);
		if (OnUnityThread()) Controller().BeforeRender(camera);
	}

	using AddRenderPassesFn = void (*)(void*, void*, void*, void*);
	inline AddRenderPassesFn orig_AddRenderPasses = nullptr;
	using ShadowObserver = void (*)(void*, void*, const char*);
	inline ShadowObserver shadowObserver = nullptr;
	inline void ObserveShadow(void* feature, void* data, const char* phase) noexcept
	{
		// A diagnostic failure must never suppress or repeat the game's draw.
		try { if (shadowObserver) shadowObserver(feature, data, phase); }
		catch (...) {}
	}
	inline void InvokeShadowOriginal(void* self, void* renderer, void* data, void* method)
	{
		ObserveShadow(self, data, "native");
		try { if (orig_AddRenderPasses) orig_AddRenderPasses(self, renderer, data, method); }
		catch (...) { ObserveShadow(self, data, "aborted"); throw; }
	}
	inline int renderingDataCameraDataOffset = -1;
	inline int cameraDataViewMatrixOffset = -1;
	inline int cameraDataWorldPosOffset = -1;
	inline bool shadowOffsetsResolved = false;

	inline void ResolveShadowFieldOffsets()
	{
		if (shadowOffsetsResolved) return;
		shadowOffsetsResolved = true;

		void* renderingDataClass = FindClass("Unity.RenderPipelines.Universal.Runtime.dll",
											 "UnityEngine.Rendering.Universal", "RenderingData");
		void* cameraDataClass = FindClass("Unity.RenderPipelines.Universal.Runtime.dll",
										  "UnityEngine.Rendering.Universal", "CameraData");
		if (!renderingDataClass || !cameraDataClass || !g_faceRuntime.findField || !il2cpp_field_get_offset)
			return;

		constexpr int kBoxedHeader = 16;
		void* fCamData = g_faceRuntime.findField(renderingDataClass, "cameraData");
		if (fCamData)
		{
			int off = il2cpp_field_get_offset(fCamData);
			if (off >= kBoxedHeader) renderingDataCameraDataOffset = off - kBoxedHeader;
		}

		void* fView = g_faceRuntime.findField(cameraDataClass, "m_ViewMatrix");
		if (!fView) fView = g_faceRuntime.findField(cameraDataClass, "viewMatrix");
		if (fView)
		{
			int off = il2cpp_field_get_offset(fView);
			if (off >= kBoxedHeader) cameraDataViewMatrixOffset = off - kBoxedHeader;
		}

		void* fPos = g_faceRuntime.findField(cameraDataClass, "worldSpaceCameraPos");
		if (fPos)
		{
			int off = il2cpp_field_get_offset(fPos);
			if (off >= kBoxedHeader) cameraDataWorldPosOffset = off - kBoxedHeader;
		}

		if (g_hostContext.logInfo)
		{
			g_hostContext.logInfo("[GakuMotion][ShadowExp] rdCamOff=%d viewOff=%d posOff=%d",
								  renderingDataCameraDataOffset, cameraDataViewMatrixOffset, cameraDataWorldPosOffset);
		}
	}

	inline void Detour_AddRenderPasses(void* self, void* renderer, void* renderingData, void* methodInfo)
	{
		ObserveShadow(self, renderingData, "entry");
		const int expMode = Controller().settings.actorShadowExperimentMode;

		// Mode 0: Native game default
		if (expMode == 0 || !renderingData || !OnUnityThread())
		{
			InvokeShadowOriginal(self, renderer, renderingData, methodInfo);
			ObserveShadow(self, renderingData, "exit");
			return;
		}

		// Mode 2: Bypass actor shadow pass completely
		if (expMode == 2)
		{
			ObserveShadow(self, renderingData, "bypass");
			return;
		}

		// Mode 1: Anchor shadow camera near actor if distance is large
		ResolveShadowFieldOffsets();
		if (renderingDataCameraDataOffset < 0 || cameraDataViewMatrixOffset < 0 || cameraDataWorldPosOffset < 0)
		{
			InvokeShadowOriginal(self, renderer, renderingData, methodInfo);
			ObserveShadow(self, renderingData, "exit");
			return;
		}

		uint8_t* pCamData = reinterpret_cast<uint8_t*>(renderingData) + renderingDataCameraDataOffset;
		float* pWorldPos = reinterpret_cast<float*>(pCamData + cameraDataWorldPosOffset);
		float* pView = reinterpret_cast<float*>(pCamData + cameraDataViewMatrixOffset);

		float origPos[3] = { pWorldPos[0], pWorldPos[1], pWorldPos[2] };
		float origViewTrans[3] = { pView[12], pView[13], pView[14] };

		bool anchored = false;
		struct RestoreShadowCamera
		{
			float* position;
			float* view;
			const float* savedPosition;
			const float* savedTranslation;
			bool& active;
			void Restore() noexcept
			{
				if (!active) return;
				std::memcpy(position, savedPosition, 3 * sizeof(float));
				std::memcpy(view + 12, savedTranslation, 3 * sizeof(float));
				active = false;
			}
			~RestoreShadowCamera() { Restore(); }
		} restore{pWorldPos, pView, origPos, origViewTrans, anchored};
		auto& ctrl = Controller();
		if (ctrl.actor && ctrl.fnActorPose)
		{
			GakuMotion::Vector3 actorPos, hips;
			GakuMotion::Quaternion actorRot;
			if (ctrl.fnActorPose(ctrl.actor, actorPos, actorRot, hips))
			{
				float dx = origPos[0] - actorPos.x;
				float dy = origPos[1] - (actorPos.y + 1.0f);
				float dz = origPos[2] - actorPos.z;
				float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

				if (dist > 2.0f && std::isfinite(dist))
				{
					float scale = 1.8f / dist;
					float anchorPos[3] = {
						actorPos.x + dx * scale,
						(actorPos.y + 1.0f) + dy * scale,
						actorPos.z + dz * scale
					};

					float newTx = -(pView[0] * anchorPos[0] + pView[4] * anchorPos[1] + pView[8] * anchorPos[2]);
					float newTy = -(pView[1] * anchorPos[0] + pView[5] * anchorPos[1] + pView[9] * anchorPos[2]);
					float newTz = -(pView[2] * anchorPos[0] + pView[6] * anchorPos[1] + pView[10] * anchorPos[2]);

					pWorldPos[0] = anchorPos[0];
					pWorldPos[1] = anchorPos[1];
					pWorldPos[2] = anchorPos[2];

					pView[12] = newTx;
					pView[13] = newTy;
					pView[14] = newTz;

					anchored = true;
				}
			}
		}

		InvokeShadowOriginal(self, renderer, renderingData, methodInfo);
		restore.Restore();
		ObserveShadow(self, renderingData, "exit");
	}

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
			const auto placement = GakuMotion::Core::Instance().player.controller.initialPositionOffset;
			origin = GakuMotion::Vector3{p.x, p.y, p.z} + placement;
			hips = {h.x, h.y, h.z};
			rotation = {q.x, q.y, q.z, q.w};
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
