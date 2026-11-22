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

	GakuMotion::portrait::PortraitCache g_portraitCache;
	GakuMotion::TrackCoverCache g_trackCoverCache;
	std::mutex g_audioProbeMutex;
	std::string WebPanelGetTrackCoverDataUri(const std::string& trackDirUtf8);

	std::string ResolvePluginGameRootUtf8()
	{
		if (g_hostContext.gameRoot && g_hostContext.gameRoot[0])
		{
			return WidePathToNarrow(g_hostContext.gameRoot);
		}
		wchar_t exePath[MAX_PATH * 4]{};
		const DWORD length = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
		if (length > 0 && length < std::size(exePath))
		{
			return GakuMotion::MotionPathToUtf8(std::filesystem::path(exePath).parent_path());
		}
		return {};
	}

	namespace PortraitUnityExtractor
	{
		struct UnityRect { float x, y, width, height; };

		struct PinGuard
		{
			std::vector<StageLoader::Handle> handles;
			void* Keep(void* obj)
			{
				if (!obj || !StageLoader::newHandle) return obj;
				if (StageLoader::Handle h = StageLoader::newHandle(obj, false)) handles.push_back(h);
				return obj;
			}
			~PinGuard()
			{
				if (!StageLoader::freeHandle) return;
				for (auto it = handles.rbegin(); it != handles.rend(); ++it) StageLoader::freeHandle(*it);
			}
		};

		inline bool resolved = false;
		inline bool attempted = false;
		inline void* tex2dClass = nullptr;
		inline void* tex2dTypeObj = nullptr;
		inline void* byteClass = nullptr;
		inline void* rtClass = nullptr;
		inline void* bundleClass = nullptr;
		inline void* (*il2cppObjectNew)(void*) = nullptr;
		inline void* (*il2cppArrayNew)(void*, uintptr_t) = nullptr;
		inline void (*il2cppStaticFieldGet)(void*, void*) = nullptr;
		inline bool (*il2cppClassIsEnum)(void*) = nullptr;
		inline const void* getWidth = nullptr;
		inline const void* getHeight = nullptr;
		inline const void* texCtor5 = nullptr;
		inline const void* texCtor2 = nullptr;
		inline const void* readPixels = nullptr;
		inline const void* apply = nullptr;
		inline const void* encodeToPng = nullptr;
		inline const void* rtGetTemp5 = nullptr;
		inline const void* rtGetTemp3 = nullptr;
		inline const void* rtGetActive = nullptr;
		inline const void* rtSetActive = nullptr;
		inline const void* rtReleaseTemp = nullptr;
		inline const void* graphicsBlit = nullptr;
		inline const void* findObjectsAll = nullptr;
		inline const void* bundleLoadMem = nullptr;
		inline const void* bundleLoadAsset = nullptr;
		inline const void* bundleUnload = nullptr;
		inline const void* objectDestroy = nullptr;
		inline int rgba32FormatVal = 4;
		inline int rtArgb32Val = 0;
		inline int rtSrgbVal = 2;
		inline bool hasEnumArgs = false;

		inline bool Invoke(const void* method, void* self, void** args, void*& outResult)
		{
			outResult = nullptr;
			if (!method || !g_faceRuntime.invoke || !ProtectionOnUnityThread()) return false;
			void* exception = nullptr;
			outResult = g_faceRuntime.invoke(method, self, args, &exception);
			return !exception;
		}

		inline bool ReadInt(const void* method, void* self, int& value)
		{
			void* boxed = nullptr;
			if (!Invoke(method, self, nullptr, boxed) || !boxed || !g_safeBones.unbox) return false;
			void* raw = g_safeBones.unbox(boxed);
			if (!raw) return false;
			std::memcpy(&value, raw, sizeof(value));
			return true;
		}

		inline bool ReadEnum(void* klass, const char* name, int& outValue)
		{
			uint32_t alignment = 0;
			if (!klass || !il2cppClassIsEnum || !il2cppClassIsEnum(klass) || !StageLoader::valueSize ||
				StageLoader::valueSize(klass, &alignment) != sizeof(int) || alignment != alignof(int) ||
				!g_faceRuntime.findField || !g_faceRuntime.fieldFlags || !g_faceRuntime.fieldType || !il2cppStaticFieldGet)
				return false;
			void* field = g_faceRuntime.findField(klass, name);
			if (!field || (g_faceRuntime.fieldFlags(field) & 0x50) != 0x50 ||
				!StageLoader::metadata.Matches(g_faceRuntime.fieldType(field), {0x11, klass}))
				return false;
			il2cppStaticFieldGet(field, &outValue);
			return true;
		}

		inline bool EnsureResolved()
		{
			if (resolved) return true;
			if (attempted || !g_gameAssembly || !ProtectionOnUnityThread()) return false;
			if (!il2cpp_class_get_type || !il2cpp_type_get_object || !StageLoader::newHandle ||
				!StageLoader::freeHandle || !StageLoader::arrayLength || !StageLoader::stringNew ||
				!StageLoader::assignable || !StageLoader::valueSize || !StageLoader::objectName || !g_faceRuntime.objectClass)
				return false;

			attempted = true;
			using Type = GakuMotion::ExactMethodMetadata::Type;
			auto find = [&](void* klass, const char* name, bool stat, Type ret, std::initializer_list<Type> args) -> const void*
			{
				if ((ret.code == 0x12 || ret.code == 0x11 || ret.code == 0x1d) && !ret.klass) return nullptr;
				for (Type t : args)
					if ((t.code == 0x12 || t.code == 0x11 || t.code == 0x1d) && !t.klass) return nullptr;
				return StageLoader::metadata.Find(klass, name, stat, ret, args);
			};

			auto core = [](const char* n) { return FindClass("UnityEngine.CoreModule.dll", "UnityEngine", n); };
			void* texBaseClass = core("Texture");
			tex2dClass = core("Texture2D");
			rtClass = core("RenderTexture");
			void* graphicsClass = core("Graphics");
			void* resourcesClass = core("Resources");
			void* objectClass = core("Object");
			void* rectClass = core("Rect");
			void* texFmtClass = core("TextureFormat");
			void* rtFmtClass = core("RenderTextureFormat");
			void* rtRwClass = core("RenderTextureReadWrite");
			void* typeClass = FindClass("mscorlib.dll", "System", "Type");
			byteClass = FindClass("mscorlib.dll", "System", "Byte");
			bundleClass = FindClass("UnityEngine.AssetBundleModule.dll", "UnityEngine", "AssetBundle");
			void* imgConvClass = FindClass("UnityEngine.ImageConversionModule.dll", "UnityEngine", "ImageConversion");
			if (!imgConvClass) imgConvClass = core("ImageConversion");

			il2cppObjectNew = reinterpret_cast<decltype(il2cppObjectNew)>(GetProcAddress(g_gameAssembly, "il2cpp_object_new"));
			il2cppArrayNew = reinterpret_cast<decltype(il2cppArrayNew)>(GetProcAddress(g_gameAssembly, "il2cpp_array_new"));
			il2cppStaticFieldGet = reinterpret_cast<decltype(il2cppStaticFieldGet)>(
				GetProcAddress(g_gameAssembly, "il2cpp_field_static_get_value"));
			il2cppClassIsEnum = reinterpret_cast<decltype(il2cppClassIsEnum)>(
				GetProcAddress(g_gameAssembly, "il2cpp_class_is_enum"));
			auto arrayClassGet = reinterpret_cast<void* (*)(void*, uint32_t)>(
				GetProcAddress(g_gameAssembly, "il2cpp_array_class_get"));
			if (!il2cppObjectNew || !il2cppArrayNew || !arrayClassGet || !byteClass || !objectClass || !rectClass) return false;

			uint32_t align = 0;
			if (StageLoader::valueSize(rectClass, &align) != sizeof(UnityRect)) return false;

			void* byteArrayClass = arrayClassGet(byteClass, 1);
			void* objArrayClass = arrayClassGet(objectClass, 1);
			if (!byteArrayClass || !objArrayClass) return false;

			getWidth = find(texBaseClass, "get_width", false, {0x08}, {});
			getHeight = find(texBaseClass, "get_height", false, {0x08}, {});
			texCtor5 = find(tex2dClass, ".ctor", false, {0x01},
				{{0x08}, {0x08}, {0x11, texFmtClass}, {0x02}, {0x02}});
			texCtor2 = find(tex2dClass, ".ctor", false, {0x01}, {{0x08}, {0x08}});
			readPixels = find(tex2dClass, "ReadPixels", false, {0x01}, {{0x11, rectClass}, {0x08}, {0x08}});
			apply = find(tex2dClass, "Apply", false, {0x01}, {});
			encodeToPng = find(imgConvClass, "EncodeToPNG", true, {0x1d, byteArrayClass}, {{0x12, tex2dClass}});
			rtGetTemp5 = find(rtClass, "GetTemporary", true, {0x12, rtClass},
				{{0x08}, {0x08}, {0x08}, {0x11, rtFmtClass}, {0x11, rtRwClass}});
			rtGetTemp3 = find(rtClass, "GetTemporary", true, {0x12, rtClass}, {{0x08}, {0x08}, {0x08}});
			rtGetActive = find(rtClass, "get_active", true, {0x12, rtClass}, {});
			rtSetActive = find(rtClass, "set_active", true, {0x01}, {{0x12, rtClass}});
			rtReleaseTemp = find(rtClass, "ReleaseTemporary", true, {0x01}, {{0x12, rtClass}});
			graphicsBlit = find(graphicsClass, "Blit", true, {0x01}, {{0x12, texBaseClass}, {0x12, rtClass}});
			findObjectsAll = find(resourcesClass, "FindObjectsOfTypeAll", true, {0x1d, objArrayClass}, {{0x12, typeClass}});
			bundleLoadMem = find(bundleClass, "LoadFromMemory", true, {0x12, bundleClass}, {{0x1d, byteArrayClass}});
			bundleLoadAsset = find(bundleClass, "LoadAsset", false, {0x12, objectClass}, {{0x0e}, {0x12, typeClass}});
			bundleUnload = find(bundleClass, "Unload", false, {0x01}, {{0x02}});
			objectDestroy = find(objectClass, "Destroy", true, {0x01}, {{0x12, objectClass}});

			hasEnumArgs = ReadEnum(texFmtClass, "RGBA32", rgba32FormatVal) &&
				ReadEnum(rtFmtClass, "ARGB32", rtArgb32Val) &&
				ReadEnum(rtRwClass, "sRGB", rtSrgbVal);

			if (!tex2dClass || !getWidth || !getHeight || (!texCtor5 && !texCtor2) ||
				!readPixels || !apply || !encodeToPng || (!rtGetTemp5 && !rtGetTemp3) ||
				!rtGetActive || !rtSetActive || !rtReleaseTemp || !graphicsBlit || !findObjectsAll || !objectDestroy)
				return false;

			if (void* typePtr = il2cpp_class_get_type(tex2dClass))
			{
				if (void* typeObj = il2cpp_type_get_object(typePtr))
				{
					StageLoader::newHandle(typeObj, false); // Process-lifetime pin for System.Type.
					tex2dTypeObj = typeObj;
				}
			}
			resolved = (tex2dTypeObj != nullptr);
			return resolved;
		}

		inline bool ReadbackTexture2DToPng(void* srcTex, std::vector<uint8_t>& outPng)
		{
			outPng.clear();
			if (!srcTex || !EnsureResolved()) return false;
			if (!StageLoader::assignable(tex2dClass, g_faceRuntime.objectClass(srcTex))) return false;

			int w = 0, h = 0;
			if (!ReadInt(getWidth, srcTex, w) || !ReadInt(getHeight, srcTex, h)) return false;
			if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;

			PinGuard pins;
			void* previousActive = nullptr;
			Invoke(rtGetActive, nullptr, nullptr, previousActive);
			if (previousActive) pins.Keep(previousActive);

			void* rt = nullptr;
			struct RtCleanup
			{
				void* prev{nullptr};
				void* tempRt{nullptr};
				void* readable{nullptr};
				bool restoreActive{false};
				~RtCleanup()
				{
					void* ignored = nullptr;
					if (restoreActive)
					{
						void* a[]{prev};
						Invoke(rtSetActive, nullptr, a, ignored);
					}
					if (tempRt)
					{
						void* a[]{tempRt};
						Invoke(rtReleaseTemp, nullptr, a, ignored);
					}
					if (readable)
					{
						void* a[]{readable};
						Invoke(objectDestroy, nullptr, a, ignored);
					}
				}
			} guard{previousActive, nullptr, nullptr, false};

			if (hasEnumArgs && rtGetTemp5)
			{
				int depth = 0;
				void* args[]{&w, &h, &depth, &rtArgb32Val, &rtSrgbVal};
				if (!Invoke(rtGetTemp5, nullptr, args, rt)) rt = nullptr;
			}
			if (!rt && rtGetTemp3)
			{
				int depth = 0;
				void* args[]{&w, &h, &depth};
				if (!Invoke(rtGetTemp3, nullptr, args, rt)) rt = nullptr;
			}
			if (!rt) return false;
			pins.Keep(rt);
			guard.tempRt = rt;

			void* ignored = nullptr;
			void* blitArgs[]{srcTex, rt};
			if (!Invoke(graphicsBlit, nullptr, blitArgs, ignored)) return false;

			void* setArgs[]{rt};
			if (!Invoke(rtSetActive, nullptr, setArgs, ignored)) return false;
			guard.restoreActive = true;

			void* readable = il2cppObjectNew(tex2dClass);
			if (!readable) return false;
			pins.Keep(readable);
			guard.readable = readable;

			bool ctorOk = false;
			if (hasEnumArgs && texCtor5)
			{
				bool mip = false, linear = false;
				void* ctorArgs[]{&w, &h, &rgba32FormatVal, &mip, &linear};
				ctorOk = Invoke(texCtor5, readable, ctorArgs, ignored);
			}
			if (!ctorOk && texCtor2)
			{
				void* ctorArgs[]{&w, &h};
				ctorOk = Invoke(texCtor2, readable, ctorArgs, ignored);
			}
			if (!ctorOk) return false;

			UnityRect rect{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)};
			int destX = 0, destY = 0;
			void* rpArgs[]{&rect, &destX, &destY};
			if (!Invoke(readPixels, readable, rpArgs, ignored)) return false;
			if (!Invoke(apply, readable, nullptr, ignored)) return false;

			void* pngObj = nullptr;
			void* encArgs[]{readable};
			if (!Invoke(encodeToPng, nullptr, encArgs, pngObj) || !pngObj) return false;
			pins.Keep(pngObj);

			const uintptr_t len = StageLoader::arrayLength(pngObj);
			if (len < 45 || len > 32ULL * 1024ULL * 1024ULL) return false;
			const uint8_t* ptr = reinterpret_cast<const uint8_t*>(pngObj) + 32;
			outPng.assign(ptr, ptr + len);
			return GakuMotion::portrait::IsValidPngBytes(outPng);
		}

		inline bool ExtractResident(const std::string& assetName, std::vector<uint8_t>& outPng)
		{
			outPng.clear();
			if (assetName.empty() || !EnsureResolved()) return false;
			PinGuard pins;
			void* allArray = nullptr;
			void* args[]{tex2dTypeObj};
			if (!Invoke(findObjectsAll, nullptr, args, allArray) || !allArray) return false;
			pins.Keep(allArray);

			const uintptr_t count = StageLoader::arrayLength(allArray);
			if (count == 0 || count > 65536) return false;
			void** items = reinterpret_cast<void**>(static_cast<char*>(allArray) + 32);
			for (uintptr_t i = 0; i < count; ++i)
			{
				void* obj = items[i];
				if (!obj || !StageLoader::assignable(tex2dClass, g_faceRuntime.objectClass(obj))) continue;
				void* nameObj = nullptr;
				if (!Invoke(StageLoader::objectName, obj, nullptr, nameObj) || !nameObj) continue;
				const std::string texName = Il2CppStringToUtf8(nameObj);
				if (texName == assetName) return ReadbackTexture2DToPng(obj, outPng);
			}
			return false;
		}

		inline bool ExtractFromBundle(const std::string& assetName, const std::vector<uint8_t>& bundleBytes, std::vector<uint8_t>& outPng)
		{
			outPng.clear();
			if (assetName.empty() || bundleBytes.empty() || !EnsureResolved()) return false;
			if (!bundleLoadMem || !bundleLoadAsset || !bundleUnload) return false;

			PinGuard pins;
			void* managedBytes = il2cppArrayNew(byteClass, bundleBytes.size());
			if (!managedBytes) return false;
			pins.Keep(managedBytes);
			std::memcpy(static_cast<char*>(managedBytes) + 32, bundleBytes.data(), bundleBytes.size());

			void* bundleObj = nullptr;
			void* loadMemArgs[]{managedBytes};
			if (!Invoke(bundleLoadMem, nullptr, loadMemArgs, bundleObj) || !bundleObj) return false;
			pins.Keep(bundleObj);

			struct BundleGuard
			{
				void* bundle{nullptr};
				~BundleGuard()
				{
					if (!bundle) return;
					bool unloadAll = true;
					void* args[]{&unloadAll};
					void* ignored = nullptr;
					Invoke(bundleUnload, bundle, args, ignored);
				}
			} bundleGuard{bundleObj};

			void* managedName = StageLoader::stringNew(assetName.c_str());
			if (!managedName) return false;
			pins.Keep(managedName);

			void* assetObj = nullptr;
			void* loadAssetArgs[]{managedName, tex2dTypeObj};
			if (!Invoke(bundleLoadAsset, bundleObj, loadAssetArgs, assetObj) || !assetObj) return false;
			pins.Keep(assetObj);
			return ReadbackTexture2DToPng(assetObj, outPng);
		}
	} // namespace PortraitUnityExtractor

	void TickPortraitExtractionOnMainThread()
	{
		if (!g_pluginInitialized || !ProtectionOnUnityThread()) return;
		if (!PortraitUnityExtractor::EnsureResolved()) return;
		g_portraitCache.ProcessOnePending(
			PortraitUnityExtractor::ExtractResident,
			PortraitUnityExtractor::ExtractFromBundle);
	}
