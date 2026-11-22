namespace GakuMotion::webpanel
{
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
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			injectionAllowed = core_.InjectionAllowed();
			injectionSession = core_.injectionSession;
			scrubbing = core_.player.scrubbing;
			scrubTime = core_.player.currentTime;
			playing = core_.player.state == PlaybackState::Playing;
			permission = PermissionJsonLocked();
			player = PlayerStateJsonLocked();
			clock = ClockJsonLocked(&clockMeta);
			physicsBackend = core_.guiPhysicsBackend.load();

			const bool forceSlowRefresh = forceDomainRefresh_.exchange(false, std::memory_order_acq_rel);
			const bool permissionOrSessionChanged =
				permission != lastPermission_ ||
				(lastPermissionAllowed_ && !injectionAllowed) ||
				(injectionSession != lastInjectionSession_ && lastInjectionSession_ != 0);
			const bool libraryOrImportChanged =
				core_.libraryRevision != lastPolledLibraryRevision_ ||
				core_.formation.revision != lastPolledFormationRevision_ ||
				core_.importProgress.active != lastPolledImportActive_ ||
				core_.importProgress.finished != lastPolledImportFinished_ ||
				core_.importProgress.progress != lastPolledImportProgress_;
			refreshSlowDomains = baselinePending_ || forceSlowRefresh ||
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
				library = LibraryJsonLocked();
				loaded = LoadedJsonLocked();
				playlist = PlaylistJsonLocked();
				actors = ActorsJsonLocked();
				formation = FormationJsonLocked();
				camera = CameraJsonLocked();
				stageNodes = StageNodesJsonLocked();
				stage = StageJsonWithNodesLocked(stageNodes);
				facial = FacialJsonLocked();
				position = PositionJsonLocked();
				importProgress = ImportProgressJsonLocked();
				gaze = GazeJsonLocked();
				bone = BoneJsonLocked();
				sole = SoleJsonLocked();
				solePitch = SolePitchJsonLocked();
				studio = StudioJsonLocked();
				experiments = ExperimentsJsonLocked();
			}
		}
		if (refreshSlowDomains)
		{
			appearance = BuildAppearanceJson();
			const bool forceAudioRefresh = forceAudioEffectRefresh_.exchange(false, std::memory_order_acq_rel);
			refreshAudioEffect = baselinePending_ || forceAudioRefresh ||
				!playing || lastAudioEffect_.empty() ||
				(now - lastAudioEffectPoll_ >= std::chrono::milliseconds(250));
			if (refreshAudioEffect)
			{
				lastAudioEffectPoll_ = now;
				audioEffect = BuildAudioEffectJson();
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

		const bool baseline = baselinePending_;
		baselinePending_ = false;
		const auto emitChanged = [&](const char* name, const std::string& json, std::string& last) {
			if (baseline || json != last)
			{
				last = json;
				Emit(name, json);
			}
		};
		emitChanged("permission.changed", permission, lastPermission_);
		if (refreshSlowDomains)
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
			emitChanged("loaded.changed", loaded, lastLoaded_);
			emitChanged("playlist.changed", playlist, lastPlaylist_);
			emitChanged("actors.changed", actors, lastActors_);
			emitChanged("formation.changed", formation, lastFormation_);
			emitChanged("camera.changed", camera, lastCamera_);
			emitChanged("stage.changed", stage, lastStage_);
			if (baseline || stageNodes != lastStageNodes_)
			{
				lastStageNodes_ = stageNodes;
				Emit("stage.nodesChanged", "{\"nodes\":" + stageNodes + "}");
			}
			emitChanged("facial.changed", facial, lastFacial_);
			emitChanged("position.changed", position, lastPosition_);
			if (importProgress != "null")
				emitChanged("import.progress", importProgress, lastImportProgress_);
			emitChanged("appearance.changed", appearance, lastAppearance_);
			emitChanged("gaze.changed", gaze, lastGaze_);
			emitChanged("bone.changed", bone, lastBone_);
			emitChanged("sole.changed", sole, lastSole_);
			emitChanged("solePitch.changed", solePitch, lastSolePitch_);
			emitChanged("studio.changed", studio, lastStudio_);
			emitChanged("experiments.changed", experiments, lastExperiments_);
			if (refreshAudioEffect)
				emitChanged("audioEffect.changed", audioEffect, lastAudioEffect_);
		}
		// clock.changed: immediate on baseline or structural clock config change,
		// throttled to ~10 Hz while playing for high-frequency evaluatedFrameSerial ticks.
		const bool clockThrottled = playing && !baseline && clockMeta == lastClockMeta_ &&
			now - lastClockEmit_ < std::chrono::milliseconds(100);
		if (!clockThrottled && (baseline || clock != lastClock_))
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
		const bool throttled = playing && !forcePlayerEvent_ && !baseline &&
			now - lastPlayerEmit_ < std::chrono::milliseconds(100);
		if (!throttled && (baseline || forcePlayerEvent_ || player != lastPlayer_))
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
