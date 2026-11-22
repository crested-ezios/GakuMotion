namespace GakuMotion
{
	inline void Core::QueueSelectMotion(int index, bool autoplay)
	{
		if (index >= 0 && index < static_cast<int>(motions.size()))
			QueueGui({GuiAction::Select, 0, autoplay, motions[index].name});
	}

	inline Core::GuiCommand Core::PositionCommand(GuiAction action, float value)
	{
		GuiCommand command{action, value, false, player.loadedMotionFile};
		command.loadedGeneration = loadedMotionGeneration;
		command.sceneSession = injectionSession;
		return command;
	}

	inline void Core::QueueSeekFrame(int64_t frame)
	{
		auto command = PositionCommand(GuiAction::SeekFrame);
		command.frame = frame;
		QueueGui(std::move(command));
	}

	inline void Core::QueueScrubBegin(float seconds)
	{
		QueueGui(PositionCommand(GuiAction::ScrubBegin, seconds));
	}

	inline void Core::QueueScrubUpdate(float seconds)
	{
		QueueGui(PositionCommand(GuiAction::ScrubUpdate, seconds));
	}

	inline void Core::QueueInitialPosition(const Vector3& offset)
	{
		GuiCommand cmd{GuiAction::InitialPosition};
		cmd.position = offset;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueFormationSelectSlot(int slotIndex)
	{
		GuiCommand cmd{GuiAction::FormationSelectSlot};
		cmd.candidateIndex = slotIndex;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueFormationSwapSlots(int fromSlot, int toSlot)
	{
		GuiCommand cmd{GuiAction::FormationSwapSlots};
		cmd.stagePresetIndex = fromSlot;
		cmd.candidateIndex = toSlot;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueFormationSetSlotOffset(int slotIndex, const Vector3& offset, float yawDegrees)
	{
		GuiCommand cmd{GuiAction::FormationSetSlotOffset};
		cmd.candidateIndex = slotIndex;
		cmd.position = offset;
		cmd.value = yawDegrees;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueFormationResetSlotOffset(int slotIndex)
	{
		GuiCommand cmd{GuiAction::FormationResetSlotOffset};
		cmd.candidateIndex = slotIndex;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueFormationSaveSlotOffset(int slotIndex)
	{
		GuiCommand cmd{GuiAction::FormationSaveSlotOffset};
		cmd.candidateIndex = slotIndex;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueFormationSetSlotSource(int slotIndex, int sourceSlotIndex)
	{
		GuiCommand cmd{GuiAction::FormationSetSlotSource};
		cmd.candidateIndex = slotIndex;
		cmd.stagePresetIndex = sourceSlotIndex;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueCameraSettings(const GakuMotion::CameraSettings& settings)
	{
		GuiCommand cmd{GuiAction::CameraSettings};
		cmd.cameraSettings = settings;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueSoleSettings(const SoleMeasurementSettings& settings)
	{
		GuiCommand cmd{GuiAction::SoleSettings};
		cmd.soleSettings = settings;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueExperimentalSolePitch(const sole_pitch::Settings& settings)
	{
		GuiCommand cmd{GuiAction::ExperimentalSolePitch};
		cmd.experimentalSolePitchSettings = settings;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueuePhysicsBackend(int backend)
	{
		if (backend >= 0 && backend <= 2) QueueGui({GuiAction::PhysicsBackend, static_cast<float>(backend)});
	}

	inline void Core::QueueExperimentalWaistSupport(bool enabled)
	{
		QueueGui({GuiAction::ExperimentalWaistSupport, 0, enabled});
	}

	inline void Core::QueueBeforeQuartzExperiment(bool enabled)
	{
		QueueGui({GuiAction::BeforeQuartzExperiment, 0, enabled});
	}

	inline void Core::QueueMuteNativeSoundEffects(bool enabled)
	{
		QueueGui({GuiAction::MuteNativeSoundEffects, 0, enabled});
	}

	inline void Core::QueueStageSettings(const StageSettings& settings)
	{
		GuiCommand cmd{GuiAction::StageSettings};
		cmd.stageSettings = settings;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueStageLoadReplacement(const std::string& key)
	{
		GuiCommand cmd{GuiAction::StageLoadReplacement};
		cmd.key = key;
		QueueGui(std::move(cmd));
	}

	inline bool Core::QueueStudioSettings(const studio::Settings& settings)
	{
		std::string error;
		if (!settings.Validate(error)) return false;
		GuiCommand command{GuiAction::StudioSettings};
		command.studioSettings = settings;
		QueueGui(std::move(command));
		return true;
	}

	inline void Core::QueueStagePreset(int presetIndex)
	{
		GuiCommand cmd{GuiAction::StagePreset};
		cmd.stagePresetIndex = presetIndex;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueToggleStageCandidate(int candidateIndex, bool active)
	{
		GuiCommand cmd{GuiAction::ToggleStageCandidate};
		cmd.candidateIndex = candidateIndex;
		cmd.flag = active;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueScanStageCandidates()
	{
		QueueGui({GuiAction::ScanStageCandidates});
	}

	inline void Core::QueueMountCustomStage(const std::string& stageKey)
	{
		GuiCommand cmd{GuiAction::MountCustomStage};
		cmd.key = stageKey;
		QueueGui(std::move(cmd));
	}

	inline void Core::QueueUnloadCustomStage()
	{
		QueueGui({GuiAction::UnloadCustomStage});
	}

	inline void Core::QueueRestoreCameraSkybox()
	{
		QueueGui({GuiAction::RestoreCameraSkybox});
	}

	inline void Core::ProcessPendingStageLoadCommands()
	{
		std::deque<GuiCommand> commands;
		{
			std::lock_guard<std::mutex> lock(guiCommandMutex);
			for (auto it = guiCommands.begin(); it != guiCommands.end();)
			{
				if (it->action != GuiAction::StageLoadReplacement && it->action != GuiAction::StudioSettings &&
					it->action != GuiAction::StudioReset) { ++it; continue; }
				commands.push_back(std::move(*it));
				it = guiCommands.erase(it);
			}
		}
		for (const auto& command : commands)
		{
			if (command.action == GuiAction::StudioSettings) SetStudioSettings(command.studioSettings);
			else if (command.action == GuiAction::StudioReset) ResetStudioSettings();
			else SetStageLoadReplacement(command.key);
		}
	}

	inline bool Core::HasPendingTargetSelection()
	{
		std::lock_guard<std::mutex> lock(guiCommandMutex);
		return std::any_of(guiCommands.begin(), guiCommands.end(), [](const auto& command)
			{ return command.action == GuiAction::SelectTarget; });
	}

	inline void Core::ProcessPendingLibraryCommands()
	{
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		std::deque<GuiCommand> commands;
		{
			std::lock_guard<std::mutex> queueLock(guiCommandMutex);
			while (!guiCommands.empty())
			{
				const auto action = guiCommands.front().action;
				if (action != GuiAction::Import && action != GuiAction::CameraImport && action != GuiAction::Refresh) break;
				commands.push_back(std::move(guiCommands.front()));
				guiCommands.pop_front();
			}
		}
		for (const auto& command : commands)
		{
			if (command.action == GuiAction::Import)
			{
				pendingImportCover = {command.importCoverSet, command.importCoverClear, command.importCoverDataUri};
				StartImport(command.import);
			}
			else if (command.action == GuiAction::CameraImport) StartCameraImport(command.cameraImport);
			else BeginLibraryRefresh();
		}
		PollImport();
		PollCameraImport();
		PollLibraryRefresh();
	}
} // namespace GakuMotion
