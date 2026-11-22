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
	g_multiBurstBridge.Configure(g_hostContext.pluginsRoot, [](void*, int32_t level, const char* message)
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
	core.fnStartImport = StartMotionImport;
	core.fnPollImport = PollMotionImport;
	core.fnPollImportProgress = PollMotionImportWithProgress;
	core.fnSaveTrackCoverPng = [](const std::vector<uint8_t>& rawBytes, std::string_view mimeHint,
		const std::filesystem::path& targetDir, std::string& outFileName, std::string& outError) {
		return GakuMotion::cover::SaveTrackCoverPng(rawBytes, mimeHint, targetDir, outFileName, outError, 512);
	};
	core.fnLoadTrackCoverDataUri = [](const std::filesystem::path& coverPath) {
		return GakuMotion::cover::LoadCoverAsDataUri(coverPath, 512);
	};
	core.fnStartCameraImport = StartCameraImport;
	core.fnPollCameraImport = PollCameraImport;
	core.fnPickFile = GakuMotion::host::active ? nullptr : PickMotionFile;
	core.fnBeginPickFile = GakuMotion::host::active ? BeginMotionFile : nullptr;
	core.fnPollPickFile = GakuMotion::host::active ? PollMotionFile : nullptr;
	core.fnActiveAudioEffectId = HelperActiveAudioEffectId;
	g_audioPlugin.SetConfigRoot(GakuMotion::MotionPathFromUtf8(selected.configs));

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
	player.fnObserveAudioWithTime = HelperAudioObserveWithTime;
	player.fnSetAudioVolume = HelperAudioSetVolume;
	player.fnAudioVolumeSupport = HelperAudioVolumeSupport;
	player.fnSupportsAudioPlaybackRate = HelperAudioSupportsRate;
	player.fnSetAudioPlaybackRate = HelperAudioSetRate;
	player.fnCanPlay = HelperCanPlay;

	if (g_hostContext.pluginsRoot)
	{
		GakuMotion::csharp_import::SetPluginsRoot(WidePathToNarrow(g_hostContext.pluginsRoot));
	}
	GakuMotion::mouth_source::reader = GakuMotion::csharp_import::ReadMouthSource;
	GakuMotion::mouth_source::report = [](const char* directory, const char* status)
	{
		GM_INFO("[GakuMotion][MouthSource] track=%s status=%s", directory, status);
	};
	core.SetGameRoot(ResolvePluginGameRootUtf8());
	core.Initialize(localRoot, &selected);
	g_portraitCache.Configure(core.configRoot, core.gameRoot);
	g_trackCoverCache.Configure(WebPanelGetTrackCoverDataUri);
	HelperAudioSetVolume(player.musicVolume);
	if (g_hostContext.logInfo) g_hostContext.logInfo("[GakuMotion][Paths] layout=%s configs=%s motions=%s",
		selected.managed ? "gakuloader-v1" : "legacy-unmigrated", core.configRoot.c_str(), core.motionsRoot.c_str());

	if (g_hostContext.logInfo)
	{
		g_hostContext.logInfo("[GakuMotion] Plugin initialized successfully. LocalRoot: %s", localRoot.c_str());
		g_hostContext.logInfo("[GakuMotion] %s", core.StatusSummary().c_str());
	}
	g_pluginInitialized = true;
	return true;
}
