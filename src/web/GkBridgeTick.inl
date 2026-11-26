namespace GakuMotion::webpanel
{
	namespace detail_tick
	{
		inline bool SameFloat(float a, float b) noexcept
		{
			return a == b || (std::isnan(a) && std::isnan(b));
		}

		inline bool SameVec3(const Vector3& a, const Vector3& b) noexcept
		{
			return SameFloat(a.x, b.x) && SameFloat(a.y, b.y) && SameFloat(a.z, b.z);
		}

		inline bool SameQuat(const Quaternion& a, const Quaternion& b) noexcept
		{
			return SameFloat(a.x, b.x) && SameFloat(a.y, b.y) && SameFloat(a.z, b.z) && SameFloat(a.w, b.w);
		}

		inline bool ExactStageSettingsEqual(const StageSettings& a, const StageSettings& b) noexcept
		{
			return a.hideBackground == b.hideBackground &&
				a.greenScreenActive == b.greenScreenActive &&
				SameFloat(a.greenScreenColor[0], b.greenScreenColor[0]) &&
				SameFloat(a.greenScreenColor[1], b.greenScreenColor[1]) &&
				SameFloat(a.greenScreenColor[2], b.greenScreenColor[2]) &&
				SameFloat(a.greenScreenColor[3], b.greenScreenColor[3]) &&
				SameFloat(a.screenDistance, b.screenDistance) &&
				SameFloat(a.screenScale, b.screenScale) &&
				SameFloat(a.screenHeightOffset, b.screenHeightOffset) &&
				a.overrideCameraClear == b.overrideCameraClear &&
				a.selectedPresetIndex == b.selectedPresetIndex &&
				a.customStageKey == b.customStageKey &&
				a.loadReplacementKey == b.loadReplacementKey &&
				SameFloat(a.stageFloorYOffset, b.stageFloorYOffset) &&
				SameFloat(a.stageYawOffset, b.stageYawOffset) &&
				SameFloat(a.stagePosXOffset, b.stagePosXOffset) &&
				SameFloat(a.stagePosZOffset, b.stagePosZOffset) &&
				a.lightingEffectsEnabled == b.lightingEffectsEnabled &&
				a.customNodeControl == b.customNodeControl;
		}

		inline uint8_t PackStageNodeFlags(const StageCandidate& c) noexcept
		{
			return static_cast<uint8_t>(
				(c.hasChildren ? 1u : 0u) |
				(c.originalActive ? 2u : 0u) |
				(c.currentlyActive ? 4u : 0u) |
				(c.userMuted ? 8u : 0u) |
				(c.isIdentifiedBackground ? 16u : 0u) |
				(c.isColliderLocked ? 32u : 0u) |
				(c.isLightingRoot ? 64u : 0u) |
				(c.isLightingNode ? 128u : 0u));
		}

		inline bool FormationEqual(const StageFormation& a, bool hasLoadedA,
			const StageFormation& b, bool hasLoadedB) noexcept
		{
			if (hasLoadedA != hasLoadedB ||
				a.active != b.active ||
				a.available != b.available ||
				a.verified != b.verified ||
				a.isGroupSong != b.isGroupSong ||
				a.trackPositionCount != b.trackPositionCount ||
				a.slotCount != b.slotCount ||
				a.selectedSlotIndex != b.selectedSlotIndex ||
				a.revision != b.revision ||
				a.actorEpoch != b.actorEpoch ||
				a.notice != b.notice ||
				a.slots.size() != b.slots.size())
			{
				return false;
			}
			for (size_t i = 0; i < a.slots.size(); ++i)
			{
				const auto& sa = a.slots[i];
				const auto& sb = b.slots[i];
				if (sa.slotIndex != sb.slotIndex ||
					sa.occupied != sb.occupied ||
					sa.actorToken != sb.actorToken ||
					sa.actorInstanceId != sb.actorInstanceId ||
					sa.character != sb.character ||
					sa.shortName != sb.shortName ||
					sa.displayName != sb.displayName ||
					sa.isOverflow != sb.isOverflow ||
					sa.sourceSlotIndex != sb.sourceSlotIndex ||
					sa.placementDirty != sb.placementDirty ||
					sa.hasActorBoneOverrides != sb.hasActorBoneOverrides ||
					sa.physicsBackend != sb.physicsBackend ||
					!SameVec3(sa.offset, sb.offset) ||
					!SameFloat(sa.yawDegrees, sb.yawDegrees) ||
					!SameFloat(sa.facialWeights.overall, sb.facialWeights.overall) ||
					!SameFloat(sa.facialWeights.brow, sb.facialWeights.brow) ||
					!SameFloat(sa.facialWeights.eye, sb.facialWeights.eye) ||
					!SameFloat(sa.facialWeights.mouth, sb.facialWeights.mouth))
				{
					return false;
				}
			}
			return true;
		}

		inline bool CameraSnapshotEqual(const CameraController::Snapshot& a,
			const CameraController::Snapshot& b) noexcept
		{
			return a.settings == b.settings &&
				a.available == b.available &&
				a.owned == b.owned &&
				a.hasTrack == b.hasTrack &&
				a.error == b.error &&
				a.followTargetError == b.followTargetError;
		}

		inline bool SoleSettingsEqual(const SoleMeasurementSettings& a,
			const SoleMeasurementSettings& b) noexcept
		{
			if (a.enabled != b.enabled ||
				a.planeCalibrated != b.planeCalibrated ||
				a.shoeCalibrated != b.shoeCalibrated ||
				a.useBootProxy != b.useBootProxy ||
				!SameVec3(a.planeNormal, b.planeNormal) ||
				!SameFloat(a.planeOffsetM, b.planeOffsetM) ||
				!SameFloat(a.displayBandM, b.displayBandM))
			{
				return false;
			}
			for (size_t side = 0; side < 2; ++side)
				for (size_t i = 0; i < 3; ++i)
					if (!SameVec3(a.points[side][i], b.points[side][i]))
						return false;
			return true;
		}

		inline bool SolePointEqual(const SolePointMeasurement& a, const SolePointMeasurement& b) noexcept
		{
			return SameVec3(a.world, b.world) &&
				SameFloat(a.signedDistanceM, b.signedDistanceM) &&
				SameFloat(a.penetrationM, b.penetrationM);
		}

		inline bool SoleFootEqual(const SoleFootMeasurement& a, const SoleFootMeasurement& b) noexcept
		{
			if (a.valid != b.valid ||
				a.usingBootProxy != b.usingBootProxy ||
				a.pose.valid != b.pose.valid ||
				!SameVec3(a.pose.hip, b.pose.hip) ||
				!SameVec3(a.pose.knee, b.pose.knee) ||
				!SameVec3(a.pose.ankle, b.pose.ankle) ||
				!SameVec3(a.pose.toe, b.pose.toe) ||
				!SameQuat(a.pose.footRotation, b.pose.footRotation) ||
				!SameFloat(a.minimumDistanceM, b.minimumDistanceM) ||
				!SameFloat(a.maximumPenetrationM, b.maximumPenetrationM))
			{
				return false;
			}
			for (size_t i = 0; i < a.points.size(); ++i)
				if (!SolePointEqual(a.points[i], b.points[i])) return false;
			for (size_t p = 0; p < a.patches.size(); ++p)
			{
				const auto& pa = a.patches[p];
				const auto& pb = b.patches[p];
				if (!SameFloat(pa.minimumDistanceM, pb.minimumDistanceM) ||
					!SameFloat(pa.maximumDistanceM, pb.maximumDistanceM) ||
					!SameFloat(pa.maximumPenetrationM, pb.maximumPenetrationM))
					return false;
				for (size_t c = 0; c < pa.corners.size(); ++c)
					if (!SolePointEqual(pa.corners[c], pb.corners[c])) return false;
			}
			return true;
		}

		inline bool SoleSnapshotEqual(const SoleMeasurementSnapshot& a, uint64_t tokenA,
			const SoleMeasurementSnapshot& b, uint64_t tokenB) noexcept
		{
			if (a.available != b.available ||
				a.calibrated != b.calibrated ||
				tokenA != tokenB ||
				!SameFloat(a.clipTime, b.clipTime))
			{
				return false;
			}
			for (size_t i = 0; i < a.feet.size(); ++i)
				if (!SoleFootEqual(a.feet[i], b.feet[i])) return false;
			return true;
		}

		inline bool SolePitchSettingsEqual(const sole_pitch::Settings& a,
			const sole_pitch::Settings& b) noexcept
		{
			return a.experimentalSolePitchCorrection == b.experimentalSolePitchCorrection &&
				SameFloat(a.maxPitchDegrees, b.maxPitchDegrees) &&
				a.useMeshEnvelope == b.useMeshEnvelope;
		}

		inline bool SameCStr(const char* a, const char* b) noexcept
		{
			if (a == b) return true;
			const char* sa = a ? a : "";
			const char* sb = b ? b : "";
			return std::strcmp(sa, sb) == 0;
		}

		inline bool SolePitchSnapshotEqual(const sole_pitch::Snapshot& a,
			const sole_pitch::Snapshot& b) noexcept
		{
			if (!SameFloat(a.clipTime, b.clipTime)) return false;
			for (size_t i = 0; i < a.feet.size(); ++i)
			{
				const auto& fa = a.feet[i];
				const auto& fb = b.feet[i];
				if (fa.freshPose != fb.freshPose ||
					fa.applied != fb.applied ||
					fa.setterReadbackMatches != fb.setterReadbackMatches ||
					!SameFloat(fa.measuredAfterMinimumDistanceM, fb.measuredAfterMinimumDistanceM) ||
					fa.measuredAfterValid != fb.measuredAfterValid ||
					!SameCStr(fa.status, fb.status) ||
					fa.solution.valid != fb.solution.valid ||
					fa.solution.actionable != fb.solution.actionable ||
					fa.solution.fullyCleared != fb.solution.fullyCleared ||
					fa.solution.usedMeshEnvelope != fb.solution.usedMeshEnvelope ||
					!SameVec3(fa.solution.localAxis, fb.solution.localAxis) ||
					!SameFloat(fa.solution.pitchDegrees, fb.solution.pitchDegrees) ||
					!SameQuat(fa.solution.localDelta, fb.solution.localDelta) ||
					!SameQuat(fa.solution.correctedWorldRotation, fb.solution.correctedWorldRotation) ||
					!SameFloat(fa.solution.beforeMinimumDistanceM, fb.solution.beforeMinimumDistanceM) ||
					!SameFloat(fa.solution.afterMinimumDistanceM, fb.solution.afterMinimumDistanceM) ||
					!SameFloat(fa.solution.beforePenetrationM, fb.solution.beforePenetrationM) ||
					!SameFloat(fa.solution.afterPenetrationM, fb.solution.afterPenetrationM) ||
					!SoleFootEqual(fa.after, fb.after))
				{
					return false;
				}
			}
			return true;
		}

		inline bool AudioEffectStateEqual(const AudioEffectState& a, const AudioEffectState& b) noexcept
		{
			if (a.available != b.available ||
				a.activeId != b.activeId ||
				a.presets.size() != b.presets.size())
			{
				return false;
			}
			for (size_t i = 0; i < a.presets.size(); ++i)
			{
				if (a.presets[i].id != b.presets[i].id ||
					a.presets[i].name != b.presets[i].name ||
					a.presets[i].category != b.presets[i].category ||
					a.presets[i].description != b.presets[i].description)
				{
					return false;
				}
			}
			return true;
		}
	} // namespace detail_tick

	inline uint64_t Bridge::NextSessionId()
	{
		static std::atomic<uint64_t> sessionCounter{0};
		return ++sessionCounter;
	}

	inline void Bridge::ResetChangeTracking()
	{
		baselinePending_ = true;
		forcePlayerEvent_ = false;
		forceDomainRefresh_.store(true, std::memory_order_release);
		forceAudioEffectRefresh_.store(true, std::memory_order_release);
		lastPermissionAllowed_ = false;
		lastInjectionSession_ = 0;
		lastPlayerEmit_ = {};
		lastClockEmit_ = {};
		lastSlowDomainPoll_ = {};
		lastAudioEffectPoll_ = {};
		lastPhysicsBackend_ = -1;
		lastPolledLibraryRevision_ = 0;
		lastPolledFormationRevision_ = 0;
		lastPolledImportActive_ = false;
		lastPolledImportFinished_ = false;
		lastPolledImportProgress_ = -1.0f;
		domainCacheValid_ = false;
		lastPermInjectionAllowed_ = false;
		lastPermInjectionStatus_.clear();
		lastPlayerState_ = PlaybackState::Stopped;
		lastPlayerLoadedKey_.clear();
		lastPlayerLibRev_ = 0;
		lastPlayerTime_ = 0.0f;
		lastPlayerDuration_ = 0.0f;
		lastPlayerTotalFrames_ = 0;
		lastPlayerFps_ = 0.0f;
		lastPlayerSpeed_ = 1.0f;
		lastPlayerVolume_ = 1.0f;
		lastPlayerPlaylistMode_ = {};
		lastClockSnapshot_ = {};
		lastClockSnapshotValid_ = false;
		lastLibRev_ = 0;
		lastLibCount_ = 0;
		lastLibScanning_ = false;
		lastLibPlaylistNotice_.clear();
		lastLoadedKey_.clear();
		lastLoadedGen_ = 0;
		lastLoadedSession_ = 0;
		lastLoadedSourceMissing_ = false;
		lastLoadedSourceChanged_ = false;
		lastPlaylistMode_ = -1;
		lastPlaylistNotice_.clear();
		lastActorsEpoch_ = 0;
		lastActorsTarget_ = nullptr;
		lastActorsBound_ = nullptr;
		lastActorsEntries_.clear();
		lastFormationState_ = {};
		lastFormationHasLoadedMotion_ = false;
		lastCameraSnapshot_ = {};
		lastCameraNotice_.clear();
		lastStageNodesCache_.clear();
		lastStageSettings_ = {};
		lastStageSceneName_.clear();
		lastStageStatusNotice_.clear();
		lastStageMountedKey_.clear();
		lastStageLoadNotice_.clear();
		lastFacialWeights_ = {};
		lastFacialNotice_.clear();
		lastInitialPosition_ = {0.0f, 0.0f, 0.0f};
		lastInitialPositionNotice_.clear();
		lastImportActive_ = false;
		lastImportFinished_ = false;
		lastImportSucceeded_ = false;
		lastImportProgressVal_ = -1.0f;
		lastImportStage_.clear();
		lastImportStatusText_.clear();
		lastImportErrorDetail_.clear();
		lastImportKey_.clear();
		lastAppearanceTheme_ = -1;
		lastAppearanceUiScale_ = -1.0f;
		lastGazeMode_ = -1;
		lastGazeDesktop_ = true;
		lastGazeEye_ = 0.0f;
		lastGazeHead_ = 0.0f;
		lastGazeBody_ = 0.0f;
		lastGazeRuntimeAvail_ = false;
		lastGazeVrifyAvail_ = false;
		lastGazeVrifyActive_ = false;
		lastGazeNotice_.clear();
		lastBoneRevision_ = 0;
		lastBoneCount_ = 0;
		lastBoneMasterEnabled_ = true;
		lastBoneNotice_.clear();
		lastSoleSettings_ = {};
		lastSoleGen_ = 0;
		lastSoleNotice_.clear();
		lastSoleActorToken_ = 0;
		lastSoleSnapshot_ = {};
		lastSolePitchSettings_ = {};
		lastSolePitchSnapshot_ = {};
		lastSolePitchNotice_.clear();
		lastStudioSettings_ = {};
		lastStudioRevision_ = 0;
		lastStudioLightingNotice_.clear();
		lastStudioVideoNotice_.clear();
		lastExpMikuSnapshot_ = 0;
		lastExpMikuMouth_ = -1;
		lastExpMikuEye_ = -1;
		lastExpMikuAllow_ = -1;
		lastExpCtrlMouth_ = false;
		lastExpCtrlEye_ = false;
		lastExpCtrlAllow_ = false;
		lastExpPlaying_ = false;
		lastExpBeforeQuartz_ = false;
		lastExpMuteSe_ = false;
		lastExpNotice_.clear();
		lastAudioEffectState_ = {};
		lastPermission_.clear();
		lastPlayer_.clear();
		lastLibrary_.clear();
		lastLoaded_.clear();
		lastPlaylist_.clear();
		lastActors_.clear();
		lastFormation_.clear();
		lastCamera_.clear();
		lastStage_.clear();
		lastStageNodes_.clear();
		lastFacial_.clear();
		lastPosition_.clear();
		lastImportProgress_.clear();
		lastAppearance_.clear();
		lastGaze_.clear();
		lastBone_.clear();
		lastSole_.clear();
		lastSolePitch_.clear();
		lastStudio_.clear();
		lastExperiments_.clear();
		lastClock_.clear();
		lastClockMeta_.clear();
		lastAudioEffect_.clear();
	}

	inline void Bridge::Tick()
	{
		PollBridgeDialog();
		PollImGuiDialog();
		const bool wantDropTarget = visible_ && dropEnabled_;
		if (hooks_.dropSetTarget && (wantDropTarget || lastDropTargetSent_))
		{
			std::string dropNotice;
			hooks_.dropSetTarget(wantDropTarget, dropNotice);
			lastDropTargetSent_ = wantDropTarget;
		}
		if (wantDropTarget)
		{
			if (hooks_.dropPeek) cachedDropPeek_ = NormalizeDropPeek(hooks_.dropPeek());
			if (hooks_.dropConsume && (!hooks_.dropPeek || cachedDropPeek_.pendingCount > 0))
			{
				const DropBatch batch = hooks_.dropConsume();
				if (batch.ok && !batch.paths.empty())
				{
					for (const auto& path : batch.paths)
					{
						if (pendingDrops_.size() >= 512) break;
						pendingDrops_.push_back(path);
					}
				}
			}
		}
		else if (!dropEnabled_)
		{
			cachedDropPeek_ = {};
		}
		if (!visible_ && !baselinePending_) return;

		const auto now = std::chrono::steady_clock::now();
		std::string permission, player, library, loaded, playlist, actors, formation, camera, stage, stageNodes,
			facial, position, importProgress, appearance,
			gaze, bone, sole, solePitch, studio, experiments, clock, clockMeta, audioEffect;
		int physicsBackend = 0;
		bool injectionAllowed = false;
		bool scrubbing = false;
		float scrubTime = 0.0f;
		uint64_t injectionSession = 0;
		bool playing = false;
		bool refreshSlowDomains = false;
		bool refreshAudioEffect = false;

		bool permissionDirty = false;
		bool needPlayerJson = false;
		bool needClockJson = false;
		bool libraryDirty = false;
		bool loadedDirty = false;
		bool playlistDirty = false;
		bool actorsDirty = false;
		bool formationDirty = false;
		bool cameraDirty = false;
		bool stageNodesDirty = false;
		bool stageDirty = false;
		bool facialDirty = false;
		bool positionDirty = false;
		bool importProgressDirty = false;
		bool gazeDirty = false;
		bool boneDirty = false;
		bool soleDirty = false;
		bool solePitchDirty = false;
		bool studioDirty = false;
		bool experimentsDirty = false;
		bool appearanceDirty = false;
		bool audioEffectDirty = false;

		const bool baseline = baselinePending_;
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			const bool coldCache = baseline || !domainCacheValid_;
			injectionAllowed = core_.InjectionAllowed();
			injectionSession = core_.injectionSession;
			scrubbing = core_.player.scrubbing;
			scrubTime = core_.player.currentTime;
			playing = core_.player.state == PlaybackState::Playing;
			physicsBackend = core_.guiPhysicsBackend.load();

			permissionDirty = coldCache ||
				injectionAllowed != lastPermInjectionAllowed_ ||
				core_.injectionStatus != lastPermInjectionStatus_;
			if (permissionDirty)
			{
				lastPermInjectionAllowed_ = injectionAllowed;
				lastPermInjectionStatus_ = core_.injectionStatus;
				permission = PermissionJsonLocked();
			}

			const bool playerThrottled = playing && !forcePlayerEvent_ && !baseline &&
				now - lastPlayerEmit_ < std::chrono::milliseconds(100);
			const float playerDuration = core_.player.TimelineDuration();
			const bool playerChanged = coldCache || forcePlayerEvent_ ||
				core_.player.state != lastPlayerState_ ||
				core_.loadedMotionKey != lastPlayerLoadedKey_ ||
				core_.libraryRevision != lastPlayerLibRev_ ||
				!detail_tick::SameFloat(core_.player.currentTime, lastPlayerTime_) ||
				!detail_tick::SameFloat(playerDuration, lastPlayerDuration_) ||
				core_.player.clip.total_frames != lastPlayerTotalFrames_ ||
				!detail_tick::SameFloat(core_.player.clip.fps, lastPlayerFps_) ||
				!detail_tick::SameFloat(core_.player.playbackSpeedMultiplier, lastPlayerSpeed_) ||
				!detail_tick::SameFloat(core_.player.musicVolume, lastPlayerVolume_) ||
				core_.playlist.mode != lastPlayerPlaylistMode_;
			needPlayerJson = !playerThrottled && (baseline || forcePlayerEvent_ || playerChanged);
			if (needPlayerJson)
			{
				lastPlayerState_ = core_.player.state;
				lastPlayerLoadedKey_ = core_.loadedMotionKey;
				lastPlayerLibRev_ = core_.libraryRevision;
				lastPlayerTime_ = core_.player.currentTime;
				lastPlayerDuration_ = playerDuration;
				lastPlayerTotalFrames_ = core_.player.clip.total_frames;
				lastPlayerFps_ = core_.player.clip.fps;
				lastPlayerSpeed_ = core_.player.playbackSpeedMultiplier;
				lastPlayerVolume_ = core_.player.musicVolume;
				lastPlayerPlaylistMode_ = core_.playlist.mode;
				player = PlayerStateJsonLocked();
			}

			GakuMotionClockSnapshotV1 clockSnap{};
			core_.ReadClockSnapshot(&clockSnap, false);
			const bool clockMetaChanged = !lastClockSnapshotValid_ || baseline ||
				clockSnap.runMode != lastClockSnapshot_.runMode ||
				clockSnap.configuredSource != lastClockSnapshot_.configuredSource ||
				clockSnap.effectiveSource != lastClockSnapshot_.effectiveSource ||
				clockSnap.fpsNumerator != lastClockSnapshot_.fpsNumerator ||
				clockSnap.fpsDenominator != lastClockSnapshot_.fpsDenominator ||
				(clockSnap.waitingForRenderComplete != 0) != (lastClockSnapshot_.waitingForRenderComplete != 0) ||
				clockSnap.leaseId != lastClockSnapshot_.leaseId ||
				std::strncmp(clockSnap.leaseOwner, lastClockSnapshot_.leaseOwner, sizeof(clockSnap.leaseOwner)) != 0;
			const bool clockSerialChanged = !lastClockSnapshotValid_ ||
				clockSnap.evaluatedFrameSerial != lastClockSnapshot_.evaluatedFrameSerial;
			const bool clockThrottled = playing && !baseline && !clockMetaChanged &&
				now - lastClockEmit_ < std::chrono::milliseconds(100);
			needClockJson = !clockThrottled && (baseline || clockMetaChanged || clockSerialChanged);
			if (needClockJson)
			{
				lastClockSnapshot_ = clockSnap;
				lastClockSnapshotValid_ = true;
				clock = ClockJsonLocked(&clockMeta);
			}

			const bool forceSlowRefresh = forceDomainRefresh_.exchange(false, std::memory_order_acq_rel);
			const bool forceAudioPending = forceAudioEffectRefresh_.load(std::memory_order_acquire);
			const bool permissionOrSessionChanged =
				(permissionDirty && permission != lastPermission_) ||
				(lastPermissionAllowed_ && !injectionAllowed) ||
				(injectionSession != lastInjectionSession_ && lastInjectionSession_ != 0);
			const bool libraryOrImportChanged =
				core_.libraryRevision != lastPolledLibraryRevision_ ||
				core_.formation.revision != lastPolledFormationRevision_ ||
				core_.importProgress.active != lastPolledImportActive_ ||
				core_.importProgress.finished != lastPolledImportFinished_ ||
				!detail_tick::SameFloat(core_.importProgress.progress, lastPolledImportProgress_);
			refreshSlowDomains = coldCache || forceSlowRefresh || forceAudioPending ||
				permissionOrSessionChanged || libraryOrImportChanged ||
				(now - lastSlowDomainPoll_ >= std::chrono::milliseconds(100));
			if (refreshSlowDomains)
			{
				EnsureFormationSyncedLocked();
				lastSlowDomainPoll_ = now;
				lastPolledLibraryRevision_ = core_.libraryRevision;
				lastPolledFormationRevision_ = core_.formation.revision;
				lastPolledImportActive_ = core_.importProgress.active;
				lastPolledImportFinished_ = core_.importProgress.finished;
				lastPolledImportProgress_ = core_.importProgress.progress;

				// 1. library
				const bool libScanning = core_.libraryScan.valid();
				libraryDirty = coldCache ||
					core_.libraryRevision != lastLibRev_ ||
					core_.motions.size() != lastLibCount_ ||
					libScanning != lastLibScanning_ ||
					core_.playlistNotice != lastLibPlaylistNotice_;
				if (libraryDirty)
				{
					lastLibRev_ = core_.libraryRevision;
					lastLibCount_ = core_.motions.size();
					lastLibScanning_ = libScanning;
					lastLibPlaylistNotice_ = core_.playlistNotice;
					library = LibraryJsonLocked();
				}

				// 2. loaded
				loadedDirty = coldCache ||
					core_.loadedMotionKey != lastLoadedKey_ ||
					core_.loadedMotionGeneration != lastLoadedGen_ ||
					core_.injectionSession != lastLoadedSession_ ||
					core_.loadedSourceMissing != lastLoadedSourceMissing_ ||
					core_.loadedSourceChanged != lastLoadedSourceChanged_;
				if (loadedDirty)
				{
					lastLoadedKey_ = core_.loadedMotionKey;
					lastLoadedGen_ = core_.loadedMotionGeneration;
					lastLoadedSession_ = core_.injectionSession;
					lastLoadedSourceMissing_ = core_.loadedSourceMissing;
					lastLoadedSourceChanged_ = core_.loadedSourceChanged;
					loaded = LoadedJsonLocked();
				}

				// 3. playlist
				const int playlistMode = static_cast<int>(core_.playlist.mode);
				playlistDirty = coldCache ||
					playlistMode != lastPlaylistMode_ ||
					core_.playlistNotice != lastPlaylistNotice_;
				if (playlistDirty)
				{
					lastPlaylistMode_ = playlistMode;
					lastPlaylistNotice_ = core_.playlistNotice;
					playlist = PlaylistJsonLocked();
				}

				// 4. actors
				const uint64_t actorsEpoch = core_.sceneActorRegistry.Epoch();
				const auto& actorEntries = core_.sceneActorRegistry.Entries();
				void* const targetActor = core_.player.controller.targetActor;
				void* const boundActor = core_.player.controller.currentBinding.actor;
				actorsDirty = coldCache ||
					actorsEpoch != lastActorsEpoch_ ||
					targetActor != lastActorsTarget_ ||
					boundActor != lastActorsBound_ ||
					actorEntries.size() != lastActorsEntries_.size();
				if (!actorsDirty)
				{
					for (size_t i = 0; i < actorEntries.size(); ++i)
					{
						const auto& cur = actorEntries[i];
						const auto& cached = lastActorsEntries_[i];
						if (cur.token != cached.token ||
							cur.actor != cached.actor ||
							cur.character != cached.character ||
							cur.displayName != cached.displayName ||
							cur.shortName != cached.shortName ||
							cur.presetName != cached.presetName)
						{
							actorsDirty = true;
							break;
						}
					}
				}
				if (actorsDirty)
				{
					lastActorsEpoch_ = actorsEpoch;
					lastActorsTarget_ = targetActor;
					lastActorsBound_ = boundActor;
					lastActorsEntries_.resize(actorEntries.size());
					for (size_t i = 0; i < actorEntries.size(); ++i)
					{
						lastActorsEntries_[i] = CachedActorEntry{
							actorEntries[i].token,
							actorEntries[i].actor,
							actorEntries[i].character,
							actorEntries[i].displayName,
							actorEntries[i].shortName,
							actorEntries[i].presetName};
					}
					actors = ActorsJsonLocked();
				}

				// 5. formation
				const bool hasLoadedMotion = !core_.loadedMotionKey.empty();
				formationDirty = coldCache ||
					!detail_tick::FormationEqual(core_.formation, hasLoadedMotion,
						lastFormationState_, lastFormationHasLoadedMotion_);
				if (formationDirty)
				{
					lastFormationState_ = core_.formation;
					lastFormationHasLoadedMotion_ = hasLoadedMotion;
					formation = FormationJsonLocked();
				}

				// 6. camera
				const auto camSnapshot = core_.camera.ReadSnapshot();
				cameraDirty = coldCache ||
					!detail_tick::CameraSnapshotEqual(camSnapshot, lastCameraSnapshot_) ||
					core_.cameraNotice != lastCameraNotice_;
				if (cameraDirty)
				{
					lastCameraSnapshot_ = camSnapshot;
					lastCameraNotice_ = core_.cameraNotice;
					camera = CameraSnapshotJsonLocked(camSnapshot);
				}

				// 7. stageNodes & stage (single stageMutex lock, zero candidate-vector copy)
				{
					std::lock_guard<std::mutex> stageLock(core_.stageManager.stageMutex);
					const auto& candidates = core_.stageManager.candidates;
					stageNodesDirty = coldCache || candidates.size() != lastStageNodesCache_.size();
					if (!stageNodesDirty)
					{
						for (size_t i = 0; i < candidates.size(); ++i)
						{
							const auto& c = candidates[i];
							const auto& cached = lastStageNodesCache_[i];
							if (c.instanceId != cached.instanceId ||
								c.parentIndex != cached.parentIndex ||
								c.depth != cached.depth ||
								detail_tick::PackStageNodeFlags(c) != cached.flags ||
								c.name != cached.name ||
								c.displayName != cached.displayName)
							{
								stageNodesDirty = true;
								break;
							}
						}
					}
					const bool stageMetaDirty = coldCache ||
						!detail_tick::ExactStageSettingsEqual(core_.stageManager.settings, lastStageSettings_) ||
						core_.stageManager.currentSceneName != lastStageSceneName_ ||
						core_.stageManager.statusNotice != lastStageStatusNotice_ ||
						core_.stageManager.mountedStageKey != lastStageMountedKey_ ||
						core_.stageLoadNotice != lastStageLoadNotice_;
					stageDirty = stageNodesDirty || stageMetaDirty;
					if (stageNodesDirty)
					{
						lastStageNodesCache_.resize(candidates.size());
						for (size_t i = 0; i < candidates.size(); ++i)
						{
							const auto& c = candidates[i];
							lastStageNodesCache_[i] = CachedStageNode{
								c.instanceId,
								c.parentIndex,
								c.depth,
								detail_tick::PackStageNodeFlags(c),
								c.name,
								c.displayName};
						}
						stageNodes = StageNodesFromVector(candidates);
					}
					if (stageDirty)
					{
						lastStageSettings_ = core_.stageManager.settings;
						lastStageSceneName_ = core_.stageManager.currentSceneName;
						lastStageStatusNotice_ = core_.stageManager.statusNotice;
						lastStageMountedKey_ = core_.stageManager.mountedStageKey;
						lastStageLoadNotice_ = core_.stageLoadNotice;
						stage = StageJsonFromPartsLocked(lastStageSettings_, lastStageSceneName_,
							lastStageStatusNotice_, lastStageMountedKey_,
							stageNodesDirty ? stageNodes : lastStageNodes_);
					}
				}

				// 8. facial
				const auto& fw = core_.player.controller.facialWeights;
				facialDirty = coldCache ||
					!detail_tick::SameFloat(fw.overall, lastFacialWeights_.overall) ||
					!detail_tick::SameFloat(fw.brow, lastFacialWeights_.brow) ||
					!detail_tick::SameFloat(fw.eye, lastFacialWeights_.eye) ||
					!detail_tick::SameFloat(fw.mouth, lastFacialWeights_.mouth) ||
					core_.facialWeightsNotice != lastFacialNotice_;
				if (facialDirty)
				{
					lastFacialWeights_ = fw;
					lastFacialNotice_ = core_.facialWeightsNotice;
					facial = FacialJsonLocked();
				}

				// 9. position
				const auto& initPos = core_.player.controller.initialPositionOffset;
				positionDirty = coldCache ||
					!detail_tick::SameVec3(initPos, lastInitialPosition_) ||
					core_.initialPositionNotice != lastInitialPositionNotice_;
				if (positionDirty)
				{
					lastInitialPosition_ = initPos;
					lastInitialPositionNotice_ = core_.initialPositionNotice;
					position = PositionJsonLocked();
				}

				// 10. importProgress
				const auto& imp = core_.importProgress;
				const std::string impKey = (imp.finished && imp.succeeded) ? core_.lastImportedKey : std::string();
				importProgressDirty = coldCache ||
					imp.active != lastImportActive_ ||
					imp.finished != lastImportFinished_ ||
					imp.succeeded != lastImportSucceeded_ ||
					!detail_tick::SameFloat(imp.progress, lastImportProgressVal_) ||
					imp.stage != lastImportStage_ ||
					imp.statusText != lastImportStatusText_ ||
					imp.errorDetail != lastImportErrorDetail_ ||
					impKey != lastImportKey_;
				if (importProgressDirty)
				{
					lastImportActive_ = imp.active;
					lastImportFinished_ = imp.finished;
					lastImportSucceeded_ = imp.succeeded;
					lastImportProgressVal_ = imp.progress;
					lastImportStage_ = imp.stage;
					lastImportStatusText_ = imp.statusText;
					lastImportErrorDetail_ = imp.errorDetail;
					lastImportKey_ = impKey;
					importProgress = ImportProgressJsonLocked();
				}

				// 11. gaze
				const auto& lookAt = core_.lookAt;
				const int gazeMode = static_cast<int>(lookAt.gazeMode.load());
				const bool gazeDesktop = lookAt.desktopLookAtEnabled.load();
				const float gazeEye = lookAt.eyeWeight.load();
				const float gazeHead = lookAt.headWeight.load();
				const float gazeBody = lookAt.bodyWeight.load();
				const bool gazeRuntime = GazeRuntimeAvailable();
				const bool gazeVrifyAvail = lookAt.fnQueryVRifyGaze != nullptr;
				const bool gazeVrifyActive = lookAt.IsVRifyLiveGazeActive();
				gazeDirty = coldCache ||
					gazeMode != lastGazeMode_ ||
					gazeDesktop != lastGazeDesktop_ ||
					!detail_tick::SameFloat(gazeEye, lastGazeEye_) ||
					!detail_tick::SameFloat(gazeHead, lastGazeHead_) ||
					!detail_tick::SameFloat(gazeBody, lastGazeBody_) ||
					gazeRuntime != lastGazeRuntimeAvail_ ||
					gazeVrifyAvail != lastGazeVrifyAvail_ ||
					gazeVrifyActive != lastGazeVrifyActive_ ||
					gazeNotice_ != lastGazeNotice_;
				if (gazeDirty)
				{
					lastGazeMode_ = gazeMode;
					lastGazeDesktop_ = gazeDesktop;
					lastGazeEye_ = gazeEye;
					lastGazeHead_ = gazeHead;
					lastGazeBody_ = gazeBody;
					lastGazeRuntimeAvail_ = gazeRuntime;
					lastGazeVrifyAvail_ = gazeVrifyAvail;
					lastGazeVrifyActive_ = gazeVrifyActive;
					lastGazeNotice_ = gazeNotice_;
					gaze = GazeJsonLocked();
				}

				// 12. bone
				boneDirty = coldCache ||
					core_.effectiveOverrides.revision != lastBoneRevision_ ||
					core_.effectiveOverrides.offsets.size() != lastBoneCount_ ||
					core_.masterOverridesEnabled != lastBoneMasterEnabled_ ||
					boneNotice_ != lastBoneNotice_;
				if (boneDirty)
				{
					lastBoneRevision_ = core_.effectiveOverrides.revision;
					lastBoneCount_ = core_.effectiveOverrides.offsets.size();
					lastBoneMasterEnabled_ = core_.masterOverridesEnabled;
					lastBoneNotice_ = boneNotice_;
					bone = BoneJsonLocked();
				}

				// 13. sole
				const auto* soleActor = core_.sceneActorRegistry.Find(core_.soleSnapshot.actor);
				const uint64_t soleActorToken = soleActor ? soleActor->token : 0;
				soleDirty = coldCache ||
					!detail_tick::SoleSettingsEqual(core_.soleSettings, lastSoleSettings_) ||
					core_.soleCalibrationGeneration != lastSoleGen_ ||
					core_.soleNotice != lastSoleNotice_ ||
					!detail_tick::SoleSnapshotEqual(core_.soleSnapshot, soleActorToken,
						lastSoleSnapshot_, lastSoleActorToken_);
				if (soleDirty)
				{
					lastSoleSettings_ = core_.soleSettings;
					lastSoleGen_ = core_.soleCalibrationGeneration;
					lastSoleNotice_ = core_.soleNotice;
					lastSoleActorToken_ = soleActorToken;
					lastSoleSnapshot_ = core_.soleSnapshot;
					sole = SoleJsonLocked();
				}

				// 14. solePitch
				solePitchDirty = coldCache ||
					!detail_tick::SolePitchSettingsEqual(core_.experimentalSolePitchSettings, lastSolePitchSettings_) ||
					!detail_tick::SolePitchSnapshotEqual(core_.experimentalSolePitchSnapshot, lastSolePitchSnapshot_) ||
					solePitchNotice_ != lastSolePitchNotice_;
				if (solePitchDirty)
				{
					lastSolePitchSettings_ = core_.experimentalSolePitchSettings;
					lastSolePitchSnapshot_ = core_.experimentalSolePitchSnapshot;
					lastSolePitchNotice_ = solePitchNotice_;
					solePitch = SolePitchJsonLocked();
				}

				// 15. studio
				studioDirty = coldCache ||
					!(core_.studioSettings == lastStudioSettings_) ||
					core_.studioRevision != lastStudioRevision_ ||
					core_.studioLightingNotice != lastStudioLightingNotice_ ||
					core_.studioVideoNotice != lastStudioVideoNotice_;
				if (studioDirty)
				{
					lastStudioSettings_ = core_.studioSettings;
					lastStudioRevision_ = core_.studioRevision;
					lastStudioLightingNotice_ = core_.studioLightingNotice;
					lastStudioVideoNotice_ = core_.studioVideoNotice;
					studio = StudioJsonLocked();
				}

				// 16. experiments
				const auto& ctrl = core_.player.controller;
				const unsigned mikuSnap = ctrl.guiMikuFitSnapshot.load(std::memory_order_relaxed);
				const bool expPlaying = core_.player.state != PlaybackState::Stopped;
				const bool expBeforeQuartz = physicsBackend == 2;
				const bool expMuteSe = core_.guiMuteNativeSoundEffects.load();
				experimentsDirty = coldCache ||
					mikuSnap != lastExpMikuSnapshot_ ||
					core_.config.facialMikuMouthFit != lastExpMikuMouth_ ||
					core_.config.facialMikuEyeFit != lastExpMikuEye_ ||
					core_.config.facialMikuFitAllowUnverified != lastExpMikuAllow_ ||
					ctrl.facialMikuMouthFit != lastExpCtrlMouth_ ||
					ctrl.facialMikuEyeFit != lastExpCtrlEye_ ||
					ctrl.facialMikuFitAllowUnverified != lastExpCtrlAllow_ ||
					expPlaying != lastExpPlaying_ ||
					expBeforeQuartz != lastExpBeforeQuartz_ ||
					expMuteSe != lastExpMuteSe_ ||
					experimentsNotice_ != lastExpNotice_;
				if (experimentsDirty)
				{
					lastExpMikuSnapshot_ = mikuSnap;
					lastExpMikuMouth_ = core_.config.facialMikuMouthFit;
					lastExpMikuEye_ = core_.config.facialMikuEyeFit;
					lastExpMikuAllow_ = core_.config.facialMikuFitAllowUnverified;
					lastExpCtrlMouth_ = ctrl.facialMikuMouthFit;
					lastExpCtrlEye_ = ctrl.facialMikuEyeFit;
					lastExpCtrlAllow_ = ctrl.facialMikuFitAllowUnverified;
					lastExpPlaying_ = expPlaying;
					lastExpBeforeQuartz_ = expBeforeQuartz;
					lastExpMuteSe_ = expMuteSe;
					lastExpNotice_ = experimentsNotice_;
					experiments = ExperimentsJsonLocked();
				}

				domainCacheValid_ = true;
			}
		}
		if (refreshSlowDomains)
		{
			AppearanceInfo appInfo;
			if (hooks_.appearance) appInfo = hooks_.appearance();
			appearanceDirty = baseline || lastAppearance_.empty() ||
				appInfo.theme != lastAppearanceTheme_ ||
				!detail_tick::SameFloat(appInfo.uiScale, lastAppearanceUiScale_);
			if (appearanceDirty)
			{
				lastAppearanceTheme_ = appInfo.theme;
				lastAppearanceUiScale_ = appInfo.uiScale;
				appearance = BuildAppearanceJsonFromInfo(appInfo);
			}

			const bool forceAudioRefresh = forceAudioEffectRefresh_.exchange(false, std::memory_order_acq_rel);
			refreshAudioEffect = baseline || forceAudioRefresh ||
				!playing || lastAudioEffect_.empty() ||
				(now - lastAudioEffectPoll_ >= std::chrono::milliseconds(250));
			if (refreshAudioEffect)
			{
				lastAudioEffectPoll_ = now;
				AudioEffectState st;
				if (hooks_.getAudioEffectsState) st = hooks_.getAudioEffectsState();
				if (st.activeId.empty()) st.activeId = "flat";
				if (st.presets.empty())
					st.presets.push_back({"flat", "原声", "flat", "直通输出"});
				audioEffectDirty = baseline || lastAudioEffect_.empty() ||
					!detail_tick::AudioEffectStateEqual(st, lastAudioEffectState_);
				if (audioEffectDirty)
				{
					lastAudioEffectState_ = std::move(st);
					audioEffect = BuildAudioEffectJsonFromState(lastAudioEffectState_);
				}
			}
		}

		// Permission loss / session change: the frontend drops its scrub without
		// sending scrubEnd, so finish the native scrub state through the queue.
		if ((lastPermissionAllowed_ && !injectionAllowed) ||
			(injectionSession != lastInjectionSession_ && lastInjectionSession_ != 0))
		{
			if (scrubbing)
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				core_.QueueScrubEnd(scrubTime);
			}
		}
		lastPermissionAllowed_ = injectionAllowed;
		lastInjectionSession_ = injectionSession;

		baselinePending_ = false;
		const auto emitChanged = [&](const char* name, const std::string& json, std::string& last) {
			if (baseline || json != last)
			{
				last = json;
				Emit(name, json);
			}
		};
		if (permissionDirty) emitChanged("permission.changed", permission, lastPermission_);
		if (refreshSlowDomains)
		{
			if (libraryDirty)
			{
				if (library != lastLibrary_)
				{
					std::string keyToInvalidate;
					bool importStillBusy = false;
					{
						std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
						importStillBusy = core_.importBusy;
					}
					if (!importStillBusy)
					{
						std::lock_guard<std::mutex> coverLock(pendingCoverInvalidateMutex_);
						if (!pendingImportCoverInvalidateKey_.empty())
						{
							keyToInvalidate = std::move(pendingImportCoverInvalidateKey_);
							pendingImportCoverInvalidateKey_.clear();
						}
					}
					if (!keyToInvalidate.empty() && hooks_.invalidateTrackCover)
						hooks_.invalidateTrackCover(keyToInvalidate);
				}
				emitChanged("library.changed", library, lastLibrary_);
			}
			if (loadedDirty) emitChanged("loaded.changed", loaded, lastLoaded_);
			if (playlistDirty) emitChanged("playlist.changed", playlist, lastPlaylist_);
			if (actorsDirty) emitChanged("actors.changed", actors, lastActors_);
			if (formationDirty) emitChanged("formation.changed", formation, lastFormation_);
			if (cameraDirty) emitChanged("camera.changed", camera, lastCamera_);
			if (stageDirty) emitChanged("stage.changed", stage, lastStage_);
			if (stageNodesDirty && (baseline || stageNodes != lastStageNodes_))
			{
				lastStageNodes_ = stageNodes;
				Emit("stage.nodesChanged", "{\"nodes\":" + stageNodes + "}");
			}
			if (facialDirty) emitChanged("facial.changed", facial, lastFacial_);
			if (positionDirty) emitChanged("position.changed", position, lastPosition_);
			if (importProgressDirty && importProgress != "null")
				emitChanged("import.progress", importProgress, lastImportProgress_);
			if (appearanceDirty) emitChanged("appearance.changed", appearance, lastAppearance_);
			if (gazeDirty) emitChanged("gaze.changed", gaze, lastGaze_);
			if (boneDirty) emitChanged("bone.changed", bone, lastBone_);
			if (soleDirty) emitChanged("sole.changed", sole, lastSole_);
			if (solePitchDirty) emitChanged("solePitch.changed", solePitch, lastSolePitch_);
			if (studioDirty) emitChanged("studio.changed", studio, lastStudio_);
			if (experimentsDirty) emitChanged("experiments.changed", experiments, lastExperiments_);
			if (audioEffectDirty) emitChanged("audioEffect.changed", audioEffect, lastAudioEffect_);
		}
		// clock.changed: immediate on baseline or structural clock config change,
		// throttled to ~10 Hz while playing for high-frequency evaluatedFrameSerial ticks.
		if (needClockJson && (baseline || clock != lastClock_))
		{
			lastClock_ = clock;
			lastClockMeta_ = clockMeta;
			lastClockEmit_ = now;
			Emit("clock.changed", clock);
		}
		if (baseline || physicsBackend != lastPhysicsBackend_)
		{
			lastPhysicsBackend_ = physicsBackend;
			Emit("settings.changed", "{\"physicsBackend\":" + JNum(physicsBackend) + "}");
		}

		// player.stateChanged: immediate after commands, ~10 Hz while playing,
		// otherwise only on change.
		if (needPlayerJson && (baseline || forcePlayerEvent_ || player != lastPlayer_))
		{
			lastPlayer_ = player;
			lastPlayerEmit_ = now;
			forcePlayerEvent_ = false;
			Emit("player.stateChanged", player);
		}

		if (hooks_.drainReadyCovers)
		{
			const auto ready = hooks_.drainReadyCovers(4);
			for (const auto& item : ready)
			{
				Emit("library.coverReady",
					"{\"key\":" + JString(item.key) +
					",\"hasCover\":" + JBool(item.hasCover) +
					",\"coverDataUri\":" + JString(item.coverDataUri) + "}");
			}
		}
	}

	inline void Bridge::PollImGuiDialog()
	{
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		auto& gui = core_.gui;
		if (gui.browsePendingKind < 0 || !core_.fnPollPickFile) return;
		std::string path;
		const int state = core_.fnPollPickFile(path, gui.browseNotice);
		if (state == GAKU_FILE_DIALOG_PENDING) return;
		if (state == GAKU_FILE_DIALOG_ACCEPTED)
		{
			char* target = gui.browsePendingKind == 0 ? gui.importVmd
				: gui.browsePendingKind == 1 ? gui.importAudio
				: gui.browsePendingKind == 3 ? gui.studioVideoPath : gui.importCameraSource;
			const size_t capacity = gui.browsePendingKind == 0 ? sizeof(gui.importVmd)
				: gui.browsePendingKind == 1 ? sizeof(gui.importAudio)
				: gui.browsePendingKind == 3 ? sizeof(gui.studioVideoPath) : sizeof(gui.importCameraSource);
			if (path.size() >= capacity) gui.browseNotice = "所选路径过长，未修改输入路径";
			else std::memcpy(target, path.c_str(), path.size() + 1);
		}
		gui.browsePendingKind = -1;
	}
} // namespace GakuMotion::webpanel
