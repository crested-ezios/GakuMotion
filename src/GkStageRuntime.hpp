#pragma once
#include "GkLog.hpp"

// Included inside the plugin's private namespace, after CameraBridge.
// No managed instance is retained between actions. Cached values below are
// metadata, scene/owner identity tokens, InstanceIDs, or native strings.
namespace StageLoader
{
	using Handle = void*; // Current Unity 6 pointer-width GCHandle ABI.
	inline Handle (*newHandle)(void*, bool) = nullptr;
	inline void (*freeHandle)(Handle) = nullptr;
	inline uintptr_t (*arrayLength)(void*) = nullptr;
	inline void* (*stringNew)(const char*) = nullptr;
	inline bool (*assignable)(void*, void*) = nullptr;
	inline int (*valueSize)(void*, uint32_t*) = nullptr;
	inline GakuMotion::ExactMethodMetadata metadata;
	inline void *goClass = nullptr, *componentClass = nullptr, *transformClass = nullptr;
	inline void *goType = nullptr, *managerClass = nullptr, *taskClass = nullptr, *tokenClass = nullptr;
	inline void *ownerClasses[2]{}, *ownerTypes[2]{};
	inline void* actorTypes[2]{};
	// il2cpp_type_get_object() hands back a managed System.Type, which belongs to
	// the Unity main thread. Runtime-ready may arrive on a loader worker, so
	// ResolveReflection() only records classes here; the three Type handles are
	// materialised on the main thread by ResolveDeferredTypeObjects(), driven from
	// MotionTick and MotionActorTick. Stage access simply fails until then.
	inline std::atomic<bool> pendingTypeObjects{false};
	inline bool ownerTypeWanted[2]{}, actorTypeWanted[2]{};
	inline const void *ownerBackground[2]{}, *loadMethod = nullptr, *unloadMethod = nullptr;
	inline const void *findAll = nullptr, *goScene = nullptr, *sceneName = nullptr, *sceneLoaded = nullptr;
	inline const void *instanceId = nullptr, *goTransform = nullptr, *componentGo = nullptr;
	inline const void *parent = nullptr, *childCount = nullptr, *child = nullptr, *objectName = nullptr;
	inline const void *setActive = nullptr, *activeSelf = nullptr;
	inline const void *setPosition = nullptr, *setRotation = nullptr;
	inline const void *cameraMain = nullptr, *cameraSetFlags = nullptr, *cameraGetFlags = nullptr;
	inline const void *cameraSetColor = nullptr, *cameraGetColor = nullptr;
	inline uint64_t generation = 1, mountedGeneration = 0;
	inline int currentScene = 0, mountedRootId = 0;
	inline int managerOwnerId = 0;
	inline uintptr_t managerIdentity = 0; // Equality token, never cast back or dereferenced.
	inline void* ownerToken = nullptr; // Equality only; never dereferenced or invoked.
	inline std::string mountedKey;
	inline std::unordered_map<int, bool> suspendedStageRoots;
	inline std::vector<int> mountedSceneHandles;
	inline GakuMotion::StageOrphanLedger orphanedStages;
	inline void RememberMountedStage()
	{
		orphanedStages.Remember(mountedKey, mountedSceneHandles);
	}
	struct Entry { void* go; int id; int scene; };
	inline std::unordered_map<void*, Entry> objects;
	inline std::unordered_map<int, void*> byId;
	inline std::unordered_set<void*> freshTransforms;
	inline std::unordered_set<void*> protectedRoots;
	inline std::vector<Handle> handles;
	inline int accessDepth = 0;
	inline bool snapshotReady = false;

