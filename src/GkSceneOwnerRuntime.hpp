#pragma once

// Included in the plugin's private namespace after the existing Unity bridges.
// Bounded Developer Preview checks: exact observed Active Scene plus the
// exact Photography presenter or current OutGame Costume TopScreen.
namespace SceneOwnerRuntime
{
	struct Observation
	{
		GakuMotion::injection::OwnerSnapshot owner;
		GakuMotion::injection::Context context{GakuMotion::injection::Context::Unknown};
		std::string sceneName, rootClassName;
		const char* reason{"owner-unavailable"};
	};
	inline void *baseClass = nullptr, *photoClass = nullptr, *outGameClass = nullptr, *sceneClass = nullptr;
	inline StageLoader::Handle typeHandle = nullptr;
	inline StageLoader::Handle liveTypeHandle = nullptr;
	inline const void* currentPresenter = nullptr;
	inline void* (*handleTarget)(StageLoader::Handle) = nullptr;
	inline const char* (*className)(void*) = nullptr;
	inline const char* (*classNamespace)(void*) = nullptr;
	inline bool ready = false;
	inline int cachedFrame = INT_MIN;
	inline bool cachedPageStable = false;
	inline std::string cachedActiveScene;
	inline Observation cached;

	inline bool Resolve()
	{
		auto& api = StageLoader::metadata;
		baseClass = FindClass("Assembly-CSharp.dll", "Campus.Common", "CampusScenePresenterBase");
		photoClass = FindClass("Assembly-CSharp.dll", "Campus.Photography", "PhotographyScenePresenter");
		outGameClass = FindClass("Assembly-CSharp.dll", "Campus.OutGame", "OutGameScenePresenter");
		sceneClass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement", "Scene");
		currentPresenter = baseClass ? api.Find(baseClass, "get_Instance", true, {0x12, baseClass}, {}) : nullptr;
		handleTarget = reinterpret_cast<decltype(handleTarget)>(GetProcAddress(g_gameAssembly, "il2cpp_gchandle_get_target"));
		className = reinterpret_cast<decltype(className)>(GetProcAddress(g_gameAssembly, "il2cpp_class_get_name"));
		classNamespace = reinterpret_cast<decltype(classNamespace)>(GetProcAddress(g_gameAssembly, "il2cpp_class_get_namespace"));
		uint32_t alignment = 0;
		ready = baseClass && photoClass && outGameClass && sceneClass &&
			StageLoader::valueSize && StageLoader::valueSize(sceneClass, &alignment) == sizeof(int) &&
			StageLoader::assignable && StageLoader::componentClass &&
			StageLoader::assignable(StageLoader::componentClass, photoClass) &&
			StageLoader::assignable(StageLoader::componentClass, outGameClass) && currentPresenter &&
			StageLoader::findAll && StageLoader::arrayLength && StageLoader::newHandle && StageLoader::freeHandle &&
			handleTarget && il2cpp_class_get_type && il2cpp_type_get_object &&
			StageLoader::componentGo && StageLoader::goScene && StageLoader::sceneLoaded && StageLoader::sceneName &&
			g_faceRuntime.invoke && g_faceRuntime.objectClass && g_safeBones.unbox && className && classNamespace &&
			GakuMotion::capture::topScreenMetadata && GakuMotion::capture::costumeClass;
		return ready;
	}

