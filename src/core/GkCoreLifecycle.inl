namespace GakuMotion
{
	inline void Core::Initialize(const std::string& rootPath, const DataLayout* selectedLayout)
	{
		if (initialized) return;

		// PROBE S1：GUI 侧与播放器侧的探针都汇入宿主 logger，宿主按 ANSI 解码，
		// 故探针文案一律 ASCII（中文会变乱码）。挂在这里保证任何早于面板首次
		// 绘制的拖动动作也能出日志。
		player.fnProbe = [](const char* line)
		{
			auto& c = Core::Instance();
			if (c.fnLogInfo) c.fnLogInfo("%s", line);
		};

		localRoot = rootPath;
		dataLayout = selectedLayout ? *selectedLayout : DataLayout::Legacy(rootPath);
		motionsRoot = dataLayout.Motions();
            // A fresh empty library is a valid first-run state. Create it
            // before the initial scan; permission and I/O errors remain visible.
            std::error_code libraryError;
            std::filesystem::create_directories(MotionPathFromUtf8(motionsRoot), libraryError);
            if (libraryError) LogWarn("[GakuMotion][Paths] Cannot create library: " + libraryError.message());
		configRoot = dataLayout.configs;
		logsRoot = dataLayout.logs;
		diagnosticsRoot = dataLayout.managed ? logsRoot + "/diagnostics" : configRoot;
		transactionRoot = dataLayout.managed ? dataLayout.temp + "/transactions" : "";
		LoadStageLoadSettings();
		LoadStudioSettings();
		playlistPath = dataLayout.Playlist();
		initialPositionPath = configRoot + "/initial_position.json";
		LoadInitialPosition();
		LoadFacialWeights();
		LoadMusicVolume();
		globalOverridePath = configRoot + "/bone_overrides.json";

		gui.motionsRoot = motionsRoot;

		// 1. 控制文件：首次运行生成模板，随后立即读取一次
		config.rootDir = configRoot;
		config.path = configRoot + "/control.json";
		const bool createdConfig = config.EnsureFileExists();
		config.Poll(0.0f, /*forceReload=*/true);
		if (!config.lastError.empty())
		{
			LogWarn("[GakuMotion][Config] control file parse failed: " + config.lastError);
		}

		// 2. 全局骨骼覆盖文件：首次运行生成「模板但不启用」的空表
		const bool createdOverrides = EnsureGlobalOverrideTemplate();

		// 3. 曲目库
		if (std::filesystem::exists(MotionPathFromUtf8(playlistPath))) playlist.Load(playlistPath, playlistNotice);
		player.loop = playlist.mode == PlaylistMode::SingleLoop;
		RefreshMotionLibrary();

		// 4. 初始曲目：优先取控制文件声明，其次取列表第一个
		if (!config.selectedMotion.empty())
		{
			const int idx = FindMotionIndex(motions, config.selectedMotion);
			if (idx >= 0)
			{
				SelectMotionByIndex(idx, false);
				applied.selectedMotion = config.selectedMotion;
			}
			else
			{
				LogWarn("[GakuMotion][Config] selected_motion not found: " + config.selectedMotion);
			}
		}
		if (currentMotionIndex < 0 && !motions.empty())
		{
			SelectMotionByIndex(0, false);
		}

		// 5. 配置与覆盖首次应用
		ApplyDeclarativeConfig();
		LoadCameraSettings();
		LoadSoleSettings();
		LoadNativeAudioSettings();
		LoadClockSettings();
		LoadOverrides(true);

		// Loading the persisted selection is not a request to take over the
		// first actor created by the game (home/work/official Live).
		player.state = PlaybackState::Stopped;
		config.ClearCommands();

		initialized = true;
		LogInfo(detail::FormatMsg(
			"[GakuMotion] Core initialized. motions=%zu, current=%s, control=%s, globalOverrides=%s",
			motions.size(),
			(currentMotionIndex >= 0 && currentMotionIndex < static_cast<int>(motions.size()))
				? motions[currentMotionIndex].name.c_str()
				: "<none>",
			config.path.c_str(),
			globalOverridePath.c_str()));
		if (createdConfig) LogInfo("[GakuMotion][Config] generated control-file template (hot reload ~0.5s)");
		if (createdOverrides) LogInfo(
			"[GakuMotion][Overrides] generated global bone-override template (enabled=false, inactive by default)");
	}

	inline void Core::OnActorChanged(void* newActor)
	{
		++injectionSession;
		if (!newActor) camera.Release(false);
		currentActor = newActor;
		player.OnActorChanged(newActor);
		ResetSoleObservation();
	}

	inline void Core::AbandonScene()
	{
		captureResumeReason = "scene_changed";
		++injectionSession;
		captureSuspended = false;
		// The old scene may already be unloading. Drop references without
		// calling RestoreMute/Rebind on its destroyed Unity objects.
		camera.Release(false);
		player.AbandonScene();
		StopAndReleaseFormationTracks(true);
		actorCustomizations.clear();
		formationAnchorActor = nullptr;
		formationAnchorCaptured = false;
		formationAnchorPos = {};
		formationAnchorRot = Quaternion::Identity();
		formationLastFrame = 0;
		formationCameraFrame = 0;
		formationFrameWrittenActors.clear();
		formationCameraTicked = false;
		ensembleProbeMissStreak = 0;
		player.additionalPerformanceDuration = 0;
		player.controller.ClearReferenceAnchor();
		player.controller.SetPlacementYawDegrees(0);
		player.controller.SetInitialPositionOffset(savedSlot0InitialOffset);
		formation = StageFormation{};
		formation.slotCount = 1;
		formation.slots.resize(1);
		formation.slots[0].slotIndex = 0;
		formation.slots[0].offset = savedSlot0InitialOffset;
		stageManager.AbandonScene();
		ResetSoleObservation();
		sceneActors.clear();
		sceneActorRegistry.Clear();
		currentActor = nullptr;
		currentActorBoundBones = currentActorTotalBones = 0;
		SyncPlayerFacialAndOverridesToSelectedSlot();
		NotifyMotionPlaying(false);
	}

	inline bool Core::DiscardPreviousActorBindings()
	{
		// Stop restores animation but keeps rest snapshots for switching songs.
		// When the actor set changes, those snapshots are no longer safe, even
		// if playback was already stopped before the actor was destroyed.
		if (player.controller.bindings.empty()) return false;
		++injectionSession;
		// Actor replacement in the SAME scene must retain stage ownership.
		// The stage manager restores/rescans independently before this call.
		captureSuspended = false;
		camera.Release(false);
		player.AbandonScene();
		ResetSoleObservation();
		sceneActors.clear();
		sceneActorRegistry.Clear();
		currentActor = nullptr;
		currentActorBoundBones = currentActorTotalBones = 0;
		NotifyMotionPlaying(false);
		return true;
	}

	inline void Core::TryLoadAuxGroundTruth(const std::string& motionDirectory)
	{
		std::error_code ec;
		const std::string auxPath = motionDirectory + "/aux_replay.json";
		if (!std::filesystem::exists(MotionPathFromUtf8(auxPath), ec) || ec) return;
		player.controller.LoadAuxGroundTruth(auxPath);
	}

	inline void Core::NotifyMotionPlaying(bool playing) const
	{
		if (fnNotifyMotionPlaying) fnNotifyMotionPlaying(playing);
	}

	inline void Core::LogInfo(const std::string& message) const
	{
		if (fnLogInfo) fnLogInfo("%s", message.c_str());
	}

	inline void Core::LogWarn(const std::string& message) const
	{
		if (fnLogWarn) fnLogWarn("%s", message.c_str());
	}

	inline void Core::LogDebug(const std::string& message) const
	{
		GM_DEBUG("%s", message.c_str());
	}

	inline std::string Core::ResolvePath(const std::string& candidate, const std::string& fallback) const
	{
		return dataLayout.ResolveOverride(candidate, fallback);
	}

	inline bool Core::EnsureGlobalOverrideTemplate()
	{
		std::error_code ec;
		if (std::filesystem::exists(MotionPathFromUtf8(globalOverridePath), ec)) return false;

		std::filesystem::create_directories(MotionPathFromUtf8(configRoot), ec);
		std::ofstream out(MotionPathFromUtf8(globalOverridePath), std::ios::binary);
		if (!out.is_open()) return false;
		out <<
			"// GakuMotion 全局骨骼旋转覆盖表（热加载）\n"
			"// space=local: R = Q_actor * (R_vmd * R_rest) * Q_offset（默认，骨骼局部空间）\n"
			"// space=world: R = Q_offset * Q_actor * (R_vmd * R_rest)（世界空间左乘）\n"
			"// euler_deg 顺序为 [pitch(X), yaw(Y), roll(Z)]，单位为度。\n"
			"{\n"
			"  \"enabled\": false,\n"
			"  \"overrides\": [\n"
			"    { \"bone\": \"LeftUpperArm\", \"humanoid_id\": 13, \"enabled\": true,\n"
			"      \"space\": \"local\", \"euler_deg\": [-6.0, 0.0, 4.0] },\n"
			"    { \"bone\": \"RightUpperArm\", \"humanoid_id\": 14, \"enabled\": true,\n"
			"      \"space\": \"local\", \"euler_deg\": [-6.0, 0.0, -4.0] }\n"
			"  ]\n"
			"}\n";
		out.close();
		return true;
	}
} // namespace GakuMotion