	inline void ClearSnapshot()
	{
		objects.clear(); byId.clear(); freshTransforms.clear(); protectedRoots.clear(); snapshotReady = false;
		if (freeHandle) for (Handle h : handles) freeHandle(h);
		handles.clear(); accessDepth = 0;
	}
	inline void Abandon()
	{
		RememberMountedStage();
		ClearSnapshot();
		++generation;
		currentScene = 0; ownerToken = nullptr;
		mountedGeneration = 0; mountedRootId = 0; mountedKey.clear();
		suspendedStageRoots.clear();
		mountedSceneHandles.clear();
		managerOwnerId = 0; managerIdentity = 0;
	}
	inline void BindContext(int scene, void* owner)
	{
		if (!scene || !owner) { Abandon(); return; }
		if (currentScene && currentScene != scene) Abandon();
		currentScene = scene;
		if (!ownerToken) ownerToken = owner;
	}
	inline bool Pin(void* object)
	{
		if (!object || !newHandle || !freeHandle) return false;
		Handle h = newHandle(object, false);
		if (!h) return false;
		handles.push_back(h);
		return true;
	}
	inline void* Invoke(const void* method, void* target = nullptr, void** args = nullptr)
	{
		if (!method || !g_faceRuntime.invoke) return nullptr;
		void* exception = nullptr;
		void* result = g_faceRuntime.invoke(method, target, args, &exception);
		return exception ? nullptr : result;
	}
	inline int ReadInt(const void* method, void* target, int fallback = 0, void** args = nullptr)
	{
		void* boxed = Invoke(method, target, args);
		void* data = boxed && g_safeBones.unbox ? g_safeBones.unbox(boxed) : nullptr;
		return data ? *static_cast<int*>(data) : fallback;
	}
	inline bool ReadBool(const void* method, void* target, bool fallback = false, void** args = nullptr)
	{
		void* boxed = Invoke(method, target, args);
		void* data = boxed && g_safeBones.unbox ? g_safeBones.unbox(boxed) : nullptr;
		return data ? *static_cast<bool*>(data) : fallback;
	}
	inline int SceneOfFreshGo(void* go) { return ReadInt(goScene, go); }
	inline void* Transform(void* go);
	inline void* Parent(void* tr);
	inline void* GameObject(void* tr);

