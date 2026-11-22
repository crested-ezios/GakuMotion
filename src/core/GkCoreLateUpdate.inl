namespace GakuMotion
{
	inline void Core::OnLateUpdate(void* actor, void* animator, void* playableDirector, float deltaTime, bool advanceClock, float cameraDeltaTime, uint64_t physicalFrame)
	{
		currentActor = actor;

		// 在 Unity 主线程安全执行由 GUI 线程发出的所有控制指令
		if (advanceClock)
		{
			ProcessPendingGuiCommands();
		}

		// 1. 配置轮询（低频，内部自带时间累加；无变化时几乎零开销）
		if (advanceClock && config.Poll(deltaTime))
		{
			if (!config.lastChangeLog.empty())
			{
				LogInfo("[GakuMotion][Config] " + config.lastChangeLog);
			}
			if (!config.lastError.empty())
			{
				LogWarn("[GakuMotion][Config] " + config.lastError);
			}
			ApplyDeclarativeConfig();
			ConsumeConfigCommands();
		}

		if (!InjectionAllowed())
		{
			config.ClearCommands();
			return;
		}
		if (player.state != PlaybackState::Stopped && !PrepareInjection()) return;

		// 2. 骨骼覆盖集合始终指向最新合成结果，并刷新主锚点世界姿态
		RefreshFormationAnchorPose();
		if (!formation.active || formationTracks.empty()) player.controller.SetBoneOverrides(&effectiveOverrides);

		// 3. 姿态灌入（仅新物理帧首个 Actor 推进全局时钟，同帧其余 Actor 仅应用姿态）
		// Marker proves that this callback wrote each Foot's original motion.
		// No historical delta/cache is retained, so pause/seek cannot integrate it.
		auto* actorPlayer = PlayerForActor(actor);
		auto& actorController = actorPlayer ? actorPlayer->controller : player.controller;
		actorController.footPoseWriteStamp.actor = nullptr;
		actorController.footPoseWriteStamp.mask = 0;
		actorController.footPoseWriteStamp.transforms = {};
		actorController.eyePoseWriteStamp.actor = nullptr;
		actorController.eyePoseWriteStamp.mask = 0;
		actorController.eyePoseWriteStamp.transforms = {};

		if (formation.active && !formationTracks.empty())
		{
			const bool isNewFrame = advanceClock && (!physicalFrame || formationLastFrame != physicalFrame);
			if (isNewFrame)
			{
				formationLastFrame = physicalFrame;
				formationCameraFrame = physicalFrame;
				formationFrameWrittenActors.clear();
				formationCameraTicked = false;
				player.AdvancePerformance(deltaTime);
				if (player.finished)
				{
					player.finished = false;
					const int next = playlist.Next(motions, currentMotionIndex);
					if (next >= 0) SelectMotionByIndex(next, true);
					else StopPerformance();
					return;
				}
			}

			const int slotIdx = SlotIndexForActor(actor);
			if (slotIdx >= 0 && actorPlayer && actorPlayer != &player)
			{
				const auto& slot = formation.slots[static_cast<size_t>(slotIdx)];
				actorController.CopyRuntimeConfigurationFrom(player.controller);
				if (formationAnchorCaptured)
				{
					actorController.SetReferenceAnchor(formationAnchorPos, formationAnchorRot);
				}
				else
				{
					actorController.ClearReferenceAnchor();
				}
				actorController.SetInitialPositionOffset(slot.offset);
				actorController.SetPlacementYawDegrees(slot.yawDegrees);
				if (const auto custIt = actorCustomizations.find(slot.actorToken); custIt != actorCustomizations.end())
				{
					actorController.facialWeights = custIt->second.facialWeights;
					actorController.SetBoneOverrides(&custIt->second.effectiveOverrides);
				}
				actorPlayer->SampleAtPerformanceTime(player.currentTime, player.state, player.scrubbing,
				                                     actor, animator, playableDirector);
				if (player.state != PlaybackState::Stopped)
				{
					formationFrameWrittenActors.insert(actor);
				}
				if (slotIdx == formation.selectedSlotIndex || (formation.selectedSlotIndex < 0 && slotIdx == 0))
				{
					currentActorBoundBones = actorController.currentBinding.boundBoneCount;
					currentActorTotalBones = actorController.currentBinding.boneTransforms.size();
				}
			}
			if (isNewFrame && player.state != PlaybackState::Stopped)
			{
				++player.evaluatedFrameSerial;
				if (player.state == PlaybackState::Playing &&
					player.clockConfig.runMode == performance::PlaybackRunMode::ForRecording &&
					player.recordingLeaseId == 0 &&
					player.clockConfig.source != performance::ClockSource::External &&
					player.gateRecordingByRenderComplete)
				{
					player.waitingForRenderComplete = true;
				}
			}
			ObserveSoles(actor);
			WriteRequestedSolePitchSnapshot(actor);

			if (player.state == PlaybackState::Stopped)
			{
				camera.Release();
			}
			else
			{
				std::vector<void*> allActiveActors;
				allActiveActors.reserve(formation.slots.size());
				for (const auto& s : formation.slots)
				{
					if (!s.occupied || s.actorToken == 0) continue;
					void* resolved = sceneActorRegistry.Resolve(s.actorToken, formation.actorEpoch);
					if (resolved &&
						(!player.controller.fnCanDriveActor || player.controller.fnCanDriveActor(resolved)) &&
						(!fnEnsembleActorEligible || fnEnsembleActorEligible(resolved)))
					{
						allActiveActors.push_back(resolved);
					}
				}
				if (!formationCameraTicked && !allActiveActors.empty() &&
					formationFrameWrittenActors.size() >= allActiveActors.size())
				{
					formationCameraTicked = true;
					std::vector<void*> camTargets;
					if (camera.settings.followSlotIndex == -1)
					{
						camTargets = allActiveActors;
					}
					else
					{
						void* specific = nullptr;
						if (HasSlot(camera.settings.followSlotIndex))
						{
							const auto& targetSlot = formation.slots[static_cast<size_t>(camera.settings.followSlotIndex)];
							if (targetSlot.occupied && targetSlot.actorToken != 0)
							{
								specific = sceneActorRegistry.Resolve(targetSlot.actorToken, formation.actorEpoch);
							}
						}
						if (specific && (!camera.fnAlive || camera.fnAlive(specific)))
						{
							camTargets.push_back(specific);
						}
						else
						{
							for (void* fallbackActor : allActiveActors)
							{
								if (!camera.fnAlive || camera.fnAlive(fallbackActor))
								{
									camTargets.push_back(fallbackActor);
									break;
								}
							}
						}
					}
					camera.LoadTrack(player.currentMotionDir, player.clip);
					player.cameraSourceDuration = camera.track.frames.empty() ? 0.0f :
						static_cast<float>(camera.track.frames.size()) / camera.track.fps;
					void* anchorPtr = formationAnchorActor ? formationAnchorActor
						                                   : (!allActiveActors.empty() ? allActiveActors.front() : actor);
					camera.TickMulti(
						player.state == PlaybackState::Playing || player.state == PlaybackState::Paused ||
						player.state == PlaybackState::Preview,
						player.CameraSourceTime(),
						cameraDeltaTime >= 0.0f ? cameraDeltaTime : deltaTime,
						camTargets, anchorPtr, physicalFrame);
				}
			}
			return;
		}

		player.controller.ClearReferenceAnchor();
		player.Tick(deltaTime, actor, animator, playableDirector, advanceClock);
		ObserveSoles(actor); // Read-only Unity queries, on the same main-thread frame.
		ApplyExperimentalSolePitch(actor); // Opt-in demo, separate from #078 evidence.
		WriteRequestedSolePitchSnapshot(actor);
		if (advanceClock && player.finished)
		{
			player.finished = false;
			const int next = playlist.Next(motions, currentMotionIndex);
			if (next >= 0) SelectMotionByIndex(next, true);
			else NotifyMotionPlaying(false);
		}

		// A stable target prevents multiple actors from fighting over one camera.
		void* cameraActor = player.controller.targetActor;
		if (cameraActor && camera.fnAlive && !camera.fnAlive(cameraActor)) cameraActor = nullptr;
		if (!cameraActor)
		{
			for (void* candidate : sceneActors)
			{
				if (!camera.fnAlive || camera.fnAlive(candidate))
				{
					cameraActor = candidate;
					break;
				}
			}
		}
		if (!cameraActor) cameraActor = actor;
		if (player.state == PlaybackState::Stopped) camera.Release();
		else if (actor == cameraActor)
		{
			camera.LoadTrack(player.currentMotionDir, player.clip);
			player.cameraSourceDuration = camera.track.frames.empty() ? 0.0f :
				static_cast<float>(camera.track.frames.size()) / camera.track.fps;
			camera.Tick(player.state == PlaybackState::Playing || player.state == PlaybackState::Paused || player.state == PlaybackState::Preview,
			            player.CameraSourceTime(), cameraDeltaTime >= 0.0f ? cameraDeltaTime : deltaTime,
			            actor, physicalFrame);
		}

		// 4. 角色状态快照（供面板/日志）
		currentActorBoundBones = player.controller.currentBinding.boundBoneCount;
		currentActorTotalBones = player.controller.currentBinding.boneTransforms.size();
	}

	inline void Core::OnLookAtUpdate(void* lookAtController, void* cameraTransform)
	{
		if (!InjectionAllowed() || player.state == PlaybackState::Stopped || !PrepareInjection()) return;
		lookAt.Update(lookAtController, cameraTransform);
	}
} // namespace GakuMotion