	inline Observation ReadCandidate(void* root)
	{
		Observation result;
		if (!ready || !ProtectionOnUnityThread()) return result;
		void* exception = nullptr;
		if (!root) { result.reason = "presenter-reference-unconfirmed"; return result; }
		void* klass = g_faceRuntime.objectClass(root);
		if (!klass || !StageLoader::assignable(StageLoader::componentClass, klass))
		{ result.reason = "current-presenter-type-unconfirmed"; return result; }
		const char* ns = classNamespace(klass);
		const char* name = className(klass);
		result.rootClassName = std::string(ns ? ns : "") + "." + (name ? name : "");
		auto& owner = result.owner;
		owner.presenterIdentity = reinterpret_cast<uintptr_t>(root);
		owner.role = klass == photoClass ? GakuMotion::injection::OwnerRole::Photography :
			klass == outGameClass ? GakuMotion::injection::OwnerRole::OutGame :
			klass == g_liveApi.sceneClass ? GakuMotion::injection::OwnerRole::Live : GakuMotion::injection::OwnerRole::Other;
		exception = nullptr;
		void* go = g_faceRuntime.invoke(StageLoader::componentGo, root, nullptr, &exception);
		if (!go || exception) { result.reason = "presenter-gameobject-unconfirmed"; return result; }
		exception = nullptr;
		void* boxed = g_faceRuntime.invoke(StageLoader::goScene, go, nullptr, &exception);
		if (!boxed || exception || g_faceRuntime.objectClass(boxed) != sceneClass)
		{ result.reason = "presenter-scene-unconfirmed"; return result; }
		void* value = g_safeBones.unbox(boxed);
		if (!value) { result.reason = "presenter-scene-unbox-failed"; return result; }
		owner.sceneHandle = *static_cast<int*>(value);
		if (!owner.sceneHandle) { result.reason = "presenter-scene-zero"; return result; }
		void* args[]{&owner.sceneHandle};
		exception = nullptr;
		boxed = g_faceRuntime.invoke(StageLoader::sceneLoaded, nullptr, args, &exception);
		value = boxed && !exception ? g_safeBones.unbox(boxed) : nullptr;
		if (!value) { result.reason = "presenter-scene-loaded-unconfirmed"; return result; }
		owner.sceneLoaded = *static_cast<bool*>(value);
		if (!owner.sceneLoaded) { result.reason = "presenter-scene-unloaded"; return result; }
		exception = nullptr;
		void* text = g_faceRuntime.invoke(StageLoader::sceneName, nullptr, args, &exception);
		if (!text || exception) { result.reason = "presenter-scene-name-unconfirmed"; return result; }
		result.sceneName = Il2CppStringToUtf8(text);
		if (owner.role == GakuMotion::injection::OwnerRole::OutGame)
		{
			exception = nullptr;
			void* top = g_faceRuntime.invoke(GakuMotion::capture::topScreenMetadata, root, nullptr, &exception);
			if (exception) { result.reason = "current-top-screen-unconfirmed"; return result; }
			owner.costumePage = GakuMotion::capture::IsCurrentCostumePage(top ? g_faceRuntime.objectClass(top) : nullptr);
		}
		owner.confirmed = true;
		result.reason = owner.AllowedContext() != GakuMotion::injection::Context::Unknown
			? "current-presenter-owner-confirmed" : "current-presenter-not-allowed";
		return result;
	}