	inline bool BeginAccessImpl(bool detached)
	{
		if (accessDepth) { ++accessDepth; return snapshotReady; }
		if ((!detached && (!currentScene || !ownerToken)) || !findAll || !goType || !goScene || !sceneLoaded || !sceneName ||
			!instanceId || !setActive || !activeSelf || !objectName || !goTransform || !parent ||
			!componentGo || !childCount || !child || (!actorTypes[0] && !actorTypes[1]) ||
			!arrayLength || !g_safeBones.unbox || !newHandle || !freeHandle) return false;
		bool includeInactive = true;
		void* args[2]{goType, &includeInactive};
		void* array = Invoke(findAll, nullptr, args);
		if (!Pin(array)) return false;
		const uintptr_t count = arrayLength(array);
		if (count > 100000) { ClearSnapshot(); return false; }
		void** items = reinterpret_cast<void**>(static_cast<char*>(array) + 32);
		std::unordered_map<int, bool> scenes;
		for (uintptr_t i = 0; i < count; ++i)
		{
			void* go = items[i];
			if (!go) continue; // Only fresh objects from a strongly rooted engine array.
			int scene = SceneOfFreshGo(go);
			if (!scene) continue;
			auto [it, inserted] = scenes.emplace(scene, false);
			if (inserted)
			{
				void* sceneArgs[1]{&scene};
				const std::string name = Il2CppStringToUtf8(Invoke(sceneName, nullptr, sceneArgs));
				it->second = !name.empty() && name != "DontDestroyOnLoad" &&
					ReadBool(sceneLoaded, nullptr, false, sceneArgs);
			}
			if (!it->second) continue;
			int id = ReadInt(instanceId, go);
			if (!id) continue;
			objects.emplace(go, Entry{go, id, scene});
			byId.emplace(id, go);
		}
		snapshotReady = true;
		// Protect actual actor hierarchies from the current engine enumeration,
		// including actors whose root names do not contain an Actor keyword.
		for (void* type : actorTypes)
		{
			if (!type) continue;
			void* actorArgs[2]{type, &includeInactive};
			void* actors = Invoke(findAll, nullptr, actorArgs);
			if (!Pin(actors) || arrayLength(actors) > 10000) { ClearSnapshot(); return false; }
			void** actorItems = reinterpret_cast<void**>(static_cast<char*>(actors) + 32);
			for (uintptr_t i = 0; i < arrayLength(actors); ++i)
			{
				if (!actorItems[i]) continue;
				void* go = Invoke(componentGo, actorItems[i]);
				void* tr = Transform(go);
				if (!tr) continue;
				for (int depth = 0; depth < 64; ++depth)
				{
					void* parentTr = Parent(tr);
					if (!parentTr) break;
					tr = parentTr;
				}
				if (void* rootGo = GameObject(tr)) protectedRoots.insert(rootGo);
			}
		}
		accessDepth = 1;
		return true;
	}
	inline bool BeginAccess() { return BeginAccessImpl(false); }
	inline void EndAccess() { if (accessDepth && --accessDepth == 0) ClearSnapshot(); }
	struct Scope
	{
		bool ready;
		explicit Scope(bool detached = false) : ready(BeginAccessImpl(detached)) {}
		~Scope() { if (ready) EndAccess(); }
	};
	inline int ObjectId(void* go)
	{
		auto it = objects.find(go); // An old pointer is an opaque lookup key only.
		return snapshotReady && it != objects.end() ? it->second.id : 0;
	}
	inline bool Matches(void* go, int id, uint64_t expectedGeneration)
	{
		return expectedGeneration == generation && id != 0 && ObjectId(go) == id;
	}
	inline void* Transform(void* go)
	{
		if (!ObjectId(go)) return nullptr;
		void* tr = Invoke(goTransform, go);
		if (!Pin(tr)) return nullptr;
		freshTransforms.insert(tr);
		return tr;
	}
	inline void* GameObject(void* tr)
	{
		if (!freshTransforms.contains(tr)) return nullptr;
		void* go = Invoke(componentGo, tr);
		return ObjectId(go) ? go : nullptr;
	}
	inline void* Parent(void* tr)
	{
		if (!freshTransforms.contains(tr)) return nullptr;
		void* value = Invoke(parent, tr);
		if (!value || !Pin(value)) return nullptr;
		freshTransforms.insert(value);
		return value;
	}
	inline int ChildCount(void* tr)
	{
		return freshTransforms.contains(tr) ? ReadInt(childCount, tr) : 0;
	}
	inline void* Child(void* tr, int index)
	{
		if (!freshTransforms.contains(tr) || index < 0 || index >= ChildCount(tr)) return nullptr;
		void* args[1]{&index};
		void* value = Invoke(child, tr, args);
		if (!Pin(value)) return nullptr;
		freshTransforms.insert(value);
		return value;
	}
	inline std::vector<void*> Roots()
	{
		std::vector<void*> roots;
		if (!snapshotReady || !goTransform || !parent) return roots;
		for (const auto& [go, entry] : objects)
		{
			if (protectedRoots.contains(go)) continue;
			void* tr = Transform(go);
			if (tr && !Parent(tr)) roots.push_back(tr);
		}
		return roots;
	}

