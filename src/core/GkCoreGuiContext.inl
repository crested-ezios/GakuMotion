namespace GakuMotion
{
#if defined(GKMOTION_ENABLE_IMGUI) || defined(HAS_IMGUI)
	inline void Core::OnRenderGui(bool hostedPanel)
	{
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		gui.Draw(BuildGuiContext(), hostedPanel);
		guiTextInputActive.store(ImGui::GetIO().WantTextInput && !ImGui::GetIO().AppFocusLost, std::memory_order_relaxed);
	}
#endif

	inline GuiContext Core::BuildGuiContext()
	{
		GuiContext ctx;
		ctx.injectionAllowed = InjectionAllowed();
		ctx.injectionNotice = injectionStatus.empty()
			? "仅偶像摄影与更衣室可以使用；其他页面保留设置但不生效。" : injectionStatus;
		ctx.player = &player;
		ctx.cameraSnapshot = camera.ReadSnapshot();
		ctx.cameraImportBusy = cameraImportBusy;
		ctx.cameraNotice = cameraNotice;
		ctx.cameraSource = player.currentMotionDir.empty() ? "" : player.currentMotionDir + "/camera.gkcamera";
		ctx.cameraFrames = static_cast<uint32_t>(camera.track.frames.size());
		ctx.cameraFps = camera.track.fps;
		ctx.fnSetCameraSettings = [](void* self, const CameraSettings& settings)
		{
			static_cast<Core*>(self)->QueueCameraSettings(settings);
		};
		ctx.fnImportCamera = [](void* self, const CameraImportRequest& request)
		{
			GuiCommand cmd{GuiAction::CameraImport};
			cmd.cameraImport = request;
			static_cast<Core*>(self)->QueueGui(std::move(cmd));
		};
		ctx.fnSaveCameraSettings = [](void* self)
		{
			static_cast<Core*>(self)->QueueGui({GuiAction::SaveCameraSettings});
		};
		ctx.fnReloadCamera = [](void* self) { static_cast<Core*>(self)->QueueGui({GuiAction::ReloadCamera}); };
		ctx.lookAt = &lookAt;
		ctx.config = &config;
		ctx.motions = &motions;
		ctx.overrides = &effectiveOverrides;
		ctx.masterOverridesEnabled = &masterOverridesEnabled;
		ctx.selectedMotionIndex = &currentMotionIndex;
		ctx.libraryRevision = libraryRevision;
		ctx.loadedMotionGeneration = loadedMotionGeneration;
		ctx.injectionSession = injectionSession;
		ctx.libraryNotice = libraryNotice;
		ctx.libraryRefreshing = libraryScan.valid();
		ctx.loadedMotionKey = loadedMotionKey;
		ctx.loadedSourceMissing = loadedSourceMissing;
		ctx.loadedSourceChanged = loadedSourceChanged;
		ctx.actorRosterEpoch = sceneActorRegistry.Epoch();
		for (const auto& item : sceneActorRegistry.Entries())
		{
			ctx.sceneActors.push_back({item.token, FormatSceneActorLabel(item, ctx.sceneActors.size() + 1),
				item.actor == player.controller.targetActor, item.character,
				ResolveSceneActorDisplayName(item), ResolveSceneActorShortName(item)});
		}
		auto actorLabel = [&](void* actor)
		{
			for (size_t i = 0; i < sceneActorRegistry.Entries().size(); ++i)
				if (sceneActorRegistry.Entries()[i].actor == actor) return ctx.sceneActors[i].label;
			return std::string("尚未绑定角色");
		};
		ctx.boundActorLabel = actorLabel(player.controller.currentBinding.actor);
		ctx.targetActorLabel = actorLabel(player.controller.targetActor);
		ctx.formation = formation;
		ctx.formation.actorEpoch = sceneActorRegistry.Epoch();
		ctx.fnSelectTargetActor = [](void* self, uint64_t token, uint64_t epoch)
		{
			GuiCommand command{GuiAction::SelectTarget};
			command.actorToken = token; command.actorEpoch = epoch;
			static_cast<Core*>(self)->QueueGui(std::move(command));
		};
		ctx.statusLine = StatusSummary();
		ctx.physicsBackend = guiPhysicsBackend.load();
		ctx.beforeQuartzExperiment = guiBeforeQuartzExperiment.load();
		ctx.experimentalWaistSupport = guiExperimentalWaistSupport.load();
		ctx.muteNativeSoundEffects = guiMuteNativeSoundEffects.load();
		ctx.fnSetMuteNativeSoundEffects = [](void* self, bool enabled)
		{
			static_cast<Core*>(self)->QueueMuteNativeSoundEffects(enabled);
		};
		ctx.playlistMode = static_cast<int>(playlist.mode);
		ctx.importBusy = importBusy;
		guiImportProgressSnapshot = importProgress;
		ctx.importProgress = &guiImportProgressSnapshot;
		ctx.playlistNotice = playlistNotice;
		ctx.initialPositionNotice = initialPositionNotice;
		ctx.facialWeightsNotice = facialWeightsNotice;
		ctx.fnSaveFacialWeights = [](void* self) { static_cast<Core*>(self)->QueueGui({GuiAction::SaveFacialWeights}); };
		ctx.soleSettings = soleSettings;
		ctx.soleSnapshot = soleSnapshot;
		ctx.soleNotice = soleNotice;
		ctx.experimentalSolePitchSettings = experimentalSolePitchSettings;
		ctx.experimentalSolePitchSnapshot = experimentalSolePitchSnapshot;
		ctx.fnSetExperimentalSolePitch = [](void* self, const sole_pitch::Settings& settings)
		{
			static_cast<Core*>(self)->QueueExperimentalSolePitch(settings);
		};
		ctx.soleCalibrationGeneration = soleCalibrationGeneration;
		ctx.fnSetSoleSettings = [](void* self, const SoleMeasurementSettings& settings)
		{
			static_cast<Core*>(self)->QueueSoleSettings(settings);
		};
		ctx.fnSaveSoleSettings = [](void* self)
		{
			static_cast<Core*>(self)->QueueGui({GuiAction::SaveSoleSettings});
		};
		ctx.fnLoadSoleSettings = [](void* self)
		{
			static_cast<Core*>(self)->QueueGui({GuiAction::LoadSoleSettings});
		};
		ctx.fnSetInitialPosition = [](void* self, const Vector3& offset)
		{
			static_cast<Core*>(self)->QueueInitialPosition(offset);
		};
		ctx.fnSaveInitialPosition = [](void* self)
		{
			static_cast<Core*>(self)->QueueGui({GuiAction::SaveInitialPosition});
		};
		ctx.fnPickFile = fnPickFile;
		ctx.fnBeginPickFile = fnBeginPickFile;
		ctx.fnPollPickFile = fnPollPickFile;
		ctx.fnPlaylistAction = [](void* self, int action, const char* key, int value)
		{
			auto& c = *static_cast<Core*>(self);
			GuiAction command;
			switch (static_cast<GuiPlaylistAction>(action))
			{
			case GuiPlaylistAction::Move: command = GuiAction::Move; break;
			case GuiPlaylistAction::SaveList: command = GuiAction::SaveList; break;
			case GuiPlaylistAction::LoadList: command = GuiAction::LoadList; break;
			case GuiPlaylistAction::SetMode: command = GuiAction::Mode; break;
			case GuiPlaylistAction::DeleteFiles: command = GuiAction::DeleteFiles; break;
			default: return;
			}
			std::string path = key ? key : "";
			// File-dialog paths already use UTF-8. Do not convert through ACP.
			GuiCommand queued{command, static_cast<float>(value), command == GuiAction::DeleteFiles && value == 1, path};
			queued.libraryRevision = c.libraryRevision;
			c.QueueGui(std::move(queued));
		};
		ctx.fnImportMotion = [](void* self, const MotionImportRequest& request)
		{
			GuiCommand cmd{GuiAction::Import};
			cmd.import = request;
			static_cast<Core*>(self)->QueueGui(std::move(cmd));
		};
		ctx.fnSetMuted = [](void* self, bool value)
		{
			static_cast<Core*>(self)->QueueGui({GuiAction::Mute, 0, value});
		};
		ctx.fnSetSpeed = [](void* self, float value)
		{
			static_cast<Core*>(self)->QueueGui({GuiAction::Speed, value});
		};
		ctx.fnSetMusicVolume = [](void* self, float value)
		{
			static_cast<Core*>(self)->QueueGui({GuiAction::MusicVolume, value});
		};
		ctx.fnSaveMusicVolume = [](void* self) { static_cast<Core*>(self)->QueueGui({GuiAction::SaveMusicVolume}); };
		ctx.musicVolumeSupport = player.fnAudioVolumeSupport ? player.fnAudioVolumeSupport() : 0;
		ctx.musicVolumeNotice = musicVolumeNotice;
		ctx.fnSeekFrame = [](void* self, int64_t frame) { static_cast<Core*>(self)->QueueSeekFrame(frame); };
		ctx.fnStepFrame = [](void* self, int delta) { static_cast<Core*>(self)->QueueStepFrame(delta); };
		ctx.fnReloadMotion = [](void* self) { static_cast<Core*>(self)->QueueReloadMotion(); };
		ctx.knownActorCount = KnownActorCount();
		ctx.fnClearTargetActor = [](void* self)
		{
			static_cast<Core*>(self)->QueueGui({GuiAction::ClearTarget});
		};
		ctx.fnNextTargetActor = [](void* self)
		{
			static_cast<Core*>(self)->QueueGui({GuiAction::NextTarget});
		};
		ctx.fnPickFocusedActor = [](void* self)
		{
			static_cast<Core*>(self)->QueueGui({GuiAction::PickTarget});
		};
		ctx.fnSelectMotion = [](void* self, int index, bool autoplay)
		{
			static_cast<Core*>(self)->QueueSelectMotion(index, autoplay);
			return true;
		};
		ctx.fnCycleMotion = [](void* self, int delta)
		{
			static_cast<Core*>(self)->QueueCycleMotion(delta);
			return true;
		};
		ctx.fnPlay = [](void* self) { static_cast<Core*>(self)->QueuePlay(); };
		ctx.fnPause = [](void* self) { static_cast<Core*>(self)->QueuePause(); };
		ctx.fnResume = [](void* self) { static_cast<Core*>(self)->QueueResume(); };
		ctx.fnStop = [](void* self) { static_cast<Core*>(self)->QueueStop(); };
		ctx.fnSeek = [](void* self, float seconds) { static_cast<Core*>(self)->QueueSeek(seconds); };
		ctx.fnSetPhysicsBackend = [](void* self, int backend)
		{
			static_cast<Core*>(self)->QueuePhysicsBackend(backend);
		};
		ctx.fnScrubBegin = [](void* self, float seconds) { static_cast<Core*>(self)->QueueScrubBegin(seconds); };
		ctx.fnScrubUpdate = [](void* self, float seconds) { static_cast<Core*>(self)->QueueScrubUpdate(seconds); };
		ctx.fnScrubEnd = [](void* self, float seconds) { static_cast<Core*>(self)->QueueScrubEnd(seconds); };
#ifdef GKMOTION_SCRUB_TRACE
		ctx.fnProbe = [](void* self, const char* line)
		{
			static_cast<Core*>(self)->LogInfo(line ? line : "");
		};
#endif
		ctx.fnReloadOverrides = [](void* self)
		{
			static_cast<Core*>(self)->LoadOverrides(true);
		};
		ctx.fnToggleOverrides = [](void* self)
		{
			return static_cast<Core*>(self)->ToggleOverrides();
		};
		ctx.fnCycleGaze = [](void* self)
		{
			return static_cast<Core*>(self)->CycleGazeMode();
		};
		ctx.fnRefreshMotions = [](void* self)
		{
			static_cast<Core*>(self)->QueueGui({GuiAction::Refresh});
			return true;
		};
		ctx.fnSetBoneOverride = [](void* self, int hid, const char* name, float pitch, float yaw, float roll,
		                           bool world, bool enabled, bool symmetrical)
		{
			static_cast<Core*>(self)->SetLiveBoneOverride(hid, name, pitch, yaw, roll, world, enabled, symmetrical);
		};
		ctx.fnResetBoneOverride = [](void* self, int hid, const char* name, bool symmetrical)
		{
			static_cast<Core*>(self)->ResetLiveBoneOverride(hid, name, symmetrical);
		};
		ctx.fnResetAllOverrides = [](void* self)
		{
			static_cast<Core*>(self)->ResetAllLiveOverrides();
		};
		ctx.fnSaveOverridesToMotion = [](void* self)
		{
			return static_cast<Core*>(self)->SaveLiveOverridesToMotion();
		};
		ctx.fnSaveOverridesToGlobal = [](void* self)
		{
			return static_cast<Core*>(self)->SaveLiveOverridesToGlobal();
		};
		ctx.fnGetBoneOverride = [](void* self, int hid, const char* name, float* outPitch, float* outYaw,
		                           float* outRoll, bool* outWorld, bool* outEnabled)
		{
			return static_cast<Core*>(self)->GetLiveBoneOverride(hid, name, outPitch, outYaw, outRoll, outWorld,
			                                                     outEnabled);
		};
		ctx.stageSettings = stageManager.settings;
		ctx.stageCandidates = stageManager.candidates;
		ctx.stageSceneName = stageManager.currentSceneName;
		ctx.stageMountedKey = stageManager.mountedStageKey;
		ctx.stageStatusNotice = stageManager.statusNotice;
		ctx.stageLoadNotice = stageLoadNotice;
		ctx.studioSettings = studioSettings;
		ctx.studioRevision = studioRevision;
		ctx.studioLightingNotice = studioLightingNotice;
		ctx.studioVideoNotice = studioVideoNotice;
		ctx.fnSetStudioSettings = [](void* self, const studio::Settings& settings)
		{
			static_cast<Core*>(self)->QueueStudioSettings(settings);
		};
		ctx.fnResetStudioSettings = [](void* self) { static_cast<Core*>(self)->QueueStudioReset(); };
		ctx.fnSetStageLoadReplacement = [](void* self, const char* key)
		{
			static_cast<Core*>(self)->QueueStageLoadReplacement(key ? key : "");
		};
		ctx.fnSetStageSettings = [](void* self, const StageSettings& settings)
		{
			static_cast<Core*>(self)->QueueStageSettings(settings);
		};
		ctx.fnSetStagePreset = [](void* self, int presetIndex)
		{
			static_cast<Core*>(self)->QueueStagePreset(presetIndex);
		};
		ctx.fnToggleStageCandidate = [](void* self, int index, bool active)
		{
			static_cast<Core*>(self)->QueueToggleStageCandidate(index, active);
		};
		ctx.fnScanStageCandidates = [](void* self)
		{
			static_cast<Core*>(self)->QueueScanStageCandidates();
		};
		ctx.fnMountCustomStage = [](void* self, const char* key)
		{
			static_cast<Core*>(self)->QueueMountCustomStage(key ? key : "");
		};
		ctx.fnUnloadCustomStage = [](void* self)
		{
			static_cast<Core*>(self)->QueueUnloadCustomStage();
		};
		ctx.fnRestoreCameraSkybox = [](void* self)
		{
			static_cast<Core*>(self)->QueueRestoreCameraSkybox();
		};
		ctx.self = this;
		return ctx;
	}
} // namespace GakuMotion
