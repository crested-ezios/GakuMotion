namespace GakuMotion
{
	inline void Core::ProcessPendingGuiCommands()
	{
		std::deque<GuiCommand> commands;
		{
			std::lock_guard<std::mutex> lock(guiCommandMutex);
			commands.swap(guiCommands);
		}
		for (const auto& cmd : commands)
		{
			if (cmd.action == GuiAction::Control)
			{
				CompleteControlCommand(cmd);
				continue;
			}
			// This command stores a future Photography-load preference only.
			// It never touches Unity, playback or the current background.
			if (cmd.action == GuiAction::StageLoadReplacement)
			{
				SetStageLoadReplacement(cmd.key);
				continue;
			}
			if (cmd.action == GuiAction::StudioSettings || cmd.action == GuiAction::StudioReset)
			{
				if (cmd.action == GuiAction::StudioSettings) SetStudioSettings(cmd.studioSettings);
				else ResetStudioSettings();
				continue;
			}
			// Never replay controls queued before a scene/capture boundary.
			if (!InjectionAllowed()) continue;
			const bool canStartInjection = cmd.action == GuiAction::Play || cmd.action == GuiAction::Resume ||
				cmd.action == GuiAction::Select || cmd.action == GuiAction::Cycle || cmd.action == GuiAction::Seek ||
				cmd.action == GuiAction::ScrubBegin || cmd.action == GuiAction::StageSettings ||
				cmd.action == GuiAction::StagePreset || cmd.action == GuiAction::ToggleStageCandidate ||
				cmd.action == GuiAction::MountCustomStage || cmd.action == GuiAction::SeekFrame ||
				cmd.action == GuiAction::StepFrame || cmd.action == GuiAction::ReloadMotion;
			const bool positionCommand = (cmd.action >= GuiAction::Seek && cmd.action <= GuiAction::ScrubEnd) ||
				cmd.action == GuiAction::SeekFrame || cmd.action == GuiAction::StepFrame || cmd.action == GuiAction::ReloadMotion;
			if (positionCommand && (cmd.key != player.loadedMotionFile ||
				cmd.loadedGeneration != loadedMotionGeneration || cmd.sceneSession != injectionSession))
				continue;
			if (canStartInjection && !PrepareInjection()) continue;
			if (canStartInjection) stageStoppedByTransport = false;
#ifdef GKMOTION_SCRUB_TRACE
			if (cmd.action >= GuiAction::ScrubBegin && cmd.action <= GuiAction::ScrubEnd)
				player.Probe("[GakuMotion][ProbeS1][CORE] dequeued event=%d value=%.3f",
				             static_cast<int>(cmd.action), cmd.value);
#endif
			switch (cmd.action)
			{
			case GuiAction::Select: SelectMotionByIndex(FindPlaylistIndex(motions, cmd.key), cmd.flag, !cmd.flag);
				break;
			case GuiAction::Cycle: CycleMotion(static_cast<int>(cmd.value), "gui");
				break;
			case GuiAction::Play:
				player.Play();
				for (auto& kv : formationTracks) if (kv.second) { kv.second->SyncClockState(player.currentTime); kv.second->state = player.state; }
				break;
			case GuiAction::Pause:
				player.Pause();
				for (auto& kv : formationTracks) if (kv.second) { kv.second->SyncClockState(player.currentTime); kv.second->state = player.state; }
				break;
			case GuiAction::Resume:
				player.Resume();
				for (auto& kv : formationTracks) if (kv.second) { kv.second->SyncClockState(player.currentTime); kv.second->state = player.state; }
				break;
			case GuiAction::Stop: StopPerformance();
				break;
			case GuiAction::Seek:
				player.Seek(cmd.value);
				for (auto& kv : formationTracks) if (kv.second) { kv.second->SyncClockState(player.currentTime); kv.second->state = player.state; }
				break;
			case GuiAction::SeekFrame:
				player.SeekFrame(cmd.frame);
				for (auto& kv : formationTracks) if (kv.second) { kv.second->SyncClockState(player.currentTime); kv.second->state = player.state; }
				break;
			case GuiAction::StepFrame:
				player.StepFrame(static_cast<int>(cmd.value));
				for (auto& kv : formationTracks) if (kv.second) { kv.second->SyncClockState(player.currentTime); kv.second->state = player.state; }
				break;
			case GuiAction::ReloadMotion: ReloadCurrentMotion();
				break;
			case GuiAction::ScrubBegin:
				player.BeginScrub(cmd.value);
				for (auto& kv : formationTracks) if (kv.second) { kv.second->SyncClockState(player.currentTime); kv.second->state = player.state; kv.second->scrubbing = player.scrubbing; }
				break;
			case GuiAction::ScrubUpdate:
				player.UpdateScrub(cmd.value);
				for (auto& kv : formationTracks) if (kv.second) { kv.second->SyncClockState(player.currentTime); kv.second->state = player.state; kv.second->scrubbing = player.scrubbing; }
				break;
			case GuiAction::ScrubEnd:
				player.EndScrub(cmd.value);
				for (auto& kv : formationTracks) if (kv.second) { kv.second->SyncClockState(player.currentTime); kv.second->state = player.state; kv.second->scrubbing = player.scrubbing; }
				break;
			case GuiAction::Refresh: BeginLibraryRefresh();
				break;
			case GuiAction::DeleteFiles:
				if (cmd.libraryRevision == libraryRevision) DeleteMotionFiles(cmd.key, cmd.flag);
				else playlistNotice = "曲库已变化，请重新选择并确认删除";
				break;
			case GuiAction::Move: MoveMotion(cmd.key, static_cast<int>(cmd.value));
				break;
			case GuiAction::SaveList: SavePlaylist(cmd.key);
				break;
			case GuiAction::LoadList:
				if (playlist.Load(cmd.key.empty() ? playlistPath : cmd.key, playlistNotice))
				{
					player.loop = playlist.mode == PlaylistMode::SingleLoop;
					RefreshMotionLibrary();
					SavePlaylist();
				}
				break;
			case GuiAction::Mode:
				playlist.mode = static_cast<PlaylistMode>(std::clamp(static_cast<int>(cmd.value), 0, 3));
				player.loop = playlist.mode == PlaylistMode::SingleLoop;
				playlist.shuffleRemaining.clear();
				SavePlaylist();
				break;
			case GuiAction::Mute: player.SetMuted(cmd.flag);
				break;
			case GuiAction::Speed:
				player.SetPlaybackSpeed(cmd.value);
				break;
			case GuiAction::MusicVolume:
				if (!player.SetMusicVolume(cmd.value)) musicVolumeNotice = "音乐音量暂不可调，原播放音量未改变";
				else musicVolumeNotice.clear();
				break;
			case GuiAction::SaveMusicVolume: SaveMusicVolume();
				break;
			case GuiAction::Import:
				pendingImportCover = {cmd.importCoverSet, cmd.importCoverClear, cmd.importCoverDataUri};
				StartImport(cmd.import);
				break;
			case GuiAction::CameraSettings:
				ApplyCameraSettings(cmd.cameraSettings);
				break;
			case GuiAction::SaveCameraSettings: SaveCameraSettings();
				break;
			case GuiAction::CameraImport: StartCameraImport(cmd.cameraImport);
				break;
			case GuiAction::ReloadCamera:
				if (!cameraImportBusy) ReloadCamera();
				break;
			case GuiAction::SoleSettings: SetSoleSettings(cmd.soleSettings);
				break;
			case GuiAction::SaveSoleSettings: SaveSoleSettings();
				break;
			case GuiAction::LoadSoleSettings: LoadSoleSettings();
				break;
			case GuiAction::ExperimentalSolePitch:
				if (cmd.experimentalSolePitchSettings.IsValid()) experimentalSolePitchSettings = cmd.
					experimentalSolePitchSettings;
				break;
			case GuiAction::ExperimentalWaistSupport:
				config.experimentalWaistSupport = 1;
				ApplyDeclarativeConfig();
				break;
			case GuiAction::BeforeQuartzExperiment:
				config.beforeQuartzExperiment = (config.secondaryPhysicsBackend == 2);
				ApplyDeclarativeConfig();
				break;
			case GuiAction::MuteNativeSoundEffects:
				guiMuteNativeSoundEffects.store(cmd.flag, std::memory_order_release);
				config.muteNativeSoundEffects = cmd.flag ? 1 : 0;
				applied.muteNativeSoundEffects = config.muteNativeSoundEffects;
				SaveNativeAudioSettings();
				break;
			case GuiAction::SelectTarget:
				if (void* actor = sceneActorRegistry.Resolve(cmd.actorToken, cmd.actorEpoch)) SetTargetActor(actor);
				break;
			case GuiAction::ClearTarget: PickBoundActorAsTarget();
				break;
			case GuiAction::NextTarget: CycleTargetActor();
				break;
			case GuiAction::PickTarget: PickBoundActorAsTarget();
				break;
			case GuiAction::InitialPosition:
				if (formation.active && HasSlot(formation.selectedSlotIndex))
				{
					SetSlotOffset(formation.selectedSlotIndex, cmd.position,
						formation.slots[static_cast<size_t>(formation.selectedSlotIndex)].yawDegrees);
				}
				else
				{
					if (player.controller.SetInitialPositionOffset(cmd.position) && !formation.slots.empty())
					{
						formation.slots[0].offset = cmd.position;
						formation.slots[0].placementDirty = true;
					}
				}
				break;
			case GuiAction::SaveInitialPosition:
				if (HasSlot(formation.selectedSlotIndex)) SaveSlotOffset(formation.selectedSlotIndex);
				else SaveInitialPosition();
				break;
			case GuiAction::SaveFacialWeights: SaveFacialWeights();
				break;
			case GuiAction::PhysicsBackend:
				{
					const int backend = static_cast<int>(cmd.value);
					if (backend < 0 || backend > 2) break;
					config.secondaryPhysicsBackend = backend;
					config.preserveAnimatorForSecondaryPhysics = backend == 2 ? 1 : 0;
					ApplyDeclarativeConfig();
					LogInfo(detail::FormatMsg("[GakuMotion][Physics] GUI selected %s",
					                          backend == 2 ? "native(experimental)" : backend == 1 ? "fallback" : "off"));
					break;
				}
			case GuiAction::StageSettings:
				{
					StageSettings settings = cmd.stageSettings;
					// A stale visual-settings snapshot cannot overwrite the separately
					// saved load-time selection from the dedicated control.
					settings.loadReplacementKey = stageManager.GetLoadReplacementKey();
					stageManager.ApplySettings(settings);
				}
				break;
			case GuiAction::StagePreset:
				stageManager.ApplyPreset(cmd.stagePresetIndex);
				break;
			case GuiAction::ToggleStageCandidate:
				stageManager.ToggleCandidate(cmd.candidateIndex, cmd.flag);
				break;
			case GuiAction::ScanStageCandidates:
				stageManager.RescanCandidates(currentActor);
				break;
			case GuiAction::MountCustomStage:
				stageManager.RequestMountStage(cmd.key);
				break;
			case GuiAction::UnloadCustomStage:
				stageStoppedByTransport = false; // Explicit cleanup may run while transport is stopped.
				stageManager.RequestUnloadStage();
				break;
			case GuiAction::RestoreCameraSkybox:
				stageManager.ForceRestoreCameraClear(nullptr);
				break;
			case GuiAction::FormationSelectSlot:
				SelectSlot(cmd.candidateIndex);
				break;
			case GuiAction::FormationSwapSlots:
				SwapSlots(cmd.stagePresetIndex, cmd.candidateIndex);
				break;
			case GuiAction::FormationSetSlotOffset:
				SetSlotOffset(cmd.candidateIndex, cmd.position, cmd.value);
				break;
			case GuiAction::FormationResetSlotOffset:
				ResetSlotOffset(cmd.candidateIndex);
				break;
			case GuiAction::FormationSaveSlotOffset:
				SaveSlotOffset(cmd.candidateIndex);
				break;
			case GuiAction::FormationSetSlotSource:
				SetSlotSource(cmd.candidateIndex, cmd.stagePresetIndex);
				break;
			}
		}
		PollImport();
		PollCameraImport();
		PollLibraryRefresh();
		NotifyMotionPlaying(player.state == PlaybackState::Playing);
	}
} // namespace GakuMotion