	inline Observation Scan(const std::string& activeScene)
	{
		Observation result;
		if (!ready || !ProtectionOnUnityThread()) return result;
		// The existing current-presenter property is populated in OutGame and
		// Produce. It is null in the observed Photography pages. Never replace a
		// confirmed current root with an old loaded Photography component.
		void* currentException = nullptr;
		void* current = g_faceRuntime.invoke(currentPresenter, nullptr, nullptr, &currentException);
		if (currentException) { result.reason = "current-presenter-unconfirmed"; return result; }
		if (current)
		{
			result = ReadCandidate(current);
			result.context = GakuMotion::injection::PreviewContext(activeScene, result.sceneName, result.owner);
			if (result.owner.Valid()) result.reason = result.context == GakuMotion::injection::Context::Unknown ?
				"preview-scene-or-presenter-not-allowed" : "preview-scene-and-presenter-confirmed";
			return result;
		}
		// Read the exact Photo component even for a denied background so an
		// existing live Photo owner can restore native state on permission loss.
		// Only PreviewContext can grant permission from the observed scene names.
		if (!typeHandle)
		{
			// Managed Type creation is Unity-thread-only; keep an independent root.
			void* type = il2cpp_type_get_object(il2cpp_class_get_type(photoClass));
			if (type) typeHandle = StageLoader::newHandle(type, false);
		}
		void* type = typeHandle ? handleTarget(typeHandle) : nullptr;
		if (!type) { result.reason = "presenter-type-unconfirmed"; return result; }
		bool includeInactive = false;
		void* args[]{type, &includeInactive};
		void* exception = nullptr;
		void* array = g_faceRuntime.invoke(StageLoader::findAll, nullptr, args, &exception);
		if (!array || exception) { result.reason = "active-presenter-query-unconfirmed"; return result; }
		struct ArrayRoot
		{
			StageLoader::Handle handle;
			~ArrayRoot() { if (handle) StageLoader::freeHandle(handle); }
		} rooted{StageLoader::newHandle(array, false)};
		if (!rooted.handle) { result.reason = "active-presenter-array-unrooted"; return result; }
		const uintptr_t count = StageLoader::arrayLength(array);
		if (count != 1)
		{
			// Live does not populate CampusScenePresenterBase.Instance in the
			// observed viewing scene. Search only if there are no active Photography
			// owners and the active scene explicitly identifies a Live scene.
			// The result is evidence only: PreviewContext still denies injection.
			if (count == 0 && (activeScene == "Live" || activeScene.rfind("env_3d_live_", 0) == 0) && g_liveApi.sceneClass)
			{
				if (!liveTypeHandle)
				{
					void* liveType = il2cpp_type_get_object(il2cpp_class_get_type(g_liveApi.sceneClass));
					if (liveType) liveTypeHandle = StageLoader::newHandle(liveType, false);
				}
				void* liveType = liveTypeHandle ? handleTarget(liveTypeHandle) : nullptr;
				if (!liveType) { result.reason = "live-presenter-type-unconfirmed"; return result; }
				void* liveArgs[]{liveType, &includeInactive};
				exception = nullptr;
				void* liveArray = g_faceRuntime.invoke(StageLoader::findAll, nullptr, liveArgs, &exception);
				if (!liveArray || exception) { result.reason = "live-presenter-query-unconfirmed"; return result; }
				ArrayRoot liveRoot{StageLoader::newHandle(liveArray, false)};
				if (!liveRoot.handle) { result.reason = "live-presenter-array-unrooted"; return result; }
				if (StageLoader::arrayLength(liveArray) != 1)
				{ result.reason = "live-presenter-not-unique"; return result; }
				void** liveItems = reinterpret_cast<void**>(static_cast<char*>(liveArray) + 32);
				if (!liveItems[0] || g_faceRuntime.objectClass(liveItems[0]) != g_liveApi.sceneClass)
				{ result.reason = "live-presenter-type-mismatch"; return result; }
				result = ReadCandidate(liveItems[0]);
				result.context = GakuMotion::injection::Context::Unknown;
				if (result.owner.Valid()) result.reason = "live-presenter-observed-read-only";
				return result;
			}
			result.reason = "exact-photography-presenter-not-unique";
			return result;
		}
		// Same validated x64 Object[] layout as the existing rooted Stage bridge.
		void** items = reinterpret_cast<void**>(static_cast<char*>(array) + 32);
		if (!items[0] || g_faceRuntime.objectClass(items[0]) != photoClass)
		{ result.reason = "exact-photography-presenter-unconfirmed"; return result; }
		result = ReadCandidate(items[0]);
		result.context = GakuMotion::injection::PreviewContext(activeScene, result.sceneName, result.owner);
		if (result.owner.Valid()) result.reason = result.context == GakuMotion::injection::Context::Unknown ?
			"preview-scene-or-presenter-not-allowed" : "preview-scene-and-presenter-confirmed";
		return result;
	}

	inline Observation Read(const std::string& activeScene, bool force = false)
	{
		if (!ProtectionOnUnityThread()) return {};
		const int frame = g_faceUnityFrame ? g_faceUnityFrame() : -1;
		const bool stable = GakuMotion::capture::IsPageStable();
		if (!force && frame >= 0 && frame == cachedFrame && stable == cachedPageStable &&
			activeScene == cachedActiveScene) return cached;
		cached = Scan(activeScene); // Values only: no managed root or array survives the call.
		GakuMotion::stage_load::receipt.Observe(cached.owner);
		cached.context = GakuMotion::stage_load::receipt.Context(activeScene, cached.sceneName, cached.owner);
		cachedFrame = frame;
		cachedPageStable = stable;
		cachedActiveScene = activeScene;
		return cached;
	}

	inline void Shutdown()
	{
		if (typeHandle && StageLoader::freeHandle) StageLoader::freeHandle(typeHandle);
		if (liveTypeHandle && StageLoader::freeHandle) StageLoader::freeHandle(liveTypeHandle);
		typeHandle = nullptr;
		liveTypeHandle = nullptr;
		cached = {}; cachedFrame = INT_MIN; ready = false;
	}

	inline bool ConfirmedUnloaded(int handle)
	{
		if (!handle || !ProtectionOnUnityThread() || !StageLoader::sceneLoaded) return false;
		void* args[]{&handle};
		void* exception = nullptr;
		void* boxed = g_faceRuntime.invoke(StageLoader::sceneLoaded, nullptr, args, &exception);
		void* value = boxed && !exception ? g_safeBones.unbox(boxed) : nullptr;
		return value && !*static_cast<bool*>(value);
	}
}