	// Explicit stage loading only. Ordinary background managers are never searched
	// as UnityEngine.Objects, and no manager/owner pointer survives this scope.
	inline void* AcquireManager()
	{
		if (!snapshotReady || !assignable || !managerClass || !componentClass) return nullptr;
		void* selected = nullptr;
		int matches = 0;
		for (int kind = 0; kind != 2; ++kind)
		{
			if (!ownerTypes[kind] || !ownerBackground[kind]) continue;
			bool inactive = true;
			void* args[2]{ownerTypes[kind], &inactive};
			void* array = Invoke(findAll, nullptr, args);
			if (!Pin(array)) continue;
			uintptr_t count = arrayLength(array);
			if (count > 10000) return nullptr;
			void** items = reinterpret_cast<void**>(static_cast<char*>(array) + 32);
			for (uintptr_t i = 0; i < count; ++i)
			{
				void* owner = items[i];
				if (!owner) continue;
				void* go = Invoke(componentGo, owner);
				auto it = objects.find(go);
				if (it == objects.end() || it->second.scene != currentScene) continue;
				if (++matches != 1) return nullptr;
				void* manager = Invoke(ownerBackground[kind], owner);
				if (!manager || !Pin(manager) || !g_faceRuntime.objectClass ||
					!assignable(managerClass, g_faceRuntime.objectClass(manager))) return nullptr;
				if (managerOwnerId && (managerOwnerId != it->second.id ||
					managerIdentity != reinterpret_cast<uintptr_t>(manager)))
				{
					// A fresh owner replaced the former loader in the same Scene.
					// Invalidate saved stage identities; retain no reference to either manager.
					RememberMountedStage();
					++generation; mountedKey.clear(); mountedRootId = 0; mountedGeneration = 0;
					suspendedStageRoots.clear();
					mountedSceneHandles.clear();
				}
				managerOwnerId = it->second.id;
				managerIdentity = reinterpret_cast<uintptr_t>(manager);
				selected = manager;
			}
		}
		return selected;
	}
	inline bool LoadBackground(const char* key)
	{
		if (key && orphanedStages.BlocksMount(key))
		{
			if (g_hostContext.logWarn) g_hostContext.logWarn(
				"[GakuMotion][Stage] retired stage key cannot be mounted again in this process; restart required: %s", key);
			return false;
		}
		Scope scope;
		if (!scope.ready || !key || !std::string(key).starts_with("env_3d_") || !loadMethod ||
			!stringNew || !valueSize || !tokenClass) return false;
		void* manager = AcquireManager();
		if (!manager) return false;
		uint32_t alignment = 0;
		const int bytes = valueSize(tokenClass, &alignment);
		if (bytes <= 0 || bytes > 64 || alignment > alignof(std::max_align_t)) return false;
		alignas(std::max_align_t) unsigned char token[64]{};
		void* text = stringNew(key);
		if (!Pin(text)) return false;
		void* args[2]{text, token};
		void* exception = nullptr;
		g_faceRuntime.invoke(loadMethod, manager, args, &exception);
		if (exception) return false;
		if (mountedKey != key) RememberMountedStage();
		mountedKey = key; mountedGeneration = generation; mountedRootId = 0;
		suspendedStageRoots.clear();
		mountedSceneHandles.clear();
		return true;
	}
	inline bool UnloadBackground()
	{
		Scope scope;
		if (mountedKey.empty()) return true;
		if (!scope.ready || mountedGeneration != generation || !unloadMethod || !stringNew) return false;
		void* manager = AcquireManager();
		if (!manager || mountedKey.empty() || mountedGeneration != generation) return false;
		void* text = stringNew(mountedKey.c_str());
		if (!Pin(text)) return false;
		void* args[1]{text};
		void* exception = nullptr;
		g_faceRuntime.invoke(unloadMethod, manager, args, &exception);
		if (exception) return false;
		RememberMountedStage();
		mountedKey.clear(); mountedRootId = 0; mountedGeneration = 0;
		suspendedStageRoots.clear();
		mountedSceneHandles.clear();
		return true;
	}
	inline bool IsMountedStageObject(void* go)
	{
		if (!snapshotReady || mountedKey.empty() || mountedGeneration != generation) return false;
		auto it = objects.find(go);
		if (it == objects.end()) return false;
		int scene = it->second.scene;
		void* args[1]{&scene};
		// The stage resource key is also its additive Scene name. A confirmed
		// root token is only accepted from the current rooted enumeration.
		const bool matches = it->second.id == mountedRootId || Il2CppStringToUtf8(Invoke(sceneName, nullptr, args)) == mountedKey;
		if (matches && std::find(mountedSceneHandles.begin(), mountedSceneHandles.end(), scene) == mountedSceneHandles.end())
			mountedSceneHandles.push_back(scene);
		return matches;
	}
	inline bool EnsureNoOrphanedStages()
	{
		if (orphanedStages.entries.empty()) return true;
		// Cleanup cannot depend on an actor, old manager, or currentScene token.
		// An async stage may first become visible after all of them were lost.
		Scope scope(true);
		if (!scope.ready) return false;
		bool complete = true;
		std::vector<int> presentScenes;
		for (const auto& [go, entry] : objects)
		{
			if (std::find(presentScenes.begin(), presentScenes.end(), entry.scene) == presentScenes.end())
				presentScenes.push_back(entry.scene);
			void* tr = Transform(go);
			if (!tr || Parent(tr)) continue;
			int scene = entry.scene;
			void* args[]{&scene};
			const std::string key = Il2CppStringToUtf8(Invoke(sceneName, nullptr, args));
			if (!orphanedStages.ShouldHide(key, scene)) continue;
			if (protectedRoots.contains(go)) { complete = false; continue; }
			bool active = false;
			void* activeArgs[]{&active}; void* exception = nullptr;
			g_faceRuntime.invoke(setActive, go, activeArgs, &exception);
			if (exception || ReadBool(activeSelf, go, true)) complete = false;
		}
		orphanedStages.FinishSnapshot(presentScenes);
		return complete && !orphanedStages.Pending();
	}
	inline bool SetMountedStageActive(bool active)
	{
		if (mountedKey.empty()) return true;
		Scope scope;
		if (!scope.ready || mountedGeneration != generation || !setActive || !activeSelf) return false;
		bool complete = true;
		if (active)
		{
			for (auto it = suspendedStageRoots.begin(); it != suspendedStageRoots.end();)
			{
				auto fresh = byId.find(it->first);
				if (fresh == byId.end()) { it = suspendedStageRoots.erase(it); continue; }
				if (!IsMountedStageObject(fresh->second)) { complete = false; ++it; continue; }
				bool value = it->second;
				void* args[1]{&value};
				void* exception = nullptr;
				g_faceRuntime.invoke(setActive, fresh->second, args, &exception);
				if (exception || ReadBool(activeSelf, fresh->second, !value) != value) { complete = false; ++it; }
				else it = suspendedStageRoots.erase(it);
			}
			return complete;
		}
		bool found = false;
		for (const auto& [go, entry] : objects)
		{
			if (protectedRoots.contains(go) || !IsMountedStageObject(go)) continue;
			void* tr = Transform(go);
			if (!tr || Parent(tr)) continue;
			found = true;
			suspendedStageRoots.try_emplace(entry.id, ReadBool(activeSelf, go, false));
			bool value = false;
			void* args[1]{&value};
			void* exception = nullptr;
			g_faceRuntime.invoke(setActive, go, args, &exception);
			if (exception || ReadBool(activeSelf, go, true)) complete = false;
		}
		// LoadBackgroundSceneAsync can still be pending after its UniTask was
		// returned. Until its roots are found, a clean capture cannot be certified.
		return found && complete;
	}
	inline void SetLoadedStageRoot(void* go) { mountedRootId = ObjectId(go); mountedGeneration = generation; }
	inline void SetStageTransform(float x, float y, float z, float yaw)
	{
		if (!mountedRootId || mountedGeneration != generation) return;
		Scope scope;
		auto it = byId.find(mountedRootId);
		if (!scope.ready || it == byId.end()) return;
		void* tr = Transform(it->second);
		if (!tr) return;
		UnityVector3 position{x, y, z};
		void* positionArgs[1]{&position};
		Invoke(setPosition, tr, positionArgs);
		float rad = yaw * 3.14159265358979323846f / 360.0f;
		UnityQuaternion rotation{0, std::sin(rad), 0, std::cos(rad)};
		void* rotationArgs[1]{&rotation};
		Invoke(setRotation, tr, rotationArgs);
	}

