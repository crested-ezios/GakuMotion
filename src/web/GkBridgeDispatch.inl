namespace GakuMotion::webpanel
{
	inline CmdResult Bridge::Dispatch(const std::string& method, const jsonlite::Value& params)
	{
		if (method == "bridge.hello")
			return CmdResult::Ok("{\"protocol\":1,\"sessionId\":" + JNum(static_cast<double>(sessionId_)) + "}");
		if (method == "bridge.ping")
			return CmdResult::Ok("{\"pong\":true}");
		if (method == "motion.hello")
			return CmdResult::Ok(BuildHello());
		if (method == "state.get")
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			EnsureFormationSyncedLocked();
			return CmdResult::Ok(BuildSnapshotLocked());
		}

		// Transport ---------------------------------------------------------
		if (method == "player.play") return TransportSimple(GuiAction::Play, true);
		if (method == "player.pause") return TransportSimple(GuiAction::Pause, false);
		if (method == "player.resume") return TransportSimple(GuiAction::Resume, true);
		if (method == "player.stop") return TransportSimple(GuiAction::Stop, false);
		if (method == "player.reload")
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			if (core_.loadedMotionKey.empty()) return CmdResult::Err("not_ready", "No motion is loaded");
			core_.QueueReloadMotion();
			ForcePlayerEvent();
			return CmdResult::Ok();
		}
		if (method == "player.cycle")
		{
			int64_t delta = 0;
			if (!IntParam(params, "delta", delta) || delta == 0 || delta < -100 || delta > 100)
				return CmdResult::Err("invalid_params", "delta must be a non-zero integer in [-100, 100]");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			if (core_.motions.empty()) return CmdResult::Err("not_ready", "Motion library is empty");
			core_.QueueCycleMotion(static_cast<int>(delta));
			ForcePlayerEvent();
			return CmdResult::Ok();
		}
		if (method == "player.seek") return SeekCommand(params, GuiAction::Seek);
		if (method == "player.scrubBegin") return SeekCommand(params, GuiAction::ScrubBegin);
		if (method == "player.scrubUpdate") return SeekCommand(params, GuiAction::ScrubUpdate);
		if (method == "player.scrubEnd") return SeekCommand(params, GuiAction::ScrubEnd);
		if (method == "player.seekFrame")
		{
			int64_t frame = 0;
			if (!IntParam(params, "frame", frame) || frame < 0)
				return CmdResult::Err("invalid_params", "frame must be a 0-based integer");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			if (!core_.player.clip.total_frames) return CmdResult::Err("not_ready", "No motion is loaded");
			if (frame >= static_cast<int64_t>(core_.player.clip.total_frames))
				return CmdResult::Err("invalid_params", "frame exceeds totalFrames - 1");
			core_.QueueSeekFrame(frame);
			ForcePlayerEvent();
			return CmdResult::Ok();
		}
		if (method == "player.stepFrame")
		{
			int64_t delta = 0;
			if (!IntParam(params, "delta", delta) || (delta != -1 && delta != 1))
				return CmdResult::Err("invalid_params", "delta must be -1 or 1");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			if (!core_.player.clip.total_frames) return CmdResult::Err("not_ready", "No motion is loaded");
			core_.QueueStepFrame(static_cast<int>(delta));
			ForcePlayerEvent();
			return CmdResult::Ok();
		}
		if (method == "player.setSpeed")
		{
			double speed = 0;
			if (!NumberParam(params, "speed", speed) || speed < 0.1 || speed > 3.0)
				return CmdResult::Err("invalid_params", "speed must be within 0.1..3.0");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			core_.QueueGui({GuiAction::Speed, static_cast<float>(speed)});
			ForcePlayerEvent();
			return CmdResult::Ok();
		}
		if (method == "player.setVolume")
		{
			double volume = 0;
			if (!NumberParam(params, "volume", volume) || volume < 0.0 || volume > 1.0)
				return CmdResult::Err("invalid_params", "volume must be within 0..1");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			core_.QueueGui({GuiAction::MusicVolume, static_cast<float>(volume)});
			ForcePlayerEvent();
			return CmdResult::Ok();
		}
		if (method == "player.saveVolume")
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			core_.QueueGui({GuiAction::SaveMusicVolume});
			return CmdResult::Ok();
		}
		if (method == "playlist.setMode")
		{
			int64_t mode = 1;
			if (!IntParam(params, "mode", mode) || mode < 0 || mode > 3) mode = 1; // protocol §6: invalid -> 1
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			core_.QueueGui({GuiAction::Mode, static_cast<float>(mode)});
			return CmdResult::Ok();
		}

		// Library -----------------------------------------------------------
		if (method == "library.refresh")
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (core_.libraryScan.valid()) return CmdResult::Ok("{\"started\":false}");
			if (hooks_.invalidateTrackCover) hooks_.invalidateTrackCover("");
			core_.QueueGui({GuiAction::Refresh});
			return CmdResult::Ok("{\"started\":true}");
		}
		if (method == "library.select")
		{
			std::string key;
			bool autoplay = false;
			int64_t revision = -1;
			if (!StringParam(params, "key", key) || key.empty() ||
				!IntParam(params, "revision", revision))
				return CmdResult::Err("invalid_params", "key and revision are required");
			if (!BoolParam(params, "autoplay", autoplay))
				return CmdResult::Err("invalid_params", "autoplay must be a boolean");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (static_cast<uint64_t>(revision) != core_.libraryRevision)
				return CmdResult::Err("stale_revision", "Motion library changed; refresh before selecting");
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			const int index = FindPlaylistIndex(core_.motions, key);
			if (index < 0) return CmdResult::Err("invalid_params", "Motion key not found: " + key);
			core_.QueueSelectMotion(index, autoplay);
			ForcePlayerEvent();
			return CmdResult::Ok("{\"key\":" + JString(key) + "}");
		}
		if (method == "library.delete")
		{
			std::string key;
			int64_t revision = -1;
			if (!StringParam(params, "key", key) || key.empty() || !IntParam(params, "revision", revision))
				return CmdResult::Err("invalid_params", "key and revision are required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (static_cast<uint64_t>(revision) != core_.libraryRevision)
				return CmdResult::Err("stale_revision", "Motion library changed; reopen the delete confirmation");
			if (core_.importBusy || core_.cameraImportBusy || core_.libraryScan.valid())
				return CmdResult::Err("busy", "Import or refresh is in progress");
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			if (FindPlaylistIndex(core_.motions, key) < 0)
				return CmdResult::Err("invalid_params", "Motion key not found: " + key);
			GuiCommand command{GuiAction::DeleteFiles, 0, true, key};
			command.libraryRevision = core_.libraryRevision;
			if (hooks_.invalidateTrackCover) hooks_.invalidateTrackCover(key);
			core_.QueueGui(std::move(command));
			return CmdResult::Ok();
		}
		if (method == "library.openFolder")
		{
			if (!hooks_.openLibraryFolder)
				return CmdResult::Err("not_implemented", "Host folder opener is unavailable");
			std::string notice;
			if (!hooks_.openLibraryFolder(notice))
				return CmdResult::Err("failed", notice.empty() ? "Cannot open the motions folder" : notice);
			return CmdResult::Ok();
		}
		if (method == "library.import") return LibraryImport(params);
		if (method == "library.probeFiles") return LibraryProbeFiles(params);
		if (method == "library.startEdit") return LibraryStartEdit(params);
		if (method == "library.probeAudio") return LibraryProbeAudio(params);
		if (method == "library.getCover") return LibraryGetCover(params);
		if (method == "library.setCover") return LibrarySetCover(params);
		if (method == "library.readImagePreview") return LibraryReadImagePreview(params);
		if (method == "screenshot.capabilities") return ScreenshotCapabilitiesMethod();
		if (method == "screenshot.capture") return ScreenshotCaptureMethod();

		// Clock & Audio Effects ---------------------------------------------
		if (method == "clock.setRunMode") return ClockSetRunMode(params);
		if (method == "clock.configure") return ClockConfigure(params);
		if (method == "audioEffect.set") return AudioEffectSet(params);

		// Actors ------------------------------------------------------------
		if (method == "actors.selectTarget")
		{
			int64_t token = 0, epoch = -1;
			if (!IntParam(params, "token", token) || token <= 0 || !IntParam(params, "epoch", epoch) || epoch < 0)
				return CmdResult::Err("invalid_params", "token and epoch are required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (static_cast<uint64_t>(epoch) != core_.sceneActorRegistry.Epoch())
				return CmdResult::Err("stale_token", "Actor roster changed; refresh before selecting");
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			if (!core_.sceneActorRegistry.Resolve(static_cast<uint64_t>(token), static_cast<uint64_t>(epoch)))
				return CmdResult::Err("stale_token", "Actor token is no longer present");
			GuiCommand command{GuiAction::SelectTarget};
			command.actorToken = static_cast<uint64_t>(token);
			command.actorEpoch = static_cast<uint64_t>(epoch);
			core_.QueueGui(std::move(command));
			return CmdResult::Ok();
		}
		if (method == "actors.getPortrait") return ActorsGetPortrait(params);

		// Formation ---------------------------------------------------------
		if (method == "formation.selectSlot") return FormationSelectSlot(params);
		if (method == "formation.swapSlots") return FormationSwapSlots(params);
		if (method == "formation.setSlotOffset") return FormationSetSlotOffset(params);
		if (method == "formation.resetSlotOffset") return FormationResetSlotOffset(params);
		if (method == "formation.saveSlotOffset") return FormationSaveSlotOffset(params);
		if (method == "formation.setSlotSource") return FormationSetSlotSource(params);

		// Facial / position ---------------------------------------------------
		if (method == "facial.setWeights")
		{
			double overall = 1, brow = 1, eye = 1, mouth = 1;
			if (!NumberParam(params, "overall", overall) || !NumberParam(params, "brow", brow) ||
				!NumberParam(params, "eye", eye) || !NumberParam(params, "mouth", mouth))
				return CmdResult::Err("invalid_params", "overall/brow/eye/mouth numbers are required");
			for (const double value : {overall, brow, eye, mouth})
				if (value < 0.0 || value > 2.0)
					return CmdResult::Err("invalid_params", "Facial weights must be within 0..2");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			EnsureFormationSyncedLocked();
			const FacialWeights fw{
				static_cast<float>(overall), static_cast<float>(brow),
				static_cast<float>(eye), static_cast<float>(mouth)};
			if (!core_.SetSlotFacialWeights(core_.formation.selectedSlotIndex, fw))
			{
				core_.player.controller.facialWeights = fw;
			}
			forceDomainRefresh_.store(true, std::memory_order_release);
			return CmdResult::Ok();
		}
		if (method == "facial.save")
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			core_.QueueGui({GuiAction::SaveFacialWeights});
			return CmdResult::Ok();
		}
		if (method == "position.set")
		{
			double x = 0, y = 0, z = 0;
			if (!NumberParam(params, "x", x) || !NumberParam(params, "y", y) || !NumberParam(params, "z", z))
				return CmdResult::Err("invalid_params", "x/y/z numbers are required");
			for (const double value : {x, y, z})
				if (value < -10.0 || value > 10.0)
					return CmdResult::Err("invalid_params", "Position offset must be within -10..10 meters");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			core_.QueueInitialPosition({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
			return CmdResult::Ok();
		}
		if (method == "position.save")
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			core_.QueueGui({GuiAction::SaveInitialPosition});
			return CmdResult::Ok();
		}

		// Camera / stage ------------------------------------------------------
		if (method == "camera.set")
		{
			const jsonlite::Value* settings = Param(params, "settings");
			if (!settings || !settings->IsObject())
				return CmdResult::Err("invalid_params", "settings object is required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			CameraSettings parsed = core_.camera.settings;
			std::string error;
			if (!ParseCameraSettings(*settings, parsed, error))
				return CmdResult::Err("invalid_params", error);
			if (!parsed.IsValid())
				return CmdResult::Err("invalid_params", "Camera settings failed validation");
			core_.QueueCameraSettings(parsed);
			forceDomainRefresh_.store(true, std::memory_order_release);
			return CmdResult::Ok();
		}
		if (method == "camera.save")
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			core_.QueueGui({GuiAction::SaveCameraSettings});
			return CmdResult::Ok();
		}
		if (method == "stage.set") return StageSet(params);
		if (method == "stage.scan")
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			core_.QueueScanStageCandidates();
			return CmdResult::Ok("{\"nodes\":" + StageNodesJsonLocked() + "}");
		}
		if (method == "stage.toggleNode") return StageToggleNode(params);
		if (method == "stage.setLoadReplacement") return StageSetLoadReplacement(params);

		// Settings / shell / appearance ---------------------------------------
		if (method == "settings.setPhysics")
		{
			int64_t backend = -1;
			if (!IntParam(params, "backend", backend) || backend < 0 || backend > 2)
				return CmdResult::Err("invalid_params", "backend must be 0, 1 or 2");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			core_.QueuePhysicsBackend(static_cast<int>(backend));
			return CmdResult::Ok();
		}
		if (method == "shell.setTextInputActive")
		{
			bool active = false;
			if (!BoolParam(params, "active", active))
				return CmdResult::Err("invalid_params", "active must be a boolean");
			core_.guiTextInputActive.store(active, std::memory_order_relaxed);
			return CmdResult::Ok();
		}
		if (method == "appearance.get")
			return CmdResult::Ok(BuildAppearanceJson());

		if (method == "dialog.pickFile") return DialogPickFile(params);
		if (method == "dialog.poll") return DialogPoll(params);
		if (method == "drop.setTarget") return DropSetTarget(params);
		if (method == "drop.poll") return DropPoll();
		if (method == "drop.peek") return DropPeekMethod();

		if (method == "bone.tree") return BoneTree();
		if (method == "bone.get") return BoneGet(params);
		if (method == "bone.set") return BoneSet(params);
		if (method == "bone.reset") return BoneReset(params);
		if (method == "bone.resetAll") return BoneResetAll();
		if (method == "bone.setMaster") return BoneSetMaster(params);
		if (method == "bone.toggle") return BoneToggle();
		if (method == "bone.reload") return BoneReload();
		if (method == "bone.saveToMotion") return BoneSaveToMotion();
		if (method == "bone.saveToGlobal") return BoneSaveToGlobal();

		if (method == "gaze.get") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); return CmdResult::Ok(GazeJsonLocked()); }
		if (method == "gaze.set") return GazeSet(params);
		if (method == "gaze.setMode") return GazeSetMode(params);
		if (method == "gaze.setDesktop") return GazeSetDesktop(params);
		if (method == "gaze.setWeights") return GazeSetWeights(params);
		if (method == "gaze.cycle") return GazeCycle();

		if (method == "sole.get" || method == "sole.read") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); return CmdResult::Ok(SoleJsonLocked()); }
		if (method == "sole.set" || method == "sole.apply") return SoleSet(params);
		if (method == "sole.save") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); core_.QueueGui({GuiAction::SaveSoleSettings}); return CmdResult::Ok(); }
		if (method == "sole.load") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); core_.QueueGui({GuiAction::LoadSoleSettings}); return CmdResult::Ok(); }

		if (method == "solePitch.get") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); return CmdResult::Ok(SolePitchJsonLocked()); }
		if (method == "solePitch.set") return SolePitchSet(params);

		if (method == "studio.get") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); return CmdResult::Ok(StudioJsonLocked()); }
		if (method == "studio.set") return StudioSet(params);
		if (method == "studio.setLighting") return StudioSetLighting(params);
		if (method == "studio.setVideo") return StudioSetVideo(params);
		if (method == "studio.reset") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); core_.QueueStudioReset(); return CmdResult::Ok(); }

		if (method == "experiments.get") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); return CmdResult::Ok(ExperimentsJsonLocked()); }
		if (method == "experiments.set") return ExperimentsSet(params);
		if (method == "experiments.setMiku") return ExperimentsSetMiku(params);
		if (method == "experiments.setWaistSupport") return ExperimentsSetWaistSupport(params);
		if (method == "experiments.setBeforeQuartz") return ExperimentsSetBeforeQuartz(params);

		if (method == "telemetry.get") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); return CmdResult::Ok(TelemetryJsonLocked()); }

		return CmdResult::Err("unknown_method", "Unknown method: " + method);
	}
} // namespace GakuMotion::webpanel
