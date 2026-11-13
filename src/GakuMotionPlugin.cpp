#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "GkCaptureGuard.hpp"
#include "GkCaptureSafety.hpp"
#include "GkGmlClient.hpp"
#include "GakuMotion.hpp"
#include "GakuMotionControl.h"
#include "GkLog.hpp"
#include "GkCsharpImport.hpp"
#include "GkHotkey.hpp"
#include "GakuSafeBoneLookup.h"
#include "GkExactMethod.hpp"
#include "GkLiveApi.hpp"
#include "GkLiveViewingGate.hpp"
#include "GkLiveTakeoverDriver.hpp"
#include "GkBgmSuppression.hpp"
#include <cstddef>
#include "GkFaceRuntime.hpp"
#include "GkBurstBridgeClient.hpp"
#include "GkFallbackPhysics.hpp"
#include "GakuPhysicsDiagnostics.h"
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <fstream>
#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <limits>
#include <sstream>
#include <iomanip>
#include <locale>
#include "GkWindowsPaths.hpp"

static bool RefreshSceneActors(GakuMotion::Core& core, bool force = false);

namespace
{
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

	void ClearSecondaryPhysics(bool abandonedScene = false)
	{
		if (abandonedScene) g_capturePhysicsUnconfirmed = false;
		else if (g_fallbackPhysics.HasCaptureWrites() || g_burstBridge.HasCaptureInput())
			g_capturePhysicsUnconfirmed = true;
		g_burstBridge.Clear();
		g_fallbackPhysics.Clear();
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

	HMODULE g_gameAssembly{nullptr};

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
	GakuMotion::live_takeover::Api g_liveApi;
	GakuMotion::live_takeover::ViewingGate g_liveViewingGate;
	GakuMotion::live_takeover::Driver g_liveDriver;
	std::atomic<bool> g_liveTakeoverEnabled{false};
	uintptr_t g_liveTakeoverOwner = 0, g_lastLiveActorOwner = 0;
	int g_liveTakeoverScene = 0, g_lastLiveActorScene = 0;
	std::chrono::steady_clock::time_point g_lastLiveActorTime{};
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

	static std::string Il2CppStringToUtf8(void* strObj)
	{
		if (!strObj) return "<null>";
		auto s = reinterpret_cast<Il2CppString*>(strObj);
		if (!s || s->length <= 0) return "";
		int size = WideCharToMultiByte(CP_UTF8, 0, s->chars, s->length, nullptr, 0, nullptr, nullptr);
		if (size <= 0) return "<conv_err>";
		std::string res(size, '\0');
		WideCharToMultiByte(CP_UTF8, 0, s->chars, s->length, res.data(), size, nullptr, nullptr);
		return res;
	}

	void* HelperGetFaceModel(void* actor)
	{
		if (!actor || !g_faceModelTypeObj || !g_getComponentInChildrenNative) return nullptr;
		return g_getComponentInChildrenNative(actor, g_faceModelTypeObj);
	}

	uint64_t HelperFaceQpcTicks()
	{
		LARGE_INTEGER ticks{};
		QueryPerformanceCounter(&ticks);
		return static_cast<uint64_t>(ticks.QuadPart);
	}

	int (*g_faceUnityFrame)(){nullptr};

	std::string HelperGetFaceShapeName(void* model, int index)
	{
		return g_faceShapeName ? Il2CppStringToUtf8(g_faceShapeName(model, index)) : "";
	}

	void* HelperGetFacial(void* actor) { return g_faceRuntime.Facial(actor); }

	bool HelperReadBlink(void* facial, bool& useBlink, bool& autoDisabled)
	{
		const bool ok = g_faceRuntime.ReadBlink(facial, useBlink, autoDisabled);
		if (!ok && g_hostContext.logWarn)
		{
			void* klass = g_faceRuntime.objectClass ? g_faceRuntime.objectClass(facial) : nullptr;
			if (g_blinkUnavailableClasses.insert(klass).second)
				g_hostContext.logWarn("[GakuMotion][Face] Blink fields unavailable; facial weights remain playable");
		}
		return ok;
	}

	bool HelperWriteBlink(void* facial, bool useBlink, bool autoDisabled)
	{
		const bool ok = g_faceRuntime.WriteBlink(facial, useBlink, autoDisabled);
		if (!ok && g_hostContext.logWarn)
		{
			void* klass = g_faceRuntime.objectClass ? g_faceRuntime.objectClass(facial) : nullptr;
			if (g_blinkUnavailableClasses.insert(klass).second)
				g_hostContext.logWarn(
					"[GakuMotion][Face] Blink fields cannot be written; native blink configuration retained");
		}
		return ok;
	}

	void HelperBeginFaceBinding()
	{
		g_blinkUnavailableClasses.clear();
		g_highlightUnavailableClasses.clear();
	}

	bool HelperRefreshHighlight(void* actor)
	{
		const bool ok = g_faceRuntime.RefreshHighlight(actor);
		if (!ok && g_hostContext.logWarn)
		{
			void* klass = g_faceRuntime.objectClass ? g_faceRuntime.objectClass(actor) : nullptr;
			if (g_highlightUnavailableClasses.insert(klass).second)
				g_hostContext.logWarn(
					"[GakuMotion][Face] Highlight refresh unavailable or raised a managed exception; native component remains enabled");
		}
		return ok;
	}

	std::string HelperGetActorName(void* actor)
	{
		if (!actor) return "";
		if (g_getGameObjectNative && g_getNameNative)
		{
			void* go = g_getGameObjectNative(actor, nullptr);
			if (go)
			{
				std::string name = Il2CppStringToUtf8(g_getNameNative(go));
				if (!name.empty()) return name;
			}
		}
		if (g_getNameNative)
		{
			std::string name = Il2CppStringToUtf8(g_getNameNative(actor));
			if (!name.empty()) return name;
		}
		return "";
	}

	std::string HelperGetFaceModelName(void* model)
	{
		// VLActorFaceModel is an existing Component. Use the same confirmed
		// Unity Component/Object methods as actor naming, without new hooks.
		if (!model || !g_getGameObjectNative || !g_getNameNative || !g_componentKlass ||
			!g_faceRuntime.objectClass || !g_faceRuntime.parentClass) return "";
		bool component = false;
		void* klass = g_faceRuntime.objectClass(model);
		for (int depth = 0; klass && depth < 64; ++depth, klass = g_faceRuntime.parentClass(klass))
			if (klass == g_componentKlass) { component = true; break; }
		if (!component) return "";
		void* go = g_getGameObjectNative(model, nullptr);
		return go ? Il2CppStringToUtf8(g_getNameNative(go)) : "";
	}

	void ProbeTransformHierarchy(void* rootTransform, const char* label, int depth = 0, int maxDepth = 3)
	{
		if (!rootTransform || depth > maxDepth) return;
		std::string indent(depth * 2, ' ');
		std::string name = "<unknown>";
		if (g_getNameNative)
		{
			name = Il2CppStringToUtf8(g_getNameNative(rootTransform));
		}
		if (g_hostContext.logInfo)
		{
			g_hostContext.logInfo("[GakuMotion][BoneTree][%s] %s- %s (ptr=%p)", label, indent.c_str(), name.c_str(),
			                      rootTransform);
		}
		if (g_getChildCountNative && g_getChildNative)
		{
			int count = g_getChildCountNative(rootTransform);
			for (int i = 0; i < count; ++i)
			{
				void* child = g_getChildNative(rootTransform, i);
				ProbeTransformHierarchy(child, label, depth + 1, maxDepth);
			}
		}
	}

	int HelperGetChildCount(void* transform)
	{
		if (!transform || !g_getChildCountNative) return 0;
		return g_getChildCountNative(transform);
	}

	void* HelperGetChild(void* transform, int index)
	{
		if (!transform || !g_getChildNative) return nullptr;
		return g_getChildNative(transform, index);
	}

	std::string HelperGetTransformName(void* transform)
	{
		if (!transform || !g_getNameNative) return "";
		return Il2CppStringToUtf8(g_getNameNative(transform));
	}

	void HelperGetLocalPos(void* transform, float* outPos)
	{
		if (g_getPosNative && transform && outPos)
		{
			UnityVector3 v = g_getPosNative(transform);
			outPos[0] = v.x;
			outPos[1] = v.y;
			outPos[2] = v.z;
		}
	}

	void HelperSetLocalPos(void* transform, float x, float y, float z)
	{
		if (g_setPosNative && transform)
		{
			UnityVector3 v{x, y, z};
			g_setPosNative(transform, v);
		}
	}

	void HelperGetLocalRot(void* transform, float* outRot)
	{
		if (g_getRotNative && transform && outRot)
		{
			UnityQuaternion q = g_getRotNative(transform);
			outRot[0] = q.x;
			outRot[1] = q.y;
			outRot[2] = q.z;
			outRot[3] = q.w;
		}
	}

	void HelperSetLocalRot(void* transform, float x, float y, float z, float w)
	{
		if (g_setRotNative && transform)
		{
			UnityQuaternion q{x, y, z, w};
			g_setRotNative(transform, q);
		}
	}

	void HelperGetWorldPos(void* transform, float* outPos)
	{
		if (g_getWorldPosNative && transform && outPos)
		{
			UnityVector3 v = g_getWorldPosNative(transform);
			outPos[0] = v.x;
			outPos[1] = v.y;
			outPos[2] = v.z;
		}
	}

	void HelperSetWorldPos(void* transform, float x, float y, float z)
	{
		if (g_setWorldPosNative && transform)
		{
			UnityVector3 v{x, y, z};
			g_setWorldPosNative(transform, v);
		}
	}

	void HelperGetWorldRot(void* transform, float* outRot)
	{
		if (g_getWorldRotNative && transform && outRot)
		{
			UnityQuaternion q = g_getWorldRotNative(transform);
			outRot[0] = q.x;
			outRot[1] = q.y;
			outRot[2] = q.z;
			outRot[3] = q.w;
		}
	}

	void HelperSetWorldRot(void* transform, float x, float y, float z, float w)
	{
		if (g_setWorldRotNative && transform)
		{
			UnityQuaternion q{x, y, z, w};
			g_setWorldRotNative(transform, q);
		}
	}

	void* HelperGetComponentTransform(void* component)
	{
		if (g_getComponentTransformNative && component)
		{
			return g_getComponentTransformNative(component);
		}
		return nullptr;
	}

	void* HelperGetGameObjectTransform(void* go)
	{
		if (g_getGameObjectTransformNative && go)
		{
			return g_getGameObjectTransformNative(go);
		}
		return nullptr;
	}

	void HelperSetAnimatorEnabled(void* animator, bool enabled)
	{
		if (g_setAnimatorEnabledNative && animator)
		{
			g_setAnimatorEnabledNative(animator, enabled);
		}
	}

	bool HelperGetAnimatorEnabled(void* animator)
	{
		return animator && g_getAnimatorEnabledNative && g_getAnimatorEnabledNative(animator);
	}

	void HelperAnimatorRebind(void* animator)
	{
		if (g_animatorRebindNative && animator)
		{
			g_animatorRebindNative(animator);
		}
	}

	void* HelperGetAnimatorController(void* animator)
	{
		if (g_getAnimatorControllerNative && animator)
		{
			return g_getAnimatorControllerNative(animator);
		}
		return nullptr;
	}

	void HelperSetAnimatorController(void* animator, void* controller)
	{
		if (g_setAnimatorControllerNative && animator)
		{
			g_setAnimatorControllerNative(animator, controller);
		}
	}

	void HelperPauseDirector(void* director)
	{
		if (g_pauseDirectorNative && director)
		{
			g_pauseDirectorNative(director);
		}
	}

	// -----------------------------------------------------------------------------
	// The host owns UTF-16 paths; all Motion string paths are UTF-8. Filesystem
	// streams use MotionPathFromUtf8 rather than the Windows ANSI code page.
	// -----------------------------------------------------------------------------
	std::string WidePathToNarrow(const wchar_t* wide)
	{
		if (!wide || !*wide) return std::string();
		std::string out;
		if (!GakuMotion::windows_path::ToUtf8(wide, out)) return {};
		return out;
	}

	std::wstring ToWide(const std::string& text, UINT codePage)
	{
		if (text.empty()) return {};
		if (codePage != CP_UTF8) return {};
		std::wstring out;
		if (!GakuMotion::windows_path::FromUtf8(text, out)) return {};
		return out;
	}

	std::string g_importLog;
	uint64_t g_importLogReadOffset{0};
	std::string g_importPendingLine;
	uint64_t g_fileDialogTicket{0};

	void ReleasePendingFileDialog()
	{
		const auto ticket = g_fileDialogTicket;
		g_fileDialogTicket = 0;
		if (ticket && GakuMotion::host::fileDialog.release)
			GakuMotion::host::fileDialog.release(GakuMotion::host::fileDialog.context, ticket);
	}

	bool BeginMotionFile(int kind, std::string& notice)
	{
		const auto& service = GakuMotion::host::fileDialog;
		if (!service.beginOpen || !service.poll || !service.release)
		{
			notice = "GakuLoader 文件选择服务不可用，请更新宿主或输入完整文件路径";
			return false;
		}
		if (g_fileDialogTicket)
		{
			notice = "已有文件选择请求正在进行";
			return false;
		}
		const GakuFileFilter filter = kind == 0 ? GakuFileFilter{L"VMD motion", L"*.vmd"}
			: kind == 2 ? GakuFileFilter{L"Camera VMD / GKCAM", L"*.vmd;*.gkcamera"}
			: GakuFileFilter{L"WAV audio", L"*.wav"};
		const GakuFileDialogRequest request{GAKU_SERVICE_VERSION, sizeof(GakuFileDialogRequest),
			kind == 0 ? L"选择 VMD 动作" : kind == 2 ? L"选择镜头文件" : L"选择 WAV 音频", 1, &filter};
		uint64_t ticket = 0;
		const auto status = service.beginOpen(service.context, &request, &ticket);
		if (status != GAKU_OK || !ticket)
		{
			char detail[128]{};
			std::snprintf(detail, sizeof(detail), "文件选择请求失败 (status=%d)", status);
			notice = detail;
			return false;
		}
		g_fileDialogTicket = ticket;
		notice.clear();
		return true;
	}

	int PollMotionFile(std::string& path, std::string& notice)
	{
		const auto& service = GakuMotion::host::fileDialog;
		if (!g_fileDialogTicket || !service.poll)
		{
			notice = "文件选择请求已失效";
			ReleasePendingFileDialog();
			return GAKU_FILE_DIALOG_FAILED;
		}
		GakuFileDialogResult result{};
		auto status = service.poll(service.context, g_fileDialogTicket, sizeof(result), &result, 0, nullptr);
		if (status == GAKU_OK && result.state == GAKU_FILE_DIALOG_PENDING) return GAKU_FILE_DIALOG_PENDING;
		if (status == GAKU_OK && result.state == GAKU_FILE_DIALOG_ACCEPTED)
		{
			// The host supplies UTF-8 including NUL. Bound allocation even for an
			// invalid provider result; the GUI separately refuses truncation.
			if (result.requiredBytes == 0 || result.requiredBytes > 1024 * 1024) status = GAKU_E_SIZE;
			else
			{
				std::string selected(result.requiredBytes, '\0');
				status = service.poll(service.context, g_fileDialogTicket, sizeof(result), &result,
					static_cast<uint32_t>(selected.size()), selected.data());
				if (status == GAKU_OK && result.state == GAKU_FILE_DIALOG_ACCEPTED && selected.back() == '\0')
				{
					selected.resize(std::strlen(selected.c_str()));
					path = std::move(selected);
					notice.clear();
					ReleasePendingFileDialog();
					return GAKU_FILE_DIALOG_ACCEPTED;
				}
				if (status == GAKU_OK) status = GAKU_E_INTERNAL;
			}
		}
		if (status == GAKU_OK && result.state == GAKU_FILE_DIALOG_CANCELLED)
		{
			notice.clear();
			ReleasePendingFileDialog();
			return GAKU_FILE_DIALOG_CANCELLED;
		}
		char detail[160]{};
		std::snprintf(detail, sizeof(detail), "文件选择失败 (status=%d, HRESULT=0x%08X)",
			status, static_cast<unsigned int>(result.osError));
		notice = detail;
		GakuMotion::host::Warn("[GakuMotion][FileDialog] %s", detail);
		ReleasePendingFileDialog();
		return GAKU_FILE_DIALOG_FAILED;
	}

	bool PickMotionFile(int kind, std::string& result)
	{
		wchar_t buffer[32768]{};
		OPENFILENAMEW dialog{};
		dialog.lStructSize = sizeof(dialog);
		// Legacy Localify owns a real desktop window. Avoid accidentally owning
		// the dialog to another process if focus changed before this callback.
		dialog.hwndOwner = FindWindowW(L"Gakumas Localify", L"Gakumas Config GUI");
		if (!dialog.hwndOwner)
		{
			HWND foreground = GetForegroundWindow();
			DWORD process = 0;
			if (foreground) GetWindowThreadProcessId(foreground, &process);
			if (process == GetCurrentProcessId()) dialog.hwndOwner = foreground;
		}
		dialog.lpstrFile = buffer;
		dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
		dialog.lpstrFilter = kind == 0
			                     ? L"VMD motion\0*.vmd\0\0"
			                     : kind == 2
			                     ? L"Camera VMD / GKCAM\0*.vmd;*.gkcamera\0\0"
			                     : L"WAV audio\0*.wav\0\0";
		dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (!GetOpenFileNameW(&dialog))
		{
			const DWORD error = CommDlgExtendedError();
			if (error && g_hostContext.logWarn)
				g_hostContext.logWarn("[GakuMotion][FileDialog] legacy open failed error=0x%08lx owner=%p thread=%lu", error, dialog.hwndOwner, GetCurrentThreadId());
			else if (!error && g_hostContext.logInfo)
				g_hostContext.logInfo("[GakuMotion][FileDialog] legacy open cancelled");
			return false;
		}
		const int n = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
		result.resize(static_cast<size_t>(n));
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, result.data(), n, nullptr, nullptr);
		result.resize(static_cast<size_t>(n - 1));
		return true;
	}