	inline void ResolveReflection(HMODULE assembly)
	{
#define STAGE_EXPORT(member, symbol) member = reinterpret_cast<decltype(member)>(GetProcAddress(assembly, symbol))
		STAGE_EXPORT(metadata.methods, "il2cpp_class_get_methods");
		STAGE_EXPORT(metadata.name, "il2cpp_method_get_name");
		STAGE_EXPORT(metadata.count, "il2cpp_method_get_param_count");
		STAGE_EXPORT(metadata.flags, "il2cpp_method_get_flags");
		STAGE_EXPORT(metadata.generic, "il2cpp_method_is_generic");
		STAGE_EXPORT(metadata.inflated, "il2cpp_method_is_inflated");
		STAGE_EXPORT(metadata.declaringClass, "il2cpp_method_get_class");
		STAGE_EXPORT(metadata.parameter, "il2cpp_method_get_param");
		STAGE_EXPORT(metadata.result, "il2cpp_method_get_return_type");
		STAGE_EXPORT(metadata.typeCode, "il2cpp_type_get_type");
		STAGE_EXPORT(metadata.byRef, "il2cpp_type_is_byref");
		STAGE_EXPORT(metadata.classFromType, "il2cpp_class_from_type");
		STAGE_EXPORT(newHandle, "il2cpp_gchandle_new");
		STAGE_EXPORT(freeHandle, "il2cpp_gchandle_free");
		STAGE_EXPORT(arrayLength, "il2cpp_array_length");
		STAGE_EXPORT(stringNew, "il2cpp_string_new");
		STAGE_EXPORT(assignable, "il2cpp_class_is_assignable_from");
		STAGE_EXPORT(valueSize, "il2cpp_class_value_size");
#undef STAGE_EXPORT
		using Type = GakuMotion::ExactMethodMetadata::Type;
		auto unity = [](const char* name) { return FindClass("UnityEngine.CoreModule.dll", "UnityEngine", name); };
		goClass = unity("GameObject"); componentClass = unity("Component"); transformClass = unity("Transform");
		void* object = unity("Object"); void* camera = unity("Camera");
		void* scene = FindClass("UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement", "Scene");
		void* type = FindClass("mscorlib.dll", "System", "Type");
		void* color = unity("Color"); void* clearFlags = unity("CameraClearFlags");
		void* array = nullptr;
		auto arrayClass = reinterpret_cast<void* (*)(void*, uint32_t)>(GetProcAddress(assembly, "il2cpp_array_class_get"));
		if (object && arrayClass) array = arrayClass(object, 1);
		// goType is created on the Unity main thread. See pendingTypeObjects.
		auto find = [&](void* klass, const char* name, bool stat, Type ret, std::initializer_list<Type> args)
		{
			// A missing reference/value class must never turn into a wildcard match.
			if ((ret.code == 0x12 || ret.code == 0x11 || ret.code == 0x1d) && !ret.klass) return static_cast<const void*>(nullptr);
			for (Type t : args) if ((t.code == 0x12 || t.code == 0x11) && !t.klass) return static_cast<const void*>(nullptr);
			return metadata.Find(klass, name, stat, ret, args);
		};
		findAll = find(object, "FindObjectsOfType", true, {0x1d, array}, {{0x12, type}, {0x02}});
		goScene = find(goClass, "get_scene", false, {0x11, scene}, {});
		uint32_t alignment = 0;
		if (!scene || !valueSize || valueSize(scene, &alignment) != sizeof(int)) goScene = nullptr;
		sceneName = find(scene, "GetNameInternal", true, {0x0e}, {{0x08}});
		sceneLoaded = find(scene, "GetIsLoadedInternal", true, {0x02}, {{0x08}});
		instanceId = find(object, "GetInstanceID", false, {0x08}, {});
		goTransform = find(goClass, "get_transform", false, {0x12, transformClass}, {});
		componentGo = find(componentClass, "get_gameObject", false, {0x12, goClass}, {});
		parent = find(transformClass, "get_parent", false, {0x12, transformClass}, {});
		childCount = find(transformClass, "get_childCount", false, {0x08}, {});
		child = find(transformClass, "GetChild", false, {0x12, transformClass}, {{0x08}});
		objectName = find(object, "get_name", false, {0x0e}, {});
		setActive = find(goClass, "SetActive", false, {0x01}, {{0x02}});
		activeSelf = find(goClass, "get_activeSelf", false, {0x02}, {});
		setPosition = find(transformClass, "set_position", false, {0x01}, {{0x11, unity("Vector3")}});
		setRotation = find(transformClass, "set_rotation", false, {0x01}, {{0x11, unity("Quaternion")}});
		cameraMain = find(camera, "get_main", true, {0x12, camera}, {});
		cameraSetFlags = find(camera, "set_clearFlags", false, {0x01}, {{0x11, clearFlags}});
		cameraGetFlags = find(camera, "get_clearFlags", false, {0x11, clearFlags}, {});
		cameraSetColor = find(camera, "set_backgroundColor", false, {0x01}, {{0x11, color}});
		cameraGetColor = find(camera, "get_backgroundColor", false, {0x11, color}, {});

		managerClass = FindClass("Assembly-CSharp.dll", "Campus.Common", "CampusBackgroundSceneManager");
		taskClass = FindClass("UniTask.dll", "Cysharp.Threading.Tasks", "UniTask");
		tokenClass = FindClass("mscorlib.dll", "System.Threading", "CancellationToken");
		loadMethod = find(managerClass, "LoadBackgroundSceneAsync", false, {0x11, taskClass}, {{0x0e}, {0x11, tokenClass}});
		unloadMethod = find(managerClass, "UnloadSceneAsync", false, {0x11, taskClass}, {{0x0e}});
		ownerClasses[0] = FindClass("Assembly-CSharp.dll", "Campus.OutGame", "OutGame3DManager");
		ownerClasses[1] = FindClass("Assembly-CSharp.dll", "Campus.InGame", "InGame3DManager");
		for (int i = 0; i != 2; ++i)
		{
			ownerTypes[i] = nullptr; ownerBackground[i] = nullptr; ownerTypeWanted[i] = false;
			if (!ownerClasses[i] || !componentClass || !assignable || !assignable(componentClass, ownerClasses[i])) continue;
			// Metadata only here; the System.Type is deferred to the main thread.
			ownerTypeWanted[i] = true;
			ownerBackground[i] = find(ownerClasses[i], "get_Background", false, {0x12, managerClass}, {});
		}
		void* actorClasses[2]{g_campusActorControllerKlass, g_campusVirtualActorControllerKlass};
		for (int i = 0; i != 2; ++i)
		{
			actorTypes[i] = nullptr; actorTypeWanted[i] = false;
			if (actorClasses[i] && componentClass && assignable && assignable(componentClass, actorClasses[i]))
				actorTypeWanted[i] = true;
		}
		const bool canCreateTypes = il2cpp_class_get_type && il2cpp_type_get_object;
		if (canCreateTypes &&
			(goClass || ownerTypeWanted[0] || ownerTypeWanted[1] || actorTypeWanted[0] || actorTypeWanted[1]))
			pendingTypeObjects.store(true, std::memory_order_release);
		if (g_hostContext.logInfo) g_hostContext.logInfo(
			"[GakuMotion][Stage] direct-scene bridge: finder=%p scene=%p loaded=%p ids=%p active=%p actors=%d/%d; exact optional load=%p unload=%p owners=%d/%d (no manager cache); managed types %s",
			findAll, goScene, sceneLoaded, instanceId, setActive, static_cast<int>(actorTypeWanted[0]), static_cast<int>(actorTypeWanted[1]),
			loadMethod, unloadMethod, static_cast<int>(ownerTypeWanted[0]), static_cast<int>(ownerTypeWanted[1]),
			canCreateTypes ? "deferred to main thread" : "UNAVAILABLE (il2cpp_class_get_type/il2cpp_type_get_object missing)");
	}

