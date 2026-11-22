#pragma once

// Included inside CameraBridge / GakuMotionPlugin.cpp's anonymous namespace.
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

	inline bool RequireDependency(const char* name, const void* value)
	{
		if (value) return true;
		if (g_hostContext.logWarn) g_hostContext.logWarn("[GakuMotion][Camera] missing required dependency: %s", name);
		return false;
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
} // namespace CameraBridge