	void DrainImportProgressLog(GakuMotion::ImportProgressState* progress)
	{
		if (g_importLog.empty()) return;
		HANDLE h = CreateFileW(ToWide(g_importLog, CP_UTF8).c_str(), GENERIC_READ,
		                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return;

		LARGE_INTEGER offset;
		offset.QuadPart = static_cast<LONGLONG>(g_importLogReadOffset);
		if (!SetFilePointerEx(h, offset, nullptr, FILE_BEGIN))
		{
			CloseHandle(h);
			return;
		}

		char buffer[4096];
		DWORD bytesRead = 0;
		while (ReadFile(h, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
		{
			g_importLogReadOffset += bytesRead;
			g_importPendingLine.append(buffer, bytesRead);
		}
		CloseHandle(h);

		size_t pos = 0;
		while ((pos = g_importPendingLine.find('\n')) != std::string::npos)
		{
			std::string line = g_importPendingLine.substr(0, pos);
			g_importPendingLine.erase(0, pos + 1);
			if (!line.empty() && line.back() == '\r') line.pop_back();

			if (line.rfind("@PROGRESS:", 0) == 0)
			{
				const size_t c1 = line.find(':', 10);
				if (c1 != std::string::npos)
				{
					const size_t c2 = line.find(':', c1 + 1);
					if (c2 != std::string::npos)
					{
						const std::string pctStr = line.substr(10, c1 - 10);
						const std::string stage = line.substr(c1 + 1, c2 - (c1 + 1));
						const std::string msg = line.substr(c2 + 1);
						char* endPtr = nullptr;
						const float pct = std::strtof(pctStr.c_str(), &endPtr);
						if (endPtr != pctStr.c_str() && progress)
						{
							progress->active = true;
							progress->progress = std::clamp(pct / 100.0f, 0.0f, 1.0f);
							progress->stage = stage;
							progress->statusText = msg;
						}
					}
				}
			}
		}
	}

	bool StartMotionImport(const GakuMotion::MotionImportRequest& r, std::string& message)
	{
		if (GakuMotion::csharp_import::Busy() || GakuMotion::csharp_import::CameraBusy())
		{
			message = "已有导入任务正在运行";
			return false;
		}
		if (!GakuMotion::csharp_import::CanHandle(r, message)) return false;
		if (!GakuMotion::csharp_import::Begin(r, message)) return false;
		g_importLog = r.log;
		g_importLogReadOffset = 0;
		g_importPendingLine.clear();
		return true;
	}

	int PollMotionImportWithProgress(std::string& message, GakuMotion::ImportProgressState* progress)
	{
		if (!GakuMotion::csharp_import::Busy()) return 0;
		DrainImportProgressLog(progress);
		return GakuMotion::csharp_import::Poll(message, progress);
	}

	int PollMotionImport(std::string& message)
	{
		return PollMotionImportWithProgress(message, nullptr);
	}

	bool StartCameraImport(const GakuMotion::CameraImportRequest& r, std::string& message)
	{
		if (GakuMotion::csharp_import::Busy() || GakuMotion::csharp_import::CameraBusy())
		{
			message = "已有导入任务正在运行";
			return false;
		}
		if (r.source.empty() || r.motionDirectory.empty())
		{
			message = "请选择镜头文件与目标曲目";
			return false;
		}
		if (!std::isfinite(r.fps) || !std::isfinite(r.scale) || r.fps < 30 || r.fps > 240 || r.scale <= 0)
		{
			message = "镜头采样率或缩放无效";
			return false;
		}
		return GakuMotion::csharp_import::BeginCamera(r.source, r.motionDirectory,
			r.fps, r.scale, r.detectCuts, r.log, message);
	}

	int PollCameraImport(std::string& message)
	{
		return GakuMotion::csharp_import::PollCamera(message);
	}

	// -----------------------------------------------------------------------------
	// Windows MCI Audio Helpers：硬件播放、毫秒时钟与「进度跳转 / 断点续播」
	// -----------------------------------------------------------------------------
	std::string g_lastAudioPath{};
	GakuMotion::audio::Observation g_audioObservation{};
	MCIERROR g_audioCommandError{0}; // A successful status query cannot erase a failed transport.
	float g_musicVolume{1.0f};
	int g_musicVolumeSupport{0}; // 0=no open source, 1=available, -1=unavailable
	bool g_audioUsesDigitalVideo{false};
	decltype(&mciSendStringA) g_sendMciAudio = &GakuMotion::windows_path::SendMciUtf8;
	GakuMotion::bgm::Lease g_bgmLease;
	GakuMotion::bgm::Lease g_voiceLease;

	bool HelperAudioMissingAlias(MCIERROR error)
	{
		return error == MCIERR_INVALID_DEVICE_ID || error == MCIERR_INVALID_DEVICE_NAME;
	}

	bool HelperAudioSetVolume(float volume)
	{
		if (!std::isfinite(volume)) return false;
		g_musicVolume = std::clamp(volume, 0.0f, 1.0f);
		if (!g_audioObservation.sourceOpen) { g_musicVolumeSupport = 0; return true; }
		if (!g_audioUsesDigitalVideo) { g_musicVolumeSupport = -1; return false; }
		char cmd[80]{};
		std::snprintf(cmd, sizeof(cmd), "setaudio mmd_song volume to %d",
		              static_cast<int>(std::lround(g_musicVolume * 1000.0f)));
		const MCIERROR result = g_sendMciAudio(cmd, NULL, 0, NULL);
		g_musicVolumeSupport = result ? -1 : 1;
		// A volume failure must not poison transport observation or synchronization.
		return result == 0;
	}
	int HelperAudioVolumeSupport() { return g_musicVolumeSupport; }

	GakuMotion::audio::Observation HelperAudioObserve()
	{
		auto observed = g_audioObservation;
		if (!observed.sourceOpen) return observed;
		char mode[32]{};
		const MCIERROR rc = g_sendMciAudio("status mmd_song mode", mode, sizeof(mode), NULL);
		if (HelperAudioMissingAlias(rc)) observed.sourceOpen = false;
		observed.errorCode = g_audioCommandError ? g_audioCommandError : rc;
		observed.confirmed = observed.errorCode == 0;
		observed.state = observed.errorCode ? GakuMotion::audio::PlaybackState::Failed :
			std::strcmp(mode, "playing") == 0 ? GakuMotion::audio::PlaybackState::Playing :
			std::strcmp(mode, "paused") == 0 ? GakuMotion::audio::PlaybackState::Paused :
			std::strcmp(mode, "stopped") == 0 ? GakuMotion::audio::PlaybackState::Stopped : GakuMotion::audio::PlaybackState::Starting;
		if (observed.state == GakuMotion::audio::PlaybackState::Starting) observed.confirmed = false;
		g_audioObservation = observed;
		return observed;
	}

	void HelperAudioRecordCommand(MCIERROR error)
	{
		g_audioCommandError = error;
		g_audioObservation.errorCode = error;
		g_audioObservation.confirmed = false;
		if (error) g_audioObservation.state = GakuMotion::audio::PlaybackState::Failed;
		g_audioObservation = HelperAudioObserve();
	}

	bool HelperAudioIsOpen()
	{
		char buf[64]{};
		const MCIERROR error = g_sendMciAudio("status mmd_song mode", buf, sizeof(buf), NULL);
		if (error)
		{
			if (HelperAudioMissingAlias(error)) g_audioObservation.sourceOpen = false;
			HelperAudioRecordCommand(error);
			return false;
		}
		g_audioObservation.sourceOpen = buf[0] != '\0';
		return g_audioObservation.sourceOpen;
	}

	int HelperAudioMsFromSeconds(float seconds)
	{
		if (!std::isfinite(seconds) || seconds <= 0.0f) return 0;
		return static_cast<int>((std::min)(double(seconds) * 1000.0 + 0.5,
			static_cast<double>((std::numeric_limits<int>::max)())));
	}

	// 打开并以 startMs 为起点播放
	void HelperAudioOpenAndPlay(const std::string& path, int startMs)
	{
		const MCIERROR closeError = g_sendMciAudio("close mmd_song", NULL, 0, NULL);
		if (closeError && !HelperAudioMissingAlias(closeError))
		{
			// The old alias may still be playing; do not replace its identity or
			// pretend it was closed because a new open was requested.
			g_audioObservation.sourceOpen = true;
			HelperAudioRecordCommand(closeError);
			return;
		}
		const uint64_t previousEpoch = g_audioObservation.sourceEpoch;
		g_audioObservation = {};
		g_musicVolumeSupport = 0;
		g_audioUsesDigitalVideo = false;
		g_audioObservation.sourceEpoch = previousEpoch;
		g_audioObservation.state = GakuMotion::audio::PlaybackState::Starting;
		g_audioCommandError = 0;
		std::string cmdOpen = "open \"" + path + "\" type mpegvideo alias mmd_song";
		MCIERROR rc = g_sendMciAudio(cmdOpen.c_str(), NULL, 0, NULL);
		if (!rc) g_audioUsesDigitalVideo = true;
		else
		{
			// Preserve the original playback backend if DirectShow cannot open this WAV.
			cmdOpen = "open \"" + path + "\" type waveaudio alias mmd_song";
			rc = g_sendMciAudio(cmdOpen.c_str(), NULL, 0, NULL);
		}
		if (!rc)
		{
			g_audioObservation.sourceOpen = true;
			g_audioObservation.sourceEpoch = previousEpoch + 1;
			if (!g_audioObservation.sourceEpoch) g_audioObservation.sourceEpoch = 1;
			rc = g_sendMciAudio("set mmd_song time format milliseconds", NULL, 0, NULL);
			if (!rc) HelperAudioSetVolume(g_musicVolume);
		}
		char cmd[128]{};
		std::snprintf(cmd, sizeof(cmd), "play mmd_song from %d", (startMs > 0) ? startMs : 0);
		if (!rc) rc = g_sendMciAudio(cmd, NULL, 0, NULL);
		if (rc)
		{
			if (g_audioObservation.sourceOpen)
			{
				const MCIERROR cleanup = g_sendMciAudio("close mmd_song", NULL, 0, NULL);
				if (!cleanup || HelperAudioMissingAlias(cleanup))
				{ g_audioObservation.sourceOpen = false; g_musicVolumeSupport = 0; g_audioUsesDigitalVideo = false; }
			}
			HelperAudioRecordCommand(rc);
			return;
		}
		HelperAudioRecordCommand(0);
	}

	// 从头播放（保持改动前的既有语义）
	void HelperAudioPlay(const std::string& path)
	{
		g_lastAudioPath = path;
		HelperAudioOpenAndPlay(path, 0);
	}

	void HelperAudioPause()
	{
		HelperAudioRecordCommand(g_sendMciAudio("pause mmd_song", NULL, 0, NULL));
	}

	// Always use the requested absolute timeline position, including zero.
	void HelperAudioResumeAt(float seconds)
	{
		const int ms = HelperAudioMsFromSeconds(seconds);
		if (HelperAudioIsOpen())
		{
			char cmd[128]{};
			std::snprintf(cmd, sizeof(cmd), "play mmd_song from %d", ms);
			HelperAudioRecordCommand(g_sendMciAudio(cmd, NULL, 0, NULL));
		}
		else if (!g_lastAudioPath.empty())
		{
			HelperAudioOpenAndPlay(g_lastAudioPath, ms);
		}
		else HelperAudioRecordCommand(MCIERR_INVALID_DEVICE_ID);
	}

	// 进度跳转：仅在音频已打开时生效；未打开时由播放器下一次 Play() 负责对齐
	void HelperAudioSeekTo(float seconds, bool playing)
	{
		if (!HelperAudioIsOpen()) return;
		const int ms = HelperAudioMsFromSeconds(seconds);
		char cmd[128]{};
		if (playing)
			std::snprintf(cmd, sizeof(cmd), "play mmd_song from %d", ms);
		else
			std::snprintf(cmd, sizeof(cmd), "seek mmd_song to %d", ms);
		HelperAudioRecordCommand(g_sendMciAudio(cmd, NULL, 0, NULL));
	}
	void HelperAudioSeek(float seconds) { HelperAudioSeekTo(seconds, true); }
	void HelperAudioSeekPaused(float seconds) { HelperAudioSeekTo(seconds, false); }

	void HelperAudioStop()
	{
		MCIERROR stopError = g_sendMciAudio("stop mmd_song", NULL, 0, NULL);
		const MCIERROR closeError = g_sendMciAudio("close mmd_song", NULL, 0, NULL);
		if (HelperAudioMissingAlias(stopError)) stopError = 0;
		const bool closed = !closeError || HelperAudioMissingAlias(closeError);
		const uint64_t epoch = g_audioObservation.sourceEpoch;
		g_audioObservation = {};
		g_audioObservation.sourceEpoch = epoch;
		g_audioObservation.sourceOpen = !closed;
		if (closed) { g_musicVolumeSupport = 0; g_audioUsesDigitalVideo = false; }
		HelperAudioRecordCommand(stopError ? stopError : closed ? 0 : closeError);
		if (closed && !g_audioCommandError) g_audioObservation.confirmed = true;
	}

	float HelperAudioGetTime()
	{
		// A failed seek/resume cannot pull the motion back to an old alias clock.
		if (!HelperAudioObserve().EffectivePlaying()) return -1.0f;
		char buf[64]{};
		MCIERROR err = g_sendMciAudio("status mmd_song position", buf, sizeof(buf), NULL);
		if (err == 0 && buf[0] != '\0')
		{
			int ms = atoi(buf);
			return static_cast<float>(ms) / 1000.0f;
		}
		return -1.0f;
	}

	void* FindClass(const char* assemblyName, const char* namespaze, const char* className)
	{
		if (!il2cpp_domain_get || !il2cpp_domain_assembly_open || !il2cpp_assembly_get_image || !il2cpp_class_from_name)
		{
			return nullptr;
		}
		void* domain = il2cpp_domain_get();
		if (!domain) return nullptr;
		void* assembly = il2cpp_domain_assembly_open(domain, assemblyName);
		if (!assembly) return nullptr;
		void* image = il2cpp_assembly_get_image(assembly);
		if (!image) return nullptr;
		return il2cpp_class_from_name(image, namespaze, className);
	}

	void* FindMethodPointer(void* klass, const char* methodName, int argsCount)
	{
		if (!klass || !il2cpp_class_get_method_from_name) return nullptr;
		Il2CppMethodInfo* m = il2cpp_class_get_method_from_name(klass, methodName, argsCount);
		return m ? m->methodPointer : nullptr;
	}

#include "GkGroundShadowBootstrap.hpp"
#include "GkBodyShadowBootstrap.hpp"
#include "GkCameraBridge.hpp"
#ifdef GKMOTION_ENABLE_SHOE_MESH_PROBE
#include "GkShoeMeshProbe.hpp"
#endif

#include "GkStageRuntime.hpp"
#include "GkActorGroundShadow.hpp"
#include "GkNativeAudio.hpp"
#include "GkGroundShadowProbe.hpp"
#include "GkBodyShadowProbe.hpp"
#include "GkEyeGazeProbe.hpp"
	void RefreshProtectedContext(bool forceOwnerRead = false);
	bool ProtectionOnUnityThread()
	{
		return GakuMotion::host::mainThread.isMainThread &&
			GakuMotion::host::mainThread.isMainThread(GakuMotion::host::mainThread.context);
	}

#include "GkSceneOwnerRuntime.hpp"
#include "GkStageLoadOverride.hpp"
#include "GkPageObservation.hpp"

	GakuMotion::injection::OwnerSnapshot g_captureOriginOwner;
	void LogCaptureRestore(const char* phase, const char* reason, bool allowed)
	{
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
		if (!g_hostContext.logInfo) return;
		const auto& core = GakuMotion::Core::Instance();
		const auto& r = core.captureRestoreReport;
		using GakuMotion::capture::StateName;
		g_hostContext.logInfo(
			"[GakuMotion][CaptureSafety] capture_id=%llu type=%s phase=%s scene=%s scene_handle=%d session=%llu capture_session=%llu gm_active=%d lease_depth=%llu body=%s face=%s gaze=%s camera=%s stage=%s physics=%s native_audio=%s allowed=%d reason=%s",
			GakuMotion::capture::CurrentTransactionId(), GakuMotion::capture::CurrentTransactionType(), phase,
			g_verifiedOwnerSceneName.c_str(), g_sceneOwner.sceneHandle, core.injectionSession, core.captureSession,
			core.capturePerformanceWasActive, core.injectionPolicy.Read().captureDepth,
			StateName(r.body), StateName(r.face), StateName(r.gaze), StateName(r.camera), StateName(r.stage),
			StateName(r.physics), StateName(r.nativeAudio), allowed, reason);
#else
		(void)phase; (void)reason; (void)allowed;
#endif
	}

	void ObserveCapture(const GakuMotion::capture::Event& event)
	{
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
		if (!g_hostContext.logInfo) return;
		if (!ProtectionOnUnityThread())
		{
			g_hostContext.logInfo("[GakuMotion][CaptureSafety] capture_id=%llu type=%s phase=%s scene=unavailable session=0 gm_active=%d lease_depth=%u reason=%s",
				event.id, event.type, event.phase, event.gmActive, event.depth, event.reason);
			return;
		}
		const auto& core = GakuMotion::Core::Instance();
		g_hostContext.logInfo(
			"[GakuMotion][CaptureSafety] capture_id=%llu parent_id=%llu type=%s phase=%s scene=%s scene_handle=%d session=%llu capture_session=%llu gm_active=%d guard_active=%d lease_depth=%u state=%d reason=%s",
			event.id, event.parentId, event.type, event.phase, g_currentSceneName.c_str(), g_verifiedActiveSceneHandle,
			core.injectionSession, core.captureSession, event.gmActive, event.gmActive, event.depth, event.state, event.reason);
#else
		(void)event;
#endif
	}

	void ApplyCaptureFailureInjection(GakuMotion::capture::RestoreReport& report)
	{
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
		char value[128]{};
		const auto size = GetEnvironmentVariableA("GAKUMOTION_DEVREVIEW_FAIL_RESTORE", value, sizeof(value));
		if (size && size < sizeof(value)) GakuMotion::capture::ApplyFailures(report, GakuMotion::capture::ParseFailureMask(value));
#else
		(void)report;
#endif
	}

	bool PrepareProtectedInjection()
	{
		if (GakuMotion::Core::Instance().liveTakeoverActive && !GakuMotion::capture::LiveAutoPhotoIdle())
			return false;
		// Only a local provenance marker for unknown-scene capture cancellation.
		// It is not a claim that any particular photo is contaminated.
		GakuMotion::Core::Instance().customWasActivated = true;
		return true;
	}

	bool CustomCaptureActive()
	{
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		return core.player.state != GakuMotion::PlaybackState::Stopped ||
			core.camera.owned || core.stageManager.injectionEnabled ||
			(core.liveTakeoverFault && core.liveOwnerToken && core.liveOwnerToken == core.liveFaultOwner);
	}

	void ManualCaptureFault(const char* reason)
	{
		// Publish loss of the manual guard immediately. Existing Unity-thread
		// revocation performs native cleanup; this callback never starts capture.
		GakuMotion::Core::Instance().injectionPolicy.SetCaptureHooksReady(false);
		g_pendingProtectionFault.store(true, std::memory_order_release);
		if (g_hostContext.logError) g_hostContext.logError("[GakuMotion][Capture] manual guard disabled: %s", reason);
	}

	void RevokeProtectedInjection()
	{
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		g_restoringNative = true;
		const bool fallbackClean = g_fallbackPhysics.RestoreForCapture(core.player.controller);
		const bool bridgeClean = g_burstBridge.ClearForCapture();
		g_capturePhysicsUnconfirmed |= !fallbackClean || !bridgeClean;
		ClearSecondaryPhysics();
		core.RevokeInjection();
		NativeAudio::Release(GakuMotion::bgm::ReleaseReason::PolicyInactive);
		g_restoringNative = false;
		g_motionSceneAllowed = false;
	}

	bool ContinueProtectedCapture(const char*);
	bool BeginProtectedCapture(const char* type)
	{
		if (!ProtectionOnUnityThread()) return false;
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		RefreshProtectedContext(true);
		const bool rosterConfirmed = !core.ensemble.HasActiveFollowers() || RefreshSceneActors(core, true);
		core.injectionPolicy.BeginCapture();
		if (core.injectionPolicy.Read().captureDepth > 1) return ContinueProtectedCapture(type);
		g_captureOriginOwner = g_sceneOwner;
		if (!g_sceneOwner.Valid() && core.customWasActivated && CustomCaptureActive())
		{
			// An unreadable/ambiguous owner is not permission to touch cached old
			// game objects or erase their snapshots. Cancel before native work.
			core.captureRestoreReport = {};
			core.captureRestoreReport.body = GakuMotion::capture::RestoreState::Unconfirmed;
			LogCaptureRestore("restore", "owner-unconfirmed-snapshots-retained", false);
			return false;
		}
		if (!rosterConfirmed || core.ensembleRestoreUnconfirmed)
		{
			core.ensembleRestoreUnconfirmed = true;
			core.captureRestoreReport = {};
			core.captureRestoreReport.body = GakuMotion::capture::RestoreState::Unconfirmed;
			LogCaptureRestore("restore", "multi-actor-restore-unconfirmed", false);
			return false;
		}
		const bool orphansClean = StageLoader::EnsureNoOrphanedStages();
		g_motionSceneAllowed = false;
		g_restoringNative = true;
		const bool physicsModified = g_fallbackPhysics.HasCaptureWrites() || g_burstBridge.HasCaptureInput();
		const bool fallbackClean = g_fallbackPhysics.RestoreForCapture(core.player.controller);
		const bool bridgeClean = g_burstBridge.ClearForCapture();
		g_capturePhysicsUnconfirmed |= !fallbackClean || !bridgeClean;
		// Preserve failed ownership in each checked provider; do not call the
		// old blind Clear path and erase the evidence before the capture gate.
		if (fallbackClean && bridgeClean) ClearSecondaryPhysics();
		const bool bgmModified = g_bgmLease.GetSnapshot().ownsMutation || g_bgmLease.GetSnapshot().recoveryPending ||
			g_voiceLease.GetSnapshot().ownsMutation || g_voiceLease.GetSnapshot().recoveryPending;
		core.SuspendForCapture();
		auto& report = core.captureRestoreReport;
		using GakuMotion::capture::RestoreState;
		report.physics = g_capturePhysicsUnconfirmed ? RestoreState::Unconfirmed :
			physicsModified ? RestoreState::Confirmed : RestoreState::NotModified;
		if (!orphansClean) report.stage = RestoreState::Unconfirmed;
		if (!g_sceneOwner.Valid() && core.customWasActivated) report.body = RestoreState::Unconfirmed;
		if (!NativeAudio::ReleaseForCapture()) report.nativeAudio = RestoreState::Unconfirmed;
		else if (bgmModified && report.nativeAudio != RestoreState::Unconfirmed) report.nativeAudio = RestoreState::Confirmed;
		// Rebind only restores the graph. Evaluate it now so a capture that renders
		// synchronously cannot consume the previous custom skeleton/rest pose.
		if (report.body != RestoreState::NotModified || report.gaze != RestoreState::NotModified)
		{
			auto evaluateAnimator = [&](void* actor)
			{
				void* animator = HelperGetActorAnimator(actor);
				if (!animator || !g_captureAnimatorUpdateMethod) { report.body = RestoreState::Unconfirmed; return; }
				float zero = 0.0f;
				void* args[]{&zero}; void* exception = nullptr;
				g_faceRuntime.invoke(g_captureAnimatorUpdateMethod, animator, args, &exception);
				if (exception) report.body = RestoreState::Unconfirmed;
			};
			for (const auto& entry : core.player.controller.bindings) evaluateAnimator(entry.first);
			for (const auto& follower : core.ensemble.Followers())
			{
				if (!follower.player) continue;
				void* actor = core.sceneActorRegistry.Resolve(follower.actorToken, core.sceneActorRegistry.Epoch());
				if (actor && follower.player->controller.bindings.find(actor) != follower.player->controller.bindings.end())
					evaluateAnimator(actor);
			}
		}
		g_restoringNative = false;
		ApplyCaptureFailureInjection(report);
		LogCaptureRestore("restore", report.FailureReason(), report.AllConfirmed());
		return report.AllConfirmed();
	}

	bool ContinueProtectedCapture(const char*)
	{
		if (!ProtectionOnUnityThread()) return false;
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		RefreshProtectedContext(true);
		if (!core.captureSuspended || core.captureSession != core.injectionSession ||
			!g_captureOriginOwner.SameOwner(g_sceneOwner))
		{
			LogCaptureRestore("continuation_gate", "scene_or_session_changed", false);
			return false;
		}
		if (!core.stageManager.SetInjectionEnabled(false) || !StageLoader::EnsureNoOrphanedStages())
			core.captureRestoreReport.stage = GakuMotion::capture::RestoreState::Unconfirmed;
		ApplyCaptureFailureInjection(core.captureRestoreReport);
		const bool clean = core.captureRestoreReport.AllConfirmed();
		LogCaptureRestore("continuation_gate", core.captureRestoreReport.FailureReason(), clean);
		return clean;
	}

	void EndProtectedCapture(const char*)
	{
		if (!ProtectionOnUnityThread())
		{
			GakuMotion::Core::Instance().injectionPolicy.SetCaptureHooksReady(false);
			g_pendingProtectionFault.store(true, std::memory_order_release);
			return;
		}
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		RefreshProtectedContext(true);
		if (!core.injectionPolicy.EndCapture()) return;
		if (core.injectionPolicy.Read().captureDepth != 0) return;
		g_motionSceneAllowed = core.InjectionAllowed();
		const bool restored = core.ResumeAfterCapture();
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
		if (g_hostContext.logInfo) g_hostContext.logInfo(
			"[GakuMotion][CaptureSafety] capture_id=%llu type=%s phase=resume scene=%s scene_handle=%d session=%llu capture_session=%llu gm_active=%d restored=%d reason=%s",
			GakuMotion::capture::CurrentTransactionId(), GakuMotion::capture::CurrentTransactionType(),
			g_verifiedOwnerSceneName.c_str(), g_sceneOwner.sceneHandle, core.injectionSession, core.captureSession,
			core.capturePerformanceWasActive, restored, core.captureResumeReason);
#else
		(void)restored;
#endif
	}

	GakuMotion::live_takeover::RuntimeAccess LiveRuntimeAccess()
	{
		return {g_faceRuntime.objectClass, g_faceRuntime.findField, g_faceRuntime.fieldType,
			g_faceRuntime.fieldFlags, g_faceRuntime.typeCode, StageLoader::metadata.classFromType,
			g_faceRuntime.readField, g_faceRuntime.invoke, g_safeBones.unbox};
	}
	GakuMotion::live_takeover::Evidence ObserveLiveTakeover(void*)
	{
		RefreshProtectedContext();
		auto& core = GakuMotion::Core::Instance();
		const auto recent = std::chrono::steady_clock::now() - g_lastLiveActorTime < std::chrono::seconds(2);
		const bool actors = core.liveTakeoverActive || (core.liveOwnerToken &&
			core.liveOwnerToken == g_lastLiveActorOwner && core.liveOwnerScene == g_lastLiveActorScene && recent);
		const bool ready = core.liveViewingConfirmed && core.liveObservation.confirmed &&
			GakuMotion::capture::LiveAutoPhotoIdle() && core.injectionPolicy.Read().captureHooksReady &&
			!core.captureSuspended && !core.ensembleRestoreUnconfirmed && actors;
		const bool sameScene = !g_liveTakeoverOwner || (core.liveOwnerToken == g_liveTakeoverOwner &&
			core.liveOwnerScene == g_liveTakeoverScene);
		return {sameScene ? core.liveOwnerToken : 0, ready && sameScene,
			core.liveObservation.confirmed && sameScene, core.liveObservation.paused};
	}
	bool PauseLiveForTakeover(void*, uint64_t owner)
	{
		auto& core = GakuMotion::Core::Instance();
		return owner && owner == core.liveOwnerToken && core.liveViewingConfirmed &&
			GakuMotion::live_takeover::SetPaused(g_liveApi, LiveRuntimeAccess(), reinterpret_cast<void*>(owner), true);
	}
	bool RestoreLiveActors(void*, uint64_t owner)
	{
		auto& core = GakuMotion::Core::Instance();
		return owner && owner == core.liveOwnerToken && BeginProtectedCapture("Live takeover exit");
	}
	bool ResumeLiveAfterTakeover(void*, uint64_t owner)
	{
		auto& core = GakuMotion::Core::Instance();
		return owner && owner == core.liveOwnerToken && core.liveViewingConfirmed &&
			GakuMotion::live_takeover::SetPaused(g_liveApi, LiveRuntimeAccess(), reinterpret_cast<void*>(owner), false);
	}

	void ProtectedCaptureFault(const char* reason)
	{
		auto& core = GakuMotion::Core::Instance();
		if (!ProtectionOnUnityThread())
		{
			core.injectionPolicy.SetCaptureHooksReady(false);
			g_pendingProtectionFault.store(true, std::memory_order_release);
			if (g_hostContext.logError) g_hostContext.logError("[GakuMotion][Capture] off-thread failure: %s", reason);
			return;
		}
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		RefreshProtectedContext(true);
		core.injectionPolicy.SetCaptureHooksReady(false);
		if (g_sceneOwner.Valid()) RevokeProtectedInjection();
		else
		{
			// Keep all body/camera/stage/physics evidence until a confirmed owner
			// permits checked recovery. Do not treat failed discovery as destruction.
			++core.injectionSession;
			core.captureResumeReason = "owner_unconfirmed";
			core.captureRestoreReport.body = GakuMotion::capture::RestoreState::Unconfirmed;
			if (core.player.audioStarted && core.player.fnPauseAudio) core.player.fnPauseAudio();
			core.player.state = GakuMotion::PlaybackState::Stopped;
			core.player.scrubbing = false;
			if (core.fnNotifyMotionPlaying) core.fnNotifyMotionPlaying(false);
			g_motionSceneAllowed = false;
		}
		core.injectionStatus = "截图保护未能确认原生状态；GakuMotion 已禁用，请查看日志。";
		if (g_hostContext.logError) g_hostContext.logError("[GakuMotion][Capture] fail closed: %s", reason);
	}

	void ProtectedPageChanged(bool pageStable)
	{
		if (!ProtectionOnUnityThread())
		{
			ManualCaptureFault("page transition off Unity thread");
			return;
		}
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		RefreshProtectedContext(true);
		// The latch only marks WillChange -> DidChange. Current page identity
		// comes from the fresh exact TopScreen read, including child navigation.
		if (g_sceneOwner.role == GakuMotion::injection::OwnerRole::OutGame && !pageStable)
		{
			core.injectionPolicy.SetSceneAllowed(false);
			if (CustomCaptureActive()) RevokeProtectedInjection();
		}
	}

	void RefreshProtectedContext(bool forceOwnerRead)
	{
		if (!ProtectionOnUnityThread()) return;
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		const bool wasAllowed = core.InjectionAllowed();
		int activeHandle = 0; void* exception = nullptr;
		void* args[]{&activeHandle};
		g_activeSceneVerified = g_activeSceneMethod && g_getSceneNameNative && g_faceRuntime.invoke;
		if (g_activeSceneVerified)
		{
			g_faceRuntime.invoke(g_activeSceneMethod, nullptr, args, &exception);
			g_activeSceneVerified = exception == nullptr && activeHandle != 0;
		}
		const std::string name = g_activeSceneVerified
			? Il2CppStringToUtf8(g_getSceneNameNative(activeHandle, nullptr)) : "";
		g_verifiedActiveSceneHandle = g_activeSceneVerified ? activeHandle : 0;
		if (GakuMotion::stage_load::receipt.owner.Valid() &&
			SceneOwnerRuntime::ConfirmedUnloaded(GakuMotion::stage_load::receipt.owner.sceneHandle))
			GakuMotion::stage_load::receipt.Clear();
		const auto ownerObservation = SceneOwnerRuntime::Read(name, forceOwnerRead || g_pendingProtectionFault.load(std::memory_order_acquire));
		g_sceneOwner = ownerObservation.owner;
		g_verifiedOwnerSceneName = ownerObservation.sceneName;
		// Read only the fresh, exact current presenter; never scan unrelated Live
		// objects or retain managed pointers across frames. This does not admit
		// injection or change the game playback state.
		if (g_activeSceneVerified && g_sceneOwner.Valid() &&
			ownerObservation.rootClassName == "Campus.Live.LiveScenePresenter")
		{
			const GakuMotion::live_takeover::RuntimeAccess access{
				g_faceRuntime.objectClass, g_faceRuntime.findField, g_faceRuntime.fieldType,
				g_faceRuntime.fieldFlags, g_faceRuntime.typeCode, StageLoader::metadata.classFromType,
				g_faceRuntime.readField, g_faceRuntime.invoke, g_safeBones.unbox};
			core.liveObservation = GakuMotion::live_takeover::Observe(g_liveApi, access,
				reinterpret_cast<void*>(g_sceneOwner.presenterIdentity));
			core.liveOwnerToken = core.liveObservation.confirmed ? g_sceneOwner.presenterIdentity : 0;
			core.liveOwnerScene = core.liveObservation.confirmed ? g_sceneOwner.sceneHandle : 0;
			core.liveViewingConfirmed = core.liveObservation.confirmed && g_liveViewingGate.Allows(
				g_liveApi, access, reinterpret_cast<void*>(g_sceneOwner.presenterIdentity));
		}
		else { core.liveObservation = {}; core.liveOwnerToken = 0; core.liveOwnerScene = 0; core.liveViewingConfirmed = false; }
		// Read-only diagnostic for Live entry and pause tests. Log only class and
		// scene state; do not log account data, actor names or managed pointers.
		if (name == "Live" || name.rfind("env_3d_live_", 0) == 0 ||
			ownerObservation.rootClassName == "Campus.Live.LiveScenePresenter")
		{
			static std::string previous;
			const std::string state = name + "|" + ownerObservation.rootClassName + "|" +
				ownerObservation.reason + "|" + (core.liveObservation.confirmed ?
					(core.liveObservation.paused ? "paused" : "playing") : "unconfirmed");
			if (state != previous && g_hostContext.logInfo)
			{
				g_hostContext.logInfo("[GakuMotion][LiveProbe] scene=%s owner_class=%s owner_reason=%s playback=%s",
					name.c_str(), ownerObservation.rootClassName.c_str(), ownerObservation.reason,
					core.liveObservation.confirmed ? (core.liveObservation.paused ? "paused" : "playing") : "unconfirmed");
				previous = state;
			}
		}
		StageLoadOverride::ObserveActiveScene(g_verifiedActiveSceneHandle, name);
		const bool liveOwner = core.liveViewingConfirmed && core.liveOwnerToken &&
			g_sceneOwner.role == GakuMotion::injection::OwnerRole::Live;
		const bool recentLiveActor = g_lastLiveActorOwner == core.liveOwnerToken && core.liveOwnerToken &&
			g_lastLiveActorScene == core.liveOwnerScene &&
			(std::chrono::steady_clock::now() - g_lastLiveActorTime < std::chrono::seconds(2));
		core.liveTakeoverAvailable = liveOwner && GakuMotion::capture::LiveAutoPhotoIdle() &&
			core.injectionPolicy.Read().captureHooksReady && core.injectionPolicy.Read().captureDepth == 0 &&
			!core.ensembleRestoreUnconfirmed && !core.liveTakeoverFault && (core.liveTakeoverActive || recentLiveActor);
		auto admitted = g_activeSceneVerified ? ownerObservation.context : GakuMotion::injection::Context::Unknown;
		const auto liveAdmission = GakuMotion::injection::LiveTakeoverContext(g_sceneOwner,
			g_liveTakeoverOwner, g_liveTakeoverScene, core.liveTakeoverActive,
			core.liveViewingConfirmed && g_activeSceneVerified, core.liveObservation.paused,
			GakuMotion::capture::LiveReady());
		if (liveAdmission != GakuMotion::injection::Context::Unknown) admitted = liveAdmission;
		core.injectionPolicy.SetContext(admitted);
		g_motionSceneAllowed = core.InjectionAllowed();
		const bool oldSceneLost = g_currentSceneHandle != INT_MIN &&
			(g_boundSceneOwner.ReplacedBy(g_sceneOwner) ||
				(!g_sceneOwner.Valid() && SceneOwnerRuntime::ConfirmedUnloaded(g_boundSceneOwner.sceneHandle)));
		if (oldSceneLost)
		{
			// The old Scene may already have destroyed its actors. Never Rebind or
			// restore cached Unity instances here; orphan stages have a fresh scan.
			ClearSecondaryPhysics(true);
			core.AbandonScene();
			HelperClearActorAnimatorCache();
			NativeAudio::Release(GakuMotion::bgm::ReleaseReason::SceneLost);
			g_currentSceneHandle = INT_MIN;
			g_currentSceneName.clear();
			g_boundSceneOwner = {};
		}
		else if (g_sceneOwner.Valid() && ((wasAllowed && !g_motionSceneAllowed) || g_pendingProtectionFault.load(std::memory_order_acquire)))
			RevokeProtectedInjection();
		if (g_sceneOwner.Valid() || oldSceneLost) g_pendingProtectionFault.store(false, std::memory_order_release);
		// A retired asynchronous load can finish even with no actor callbacks.
		StageLoader::EnsureNoOrphanedStages();
		const auto state = core.injectionPolicy.Read();
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
		PageObservation::Poll(CustomCaptureActive());
#endif
		if (state.captureDepth != 0)
			core.injectionStatus = core.captureSuspended
				? "游戏截图事务尚未结束，自定义注入保持暂停；仅在同一有效会话结束后恢复。"
				: "仍有游戏截图事务未结束，GakuMotion 保持暂停；角色或场景变化后不会恢复旧状态。";
		else if (!g_motionSceneAllowed && !core.captureSuspended)
		{
			core.injectionStatus = !state.captureHooksReady
				? "手动拍照保护不可用，GakuMotion 已禁用；设置仍保留，请查看日志。"
				: "仅支持已验证的偶像摄影场景与更衣室；当前页面的自定义设置不生效。";
		}
		else if (core.captureSuspended)
			core.injectionStatus = "游戏正在截图，自定义注入已临时暂停；拍摄结束后恢复。";
		else
			core.injectionStatus.clear();
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
		// Stored scene identities are copied values only. Never dereference an
		// old actor merely to explain an admission failure after a scene change.
		struct AdmissionDiagnosticState
		{
			bool activeVerified;
			int activeHandle;
			std::string activeName, ownerName, rootClass, ownerReason;
			GakuMotion::injection::OwnerSnapshot owner;
			int actorHandle;
			bool sceneAllowed, captureHooksReady;
			uint64_t captureDepth;
			bool suspended;
		};
		const AdmissionDiagnosticState current{g_activeSceneVerified, g_verifiedActiveSceneHandle, name,
			g_verifiedOwnerSceneName, ownerObservation.rootClassName, ownerObservation.reason, g_sceneOwner,
			g_currentSceneHandle, state.sceneAllowed, state.captureHooksReady, state.captureDepth, core.captureSuspended};
		static std::string previous;
		const std::string signature = std::to_string(current.activeHandle) + ":" + current.activeName + ":" +
			std::to_string(current.owner.presenterIdentity) + ":" + std::to_string(current.owner.sceneHandle) + ":" +
			current.ownerReason + ":" + std::to_string(current.actorHandle) + ":" + std::to_string(current.sceneAllowed) + ":" +
			std::to_string(current.captureHooksReady) + ":" + std::to_string(current.captureDepth) + ":" +
			std::to_string(current.suspended) + ":" + std::to_string(current.owner.costumePage);
		if (signature != previous && g_hostContext.logInfo)
		{
			g_hostContext.logInfo(
				"[GakuMotion][Admission] active_verified=%d active_scene_handle=%d active_scene=%s owner_confirmed=%d presenter=%llu presenter_class=%s owner_scene_handle=%d owner_scene=%s actor_scene_handle=%d scene_allowed=%d capture_hooks_ready=%d capture_depth=%llu capture_suspended=%d costume_page=%d reason=%s",
				current.activeVerified, current.activeHandle, current.activeName.c_str(), current.owner.confirmed,
				static_cast<unsigned long long>(current.owner.presenterIdentity), current.rootClass.c_str(),
				current.owner.sceneHandle, current.ownerName.c_str(), current.actorHandle, current.sceneAllowed,
				current.captureHooksReady, current.captureDepth, current.suspended, current.owner.costumePage, current.ownerReason.c_str());
			previous = signature;
		}
#endif
	}

	bool HelperLoadBackground(const char* stageKey)
	{
		return StageLoader::LoadBackground(stageKey);
	}

	bool HelperUnloadBackground()
	{
		return StageLoader::UnloadBackground();
	}

	void HelperSetStageTransform(float x, float y, float z, float rotY)
	{
		StageLoader::SetStageTransform(x, y, z, rotY);
	}

	void HelperSetLoadedStageRoot(void* go)
	{
		StageLoader::SetLoadedStageRoot(go);
	}

	// -----------------------------------------------------------------------------
	// Phase 0：宿主 context 能力探测（HostCaps）
	// -----------------------------------------------------------------------------
	// Init copies only the prefix guaranteed by the host API version. API v1
	// has no playback-state callbacks; those fields remain zero. VirtualQuery
	// is an additional callback sanity check, not a context-size negotiation.
	struct HostCaps
	{
		uint32_t apiVersion{0};
		bool hasLogInfo{false};
		bool hasLogWarn{false};
		bool hasIsVRActive{false};
		bool hasVRifyLiveGaze{false};
		bool setMotionPlayingValid{false};
		bool isMotionPlayingValid{false};
	};

	HostCaps g_hostCaps{};

	bool LooksCallable(const void* ptr)
	{
		if (!ptr) return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
		if (mbi.State != MEM_COMMIT) return false;
		if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
		const DWORD prot = mbi.Protect & 0xFF;
		return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
			prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
	}

	void ValidateHostContext()
	{
		g_hostCaps = HostCaps{};
		g_hostCaps.apiVersion = g_hostContext.apiVersion;
		g_hostCaps.hasLogInfo = LooksCallable(reinterpret_cast<const void*>(g_hostContext.logInfo));
		g_hostCaps.hasLogWarn = LooksCallable(reinterpret_cast<const void*>(g_hostContext.logWarn));
		g_hostCaps.hasIsVRActive = LooksCallable(reinterpret_cast<const void*>(g_hostContext.isVRActive));
		g_hostCaps.hasVRifyLiveGaze = LooksCallable(reinterpret_cast<const void*>(g_hostContext.isVRifyLiveGazeActive));
		// These adapter hooks always exist; VR queries have no service equivalent.
		g_hostCaps.setMotionPlayingValid = LooksCallable(reinterpret_cast<const void*>(g_hostContext.setMotionPlaying));
		g_hostCaps.isMotionPlayingValid = LooksCallable(reinterpret_cast<const void*>(g_hostContext.isMotionPlaying));

		if (g_hostCaps.hasLogInfo)
		{
			g_hostContext.logInfo(
				"[GakuMotion] Host API v%u caps: log=%d warn=%d vr=%d liveGaze=%d setMotionPlaying=%d isMotionPlaying=%d",
				g_hostCaps.apiVersion, g_hostCaps.hasLogInfo, g_hostCaps.hasLogWarn,
				g_hostCaps.hasIsVRActive, g_hostCaps.hasVRifyLiveGaze,
				g_hostCaps.setMotionPlayingValid, g_hostCaps.isMotionPlayingValid);
		}
		if (g_hostCaps.hasLogWarn && !g_hostCaps.setMotionPlayingValid)
		{
			g_hostContext.logWarn(
				"[GakuMotion] Host provided no valid setMotionPlaying callback (older context?); playback-state reporting disabled");
		}
	}

	// 统一的播放状态上报（能力缺失时静默跳过，绝不调用野指针）
	void NotifyHostMotionPlaying(bool playing)
	{
		playing = playing && GakuMotion::Core::Instance().player.state == GakuMotion::PlaybackState::Playing;
		if (g_hostCaps.setMotionPlayingValid && g_hostContext.setMotionPlaying)
		{
			g_hostContext.setMotionPlaying(playing);
		}
	}
} // namespace

// Native activation body: config, audio, import bridges and Core initialization.
// Runs before Unity exists, so it may not touch managed code.
bool InitializeMotion()
{
	std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
	if (g_pluginInitialized) return true;
	g_burstBridge.Configure(g_hostContext.pluginsRoot, [](void*, int32_t level, const char* message)
	{
		auto log = level > 0 ? g_hostContext.logWarn : g_hostContext.logInfo;
		if (log) log("[GakuMotion][BurstBridge] %s", message);
	}, nullptr);
	g_gameAssembly = reinterpret_cast<HMODULE>(g_hostContext.gameAssemblyHandle);
	if (!g_gameAssembly)
	{
		g_gameAssembly = GetModuleHandleW(L"GameAssembly.dll");
	}

	std::string localRoot = WidePathToNarrow(g_hostContext.localRoot ? g_hostContext.localRoot : L"gakumas-local");
	if (localRoot.empty()) return false;
	GakuMotion::DataLayout offered, selected;
	const GakuMotion::DataLayout* available = nullptr;
	if (GakuMotion::host::paths.getRoot)
	{
		offered.localRoot = localRoot;
		const std::pair<GakuPathKind, std::string*> roots[] = {
			{GAKU_PATH_CONFIGS, &offered.configs}, {GAKU_PATH_USER_DATA, &offered.userData},
			{GAKU_PATH_PLUGIN_DATA, &offered.pluginData}, {GAKU_PATH_LOGS, &offered.logs},
			{GAKU_PATH_TEMP, &offered.temp}};
		for (const auto& entry : roots)
		{
			std::filesystem::path native;
			if (GakuPaths::GetRoot(GakuMotion::host::paths, entry.first, native) != GAKU_OK)
			{
				if (g_hostContext.logError) g_hostContext.logError("[GakuMotion][Paths] Cannot resolve managed category %u; no old-root fallback", entry.first);
				return false;
			}
			*entry.second = GakuMotion::MotionPathToUtf8(native);
		}
		available = &offered;
	}
	std::string layoutError;
	if (!GakuMotion::SelectDataLayout(localRoot, available, selected, layoutError))
	{
		if (g_hostContext.logError) g_hostContext.logError("[GakuMotion][Paths] %s", layoutError.c_str());
		return false;
	}
	if (selected.managed)
	{
		for (const auto kind : {GAKU_PATH_CONFIGS, GAKU_PATH_PLUGIN_DATA, GAKU_PATH_LOGS})
			if (GakuMotion::host::paths.ensureRoot(GakuMotion::host::paths.context, kind) != GAKU_OK)
				return false;
		std::error_code ec;
		std::filesystem::create_directories(GakuMotion::MotionPathFromUtf8(selected.logs + "/diagnostics"), ec);
		if (ec) return false;
	}

	// Phase 0：先探测宿主能力，任何回调都可能缺失，必须先验后调
	ValidateHostContext();

	// 先把日志与回调接上，再初始化 Core；否则 Core::Initialize 的日志会丢失
	auto& core = GakuMotion::Core::Instance();
	core.enforceInjectionPolicy = true;
	core.player.controller.captureCheckEnforced = true;
	core.camera.captureCheckEnforced = true;
	core.fnBeforeInjection = PrepareProtectedInjection;
	core.fnLogInfo = g_hostCaps.hasLogInfo ? g_hostContext.logInfo : nullptr;
	core.fnLogWarn = g_hostCaps.hasLogWarn ? g_hostContext.logWarn : nullptr;
	core.fnNotifyMotionPlaying = NotifyHostMotionPlaying;
	core.fnReadFormationOrigin = GakuMotion::csharp_import::ReadFormationOrigin;
	core.fnStartImport = StartMotionImport;
	core.fnPollImport = PollMotionImport;
	core.fnPollImportProgress = PollMotionImportWithProgress;
	core.fnStartCameraImport = StartCameraImport;
	core.fnPollCameraImport = PollCameraImport;
	core.fnPickFile = GakuMotion::host::active ? nullptr : PickMotionFile;
	core.fnBeginPickFile = GakuMotion::host::active ? BeginMotionFile : nullptr;
	core.fnPollPickFile = GakuMotion::host::active ? PollMotionFile : nullptr;

	// Bind audio helpers to player
	auto& player = core.player;
	player.fnPlayAudio = HelperAudioPlay;
	player.fnPauseAudio = HelperAudioPause;
	player.fnStopAudio = HelperAudioStop;
	player.fnGetAudioTime = HelperAudioGetTime;
	player.audioPositionIsAbsolute = true;
	player.fnResumeAudio = HelperAudioResumeAt;
	player.fnSeekAudio = HelperAudioSeek;
	player.fnSeekPausedAudio = HelperAudioSeekPaused;
	player.fnObserveAudio = HelperAudioObserve;
	player.fnSetAudioVolume = HelperAudioSetVolume;
	player.fnAudioVolumeSupport = HelperAudioVolumeSupport;
	player.fnCanPlay = HelperCanPlay;

	core.Initialize(localRoot, &selected);
	HelperAudioSetVolume(player.musicVolume);
	if (g_hostContext.logInfo) g_hostContext.logInfo("[GakuMotion][Paths] layout=%s configs=%s motions=%s",
		selected.managed ? "gakuloader-v1" : "legacy-unmigrated", core.configRoot.c_str(), core.motionsRoot.c_str());
	if (g_hostContext.pluginsRoot)
	{
		// Only records the directory; the companion DLL is loaded on first use so
		// early activation stays free of managed-runtime initialization.
		GakuMotion::csharp_import::SetPluginsRoot(WidePathToNarrow(g_hostContext.pluginsRoot));
	}

	if (g_hostContext.logInfo)
	{
		g_hostContext.logInfo("[GakuMotion] Plugin initialized successfully. LocalRoot: %s", localRoot.c_str());
		g_hostContext.logInfo("[GakuMotion] %s", core.StatusSummary().c_str());
	}
	g_pluginInitialized = true;
	return true;
}

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

	// 7. Bind function pointers into GakuMotion Core
	auto& ctrl = GakuMotion::Core::Instance().player.controller;
	g_getSceneNative = reinterpret_cast<decltype(g_getSceneNative)>(FindMethodPointer(
		FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject"), "get_scene_Injected", 1));
	g_getSceneValueNative = reinterpret_cast<decltype(g_getSceneValueNative)>(FindMethodPointer(
		FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject"), "get_scene", 0));
	g_getSceneNameNative = reinterpret_cast<decltype(g_getSceneNameNative)>(FindMethodPointer(
		FindClass("UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement", "Scene"), "GetNameInternal", 1));
	ctrl.fnCanDriveActor = HelperCanDriveActor;
	g_faceUnityFrame = reinterpret_cast<int (*)()>(FindMethodPointer(
		FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Time"), "get_frameCount", 0));
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
			GM_DEBUG("[GakuMotion][FacialFit] component=%s decision=%s actual_face=%s (prefab identity; asset bytes not runtime-verified)",
				component, decision, faceName);
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
		auto& controller = GakuMotion::Core::Instance().player.controller;
		const bool fallbackClean = g_fallbackPhysics.RestoreForCapture(controller);
		const bool bridgeClean = g_burstBridge.ClearForCapture();
		if (g_capturePhysicsUnconfirmed || !fallbackClean || !bridgeClean)
		{
			g_capturePhysicsUnconfirmed = true;
			return false; // Preserve failed ownership snapshots for the capture gate.
		}
		ClearSecondaryPhysics();
		return true;
	};
	ctrl.fnLogInfo = g_hostContext.logInfo;
	ctrl.fnLogWarn = g_hostContext.logWarn;

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
	// Metadata inspection only. Live remains denied by the existing policy;
	// resolving these signatures does not discover or modify a game object.
	g_liveApi = GakuMotion::live_takeover::Api::Resolve(FindClass, StageLoader::metadata);
	auto readStaticField = reinterpret_cast<void (*)(void*, void*)>(
		GetProcAddress(g_gameAssembly, "il2cpp_field_static_get_value"));
	g_liveViewingGate = GakuMotion::live_takeover::ViewingGate::Resolve(FindClass, StageLoader::metadata,
		g_faceRuntime.findField, g_faceRuntime.fieldType, g_faceRuntime.fieldFlags,
		readStaticField, StageLoader::valueSize);
	g_liveDriver.SetAdapter({nullptr, ObserveLiveTakeover, PauseLiveForTakeover,
		RestoreLiveActors, ResumeLiveAfterTakeover});
	if (g_hostContext.logInfo) g_hostContext.logInfo(
		"[GakuMotion][Live] exact playback metadata %s; viewing gate %s; takeover remains disabled",
		g_liveApi.Ready() ? "resolved" : "unavailable", g_liveViewingGate.ready ? "resolved" : "unavailable");
	g_captureAnimatorUpdateMethod = StageLoader::metadata.Find(g_animatorKlass, "Update", false, {0x01}, {{0x0c}});
	void* sceneClass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement", "Scene");
	g_activeSceneMethod = StageLoader::metadata.Find(
		FindClass("UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement", "SceneManager"),
		"GetActiveScene_Injected", true, {0x01}, {{0x11, sceneClass, true}});
	uint32_t sceneAlignment = 0;
	if (!StageLoader::valueSize || StageLoader::valueSize(sceneClass, &sceneAlignment) != sizeof(int))
		g_activeSceneMethod = nullptr;
	const GakuMotion::capture::Callbacks protection{
		CustomCaptureActive, BeginProtectedCapture, EndProtectedCapture, ManualCaptureFault, ProtectedPageChanged,
		ObserveCapture, ContinueProtectedCapture,
		[]() { return g_liveTakeoverEnabled.load(std::memory_order_acquire); }, ProtectionOnUnityThread};
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
	g_runtimeReady.store(true, std::memory_order_release);
}

// =============================================================================
// 热键采样辅助
// =============================================================================
// 实机故障：暂停一段时间后无人操作，日志却出现
//   [Config] switch prev -> like JENNIE
//   [Config] switch next -> Fighting My Way
// 即`.` / `,` 在「无人按键」时触发。根因是旧写法`GetAsyncKeyState(vk) & 1`：
//低位 bit 的语义是「自上次调用以来该键是否被按下过」，它有两个致命性质：
//   (a) 粘滞——按键状态是在你读它的时候才被清掉的。掉帧/切场景/窗口失焦期间
//       本函数可能几十秒不被调用，用户在别的程序里敲的键全部堆积在bit 里，
//       帧循环恢复后的第一次读取就会一次性「补发」历史按键；
//   (b) 不区分前台窗口——GetAsyncKeyState 报的是物理键态，与「发给谁」无关。
// 此外长按的系统自动重复会被当成一串新按下，导致按住不放连环切歌。
//
// 修法（见 GkHotkey.hpp）：只用 0x8000 高位（真实物理按下态）自己做上升沿，
// 焦点不在本进程时一律不触发，且焦点切换瞬间清空沿基线，使失焦期间的按键
// 永不补发。下面两个函数把 Win32 状态翻译成 HotkeyGate 的输入。
static bool IsGameForeground()
{
	HWND fg = GetForegroundWindow();
	if (!fg) return false;
	DWORD fgPid = 0;
	GetWindowThreadProcessId(fg, &fgPid);
	return fgPid == GetCurrentProcessId();
}

static GakuMotion::HotkeyGate g_hotkeys;

// Materialises the deferred managed System.Type handles. Runtime-ready may run
// on a loader worker thread, where creating managed objects is not allowed, so
// every il2cpp_type_get_object call waits for the Unity main thread.
static void ResolveDeferredManagedTypes()
{
	if (!il2cpp_class_get_type || !il2cpp_type_get_object) return;
	if (g_pendingAnimatorTypeObj.load(std::memory_order_acquire) && g_animatorKlass)
	{
		if (void* typePtr = il2cpp_class_get_type(g_animatorKlass))
		{
			g_animatorTypeObj = il2cpp_type_get_object(typePtr);
			g_pendingAnimatorTypeObj.store(false, std::memory_order_release);
		}
	}
	if (g_pendingTransformTypeObj.load(std::memory_order_acquire) && g_transformKlass)
	{
		if (void* typePtr = il2cpp_class_get_type(g_transformKlass))
		{
			g_transformTypeObj = il2cpp_type_get_object(typePtr);
			g_pendingTransformTypeObj.store(false, std::memory_order_release);
		}
	}
	if (g_pendingFaceModelTypeObj.load(std::memory_order_acquire) && g_faceModelClass)
	{
		if (void* typePtr = il2cpp_class_get_type(g_faceModelClass))
		{
			g_faceModelTypeObj = il2cpp_type_get_object(typePtr);
			g_pendingFaceModelTypeObj.store(false, std::memory_order_release);
		}
	}
}

static bool EnsureRuntimeReadyForActor(void* activeActor, void (*initialize)(void*))
{
	if (g_runtimeReady.load(std::memory_order_acquire)) return true;
	if (!activeActor || !initialize) return false;
	// The loader may not have delivered runtime-ready yet; the first actor
	// callback is a safe Unity-thread retry and is a no-op once it has run.
	initialize(nullptr);
	return g_runtimeReady.load(std::memory_order_acquire);
}

// Reuse the exact typed Unity finder already validated for the scene bridge.
// Search only an admitted owner scene, at most four times per second unless a
// scene boundary or a target-selection command requires a fresh admission check.
// No callbacks, managed pointers or speculative metadata are exposed to ImGui.
static bool RefreshSceneActors(GakuMotion::Core& core, bool force)
{
	static auto lastScan = std::chrono::steady_clock::time_point{};
	const auto now = std::chrono::steady_clock::now();
	if (!core.InjectionAllowed() || !g_sceneOwner.Valid() || g_currentSceneHandle != g_sceneOwner.sceneHandle)
		return false;
	if (!force && !core.HasPendingTargetSelection() && now - lastScan < std::chrono::milliseconds(250)) return true;
	lastScan = now;
	auto unavailable = [&]
	{
		if (core.ensemble.HasActiveFollowers())
		{
			// A failed scan is not proof of destruction. Retain ownership without
			// touching actor pointers; block further injection and native capture.
			core.ensembleRestoreUnconfirmed = true;
			core.ensembleNotice = "无法确认多角色状态，已暂停注入；请重新进入场景";
			core.player.Pause();
			if (core.fnNotifyMotionPlaying) core.fnNotifyMotionPlaying(false);
			g_motionSceneAllowed = false;
			return false;
		}
		// A failed fresh check must not admit a command through yesterday's token.
		if (core.PublishSceneActors({}))
		{
			ClearSecondaryPhysics(true);
			HelperClearActorAnimatorCache();
			NativeAudio::Release(GakuMotion::bgm::ReleaseReason::SceneLost);
		}
		return false;
	};
	if (!StageLoader::findAll || !StageLoader::componentGo || !StageLoader::goScene || !StageLoader::instanceId ||
		!StageLoader::arrayLength || !StageLoader::newHandle || !StageLoader::freeHandle ||
		(!StageLoader::actorTypes[0] && !StageLoader::actorTypes[1])) return unavailable();
	std::vector<GakuMotion::SceneActorObservation> observations;
	for (void* actorType : StageLoader::actorTypes)
	{
		if (!actorType) continue;
		bool includeInactive = false;
		void* args[]{actorType, &includeInactive};
		void* array = StageLoader::Invoke(StageLoader::findAll, nullptr, args);
		if (!array) return unavailable();
		StageLoader::Handle pin = StageLoader::newHandle(array, false);
		if (!pin) return unavailable();
		struct Release { StageLoader::Handle value; ~Release() { StageLoader::freeHandle(value); } } release{pin};
		const uintptr_t count = StageLoader::arrayLength(array);
		if (count > 4096) return unavailable();
		void** items = reinterpret_cast<void**>(static_cast<char*>(array) + 32);
		for (uintptr_t i = 0; i < count; ++i)
		{
			void* actor = items[i];
			if (!actor || std::any_of(observations.begin(), observations.end(), [&](const auto& item) { return item.actor == actor; })) continue;
			void* go = StageLoader::Invoke(StageLoader::componentGo, actor);
			if (!go || StageLoader::SceneOfFreshGo(go) != g_sceneOwner.sceneHandle) continue;
			const int id = StageLoader::ReadInt(StageLoader::instanceId, actor);
			if (!id) continue;
			// Prefer the loaded face identity, including a replacement's actual face.
			void* face = HelperGetFaceModel(actor);
			std::string character = GakuMotion::DetectIdolCharacterCode(face ? HelperGetFaceModelName(face) : "");
			if (character.empty()) character = GakuMotion::DetectIdolCharacterCode(HelperGetActorName(actor));
			observations.push_back({actor, id, std::move(character)});
		}
	}
	const bool hadActors = !core.sceneActorRegistry.Entries().empty();
	const auto oldEpoch = core.sceneActorRegistry.Epoch();
	if (core.PublishSceneActors(observations))
	{
		ClearSecondaryPhysics(true);
		HelperClearActorAnimatorCache();
		NativeAudio::Release(GakuMotion::bgm::ReleaseReason::SceneLost);
		core.PickBoundActorAsTarget(); // Old physics has been abandoned; bind only a fresh actor.
	}
	if (hadActors && oldEpoch != core.sceneActorRegistry.Epoch())
	{
		void* current = observations.empty() ? nullptr : observations.front().actor;
		if (current) StageLoader::BindContext(g_sceneOwner.sceneHandle, current);
		if (!core.stageManager.OnActorSetChanged(current))
			ProtectedCaptureFault("stage restoration failed during actor replacement");
	}
	return true;
}

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
	if (core.liveObservation.confirmed && activeActor)
	{
		if (core.liveViewingConfirmed && g_sceneOwner.Valid() && sceneHandle == g_sceneOwner.sceneHandle)
		{
			g_lastLiveActorOwner = core.liveOwnerToken;
			g_lastLiveActorScene = sceneHandle;
			g_lastLiveActorTime = std::chrono::steady_clock::now();
		}
		static auto nextReport = std::chrono::steady_clock::time_point{};
		static unsigned callbacks = 0, ownerMatches = 0;
		++callbacks;
		if (g_sceneOwner.Valid() && sceneHandle == g_sceneOwner.sceneHandle) ++ownerMatches;
		const auto now = std::chrono::steady_clock::now();
		if (now >= nextReport)
		{
			if (g_hostContext.logInfo) g_hostContext.logInfo(
				"[GakuMotion][LiveProbe] actor_callbacks=%u owner_scene_matches=%u playback=%s",
				callbacks, ownerMatches, core.liveObservation.paused ? "paused" : "playing");
			callbacks = ownerMatches = 0;
			nextReport = now + std::chrono::seconds(1);
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

	// All actors share one physical playback clock. Target identity and roster
	// lifetimes are managed by the fresh typed snapshot below, not callback order.
	static auto s_lastPhysicalFrameTime = std::chrono::steady_clock::now();
	static int s_lastUnityFrame = -1, s_fallbackFrame = 0;
	// The camera target may be a later actor in this frame. Cache its unscaled
	// wall-clock delta independently of the existing playback/physics delta.
	static float s_cameraFrameDelta = 0.0333f;
	static uint64_t s_cameraPhysicalFrame = 0;
	auto now = std::chrono::steady_clock::now();
	const float elapsedMs = std::chrono::duration<float, std::milli>(now - s_lastPhysicalFrameTime).count();
	const int unityFrame = g_faceUnityFrame ? g_faceUnityFrame() : -1;
	const bool isNewPhysicalFrame = unityFrame >= 0 ? unityFrame != s_lastUnityFrame : elapsedMs > 2.0f;
	float realDeltaTime = deltaTime;
	if (isNewPhysicalFrame)
	{
		s_lastUnityFrame = unityFrame;
		++s_fallbackFrame;
		s_lastPhysicalFrameTime = now;
		realDeltaTime = elapsedMs > 100.0f ? 0.0333f : elapsedMs / 1000.0f;
		s_cameraFrameDelta = s_cameraPhysicalFrame == 0 ? 0.0333f :
			(std::max)(0.0f, elapsedMs / 1000.0f);
		// Zero is reserved by CameraController for callers without a frame ID.
		if (++s_cameraPhysicalFrame == 0) ++s_cameraPhysicalFrame;
	}
	// Without Unity's frame counter, the first callbacks can precede the 2ms
	// fallback boundary. They still share a nonzero camera deduplication ID.
	if (s_cameraPhysicalFrame == 0) s_cameraPhysicalFrame = 1;
	// Fresh typed snapshots remove disabled/destroyed actors even when no actor
	// callback remains. Never retain an accumulating list of seen scene actors.
	RefreshSceneActors(core, sceneChanged);
	StageLoader::BindContext(sceneHandle, activeActor);
	if (isNewPhysicalFrame) core.ProcessPendingGuiCommands();
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
	if (isNewPhysicalFrame)
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
			if (pressed && !editingPanel && core.ensemble.HasActiveFollowers() &&
			    (vk == VK_F10 || vk == VK_OEM_COMMA || vk == VK_OEM_PERIOD) &&
			    (!RefreshSceneActors(core, true) || core.ensembleRestoreUnconfirmed)) return false;
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
		const float sinceStatusEmit = std::chrono::duration<float>(now - s_lastStatusEmit).count();
		if (sinceStatusEmit >= 0.25f) // 去抖：最多 4 次/秒，顺带避免每帧构造摘要
		{
			const std::string summary = core.StatusSummary();
			const std::string fingerprint = StatusFingerprint(summary);
			if (fingerprint != s_lastStatusFingerprint || sinceStatusEmit >= 30.0f)
			{
				s_lastStatusFingerprint = fingerprint;
				s_lastStatusEmit = now;
				// ASCII only: the host decodes log lines as ANSI, so a Chinese
				// literal here arrives as mojibake (was "本帧覆盖生效"). It also
				// adds nothing -- the count is already in StatusSummary()'s
				// "overrides=ON(n)" field.
				GM_INFO("[GakuMotion][Status] %s", summary.c_str());
			}
		}
	}

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
	EyeGazeProbe::Before(activeActor);
	if (player.controller.targetActor)
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
	if (core.IsTargetActor(activeActor))
	{
		auto binding = player.controller.bindings.find(activeActor);
		const int frame = unityFrame >= 0 ? unityFrame : s_fallbackFrame;
		const bool canDrive = g_motionSceneAllowed && !core.ensembleRestoreUnconfirmed && binding != player.controller.bindings.end() &&
			binding->second.bindPoseCaptured && player.controller.CanDriveActor(activeActor);
		if (!canDrive || player.state == GakuMotion::PlaybackState::Stopped)
		{
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
			if (requested != g_physicsBackend || core.config.beforeQuartzExperiment != g_physicsBeforeQuartz ||
				core.config.sleeveMuscleDiagnostics != g_physicsSleeveDiagnostics ||
				activeActor != g_physicsActor || core.currentMotionIndex !=
				g_physicsMotion ||
				player.SeekCount() != g_physicsSeeks || player.scrubbing || player.currentTime + .001f < g_physicsTime)
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
			g_physicsSeeks = player.SeekCount();
			g_physicsTime = player.currentTime;
			g_physicsFrame = frame;
			const auto bodyEnd = std::chrono::steady_clock::now();
			auto& d = g_physicsDiagnostic;
			++d.body_calls;
			d.body_total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(bodyEnd - bodyBegin).count();
			const auto dispatchBegin = bodyEnd;
			const float physicalDt = g_physicsLastTick.time_since_epoch().count() == 0
				                         ? 0.0f
				                         : std::chrono::duration<float>(dispatchBegin - g_physicsLastTick).count();
			g_physicsLastTick = dispatchBegin;
			bool native = requested == 2 && g_burstBridge.Publish(binding->second, player.controller,
			                                                      player.currentTime, frame,
			                                                      core.config.beforeQuartzExperiment,
			                                                      core.config.sleeveMuscleDiagnostics);
			if (native && g_physicsActual == 1)
			{
				g_fallbackPhysics.Restore(player.controller);
				g_fallbackPhysics.Clear();
			}
			g_physicsActual = requested == 0 ? 0 : native ? 2 : 1;
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
			const std::string reason = requested == 2 ? g_burstBridge.reason : requested == 1 ? "fallback" : "off";
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
	if (core.burstTraceRequestedSeq > 0 && core.IsTargetActor(activeActor))
	{
		const auto seq = core.burstTraceRequestedSeq;
		core.burstTraceRequestedSeq = 0;
		const std::filesystem::path directory(core.diagnosticsRoot);
		const int result = g_burstBridge.DumpTraces((directory / "before_quartz_trace.json").wstring().c_str());
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

extern "C" GAKU_PLUGIN_EXPORT int GakuMotion_GetPhysicsDiagnostics(
	void* actor, GakuPhysicsDiagnosticSnapshot* out, uint32_t size)
{
	if (!actor || !out || size != sizeof(*out)) return -1;
	if (g_physicsDiagnostic.actor != reinterpret_cast<uintptr_t>(actor)) return 0;
	*out = g_physicsDiagnostic;
	return 1;
}

// Optional read-only observer entry point. Recorder calls on Unity's main
// thread; the lease enables readback on subsequent writes for two seconds.
extern "C" GAKU_PLUGIN_EXPORT int GakuMotion_GetFaceDiagnostics(
	void* actor, GakuFaceDiagnosticSnapshot* out, uint32_t size)
{
	if (!actor || !out || size != sizeof(*out)) return -1;
	auto& player = GakuMotion::Core::Instance().player;
	auto it = player.controller.bindings.find(actor);
	if (it == player.controller.bindings.end() || player.state == GakuMotion::PlaybackState::Stopped) return 0;
	auto& face = it->second.face;
	face.Observe();
	std::memset(out, 0, sizeof(*out));
	out->version = GAKU_FACE_DIAGNOSTICS_VERSION;
	out->size = sizeof(*out);
	out->status = face.diagnosticStatus;
	out->sequence = face.diagnosticSequence;
	out->qpc_ticks = face.diagnosticTicks;
	out->actor = reinterpret_cast<uintptr_t>(actor);
	out->model = reinterpret_cast<uintptr_t>(face.diagnosticModel);
	out->clip_time = face.diagnosticTime;
	out->playback_state = static_cast<int32_t>(player.state);
	out->unity_frame = face.diagnosticUnityFrame;
	out->channel_count = static_cast<uint32_t>(face.diagnosticChannels.size());
	for (size_t i = 0; i < face.diagnosticChannels.size(); ++i)
		out->channels[i] = face.diagnosticChannels[i];
	return 1;
}

constexpr const char* kShowGuiActionId = "show-gui";
constexpr const char* kReloadOverridesActionId = "reload-overrides";
constexpr const char* kToggleOverridesActionId = "toggle-overrides";

void ReloadOverridesHotkey(void*)
{
	auto& core = GakuMotion::Core::Instance();
	std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
	if (!core.InjectionAllowed()) return;
	core.LoadOverrides(true);
}
void ToggleOverridesHotkey(void*)
{
	auto& core = GakuMotion::Core::Instance();
	std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
	if (!core.InjectionAllowed()) return;
	core.ToggleOverrides();
}

// Runs on the Unity main thread: the loader's hotkey registry dispatches there,
// so touching Core/gui state is safe.
void ShowGuiHotkey(void*)
{
	GakuMotion::Core::Instance().gui.showWindow = true;
	if (GakuMotion::host::desktopWindow.requestShow)
		GakuMotion::host::desktopWindow.requestShow(GakuMotion::host::desktopWindow.context);
	GakuMotion::host::Info("[GakuMotion] Hotkey Ctrl+M: showWindow=TRUE");
}

void MotionShutdown(void*)
{
	std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
	if (!g_pluginInitialized) return;
	g_runtimeReady.store(false, std::memory_order_release);
	GakuMotion::host::UnregisterHotkey(kShowGuiActionId);
	GakuMotion::host::UnregisterHotkey(kReloadOverridesActionId);
	GakuMotion::host::UnregisterHotkey(kToggleOverridesActionId);
	auto& core = GakuMotion::Core::Instance();
	std::lock_guard<std::recursive_mutex> stateLock(core.stateMutex);
	core.WaitForLibraryRefresh();
	core.SetControlActive(false);
	core.injectionPolicy.SetSceneAllowed(false);
	core.injectionPolicy.SetCaptureHooksReady(false);
	if (GakuMotion::host::mainThread.isMainThread &&
		GakuMotion::host::mainThread.isMainThread(GakuMotion::host::mainThread.context))
		RevokeProtectedInjection();
	GakuMotion::capture::Shutdown();
	StageLoadOverride::Shutdown();
	SceneOwnerRuntime::Shutdown();
	ReleasePendingFileDialog();
	core.gui.browsePendingKind = -1;
	NativeAudio::Release(GakuMotion::bgm::ReleaseReason::Shutdown);
	const auto bgmStatus = g_bgmLease.GetSnapshot().status;
	const auto& bgmSnapshot = g_bgmLease.GetSnapshot();
	if (bgmSnapshot.held && g_hostContext.logWarn)
		g_hostContext.logWarn("[GakuMotion][NativeBgm] shutdown restoration pending: %s; held=%d owns_mutation=%d (no off-thread game call)",
			GakuMotion::bgm::StatusName(bgmStatus), bgmSnapshot.held, bgmSnapshot.ownsMutation);
	if (g_voiceLease.GetSnapshot().held && g_hostContext.logWarn)
		g_hostContext.logWarn("[GakuMotion][NativeAudio] Voice shutdown restoration pending: %s (no off-thread game call)",
			GakuMotion::bgm::StatusName(g_voiceLease.GetSnapshot().status));
	// Join native jobs before plugin code disappears. NativeAOT stays loaded
	// for the process lifetime; unloading its runtime is unsupported.
	GakuMotion::csharp_import::Shutdown();
	ClearSecondaryPhysics();
	g_burstBridge.Shutdown();
	EyeGazeProbe::Shutdown();
	BodyShadowProbe::Shutdown();
	g_bodyShadowProbeAttempted = false;
	GroundShadowProbe::Shutdown();
	g_groundShadowProbeAttempted = false;
	ActorGroundShadow::Reset();
	CameraBridge::Shutdown();
	g_cameraAttempted = false;
	core.AbandonScene();
	HelperAudioStop();
	GakuMotion::host::SetPlaying(false);
	if (g_hostContext.logInfo)
	{
		g_hostContext.logInfo("[GakuMotion] Plugin shut down.");
	}
	g_pluginInitialized = false;
}

namespace {
// The loader copies this domain service table. No new loader ABI or DLL export
// is needed, and the resident functions reject mutations after shutdown.
template<class Function>
GakuMotion::control::v0::Error ControlCall(Function&& function) noexcept
{
	try { return function(GakuMotion::Core::Instance()); }
	catch (...) { return GakuMotion::control::v0::Error::Failed; }
}

GakuStatus PublishControlApi()
{
	using namespace GakuMotion::control::v0;
	const Api service{Version, sizeof(Api),
		[](uint32_t size, State* output) { return ControlCall([&](auto& core) { return core.ReadControlState(size, output); }); },
		[](uint32_t size, Capabilities* output) { return ControlCall([&](auto& core) { return core.ReadControlCapabilities(size, output); }); },
		[](uint64_t revision, uint32_t index, uint32_t size, Motion* output) {
			return ControlCall([&](auto& core) { return core.ReadControlMotion(revision, index, size, output); }); },
		[](const Command* command, uint64_t* ticket) {
			if (ticket) *ticket = 0;
			return ControlCall([&](auto& core) { return core.SubmitControl(command, ticket); }); },
		[](uint64_t ticket, uint32_t size, Result* output) { return ControlCall([&](auto& core) { return core.PollControl(ticket, size, output); }); },
		[](uint64_t ticket) { return ControlCall([&](auto& core) { return core.ReleaseControl(ticket); }); }};
	return GakuMotion::host::api.registerService(GakuMotion::host::api.hostContext, ServiceName, sizeof(service), &service);
}

template<class Function>
GakuMotion::scene_nodes::v1::Status SceneNodesCall(bool mutation, Function&& function) noexcept
{
	using GakuMotion::scene_nodes::v1::Status;
	if (mutation && (!GakuMotion::host::mainThread.isMainThread ||
		!GakuMotion::host::mainThread.isMainThread(GakuMotion::host::mainThread.context)))
		return Status::WrongThread;
	try { return function(GakuMotion::Core::Instance()); }
	catch (...) { return Status::Failed; }
}

GakuStatus PublishSceneNodesApi()
{
	using namespace GakuMotion::scene_nodes::v1;
	const Api service{Version, sizeof(Api),
		[](uint32_t size, State* output) {
			return SceneNodesCall(false, [&](auto& core) { return core.stageManager.ReadSceneNodesState(size, output); }); },
		[](uint64_t revision, uint32_t index, uint32_t size, Node* output) {
			return SceneNodesCall(false, [&](auto& core) { return core.stageManager.ReadSceneNode(revision, index, size, output); }); },
		[](uint64_t revision, uint64_t* lease) {
			if (lease) *lease = 0;
			return SceneNodesCall(true, [&](auto& core) { return core.AcquireSceneNodes(revision, lease); }); },
		[](uint64_t lease, uint64_t revision, uint32_t count, const Visibility* values) {
			return SceneNodesCall(true, [&](auto& core) { return core.ApplySceneNodes(lease, revision, count, values); }); },
		[](uint64_t lease) {
			return SceneNodesCall(true, [&](auto& core) { return core.stageManager.ReleaseSceneNodes(lease); }); }};
	return GakuMotion::host::api.registerService(GakuMotion::host::api.hostContext,
		ServiceName, sizeof(service), &service);
}

// Native activation: connect the loader services, publish the playback contract,
// fill the host adapter and initialize config/audio/import. Runs before Unity
// exists, so it must not call managed code.
GakuStatus MotionActivate(const GakuHostApi* host, void** pluginState)
{
	if (!pluginState) return GAKU_E_INVALID_ARGUMENT;
	if (g_pluginInitialized) return GAKU_E_CONFLICT;
	const auto status = GakuMotion::host::Connect(host);
	if (status != GAKU_OK) return status;
	const auto published = GakuMotion::host::PublishPlayback();
	if (published != GAKU_OK) return published;
	const auto controlPublished = PublishControlApi();
	if (controlPublished != GAKU_OK) return controlPublished;
	const auto sceneNodesPublished = PublishSceneNodesApi();
	if (sceneNodesPublished != GAKU_OK) return sceneNodesPublished;

	g_hostContext = MotionHostContext{};
	g_hostContext.apiVersion = GAKU_ABI_VERSION;
	g_hostContext.gameRoot = GakuMotion::host::runtime.gameRoot;
	g_hostContext.localRoot = GakuMotion::host::runtime.localRoot;
	g_hostContext.pluginsRoot = GakuMotion::host::runtime.pluginsRoot;
	g_hostContext.gameAssemblyHandle = GakuMotion::host::runtime.getGameAssembly();
	g_hostContext.logInfo = MotionLogInfo;
	g_hostContext.logWarn = MotionLogWarn;
	g_hostContext.logError = MotionLogError;
	// The loader ABI exposes no VR query; those probes stay null and the gaze
	// feature reports itself unavailable.
	g_hostContext.isVRActive = nullptr;
	g_hostContext.isVRifyLiveGazeActive = nullptr;
	g_hostContext.setMotionPlaying = MotionSetMotionPlaying;
	g_hostContext.isMotionPlaying = MotionIsMotionPlaying;

	if (!InitializeMotion()) return GAKU_E_INTERNAL;
	GakuMotion::Core::Instance().SetControlActive(true);
	*pluginState = &GakuMotion::Core::Instance();

	const auto hotkey = GakuMotion::host::RegisterHotkey(kShowGuiActionId, "Show Motion controls",
		'M', GAKU_HOTKEY_MOD_CTRL, ShowGuiHotkey);
	if (hotkey == GAKU_OK)
		GakuMotion::host::Info("[GakuMotion] Hotkey Ctrl+M registered for the motion controls panel");
	else
		GakuMotion::host::Warn(
			"[GakuMotion] hotkey service unavailable (status=%d); Ctrl+M disabled", hotkey);
	// Plain F11 belongs to Localify (toggle-fullscreen). Registering it here too
	// made the loader refuse BOTH bindings, so the user lost fullscreen and the
	// reload. Shift+F11 keeps the pair distinct without touching Localify.
	const auto reloadKey = GakuMotion::host::RegisterHotkey(kReloadOverridesActionId,
		"Reload bone overrides", VK_F11, GAKU_HOTKEY_MOD_SHIFT, ReloadOverridesHotkey);
	const auto toggleKey = GakuMotion::host::RegisterHotkey(kToggleOverridesActionId,
		"Toggle bone overrides", VK_F11, GAKU_HOTKEY_MOD_CTRL, ToggleOverridesHotkey);
	if (reloadKey != GAKU_OK || toggleKey != GAKU_OK)
		GakuMotion::host::Warn("[GakuMotion] hotkey registrations: Shift+F11 reload=%d Ctrl+F11 toggle=%d",
			reloadKey, toggleKey);
	GakuMotion::host::Info("[GakuMotion][GakuLoader] ABI v1 services connected; camera hooks leased from the loader; KineTexture consumer active");
	return GAKU_OK;
}

// Unity main thread, at most once per frame. Frame-scoped work that needs no
// actor (deferred queue drains, timers) would go here; actor work belongs to
// MotionActorTick.
void MotionTick(void*, float)
{
	// Preferences and file jobs do not need a dancer. The import UI is already
	// usable in Photography preparation, before any actor callback can arrive.
	// Playback and scene commands retain their actor timing and queue order.
	{
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		core.ProcessPendingStageLoadCommands();
		core.ProcessPendingLibraryCommands();
	}
	if (g_runtimeReady.load(std::memory_order_acquire))
	{
		ResolveDeferredManagedTypes();
		StageLoader::ResolveDeferredTypeObjects();
		RefreshProtectedContext();
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		if (core.liveTakeoverFault && core.liveOwnerToken && core.liveFaultOwner &&
			core.liveOwnerToken != core.liveFaultOwner)
		{
			g_liveDriver.Observe({core.liveOwnerToken, false, false, false});
			if (g_liveDriver.State() == GakuMotion::live_takeover::Phase::Off)
			{
				core.liveTakeoverFault = false;
				core.liveFaultOwner = 0;
				g_liveTakeoverEnabled.store(false, std::memory_order_release);
			}
		}
		if (core.liveTakeoverActive && core.injectionPolicy.Read().captureDepth == 0 &&
			GakuMotion::capture::LiveAutoPhotoIdle())
		{
			g_liveDriver.Check();
			if (!g_liveDriver.MayInject())
			{
				core.liveTakeoverFault = g_liveDriver.State() == GakuMotion::live_takeover::Phase::Failed;
				if (core.liveTakeoverFault) core.liveFaultOwner = g_liveTakeoverOwner;
				core.liveTakeoverActive = false;
				g_liveTakeoverEnabled.store(core.liveTakeoverFault, std::memory_order_release);
				g_liveTakeoverOwner = 0;
				g_liveTakeoverScene = 0;
				core.liveControlNotice = core.liveTakeoverFault ?
					"Live 状态或角色归属无法确认，已停止接管；请重新进入场景。" : "Live 已离开，接管已结束。";
				RefreshProtectedContext(true);
			}
		}
		if (core.liveTakeoverRequest)
		{
			const int request = core.liveTakeoverRequest;
			const uintptr_t requestedOwner = core.liveTakeoverRequestOwner;
			const int requestedScene = core.liveTakeoverRequestScene;
			core.liveTakeoverRequest = 0; core.liveTakeoverRequestOwner = 0; core.liveTakeoverRequestScene = 0;
			core.livePauseRequest = 0; core.livePauseRequestOwner = 0; core.livePauseRequestScene = 0;
			RefreshProtectedContext(true);
			if (request == 1 && requestedOwner && requestedOwner == core.liveOwnerToken && requestedScene == core.liveOwnerScene &&
				core.liveTakeoverAvailable && !core.liveTakeoverActive)
			{
				g_liveTakeoverOwner = requestedOwner;
				g_liveTakeoverScene = requestedScene;
				g_liveDriver.Enable();
				core.liveTakeoverActive = g_liveDriver.MayInject();
				g_liveTakeoverEnabled.store(core.liveTakeoverActive, std::memory_order_release);
				core.liveTakeoverFault = g_liveDriver.State() == GakuMotion::live_takeover::Phase::Failed;
				if (core.liveTakeoverFault) core.liveFaultOwner = requestedOwner;
				if (!core.liveTakeoverActive) { g_liveTakeoverOwner = 0; g_liveTakeoverScene = 0; }
				core.liveControlNotice = core.liveTakeoverActive ? "Live 已暂停，可以为场景角色选择动作。" :
					"未能确认 Live 暂停和角色状态，未开启接管。";
			}
			else if (request == 2 && requestedOwner && requestedOwner == core.liveOwnerToken && requestedScene == core.liveOwnerScene &&
				core.liveTakeoverActive && core.injectionPolicy.Read().captureDepth == 0)
			{
				const bool resumeLive = g_liveDriver.OwnsPause();
				g_liveDriver.Disable();
				const bool clean = g_liveDriver.State() == GakuMotion::live_takeover::Phase::Off;
				if (core.injectionPolicy.Read().captureDepth != 0) core.injectionPolicy.EndCapture();
				if (clean)
				{
					core.captureSuspended = false;
					core.capturePerformanceWasActive = false;
					++core.injectionSession;
				}
				core.liveTakeoverActive = false;
				core.liveTakeoverFault = !clean;
				if (core.liveTakeoverFault) core.liveFaultOwner = requestedOwner;
				g_liveTakeoverEnabled.store(core.liveTakeoverFault, std::memory_order_release);
				g_liveTakeoverOwner = 0;
				g_liveTakeoverScene = 0;
				core.liveControlNotice = clean ? (resumeLive ? "已恢复角色与 Live 播放。" : "已恢复角色，Live 保持原有暂停状态。") :
					"角色或 Live 恢复未确认；已停止接管，请重新进入场景。";
			}
			else core.liveControlNotice = "Live 场景或拍照状态已变化，操作未执行。";
			RefreshProtectedContext(true);
		}
		if (core.livePauseRequest)
		{
			const int request = core.livePauseRequest;
			const uintptr_t requestedOwner = core.livePauseRequestOwner;
			const int requestedScene = core.livePauseRequestScene;
			core.livePauseRequest = 0;
			core.livePauseRequestOwner = 0;
			core.livePauseRequestScene = 0;
			RefreshProtectedContext(true);
			bool changed = false;
			if (requestedOwner && requestedOwner == core.liveOwnerToken && requestedScene == core.liveOwnerScene && core.liveViewingConfirmed &&
				core.liveObservation.confirmed)
			{
				const GakuMotion::live_takeover::RuntimeAccess access{
					g_faceRuntime.objectClass, g_faceRuntime.findField, g_faceRuntime.fieldType,
					g_faceRuntime.fieldFlags, g_faceRuntime.typeCode, StageLoader::metadata.classFromType,
					g_faceRuntime.readField, g_faceRuntime.invoke, g_safeBones.unbox};
				changed = GakuMotion::live_takeover::SetPaused(g_liveApi, access,
					reinterpret_cast<void*>(requestedOwner), request == 1);
			}
			core.liveControlNotice = changed ? (request == 1 ? "Live 已暂停。" : "Live 已恢复播放。") :
				"未能确认 Live 状态，操作未完成；请检查场景或日志。";
			RefreshProtectedContext(true);
		}
		RefreshSceneActors(core);
		core.RejectObsoleteControlCommands();
	}
}

void RenderGmlMotion(void*)
{
#if defined(GKMOTION_ENABLE_IMGUI) || defined(HAS_IMGUI)
	if (!g_runtimeReady.load(std::memory_order_acquire) || !GakuMotion::host::desktop.bind) return;
	const GakuImGuiAbi abi{IMGUI_VERSION_NUM, sizeof(ImGuiIO), sizeof(ImGuiStyle),
		sizeof(ImDrawVert), sizeof(ImDrawIdx), sizeof(ImWchar)};
	GakuDesktopUiBinding binding{};
	const auto status = GakuMotion::host::desktop.bind(&abi, sizeof(binding), &binding);
	if (status != GAKU_OK || !binding.imguiContext || !binding.alloc || !binding.free) return;
	// ImGui allocations may persist in plugin UI objects between callbacks. Keep
	// the negotiated host allocator for their entire lifetime, including teardown.
	ImGui::SetAllocatorFunctions(binding.alloc, binding.free, binding.allocatorUserData);
	auto* previous = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(static_cast<ImGuiContext*>(binding.imguiContext));
	GakuMotion::Core::Instance().OnRenderGui(
		GakuMotion::host::desktopWindow.presentationKind == GAKU_DESKTOP_PRESENTATION_PLUGIN_PANEL);
	ImGui::SetCurrentContext(previous);
	static bool logged = false;
	if (!logged) {
		logged = true;
		GakuMotion::host::Info("[GakuMotion][GakuLoader] desktop UI callback rendered with negotiated ImGui context/allocator");
	}
#endif
}
}

namespace GakuMotion::Log {
// Single choke point for every GakuMotion diagnostic that goes through GkLog.
// Kept out of the anonymous namespace above so the header declaration resolves
// to external linkage; the wire levels stay exactly the three the ABI defines.
void Write(Level level, const char* format, ...)
{
	if (!format) return;
	char buffer[2048]{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	switch (level)
	{
	case Level::Error: MotionLogError("%s", buffer); break;
	case Level::Warn: MotionLogWarn("%s", buffer); break;
	case Level::Info: MotionLogInfo("%s", buffer); break;
	case Level::Debug:
		// Reachable only from a development build: GM_DEBUG compiles to nothing
		// when GAKUMOTION_LOG_FLOOR > 0, so release never emits this path.
		MotionLogInfo("[D] %s", buffer);
		break;
	}
}
} // namespace GakuMotion::Log

extern "C" GAKU_PLUGIN_EXPORT GakuStatus GakuPlugin_Query(
	uint32_t requestedAbi, uint32_t outputSize, GakuPluginDescriptor* output)
{
	if (requestedAbi != GAKU_ABI_VERSION) return GAKU_E_VERSION;
	if (!output) return GAKU_E_INVALID_ARGUMENT;
	if (outputSize < sizeof(GakuPluginDescriptor)) return GAKU_E_SIZE;
	const GakuPluginDescriptor descriptor{GAKU_ABI_VERSION, sizeof(GakuPluginDescriptor),
		"gakumotion", "GakuMotion", "0.1.1",
		MotionActivate, MotionRuntimeReady, MotionTick, MotionActorTick,
		RenderGmlMotion, MotionShutdown};
	*output = descriptor;
	return GAKU_OK;
}