	// Materialises goType / ownerTypes / actorTypes. Unity main thread only;
	// returns true once nothing is pending. Safe to call every frame.
	inline bool ResolveDeferredTypeObjects()
	{
		if (!pendingTypeObjects.load(std::memory_order_acquire)) return true;
		if (!il2cpp_class_get_type || !il2cpp_type_get_object) return false;
		bool done = true;
		if (goClass && !goType)
		{
			if (void* typePtr = il2cpp_class_get_type(goClass)) goType = il2cpp_type_get_object(typePtr);
			else done = false;
		}
		for (int i = 0; i != 2; ++i)
		{
			if (!ownerTypeWanted[i] || ownerTypes[i]) continue;
			if (void* typePtr = il2cpp_class_get_type(ownerClasses[i])) ownerTypes[i] = il2cpp_type_get_object(typePtr);
			else done = false;
		}
		void* actorClasses[2]{g_campusActorControllerKlass, g_campusVirtualActorControllerKlass};
		for (int i = 0; i != 2; ++i)
		{
			if (!actorTypeWanted[i] || actorTypes[i]) continue;
			if (void* typePtr = il2cpp_class_get_type(actorClasses[i])) actorTypes[i] = il2cpp_type_get_object(typePtr);
			else done = false;
		}
		if (done) pendingTypeObjects.store(false, std::memory_order_release);
		return done;
	}
}

