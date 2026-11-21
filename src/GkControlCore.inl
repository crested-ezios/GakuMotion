// Included inside Core. All state access is guarded by stateMutex; mutations
// below are called only by the existing admitted Unity actor command drain.
		void SyncControlSession()
		{
			if (controlObservedInjectionSession != injectionSession)
			{
				controlObservedInjectionSession = injectionSession;
				++controlSession;
			}
		}

		void InvalidateControlSession()
		{
			std::lock_guard<std::recursive_mutex> lock(stateMutex);
			++controlSession;
		}

		void SetControlActive(bool active)
		{
			std::lock_guard<std::recursive_mutex> lock(stateMutex);
			controlActive = active;
			++controlSession;
			if (!active)
			{
				controlRequests.CancelPending();
				std::lock_guard<std::mutex> queueLock(guiCommandMutex);
				std::erase_if(guiCommands, [](const GuiCommand& command) { return command.action == GuiAction::Control; });
			}
		}

		bool ControlReady() const
		{
			return controlActive && initialized && currentActor && !captureSuspended && InjectionAllowed();
		}

		control_v0::Error ReadControlState(uint32_t size, control_v0::State* output)
		{
			using namespace control_v0;
			if (!output) return Error::InvalidArgument;
			if (size != sizeof(State)) return Error::SizeMismatch;
			std::lock_guard<std::recursive_mutex> lock(stateMutex);
			SyncControlSession();
			State state;
			state.available = ControlReady();
			state.protectedContext = captureSuspended || !InjectionAllowed();
			state.captureSuspended = captureSuspended;
			state.session = controlSession;
			state.generation = loadedMotionGeneration;
			state.libraryRevision = libraryRevision;
			state.playback = static_cast<uint32_t>(player.state);
			state.time = player.currentTime;
			state.duration = player.TimelineDuration();
			state.speed = player.playbackSpeedMultiplier;
			const auto& offset = player.controller.initialPositionOffset;
			state.offset = {offset.x, offset.y, offset.z};
			const auto& settings = camera.settings;
			state.camera = {settings.enabled, static_cast<uint32_t>(settings.mode),
				static_cast<uint32_t>(settings.followBehavior), static_cast<uint32_t>(settings.followTarget),
				{settings.followRate.x, settings.followRate.y, settings.followRate.z}};
			const auto cameraState = camera.ReadSnapshot();
			state.cameraAvailable = cameraState.available;
			state.cameraOwned = cameraState.owned;
			state.cameraRestorePending = camera.captureRestorePending;
			std::snprintf(state.cameraError, sizeof(state.cameraError), "%s",
				cameraState.followTargetError.empty() ? cameraState.error.c_str() : cameraState.followTargetError.c_str());
			{
				std::lock_guard<std::mutex> stageLock(stageManager.stageMutex);
				state.scene = static_cast<uint32_t>(stageManager.settings.selectedPresetIndex);
				state.sceneRestorePending = !stageManager.suspensionRestored;
			}
			state.motionCount = static_cast<uint32_t>(motions.size());
			if (loadedMotionKey.size() >= sizeof(state.loadedMotionKey)) return Error::SizeMismatch;
			std::snprintf(state.loadedMotionKey, sizeof(state.loadedMotionKey), "%s", loadedMotionKey.c_str());
			*output = state;
			return Error::Ok;
		}

		control_v0::Error ReadControlCapabilities(uint32_t size, control_v0::Capabilities* output)
		{
			using namespace control_v0;
			if (!output) return Error::InvalidArgument;
			if (size != sizeof(Capabilities)) return Error::SizeMismatch;
			std::lock_guard<std::recursive_mutex> lock(stateMutex);
			Capabilities caps;
			caps.supportedMask = AllCommandsMask;
			if (ControlReady())
			{
				caps.availableMask = AllCommandsMask;
				if (formation.active) caps.availableMask &= ~(Mask(CommandKind::Load) | Mask(CommandKind::Offset));
				if (!player.clip.total_frames)
					caps.availableMask &= ~(Mask(CommandKind::Play) | Mask(CommandKind::Pause) |
						Mask(CommandKind::Seek) | Mask(CommandKind::Speed));
				if (!camera.ReadSnapshot().available) caps.availableMask &= ~Mask(CommandKind::Camera);
				if (player.state != PlaybackState::Playing && player.state != PlaybackState::Paused)
					caps.availableMask &= ~Mask(CommandKind::Pause);
				if (motions.empty()) caps.availableMask &= ~Mask(CommandKind::Load);
				scene_nodes::v1::State sceneNodes;
				stageManager.ReadSceneNodesState(sizeof(sceneNodes), &sceneNodes);
				if (sceneNodes.owned || sceneNodes.restorePending) caps.availableMask &= ~Mask(CommandKind::Scene);
			}
			*output = caps;
			return Error::Ok;
		}

		control_v0::Error ReadControlMotion(uint64_t revision, uint32_t index, uint32_t size, control_v0::Motion* output)
		{
			using namespace control_v0;
			if (!output) return Error::InvalidArgument;
			if (size != sizeof(Motion)) return Error::SizeMismatch;
			std::lock_guard<std::recursive_mutex> lock(stateMutex);
			if (!controlActive) return Error::Unavailable;
			if (revision != libraryRevision) return Error::StaleMotion;
			if (index >= motions.size()) return Error::NotFound;
			Motion motion;
			if (motions[index].name.size() >= sizeof(motion.key)) return Error::SizeMismatch;
			std::snprintf(motion.key, sizeof(motion.key), "%s", motions[index].name.c_str());
			*output = motion;
			return Error::Ok;
		}

		control_v0::Error CheckControl(const control_v0::Command& command)
		{
			using namespace control_v0;
			SyncControlSession();
			if (!controlActive) return Error::Unavailable;
			if (formation.active && (command.kind == CommandKind::Load || command.kind == CommandKind::Offset)) return Error::Unsupported;
			if (command.session != controlSession) return Error::StaleSession;
			if (command.motionGeneration != loadedMotionGeneration) return Error::StaleMotion;
			if (captureSuspended || !InjectionAllowed()) return Error::Protected;
			if (!initialized || !currentActor) return Error::NotReady;
			if (command.kind == CommandKind::Load)
			{
				if (command.libraryRevision != libraryRevision) return Error::StaleMotion;
				if (FindPlaylistIndex(motions, command.motionKey) < 0) return Error::NotFound;
			}
			if ((command.kind == CommandKind::Play || command.kind == CommandKind::Pause ||
				command.kind == CommandKind::Seek || command.kind == CommandKind::Speed) && !player.clip.total_frames)
				return Error::NotReady;
			if (command.kind == CommandKind::Seek && command.value > player.TimelineDuration()) return Error::InvalidArgument;
			if (command.kind == CommandKind::Pause && player.state != PlaybackState::Playing && player.state != PlaybackState::Paused)
				return Error::NotReady;
			if (command.kind == CommandKind::Camera && command.camera.enabled && !camera.ReadSnapshot().available)
				return Error::NotReady;
			if (command.kind == CommandKind::Scene)
			{
				scene_nodes::v1::State sceneNodes;
				stageManager.ReadSceneNodesState(sizeof(sceneNodes), &sceneNodes);
				if (sceneNodes.owned || sceneNodes.restorePending) return Error::Busy;
			}
			return Error::Ok;
		}

		control_v0::Error SubmitControl(const control_v0::Command* command, uint64_t* ticket)
		{
			using namespace control_v0;
			if (!ticket) return Error::InvalidArgument;
			*ticket = 0;
			const auto valid = ValidateCommand(command);
			if (valid != Error::Ok) return valid;
			std::lock_guard<std::recursive_mutex> lock(stateMutex);
			const auto ready = CheckControl(*command);
			if (ready != Error::Ok) return ready;
			const auto allocated = controlRequests.Allocate(*ticket);
			if (allocated != Error::Ok) return allocated;
			try
			{
				GuiCommand queued{GuiAction::Control};
				queued.controlCommand = *command;
				queued.controlTicket = *ticket;
				QueueGui(std::move(queued));
			}
			catch (...)
			{
				controlRequests.Complete(*ticket, Error::Failed);
				controlRequests.Release(*ticket);
				*ticket = 0;
				return Error::Failed;
			}
			return Error::Pending;
		}

		control_v0::Error PollControl(uint64_t ticket, uint32_t size, control_v0::Result* output)
		{
			return controlRequests.Poll(ticket, size, output);
		}
		control_v0::Error ReleaseControl(uint64_t ticket) { return controlRequests.Release(ticket); }

		control_v0::Error ExecuteControl(const control_v0::Command& command)
		{
			using namespace control_v0;
			const auto checked = CheckControl(command);
			if (checked != Error::Ok) return checked;
			const bool starts = command.kind == CommandKind::Load || command.kind == CommandKind::Play ||
				command.kind == CommandKind::Seek || command.kind == CommandKind::Scene;
			if (starts && !PrepareInjection()) return Error::Protected;
			if (starts) stageStoppedByTransport = false;
			switch (command.kind)
			{
			case CommandKind::Load:
				if (!SelectMotionByIndex(FindPlaylistIndex(motions, command.motionKey), false, true))
				{
					++loadedMotionGeneration; // A failed loader may already have cleared the old clip.
					return Error::Failed;
				}
				break;
			case CommandKind::Play:
				if (player.state == PlaybackState::Paused) player.Resume();
				else if (player.state != PlaybackState::Playing) player.Play();
				if (player.state != PlaybackState::Playing) return Error::Failed;
				break;
			case CommandKind::Pause:
				player.Pause();
				break;
			case CommandKind::Stop:
				StopPerformance();
				{
					std::lock_guard<std::mutex> stageLock(stageManager.stageMutex);
					if (camera.captureRestorePending || !stageManager.suspensionRestored) return Error::Failed;
				}
				break;
			case CommandKind::Seek: player.Seek(command.value); break;
			case CommandKind::Speed: player.SetPlaybackSpeed(command.value); break;
			case CommandKind::Offset:
				if (!player.controller.SetInitialPositionOffset({command.offset.x, command.offset.y, command.offset.z}))
					return Error::InvalidArgument;
				break;
			case CommandKind::Camera:
				{
					auto settings = camera.settings;
					settings.enabled = command.camera.enabled != 0;
					settings.mode = static_cast<CameraMode>(command.camera.mode);
					settings.followBehavior = static_cast<FollowBehavior>(command.camera.followBehavior);
					settings.followTarget = static_cast<CameraFollowTarget>(command.camera.followTarget);
					settings.followRate = {command.camera.rateXYZ[0], command.camera.rateXYZ[1], command.camera.rateXYZ[2]};
					if (!ApplyCameraSettings(settings, false)) return Error::Failed;
					if (!settings.enabled && camera.captureRestorePending) return Error::Failed;
				}
				break;
			case CommandKind::Scene: stageManager.ApplyPreset(static_cast<int>(command.scene)); break;
			default: return Error::Unsupported;
			}
			NotifyMotionPlaying(player.state == PlaybackState::Playing);
			return Error::Ok;
		}

		void CompleteControlCommand(const GuiCommand& command)
		{
			control_v0::Error result;
			try { result = ExecuteControl(command.controlCommand); }
			catch (...) { result = control_v0::Error::Failed; }
			const char* message = result == control_v0::Error::Ok &&
				(command.controlCommand.kind == control_v0::CommandKind::Camera ||
				 command.controlCommand.kind == control_v0::CommandKind::Scene)
				? "Settings accepted; native application and rendering follow the normal tick" : nullptr;
			if (result == control_v0::Error::Failed && command.controlCommand.kind == control_v0::CommandKind::Stop)
				message = "Stop dispatched; native camera or scene restoration is unconfirmed";
			controlRequests.Complete(command.controlTicket, result, message);
		}

		// onTick can reject obsolete requests even without an actor. It never
		// executes valid mutations here: the admitted actor dispatch owns that.
		void RejectObsoleteControlCommands()
		{
			std::lock_guard<std::recursive_mutex> lock(stateMutex);
			std::lock_guard<std::mutex> queueLock(guiCommandMutex);
			for (auto it = guiCommands.begin(); it != guiCommands.end();)
			{
				if (it->action != GuiAction::Control) { ++it; continue; }
				const auto checked = CheckControl(it->controlCommand);
				if (checked == control_v0::Error::Ok) { ++it; continue; }
				controlRequests.Complete(it->controlTicket, checked);
				it = guiCommands.erase(it);
			}
		}
