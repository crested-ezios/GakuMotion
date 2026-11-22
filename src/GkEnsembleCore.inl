// Included inside Core. Unified multi-actor StageFormation management,
// anchor capture/updates, slot swapping/offsets/source reuse, and track
// synchronization across single-actor and multi-actor sessions.

		bool HasSlot(int slotIndex) const noexcept
		{
			return slotIndex >= 0 && slotIndex < static_cast<int>(formation.slots.size());
		}

		MotionPlayer* PlayerForActor(void* actor)
		{
			if (!actor) return nullptr;
			if (!formation.active)
			{
				return IsTargetActor(actor) ? &player : nullptr;
			}
			const int idx = SlotIndexForActor(actor);
			return idx >= 0 ? TrackForSlot(idx) : nullptr;
		}

		bool IsPerformanceActor(void* actor) { return PlayerForActor(actor) != nullptr; }

		int SlotIndexForActor(void* actor) const noexcept
		{
			if (!actor) return -1;
			const auto* found = sceneActorRegistry.Find(actor);
			if (!found || found->token == 0) return -1;
			return formation.FindSlotIndexByToken(found->token);
		}

		MotionPlayer* TrackForSlot(int slotIndex) noexcept
		{
			if (!HasSlot(slotIndex)) return nullptr;
			const auto& slot = formation.slots[static_cast<size_t>(slotIndex)];
			if (!slot.occupied || slot.actorToken == 0) return nullptr;
			if (!formation.active)
			{
				return player.controller.targetActor == sceneActorRegistry.Resolve(slot.actorToken, formation.actorEpoch)
					       ? &player
					       : nullptr;
			}
			auto it = formationTracks.find(slot.actorToken);
			return it != formationTracks.end() ? it->second.get() : nullptr;
		}

		void SyncSelectedFacialWeightsFromPlayer() noexcept
		{
			if (!playerFacialWeightsBaselineInit)
			{
				lastSyncedPlayerFacialWeights = player.controller.facialWeights;
				playerFacialWeightsBaselineInit = true;
				return;
			}
			const auto sameWeights = [](const FacialWeights& a, const FacialWeights& b) noexcept
			{
				auto eq = [](float x, float y) noexcept
				{
					return (std::isnan(x) && std::isnan(y)) || x == y;
				};
				return eq(a.overall, b.overall) && eq(a.brow, b.brow) &&
					eq(a.eye, b.eye) && eq(a.mouth, b.mouth);
			};
			if (sameWeights(player.controller.facialWeights, lastSyncedPlayerFacialWeights))
			{
				return;
			}
			lastSyncedPlayerFacialWeights = player.controller.facialWeights;
			if (auto* sel = formation.SelectedSlot(); sel && sel->occupied && sel->actorToken != 0)
			{
				sel->facialWeights = lastSyncedPlayerFacialWeights;
				auto& cust = actorCustomizations[sel->actorToken];
				cust.facialWeights = lastSyncedPlayerFacialWeights;
				cust.facialCustomized = true;
				if (auto it = formationTracks.find(sel->actorToken); it != formationTracks.end() && it->second)
				{
					it->second->controller.facialWeights = lastSyncedPlayerFacialWeights;
				}
			}
		}

		void SyncPlayerFacialAndOverridesToSelectedSlot() noexcept
		{
			if (const auto* sel = formation.SelectedSlot(); sel && sel->occupied && sel->actorToken != 0)
			{
				const auto custIt = actorCustomizations.find(sel->actorToken);
				const FacialWeights targetWeights = (custIt != actorCustomizations.end())
					                                    ? custIt->second.facialWeights
					                                    : sel->facialWeights;
				player.controller.facialWeights = targetWeights;
				lastSyncedPlayerFacialWeights = targetWeights;
				playerFacialWeightsBaselineInit = true;

				if (custIt != actorCustomizations.end())
				{
					motionOverrides = custIt->second.actorMotionOverrides;
					BoneOverrideSet::BuildEffective(globalOverrides, motionOverrides,
					                                masterOverridesEnabled, effectiveOverrides);
				}
				else
				{
					motionOverrides.Clear();
					BoneOverrideSet::BuildEffective(globalOverrides, motionOverrides,
					                                masterOverridesEnabled, effectiveOverrides);
				}
				return;
			}
			player.controller.facialWeights = defaultFacialWeights;
			lastSyncedPlayerFacialWeights = defaultFacialWeights;
			playerFacialWeightsBaselineInit = true;
			motionOverrides.Clear();
			BoneOverrideSet::BuildEffective(globalOverrides, motionOverrides,
			                                masterOverridesEnabled, effectiveOverrides);
		}

		FacialWeights ResolveSavedFacialWeightsForCharacter(const std::string& character) const
		{
			if (!character.empty())
			{
				if (const auto it = facialWeightsByCharacter.find(character); it != facialWeightsByCharacter.end())
				{
					return it->second;
				}
			}
			return defaultFacialWeights;
		}

		void UpdateFormationDuration() noexcept
		{
			float maxDur = 0.0f;
			for (const auto& kv : formationTracks)
			{
				if (kv.second)
				{
					maxDur = (std::max)(maxDur, kv.second->TimelineDuration());
				}
			}
			player.additionalPerformanceDuration = maxDur;
		}

		void CaptureFormationAnchorFromScene(const std::vector<SceneActorEntry>& eligible)
		{
			void* candidate = nullptr;
			if (!formation.slots.empty() && formation.slots[0].occupied && formation.slots[0].actorToken != 0)
			{
				candidate = sceneActorRegistry.Resolve(formation.slots[0].actorToken, formation.actorEpoch);
			}
			if (!candidate && !eligible.empty())
			{
				candidate = eligible.front().actor;
			}
			if (!candidate)
			{
				formationAnchorActor = nullptr;
				formationAnchorCaptured = false;
				formationAnchorPos = {};
				formationAnchorRot = Quaternion::Identity();
				return;
			}
			formationAnchorActor = candidate;
			RefreshFormationAnchorPose();
		}

		void RefreshFormationAnchorPose() noexcept
		{
			if (!formationAnchorActor) return;
			const auto* entry = sceneActorRegistry.Find(formationAnchorActor);
			if (!entry) return;
			if (player.controller.fnCanDriveActor && !player.controller.fnCanDriveActor(formationAnchorActor)) return;
			if (!player.controller.fnGetComponentTransform || !player.controller.fnGetWorldPosition ||
				!player.controller.fnGetWorldRotation)
			{
				return;
			}
			void* tr = player.controller.fnGetComponentTransform(formationAnchorActor);
			if (!tr) return;
			float pos[3]{0.0f, 0.0f, 0.0f};
			float rot[4]{0.0f, 0.0f, 0.0f, 1.0f};
			player.controller.fnGetWorldPosition(tr, pos);
			player.controller.fnGetWorldRotation(tr, rot);
			const Vector3 nextPos(pos[0], pos[1], pos[2]);
			const Quaternion nextRot(rot[0], rot[1], rot[2], rot[3]);
			const float n2 = nextRot.x * nextRot.x + nextRot.y * nextRot.y + nextRot.z * nextRot.z + nextRot.w * nextRot.w;
			if (std::isfinite(nextPos.x) && std::isfinite(nextPos.y) && std::isfinite(nextPos.z) &&
				std::isfinite(n2) && n2 > 1e-8f)
			{
				formationAnchorPos = nextPos;
				formationAnchorRot = nextRot.Normalized();
				formationAnchorCaptured = true;
			}
		}

		void EnsureActorCustomizationInitialized(StageSlotState& slot, const MotionEntry* motion)
		{
			if (!slot.occupied || slot.actorToken == 0)
			{
				slot.hasActorBoneOverrides = false;
				return;
			}
			auto& cust = actorCustomizations[slot.actorToken];
			if (!cust.facialCustomized)
			{
				cust.facialWeights = ResolveSavedFacialWeightsForCharacter(slot.character);
			}
			slot.facialWeights = cust.facialWeights;

			const std::string slotDir = motion
				                            ? motion->SlotDirectory(slot.sourceSlotIndex + 1)
				                            : (!player.currentMotionDir.empty()
					                               ? ResolveMotionSlotDirectory(player.currentMotionDir, slot.sourceSlotIndex + 1)
					                               : std::string());
			if (!cust.overridesLoaded || cust.loadedSlotDir != slotDir)
			{
				cust.actorMotionOverrides.Clear();
				if (!slotDir.empty())
				{
					const std::string overridePath = slotDir + "/bone_overrides.json";
					if (std::filesystem::exists(MotionPathFromUtf8(overridePath)))
					{
						cust.actorMotionOverrides.LoadFromFile(overridePath);
					}
				}
				cust.loadedSlotDir = slotDir;
				cust.overridesLoaded = true;
			}
			BoneOverrideSet::BuildEffective(globalOverrides, cust.actorMotionOverrides,
			                                masterOverridesEnabled, cust.effectiveOverrides);
			slot.hasActorBoneOverrides = !cust.actorMotionOverrides.Empty();
		}

		void RebuildSlotActorOverrides(StageSlotState& slot)
		{
			if (!slot.occupied || slot.actorToken == 0)
			{
				slot.hasActorBoneOverrides = false;
				return;
			}
			auto& cust = actorCustomizations[slot.actorToken];
			BoneOverrideSet::BuildEffective(globalOverrides, cust.actorMotionOverrides,
			                                masterOverridesEnabled, cust.effectiveOverrides);
			slot.hasActorBoneOverrides = !cust.actorMotionOverrides.Empty();
			if (auto it = formationTracks.find(slot.actorToken); it != formationTracks.end() && it->second)
			{
				it->second->controller.SetBoneOverrides(&cust.effectiveOverrides);
			}
		}

		bool ConfigureFormationTrackForSlot(const StageSlotState& slot, const MotionEntry* motion, bool forceReloadClip)
		{
			if (!formation.active || !slot.occupied || slot.actorToken == 0) return false;
			void* actor = sceneActorRegistry.Resolve(slot.actorToken, formation.actorEpoch);
			if (!actor) return false;

			auto& trackPtr = formationTracks[slot.actorToken];
			const bool newlyCreated = !trackPtr;
			if (!trackPtr)
			{
				trackPtr = std::make_unique<MotionPlayer>();
			}
			MotionPlayer* track = trackPtr.get();
			track->controller.CopyRuntimeConfigurationFrom(player.controller);
			track->audioSourceDuration = 0.0f;
			track->cameraSourceDuration = 0.0f;
			track->fnPlayAudio = nullptr;
			track->fnPauseAudio = nullptr;
			track->fnStopAudio = nullptr;
			track->fnResumeAudio = nullptr;
			track->fnSeekAudio = nullptr;
			track->fnSeekPausedAudio = nullptr;
			track->fnGetAudioTime = nullptr;
			track->fnObserveAudio = nullptr;
			track->fnObserveAudioWithTime = nullptr;

			const std::string trackDir = motion ? motion->dir : player.currentMotionDir;
			const int targetSlot1Based = slot.sourceSlotIndex + 1;
			bool loadedSlotClip = false;
			if (!trackDir.empty() &&
				(forceReloadClip || newlyCreated || track->currentMotionDir != trackDir ||
				 track->currentMotionSlot != targetSlot1Based || track->clip.total_frames == 0))
			{
				if (track->LoadMotionSlot(trackDir, targetSlot1Based))
				{
					loadedSlotClip = true;
				}
				else if (targetSlot1Based != 1)
				{
					loadedSlotClip = track->LoadMotionSlot(trackDir, 1);
				}
				track->audioSourceDuration = 0.0f;
				track->cameraSourceDuration = 0.0f;
				track->controller.LoadAuxGroundTruth(track->SlotAuxReplayPath());
			}
			if (motion)
			{
				track->timing = ResolveSlotPerformanceTiming(motion, targetSlot1Based, player.timing);
			}
			else if (!loadedSlotClip && track->currentMotionDir.empty())
			{
				track->timing = player.timing;
			}
			track->controller.SetTargetActor(actor);
			if (formationAnchorCaptured)
			{
				track->controller.SetReferenceAnchor(formationAnchorPos, formationAnchorRot);
			}
			else
			{
				track->controller.ClearReferenceAnchor();
			}
			track->controller.SetInitialPositionOffset(slot.offset);
			track->controller.SetPlacementYawDegrees(slot.yawDegrees);
			if (auto it = actorCustomizations.find(slot.actorToken); it != actorCustomizations.end())
			{
				track->controller.facialWeights = it->second.facialWeights;
				track->controller.SetBoneOverrides(&it->second.effectiveOverrides);
			}
			track->SyncClockState(player.currentTime);
			track->state = player.state;
			return true;
		}

		std::vector<SceneActorEntry> CollectDrivableSceneActors() const
		{
			std::vector<SceneActorEntry> drivable;
			for (const auto& entry : sceneActorRegistry.Entries())
			{
				if (!entry.actor) continue;
				if (player.controller.fnCanDriveActor && !player.controller.fnCanDriveActor(entry.actor)) continue;
				drivable.push_back(entry);
			}
			return drivable;
		}

		std::vector<SceneActorEntry> CollectEnsembleEligibleActors(const std::vector<SceneActorEntry>& drivable) const
		{
			std::vector<SceneActorEntry> eligible;
			for (const auto& entry : drivable)
			{
				if (fnEnsembleActorEligible && !fnEnsembleActorEligible(entry.actor)) continue;
				eligible.push_back(entry);
			}
			return eligible;
		}

		std::vector<SceneActorEntry> CollectEligibleSceneActors() const
		{
			const auto drivable = CollectDrivableSceneActors();
			auto eligible = CollectEnsembleEligibleActors(drivable);
			return !eligible.empty() ? eligible : drivable;
		}

		void StopAndReleaseFormationTracks(bool abandonOnly)
		{
			for (auto& kv : formationTracks)
			{
				if (!kv.second) continue;
				if (abandonOnly) kv.second->AbandonScene();
				else
				{
					kv.second->Stop();
					kv.second->controller.DiscardBindings();
					kv.second->controller.SetTargetActor(nullptr);
				}
			}
			formationTracks.clear();
		}

		void ApplySlotCustomizationToState(StageSlotState& slot, const MotionEntry* motion)
		{
			EnsureActorCustomizationInitialized(slot, motion);
			if (formation.active)
			{
				ConfigureFormationTrackForSlot(slot, motion, false);
			}
		}

		// Called when loading a new track in SelectMotionByIndex, or initializing with 0..N actors.
		void RebuildFormationForTrack(const MotionEntry* motion)
		{
			SyncSelectedFacialWeightsFromPlayer();
			const auto drivable = CollectDrivableSceneActors();
			const auto ensembleEligible = CollectEnsembleEligibleActors(drivable);
			const auto& eligible = !ensembleEligible.empty() ? ensembleEligible : drivable;
			const bool wasActive = formation.active;
			formation.active = ensembleEligible.size() >= 2;
			if (formation.active) ensembleProbeMissStreak = 0;
			else if (drivable.size() < 2) ensembleProbeMissStreak = 0;
			if (!enforceInjectionPolicy)
			{
				formation.available = !eligible.empty();
				formation.verified = !eligible.empty();
			}
			formation.actorEpoch = sceneActorRegistry.Epoch();
			formation.isGroupSong = (motion && motion->isGroup && motion->positionCount >= 2);
			formation.trackPositionCount = formation.isGroupSong ? motion->positionCount : 1;
			formation.slotCount = (std::max)(1, (std::max)(static_cast<int>(eligible.size()), formation.trackPositionCount));

			// Clean up stale secondary tracks from any prior song or solo transition.
			if (!formation.active && wasActive)
			{
				StopAndReleaseFormationTracks(false);
			}
			if (formation.active && !player.controller.bindings.empty())
			{
				player.controller.RestoreMute();
				player.controller.DiscardBindings();
			}

			// Determine actor ordering: preserve existing slot occupants when they still
			// exist in eligible and fit within [0, slotCount), otherwise compact and fill.
			std::vector<SceneActorEntry> orderedActors(static_cast<size_t>(formation.slotCount));
			std::vector<bool> slotHasActor(static_cast<size_t>(formation.slotCount), false);
			std::unordered_set<uint64_t> assignedTokens;

			for (const auto& prevSlot : formation.slots)
			{
				if (!prevSlot.occupied || prevSlot.actorToken == 0) continue;
				for (const auto& item : eligible)
				{
					if (item.token == prevSlot.actorToken)
					{
						if (prevSlot.slotIndex >= 0 && prevSlot.slotIndex < formation.slotCount &&
							!slotHasActor[static_cast<size_t>(prevSlot.slotIndex)])
						{
							orderedActors[static_cast<size_t>(prevSlot.slotIndex)] = item;
							slotHasActor[static_cast<size_t>(prevSlot.slotIndex)] = true;
							assignedTokens.insert(item.token);
						}
						break;
					}
				}
			}
			for (const auto& item : eligible)
			{
				if (assignedTokens.count(item.token)) continue;
				for (int i = 0; i < formation.slotCount; ++i)
				{
					if (!slotHasActor[static_cast<size_t>(i)])
					{
						orderedActors[static_cast<size_t>(i)] = item;
						slotHasActor[static_cast<size_t>(i)] = true;
						assignedTokens.insert(item.token);
						break;
					}
				}
			}

			std::vector<StageSlotState> nextSlots(static_cast<size_t>(formation.slotCount));
			for (int i = 0; i < formation.slotCount; ++i)
			{
				auto& dst = nextSlots[static_cast<size_t>(i)];
				dst.slotIndex = i;
				dst.isOverflow = formation.isGroupSong ? (i >= formation.trackPositionCount) : false;
				dst.sourceSlotIndex = (formation.isGroupSong && i < formation.trackPositionCount) ? i : 0;
				dst.offset = ComputeDefaultSlotOffset(motion, i, savedSlot0InitialOffset, nextSlots);
				dst.yawDegrees = ComputeDefaultSlotYaw(motion, i);
				dst.placementDirty = false;
				if (slotHasActor[static_cast<size_t>(i)])
				{
					const auto& item = orderedActors[static_cast<size_t>(i)];
					dst.occupied = true;
					dst.actorToken = item.token;
					dst.actorInstanceId = item.instanceId;
					dst.character = item.character;
					dst.displayName = ResolveSceneActorDisplayName(item);
					dst.shortName = ResolveSceneActorShortName(item);
				}
			}
			formation.slots = std::move(nextSlots);

			// Drop actorCustomizations/tracks for actors no longer in the eligible roster.
			for (auto it = formationTracks.begin(); it != formationTracks.end();)
			{
				if (!assignedTokens.count(it->first))
				{
					if (it->second) it->second->AbandonScene();
					it = formationTracks.erase(it);
				}
				else
				{
					++it;
				}
			}
			for (auto it = actorCustomizations.begin(); it != actorCustomizations.end();)
			{
				if (!assignedTokens.count(it->first)) it = actorCustomizations.erase(it);
				else ++it;
			}

			CaptureFormationAnchorFromScene(eligible);

			for (auto& kv : actorCustomizations)
			{
				kv.second.overridesLoaded = false;
			}
			for (auto& slot : formation.slots)
			{
				if (!slot.occupied) continue;
				// Reset facialCustomized on track load only if not yet populated?
				// Actually Keep user's explicit unsaved facial tweaks across track switches if actor persisted,
				// OR load saved per-character on first appearance. Since actorCustomizations only created
				// once per actor session, LoadFacialWeights/default populates first time smoothly.
				EnsureActorCustomizationInitialized(slot, motion);
				if (formation.active)
				{
					ConfigureFormationTrackForSlot(slot, motion, /*forceReloadClip=*/true);
				}
			}
			UpdateFormationDuration();

			if (formation.selectedSlotIndex < 0 || formation.selectedSlotIndex >= formation.slotCount)
			{
				formation.selectedSlotIndex = 0;
			}

			// Sync solo controller & selected-slot facade.
			if (!formation.active)
			{
				player.controller.ClearReferenceAnchor();
				if (eligible.empty())
				{
					player.controller.SetInitialPositionOffset(savedSlot0InitialOffset);
					player.controller.SetPlacementYawDegrees(0.0f);
				}
				else
				{
					const int idx = formation.FindSlotIndexByToken(eligible.front().token);
					if (idx >= 0)
					{
						player.controller.SetTargetActor(eligible.front().actor);
						player.controller.SetInitialPositionOffset(formation.slots[static_cast<size_t>(idx)].offset);
						player.controller.SetPlacementYawDegrees(formation.slots[static_cast<size_t>(idx)].yawDegrees);
					}
				}
			}
			else if (void* anchorTarget = formationAnchorActor ? formationAnchorActor : (!eligible.empty() ? eligible.front().actor : nullptr))
			{
				player.controller.SetTargetActor(anchorTarget);
			}

			SyncPlayerFacialAndOverridesToSelectedSlot();
			if (!formation.active)
			{
				player.controller.SetBoneOverrides(&effectiveOverrides);
			}
			formationLastFrame = 0;
			formationFrameWrittenActors.clear();
			formationCameraTicked = false;
			++formation.revision;
		}

		// Called from PublishSceneActors when scene actors arrive, depart, or reuse addresses.
		void SyncFormationWithRoster()
		{
			const auto slotsEqual = [](const StageFormation& lhs, const std::vector<StageSlotState>& prevSlots,
			                           bool prevActive, bool prevAvailable, bool prevVerified, bool prevIsGroupSong,
			                           int prevTrackPositionCount, int prevSlotCount, int prevSelectedSlotIndex,
			                           uint64_t prevActorEpoch) noexcept -> bool
			{
				if (lhs.active != prevActive || lhs.available != prevAvailable || lhs.verified != prevVerified ||
					lhs.isGroupSong != prevIsGroupSong || lhs.trackPositionCount != prevTrackPositionCount ||
					lhs.slotCount != prevSlotCount || lhs.selectedSlotIndex != prevSelectedSlotIndex ||
					lhs.actorEpoch != prevActorEpoch || lhs.slots.size() != prevSlots.size())
				{
					return false;
				}
				for (size_t i = 0; i < lhs.slots.size(); ++i)
				{
					const auto& a = lhs.slots[i];
					const auto& b = prevSlots[i];
					if (a.slotIndex != b.slotIndex || a.actorToken != b.actorToken ||
						a.actorInstanceId != b.actorInstanceId || a.occupied != b.occupied ||
						a.isOverflow != b.isOverflow || a.sourceSlotIndex != b.sourceSlotIndex ||
						a.placementDirty != b.placementDirty || a.hasActorBoneOverrides != b.hasActorBoneOverrides ||
						a.offset.x != b.offset.x || a.offset.y != b.offset.y || a.offset.z != b.offset.z ||
						a.yawDegrees != b.yawDegrees || a.character != b.character ||
						a.displayName != b.displayName || a.shortName != b.shortName ||
						a.facialWeights.overall != b.facialWeights.overall ||
						a.facialWeights.brow != b.facialWeights.brow ||
						a.facialWeights.eye != b.facialWeights.eye ||
						a.facialWeights.mouth != b.facialWeights.mouth)
					{
						return false;
					}
				}
				return true;
			};

			const auto prevSlots = formation.slots;
			const bool prevActive = formation.active;
			const bool prevAvailable = formation.available;
			const bool prevVerified = formation.verified;
			const bool prevIsGroupSong = formation.isGroupSong;
			const int prevTrackPositionCount = formation.trackPositionCount;
			const int prevSlotCount = formation.slotCount;
			const int prevSelectedSlotIndex = formation.selectedSlotIndex;
			const uint64_t prevActorEpoch = formation.actorEpoch;

			SyncSelectedFacialWeightsFromPlayer();
			const MotionEntry* motion = CurrentMotion();
			const bool hasLoadedMotion = !player.currentMotionDir.empty() || motion != nullptr;
			const auto drivable = CollectDrivableSceneActors();
			const auto ensembleEligible = CollectEnsembleEligibleActors(drivable);
			std::unordered_map<uint64_t, SceneActorEntry> drivableByToken;
			for (const auto& item : drivable)
			{
				drivableByToken[item.token] = item;
			}

			const bool wasActive = formation.active;
			bool keepActiveByHysteresis = false;
			if (ensembleEligible.size() >= 2)
			{
				ensembleProbeMissStreak = 0;
			}
			else if (wasActive && drivable.size() >= 2 && ensembleEligible.empty())
			{
				bool allOccupiedDrivable = true;
				int occupiedCount = 0;
				for (const auto& s : formation.slots)
				{
					if (!s.occupied) continue;
					++occupiedCount;
					const auto found = drivableByToken.find(s.actorToken);
					if (found == drivableByToken.end() || found->second.instanceId != s.actorInstanceId)
					{
						allOccupiedDrivable = false;
						break;
					}
				}
				if (allOccupiedDrivable && occupiedCount >= 2 && ensembleProbeMissStreak < 3)
				{
					++ensembleProbeMissStreak;
					keepActiveByHysteresis = true;
				}
			}
			else if (drivable.size() < 2)
			{
				ensembleProbeMissStreak = 0;
			}

			const std::vector<SceneActorEntry>& eligible =
				(!ensembleEligible.empty() && !keepActiveByHysteresis) ? ensembleEligible : drivable;
			std::unordered_map<uint64_t, SceneActorEntry> eligibleByToken;
			for (const auto& item : eligible)
			{
				eligibleByToken[item.token] = item;
			}

			// Step 1: Retire or abandon any actor no longer valid/eligible WITHOUT touching dead actors.
			for (auto& slot : formation.slots)
			{
				if (!slot.occupied) continue;
				const auto found = eligibleByToken.find(slot.actorToken);
				const bool stillValid = (found != eligibleByToken.end()) &&
					(found->second.instanceId == slot.actorInstanceId);
				if (!stillValid)
				{
					if (auto trIt = formationTracks.find(slot.actorToken); trIt != formationTracks.end())
					{
						if (trIt->second) trIt->second->AbandonScene();
						formationTracks.erase(trIt);
					}
					actorCustomizations.erase(slot.actorToken);
					slot.occupied = false;
					slot.actorToken = 0;
					slot.actorInstanceId = 0;
					slot.character.clear();
					slot.displayName.clear();
					slot.shortName.clear();
					slot.hasActorBoneOverrides = false;
					slot.physicsBackend.clear();
				}
			}
			for (auto it = formationTracks.begin(); it != formationTracks.end();)
			{
				const auto found = eligibleByToken.find(it->first);
				bool inSlot = false;
				for (const auto& s : formation.slots)
				{
					if (s.occupied && s.actorToken == it->first) { inSlot = true; break; }
				}
				if (found == eligibleByToken.end() || !inSlot)
				{
					if (it->second) it->second->AbandonScene();
					it = formationTracks.erase(it);
				}
				else
				{
					++it;
				}
			}

			// Solo actors remain bound as long as they are present in the registry and drivable;
			// Photography ensemble gating must never evict a single-actor controller binding.
			if (player.controller.targetActor)
			{
				const auto* reg = sceneActorRegistry.Find(player.controller.targetActor);
				const bool canDrive = !player.controller.fnCanDriveActor ||
					(reg && player.controller.fnCanDriveActor(player.controller.targetActor));
				if (!reg || !canDrive)
				{
					player.controller.DiscardBindings();
					player.controller.SetTargetActor(nullptr);
				}
			}

			formation.active = (ensembleEligible.size() >= 2) || keepActiveByHysteresis;
			if (!enforceInjectionPolicy)
			{
				formation.available = !eligible.empty();
				formation.verified = !eligible.empty();
			}
			formation.actorEpoch = sceneActorRegistry.Epoch();
			formation.isGroupSong = (motion && motion->isGroup && motion->positionCount >= 2);
			formation.trackPositionCount = formation.isGroupSong ? motion->positionCount : 1;

			// Compute target slot count: keep at least max(eligible.size(), trackPositionCount)
			// and keep any already-occupied higher slot if shrink still needs its index.
			int targetSlotCount = (std::max)(1, (std::max)(static_cast<int>(eligible.size()), formation.trackPositionCount));
			for (int i = static_cast<int>(formation.slots.size()) - 1; i >= targetSlotCount; --i)
			{
				if (formation.slots[static_cast<size_t>(i)].occupied)
				{
					targetSlotCount = i + 1;
					break;
				}
			}
			formation.slotCount = targetSlotCount;

			if (static_cast<int>(formation.slots.size()) < formation.slotCount)
			{
				const int oldCount = static_cast<int>(formation.slots.size());
				for (int i = oldCount; i < formation.slotCount; ++i)
				{
					StageSlotState next;
					next.slotIndex = i;
					next.isOverflow = formation.isGroupSong ? (i >= formation.trackPositionCount) : false;
					next.sourceSlotIndex = (formation.isGroupSong && i < formation.trackPositionCount) ? i : 0;
					next.offset = ComputeDefaultSlotOffset(motion, i, savedSlot0InitialOffset, formation.slots);
					next.yawDegrees = ComputeDefaultSlotYaw(motion, i);
					next.placementDirty = false;
					formation.slots.push_back(std::move(next));
				}
			}
			else if (static_cast<int>(formation.slots.size()) > formation.slotCount)
			{
				formation.slots.resize(static_cast<size_t>(formation.slotCount));
			}

			// Re-verify slot properties on existing empty or uncustomized slots if needed.
			std::unordered_set<uint64_t> assignedTokens;
			for (const auto& s : formation.slots)
			{
				if (s.occupied && s.actorToken != 0) assignedTokens.insert(s.actorToken);
			}
			for (const auto& item : eligible)
			{
				if (assignedTokens.count(item.token)) continue;
				for (auto& slot : formation.slots)
				{
					if (!slot.occupied)
					{
						slot.occupied = true;
						slot.actorToken = item.token;
						slot.actorInstanceId = item.instanceId;
						slot.character = item.character;
						slot.displayName = ResolveSceneActorDisplayName(item);
						slot.shortName = ResolveSceneActorShortName(item);
						assignedTokens.insert(item.token);
						break;
					}
				}
			}

			if (!formationAnchorCaptured || !formationAnchorActor || !sceneActorRegistry.Find(formationAnchorActor))
			{
				CaptureFormationAnchorFromScene(eligible);
			}
			else if (formation.active)
			{
				RefreshFormationAnchorPose();
			}

			if (formation.active && !wasActive)
			{
				// Transitioning solo -> multi: hand off single actor cleanly from player.controller
				// to formationTracks without double-detaching AnimatorController.
				if (!player.controller.bindings.empty())
				{
					player.controller.RestoreMute();
					player.controller.DiscardBindings();
				}
			}
			else if (!formation.active && wasActive)
			{
				// Transitioning multi -> solo: cleanly restore surviving actors from formationTracks
				// so the sole survivor binds cleanly onto player.controller.
				StopAndReleaseFormationTracks(false);
				player.additionalPerformanceDuration = 0.0f;
			}

			for (auto& slot : formation.slots)
			{
				if (!slot.occupied) continue;
				EnsureActorCustomizationInitialized(slot, motion);
				if (formation.active && hasLoadedMotion)
				{
					ConfigureFormationTrackForSlot(slot, motion, false);
				}
			}

			if (formation.active)
			{
				UpdateFormationDuration();
				if (!player.controller.targetActor || !sceneActorRegistry.Find(player.controller.targetActor))
				{
					if (void* primary = formationAnchorActor ? formationAnchorActor : (!eligible.empty() ? eligible.front().actor : nullptr))
					{
						player.controller.SetTargetActor(primary);
					}
				}
			}
			else
			{
				player.controller.ClearReferenceAnchor();
				if (!eligible.empty())
				{
					const int idx = formation.FindSlotIndexByToken(eligible.front().token);
					if (idx >= 0)
					{
						const auto& s = formation.slots[static_cast<size_t>(idx)];
						const bool needFilterSwitch = !ensembleEligible.empty() && drivable.size() > ensembleEligible.size() &&
							player.controller.targetActor != eligible.front().actor;
						if (!player.controller.targetActor || !sceneActorRegistry.Find(player.controller.targetActor) ||
							needFilterSwitch)
						{
							if (needFilterSwitch && !player.controller.bindings.empty())
							{
								player.controller.DiscardBindings();
							}
							player.controller.SetTargetActor(eligible.front().actor);
						}
						player.controller.SetInitialPositionOffset(s.offset);
						player.controller.SetPlacementYawDegrees(s.yawDegrees);
					}
				}
				else
				{
					player.controller.SetTargetActor(nullptr);
				}
			}

			if (formation.selectedSlotIndex < 0 || formation.selectedSlotIndex >= formation.slotCount)
			{
				formation.selectedSlotIndex = 0;
			}
			SyncPlayerFacialAndOverridesToSelectedSlot();
			if (!formation.active)
			{
				player.controller.SetBoneOverrides(&effectiveOverrides);
			}

			const bool changed = !slotsEqual(formation, prevSlots, prevActive, prevAvailable, prevVerified,
			                                 prevIsGroupSong, prevTrackPositionCount, prevSlotCount,
			                                 prevSelectedSlotIndex, prevActorEpoch);
			if (changed)
			{
				formationFrameWrittenActors.clear();
				formationCameraTicked = false;
				++formation.revision;
			}
		}

		// ---- Public Formation API (M3) ------------------------------------------
		bool SelectSlot(int slotIndex)
		{
			if (!HasSlot(slotIndex)) return false;
			SyncSelectedFacialWeightsFromPlayer();
			formation.selectedSlotIndex = slotIndex;
			SyncPlayerFacialAndOverridesToSelectedSlot();
			if (!formation.active)
			{
				player.controller.SetBoneOverrides(&effectiveOverrides);
			}
			++formation.revision;
			return true;
		}

		bool SwapSlots(int fromSlot, int toSlot)
		{
			if (!HasSlot(fromSlot) || !HasSlot(toSlot)) return false;
			if (fromSlot == toSlot) return true;
			SyncSelectedFacialWeightsFromPlayer();

			auto& a = formation.slots[static_cast<size_t>(fromSlot)];
			auto& b = formation.slots[static_cast<size_t>(toSlot)];

			// Swap ONLY actor entity state; offset, yawDegrees, sourceSlotIndex,
			// isOverflow, placementDirty stay bound to the slot position.
			std::swap(a.occupied, b.occupied);
			std::swap(a.actorToken, b.actorToken);
			std::swap(a.actorInstanceId, b.actorInstanceId);
			std::swap(a.character, b.character);
			std::swap(a.displayName, b.displayName);
			std::swap(a.shortName, b.shortName);
			std::swap(a.facialWeights, b.facialWeights);
			std::swap(a.hasActorBoneOverrides, b.hasActorBoneOverrides);
			std::swap(a.physicsBackend, b.physicsBackend);

			const MotionEntry* motion = CurrentMotion();
			if (formation.active)
			{
				if (a.occupied) ConfigureFormationTrackForSlot(a, motion, false);
				if (b.occupied) ConfigureFormationTrackForSlot(b, motion, false);
				UpdateFormationDuration();
			}
			else
			{
				// Single-actor swapped into a different slot updates its offset/yaw and clip source if needed.
				for (const auto& s : {a, b})
				{
					if (s.occupied)
					{
						player.controller.SetInitialPositionOffset(s.offset);
						player.controller.SetPlacementYawDegrees(s.yawDegrees);
					}
				}
			}

			SyncPlayerFacialAndOverridesToSelectedSlot();
			if (!formation.active)
			{
				player.controller.SetBoneOverrides(&effectiveOverrides);
			}
			formation.notice.clear();
			++formation.revision;
			return true;
		}

		bool SetSlotOffset(int slotIndex, const Vector3& offset, float yawDegrees)
		{
			if (!HasSlot(slotIndex)) return false;
			if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z) ||
				!std::isfinite(yawDegrees))
			{
				return false;
			}
			auto& slot = formation.slots[static_cast<size_t>(slotIndex)];
			slot.offset = {
				std::clamp(offset.x, -256.0f, 256.0f),
				std::clamp(offset.y, -256.0f, 256.0f),
				std::clamp(offset.z, -256.0f, 256.0f)
			};
			slot.yawDegrees = std::remainder(yawDegrees, 360.0f);
			slot.placementDirty = true;

			if (slotIndex == 0 && !formation.active)
			{
				player.controller.SetInitialPositionOffset(slot.offset);
				player.controller.SetPlacementYawDegrees(slot.yawDegrees);
			}
			else if (slot.occupied && slot.actorToken != 0)
			{
				if (formation.active)
				{
					if (auto it = formationTracks.find(slot.actorToken); it != formationTracks.end() && it->second)
					{
						it->second->controller.SetInitialPositionOffset(slot.offset);
						it->second->controller.SetPlacementYawDegrees(slot.yawDegrees);
					}
				}
				else
				{
					player.controller.SetInitialPositionOffset(slot.offset);
					player.controller.SetPlacementYawDegrees(slot.yawDegrees);
				}
			}
			++formation.revision;
			return true;
		}

		bool ResetSlotOffset(int slotIndex)
		{
			if (!HasSlot(slotIndex)) return false;
			const MotionEntry* motion = CurrentMotion();
			const Vector3 defaultOff = ComputeDefaultSlotOffset(motion, slotIndex, savedSlot0InitialOffset, formation.slots);
			const float defaultYaw = ComputeDefaultSlotYaw(motion, slotIndex);
			if (!SetSlotOffset(slotIndex, defaultOff, defaultYaw)) return false;
			formation.slots[static_cast<size_t>(slotIndex)].placementDirty = false;
			return true;
		}

		bool SaveSlotOffset(int slotIndex)
		{
			if (!HasSlot(slotIndex)) return false;
			auto& slot = formation.slots[static_cast<size_t>(slotIndex)];
			if (slotIndex == 0)
			{
				savedSlot0InitialOffset = slot.offset;
				player.controller.SetInitialPositionOffset(slot.offset);
				const bool ok = SaveInitialPosition();
				if (ok) slot.placementDirty = false;
				return ok;
			}
			if (!formation.isGroupSong || slotIndex >= formation.trackPositionCount)
			{
				initialPositionNotice = "当前位置不支持独立保存（仅团曲 2 号位及以后支持写入曲目）";
				return false;
			}
			const MotionEntry* m = CurrentMotion();
			if (!m || m->dir.empty())
			{
				initialPositionNotice = "保存初始位置失败：当前未加载曲目";
				return false;
			}
			const std::filesystem::path trackDir = MotionPathFromUtf8(m->dir);
			std::error_code ec;
			if (!std::filesystem::exists(trackDir / "info.json", ec))
			{
				initialPositionNotice = "保存初始位置失败：缺少 info.json";
				return false;
			}
			std::string err;
			const int slot1Based = slotIndex + 1;
			if (!UpdateTrackInfoJsonPositionPlacement(trackDir, slot1Based,
				                                      slot.offset.x, slot.offset.y, slot.offset.z,
				                                      slot.yawDegrees, err))
			{
				initialPositionNotice = "保存初始位置失败: " + err;
				return false;
			}
			// Sync in-memory MotionEntry positions so subsequent loads reuse saved placement.
			auto updateEntryPositions = [&](MotionEntry& target)
			{
				for (auto& pos : target.positions)
				{
					if (pos.slot == slot1Based)
					{
						pos.x = slot.offset.x;
						pos.y = slot.offset.y;
						pos.z = slot.offset.z;
						pos.yawDegrees = slot.yawDegrees;
						return;
					}
				}
			};
			if (currentMotionIndex >= 0 && currentMotionIndex < static_cast<int>(motions.size()))
			{
				updateEntryPositions(motions[static_cast<size_t>(currentMotionIndex)]);
			}
			if (!loadedMotionEntry.dir.empty())
			{
				updateEntryPositions(loadedMotionEntry);
			}
			slot.placementDirty = false;
			initialPositionNotice = "已保存当前槽位初始位置至团曲配置";
			++formation.revision;
			return true;
		}

		bool SetSlotSource(int slotIndex, int sourceSlotIndex)
		{
			if (!HasSlot(slotIndex)) return false;
			const int maxSource = (std::max)(1, formation.trackPositionCount);
			if (sourceSlotIndex < 0 || sourceSlotIndex >= maxSource) return false;
			auto& slot = formation.slots[static_cast<size_t>(slotIndex)];
			slot.sourceSlotIndex = sourceSlotIndex;
			const MotionEntry* motion = CurrentMotion();
			if (slot.occupied && slot.actorToken != 0 && formation.active)
			{
				ConfigureFormationTrackForSlot(slot, motion, false);
				UpdateFormationDuration();
			}
			else if (slot.occupied && !formation.active && motion)
			{
				// Single actor in overflow slot on group song: reload slot clip on player if needed.
				const float savedTime = player.currentTime;
				const PlaybackState savedState = player.state;
				if (player.LoadMotionSlot(motion->dir, sourceSlotIndex + 1) ||
					(sourceSlotIndex != 0 && player.LoadMotionSlot(motion->dir, 1)))
				{
					player.timing = ResolveSlotPerformanceTiming(motion, sourceSlotIndex + 1, player.timing);
					player.SyncClockState(savedTime);
					player.state = savedState;
				}
			}
			++formation.revision;
			return true;
		}

		bool SetSlotFacialWeights(int slotIndex, const FacialWeights& weights)
		{
			if (!HasSlot(slotIndex)) return false;
			for (const float v : {weights.overall, weights.brow, weights.eye, weights.mouth})
			{
				if (!std::isfinite(v) || v < 0.0f || v > 2.0f) return false;
			}
			auto& slot = formation.slots[static_cast<size_t>(slotIndex)];
			slot.facialWeights = weights;
			if (slot.occupied && slot.actorToken != 0)
			{
				auto& cust = actorCustomizations[slot.actorToken];
				cust.facialWeights = weights;
				cust.facialCustomized = true;
				if (formation.active)
				{
					if (auto it = formationTracks.find(slot.actorToken); it != formationTracks.end() && it->second)
					{
						it->second->controller.facialWeights = weights;
					}
				}
			}
			if (slotIndex == formation.selectedSlotIndex)
			{
				player.controller.facialWeights = weights;
				lastSyncedPlayerFacialWeights = weights;
				playerFacialWeightsBaselineInit = true;
			}
			++formation.revision;
			return true;
		}
