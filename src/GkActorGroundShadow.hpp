#pragma once
#include "GkLog.hpp"

// Included in the plugin's private namespace after GkStageRuntime.hpp.
// The native actor LateUpdate runs before Motion's post-actor callback. Its
// foot AO can therefore contain the native root placement even after Motion
// replaces the skeleton. Rebuild only that derived data from the final pose;
// never move the actor parent or apply the root translation a second time.
namespace ActorGroundShadow
{
	inline std::atomic<DWORD> unityThreadId{0};
	inline bool attempted = false, faulted = false, refreshing = false, logged = false;
	inline void *actorClass = nullptr, *aoClass = nullptr, *aoField = nullptr;
	inline const void *lateUpdate = nullptr, *aliveMethod = nullptr;
	inline uint64_t lastPoseSerial = 0;

	inline void Reset()
	{
		unityThreadId.store(0, std::memory_order_release);
		attempted = faulted = refreshing = logged = false;
		actorClass = aoClass = aoField = nullptr;
		lateUpdate = aliveMethod = nullptr;
		lastPoseSerial = 0;
	}

	inline void BindUnityThread()
	{
		DWORD empty = 0;
		unityThreadId.compare_exchange_strong(empty, GetCurrentThreadId(), std::memory_order_acq_rel);
	}

	inline bool IsClass(void* object, void* expected)
	{
		if (!object || !expected || !g_faceRuntime.objectClass || !g_faceRuntime.parentClass) return false;
		void* klass = g_faceRuntime.objectClass(object);
		for (int depth = 0; klass && depth < 64; ++depth, klass = g_faceRuntime.parentClass(klass))
			if (klass == expected) return true;
		return false;
	}

	// Metadata only: no retained actor/AO/Transform instances or NativeArray
	// layout assumptions. Exact signatures and the field's declared class must
	// match this game build before any managed method can be invoked.
	inline bool ResolveMetadata(void* actor, void* ao, void* unityObject)
	{
		attempted = true;
		actorClass = actor;
		aoClass = ao;
		aoField = nullptr;
		lateUpdate = aliveMethod = nullptr;
		const auto& metadata = StageLoader::metadata;
		if (!actor || !ao || !unityObject || !g_safeBones.unbox || !g_faceRuntime.findField || !g_faceRuntime.fieldType ||
			!g_faceRuntime.fieldFlags || !g_faceRuntime.readField || !g_faceRuntime.invoke ||
			!g_faceRuntime.objectClass || !g_faceRuntime.parentClass || !metadata.classFromType ||
			!metadata.typeCode || !metadata.byRef) return false;
		void* field = g_faceRuntime.findField(actor, "_actorAO");
		void* type = field ? g_faceRuntime.fieldType(field) : nullptr;
		if (!type || (g_faceRuntime.fieldFlags(field) & 0x10) || metadata.byRef(type) ||
			metadata.typeCode(type) != 0x12 || metadata.classFromType(type) != ao) return false;
		const void* method = metadata.Find(ao, "OnLateUpdate", false, {0x01}, {});
		const void* alive = metadata.Find(unityObject, "op_Implicit", true, {0x02}, {{0x12, unityObject}});
		if (!method || !alive) return false;
		aoField = field;
		lateUpdate = method;
		aliveMethod = alive;
		return true;
	}

	inline bool Resolve()
	{
		if (attempted) return aoField && lateUpdate && aliveMethod;
		const bool ok = ResolveMetadata(
			FindClass("campus-submodule.Runtime.dll", "Campus.Common", "CampusActorController"),
			FindClass("vl-unity.Runtime.dll", "VL", "VLActorAO"),
			FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Object"));
		if (!ok && g_hostContext.logWarn) g_hostContext.logWarn(
			"[GakuMotion][FootAO] exact native AO contract unavailable; post-pose refresh disabled");
		return ok;
	}

	inline bool IsAlive(void* object)
	{
		if (!object) return false;
		void* args[]{object};
		void* exception = nullptr;
		void* boxed = g_faceRuntime.invoke(aliveMethod, nullptr, args, &exception);
		void* value = !exception && boxed ? g_safeBones.unbox(boxed) : nullptr;
		return value && *static_cast<bool*>(value);
	}

	inline bool RefreshAfterPose(void* actor, uint64_t serialBefore)
	{
		// All gates precede metadata/managed access. A serial change proves the
		// actual body consumer wrote this actor in this callback, including seek
		// and paused preview. No refresh from UI, cameras, or an idle/stopped tick.
		if (!actor || GetCurrentThreadId() != unityThreadId.load(std::memory_order_acquire) ||
			faulted || refreshing || !g_motionSceneAllowed) return false;
		auto& player = GakuMotion::Core::Instance().player;
		const auto& controller = player.controller;
		const auto& stamp = controller.footPoseWriteStamp;
		if (player.state == GakuMotion::PlaybackState::Stopped || !controller.IsTargetActor(actor) ||
			!controller.CanDriveActor(actor) || stamp.actor != actor || stamp.serial == serialBefore ||
			stamp.serial == lastPoseSerial) return false;
		auto binding = controller.bindings.find(actor);
		if (binding == controller.bindings.end() || !binding->second.bindPoseCaptured || !Resolve() ||
			!IsClass(actor, actorClass) || !IsAlive(actor)) return false;
		// Borrow the current field each time: costume replacement/disposal must
		// never leave a cached managed AO or its job buffers behind.
		void* ao = nullptr;
		g_faceRuntime.readField(actor, aoField, &ao);
		if (!IsClass(ao, aoClass) || !IsAlive(ao)) return false;
		refreshing = true;
		lastPoseSerial = stamp.serial;
		void* exception = nullptr;
		g_faceRuntime.invoke(lateUpdate, ao, nullptr, &exception);
		refreshing = false;
		if (exception)
		{
			faulted = true;
			if (g_hostContext.logWarn) g_hostContext.logWarn(
				"[GakuMotion][FootAO] native refresh raised a managed exception; disabled until plugin restart");
			return false;
		}
		if (!logged)
		{
			logged = true;
			GM_DEBUG("[GakuMotion][FootAO] native left/right foot AO refreshed after final MMD pose");
		}
		return true;
	}
}