bool HelperGameObjectSetActive(void* go, bool value)
{
	if (!StageLoader::ObjectId(go) || !StageLoader::setActive) return false;
	void* args[1]{&value};
	void* exception = nullptr;
	g_faceRuntime.invoke(StageLoader::setActive, go, args, &exception);
	if (exception) return false;
	const bool readback = StageLoader::ReadBool(StageLoader::activeSelf, go, !value);
	if (g_hostContext.logInfo) g_hostContext.logInfo(
		"[GakuMotion][Stage] background id=%d generation=%llu requested=%d readback=%d",
		StageLoader::ObjectId(go), static_cast<unsigned long long>(StageLoader::generation), value,
		readback);
	return readback == value;
}
bool HelperGameObjectGetActiveSelf(void* go)
{
	return StageLoader::ObjectId(go) && StageLoader::ReadBool(StageLoader::activeSelf, go);
}
bool HelperTryGetActiveSelf(void* go, bool& value)
{
	if (!StageLoader::ObjectId(go) || !StageLoader::activeSelf || !g_safeBones.unbox) return false;
	void* exception = nullptr;
	void* boxed = g_faceRuntime.invoke(StageLoader::activeSelf, go, nullptr, &exception);
	void* data = boxed && !exception ? g_safeBones.unbox(boxed) : nullptr;
	if (!data) return false;
	value = *static_cast<bool*>(data);
	return true;
}
void* HelperCameraGetMain() { return StageLoader::Invoke(StageLoader::cameraMain); }
void HelperCameraSetClearFlags(void* camera, int flags)
{
	void* args[1]{&flags};
	StageLoader::Invoke(StageLoader::cameraSetFlags, camera, args);
}
int HelperCameraGetClearFlags(void* camera) { return StageLoader::ReadInt(StageLoader::cameraGetFlags, camera, 0); }
void HelperCameraSetBgColor(void* camera, const float* color)
{
	void* args[1]{const_cast<float*>(color)};
	StageLoader::Invoke(StageLoader::cameraSetColor, camera, args);
}
void HelperCameraGetBgColor(void* camera, float* color)
{
	void* boxed = StageLoader::Invoke(StageLoader::cameraGetColor, camera);
	void* data = boxed && g_safeBones.unbox ? g_safeBones.unbox(boxed) : nullptr;
	if (data && color) std::memcpy(color, data, sizeof(float) * 4);
}
void* HelperGetGameObject(void* comp) { return StageLoader::GameObject(comp); }
std::string HelperGetObjectName(void* obj)
{
	return StageLoader::ObjectId(obj) ? Il2CppStringToUtf8(StageLoader::Invoke(StageLoader::objectName, obj)) : "";
}
std::vector<void*> HelperFindSceneRoots()
{
	auto roots = StageLoader::Roots();
	GM_DEBUG("[GakuMotion][Stage] scan scene=%d generation=%llu roots=%zu objects=%zu",
		StageLoader::currentScene, static_cast<unsigned long long>(StageLoader::generation), roots.size(), StageLoader::objects.size());
	return roots;
}
