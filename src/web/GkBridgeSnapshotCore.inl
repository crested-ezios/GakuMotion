namespace GakuMotion::webpanel
{
	inline std::string Bridge::BuildHello() const
	{
		std::string methods = "[";
		for (size_t i = 0; i < std::size(kSupportedMethods); ++i)
		{
			if (i) methods += ",";
			methods += JString(kSupportedMethods[i]);
		}
		methods += "]";
		std::string events = "[";
		for (size_t i = 0; i < std::size(kSupportedEvents); ++i)
		{
			if (i) events += ",";
			events += JString(kSupportedEvents[i]);
		}
		events += "]";
		return "{\"protocol\":1,\"sessionId\":" + JNum(static_cast<double>(sessionId_)) +
			",\"pluginVersion\":" + JString(hooks_.pluginVersion) +
			",\"supportedMethods\":" + methods + ",\"supportedEvents\":" + events + "}";
	}

	inline std::string Bridge::PlayerStateJsonLocked() const
	{
		const auto& player = core_.player;
		const char* state = "stopped";
		switch (player.state)
		{
		case PlaybackState::Playing: state = "playing"; break;
		case PlaybackState::Paused: state = "paused"; break;
		case PlaybackState::Preview: state = "preview"; break;
		default: break;
		}
		std::string title = "未加载曲目";
		if (!core_.loadedMotionKey.empty())
		{
			const int index = FindPlaylistIndex(core_.motions, core_.loadedMotionKey);
			title = index >= 0 && !core_.motions[index].title.empty()
				? core_.motions[index].title : core_.loadedMotionKey;
		}
		const int playMode = static_cast<int>(core_.playlist.mode);
		return "{\"state\":" + JString(state) + ",\"title\":" + JString(title) +
			",\"time\":" + JNum(player.currentTime) +
			",\"duration\":" + JNum(player.TimelineDuration()) +
			",\"totalFrames\":" + JNum(player.clip.total_frames) +
			",\"fps\":" + JNum(player.clip.fps) +
			",\"speed\":" + JNum(player.playbackSpeedMultiplier) +
			",\"volume\":" + JNum(player.musicVolume) +
			",\"playMode\":" + JNum(playMode) + "}";
	}

	inline std::string Bridge::PermissionJsonLocked() const
	{
		const bool allowed = core_.InjectionAllowed();
		const std::string notice = core_.injectionStatus.empty()
			? "仅偶像摄影与更衣室可以使用；其他页面保留设置但不生效。" : core_.injectionStatus;
		return "{\"injectionAllowed\":" + JBool(allowed) + ",\"notice\":" + JString(notice) + "}";
	}

	inline std::string Bridge::LibraryJsonLocked() const
	{
		std::string entries = "[";
		for (size_t i = 0; i < core_.motions.size(); ++i)
		{
			const auto& entry = core_.motions[i];
			if (i) entries += ",";
			const int posCount = entry.isGroup || !entry.positions.empty()
				? (std::max)(entry.positionCount, static_cast<int>(entry.positions.size()))
				: (std::max)(1, entry.positionCount);
			entries += "{\"id\":" + JString(entry.name) + ",\"title\":" + JString(entry.title) +
				",\"subtitle\":" + JString(entry.subtitle) +
				",\"directory\":" + JString(entry.dir) +
				",\"durationSeconds\":" + JNum(entry.durationSeconds) +
				",\"totalFrames\":" + JNum(entry.totalFrames) +
				",\"hasMotion\":true,\"hasFacial\":" + JBool(entry.hasFacial) +
				",\"hasCamera\":" + JBool(entry.hasCamera) +
				",\"hasAudio\":" + JBool(entry.hasAudio) +
				",\"audioFile\":" + JString(entry.audioFile) +
				",\"hasCover\":" + JBool(entry.hasCover) +
				",\"isGroup\":" + JBool(entry.isGroup) +
				",\"positionCount\":" + JNum(posCount) +
				",\"positions\":" + MotionPositionsJson(entry.positions) + "}";
		}
		entries += "]";
		return "{\"revision\":" + JNum(static_cast<double>(core_.libraryRevision)) +
			",\"entries\":" + entries +
			",\"refreshing\":" + JBool(core_.libraryScan.valid()) +
			",\"notice\":" + JString(core_.playlistNotice) + "}";
	}

	inline std::string Bridge::LoadedJsonLocked() const
	{
		return "{\"motionKey\":" + JString(core_.loadedMotionKey) +
			",\"generation\":" + JNum(static_cast<double>(core_.loadedMotionGeneration)) +
			",\"injectionSession\":" + JNum(static_cast<double>(core_.injectionSession)) +
			",\"sourceMissing\":" + JBool(core_.loadedSourceMissing) +
			",\"sourceChanged\":" + JBool(core_.loadedSourceChanged) + "}";
	}

	inline std::string Bridge::PlaylistJsonLocked() const
	{
		const int mode = static_cast<int>(core_.playlist.mode);
		return "{\"mode\":" + JNum(mode) + ",\"notice\":" + JString(core_.playlistNotice) + "}";
	}

	inline std::string Bridge::ActorsJsonLocked() const
	{
		const auto& entries = core_.sceneActorRegistry.Entries();
		std::string actors = "[";
		for (size_t i = 0; i < entries.size(); ++i)
		{
			const auto& item = entries[i];
			const std::string label = FormatSceneActorLabel(item, i + 1);
			std::string displayName = item.displayName;
			if (displayName.empty() && !item.character.empty())
			{
				if (const char* cn = LookupIdolChineseName(item.character.c_str()))
					displayName = cn;
			}
			if (displayName.empty()) displayName = item.presetName;
			const std::string shortName = !item.shortName.empty()
				? item.shortName
				: (!item.character.empty() || !item.displayName.empty() || !item.presetName.empty()
					? ResolveSceneActorShortName(item)
					: std::string());
			if (i) actors += ",";
			actors += "{\"token\":" + JNum(static_cast<double>(item.token)) +
				",\"label\":" + JString(label) +
				",\"character\":" + JString(item.character) +
				",\"displayName\":" + JString(displayName) +
				",\"shortName\":" + JString(shortName) +
				",\"selected\":" + JBool(item.actor == core_.player.controller.targetActor) + "}";
		}
		actors += "]";
		const auto labelFor = [&](void* actor) -> std::string {
			for (size_t i = 0; i < entries.size(); ++i)
				if (entries[i].actor == actor) return FormatSceneActorLabel(entries[i], i + 1);
			return "尚未绑定角色";
		};
		return "{\"epoch\":" + JNum(static_cast<double>(core_.sceneActorRegistry.Epoch())) +
			",\"actors\":" + actors +
			",\"targetLabel\":" + JString(labelFor(core_.player.controller.targetActor)) +
			",\"boundLabel\":" + JString(labelFor(core_.player.controller.currentBinding.actor)) + "}";
	}

	inline std::string Bridge::FormationJsonLocked() const
	{
		const auto& fm = core_.formation;
		std::string slotsJson = "[";
		for (size_t i = 0; i < fm.slots.size(); ++i)
		{
			const auto& s = fm.slots[i];
			if (i) slotsJson += ",";
			const bool hasTrackMotion = !core_.loadedMotionKey.empty() && !s.isOverflow;
			slotsJson += "{\"slotIndex\":" + JNum(s.slotIndex) +
				",\"occupied\":" + JBool(s.occupied) +
				",\"actorToken\":" + JNum(static_cast<double>(s.actorToken)) +
				",\"actorInstanceId\":" + JNum(s.actorInstanceId) +
				",\"character\":" + JString(s.character) +
				",\"shortName\":" + JString(s.shortName) +
				",\"displayName\":" + JString(s.displayName) +
				",\"isOverflow\":" + JBool(s.isOverflow) +
				",\"hasTrackMotion\":" + JBool(hasTrackMotion) +
				",\"sourceSlotIndex\":" + JNum(s.sourceSlotIndex) +
				",\"placementDirty\":" + JBool(s.placementDirty) +
				",\"hasActorBoneOverrides\":" + JBool(s.hasActorBoneOverrides) +
				",\"physicsBackend\":" + JString(s.physicsBackend) +
				",\"offset\":{\"x\":" + JNum(s.offset.x) +
				",\"y\":" + JNum(s.offset.y) +
				",\"z\":" + JNum(s.offset.z) +
				",\"yawDegrees\":" + JNum(s.yawDegrees) + "}" +
				",\"facialWeights\":{\"overall\":" + JNum(s.facialWeights.overall) +
				",\"brow\":" + JNum(s.facialWeights.brow) +
				",\"eye\":" + JNum(s.facialWeights.eye) +
				",\"mouth\":" + JNum(s.facialWeights.mouth) + "}}";
		}
		slotsJson += "]";
		return "{\"active\":" + JBool(fm.active) +
			",\"available\":" + JBool(fm.available) +
			",\"verified\":" + JBool(fm.verified) +
			",\"isGroupSong\":" + JBool(fm.isGroupSong) +
			",\"trackPositionCount\":" + JNum(fm.trackPositionCount) +
			",\"slotCount\":" + JNum(fm.slotCount) +
			",\"selectedSlotIndex\":" + JNum(fm.selectedSlotIndex) +
			",\"revision\":" + JNum(static_cast<double>(fm.revision)) +
			",\"actorEpoch\":" + JNum(static_cast<double>(fm.actorEpoch)) +
			",\"notice\":" + JString(fm.notice) +
			",\"slots\":" + slotsJson + "}";
	}

	inline std::string Bridge::CameraTransformJson(const CameraModeTransform& transform)
	{
		return "{\"position\":" + JVec(transform.position) + ",\"rotation\":" + JVec(transform.rotation) + "}";
	}

	inline std::string Bridge::CameraSettingsJson(const CameraSettings& settings)
	{
		return "{\"enabled\":" + JBool(settings.enabled) +
			",\"mode\":" + JNum(static_cast<int>(settings.mode)) +
			",\"followBehavior\":" + JNum(static_cast<int>(settings.followBehavior)) +
			",\"followTarget\":" + JNum(static_cast<int>(settings.followTarget)) +
			",\"followSlotIndex\":" + JNum(settings.followSlotIndex) +
			",\"followActor\":" + JBool(settings.followActor) +
			",\"limitToScene\":" + JBool(settings.limitToScene) +
			",\"boundsMin\":" + JVec(settings.boundsMin) +
			",\"boundsMax\":" + JVec(settings.boundsMax) +
			",\"followOffset\":" + JVec(settings.followOffset) +
			",\"targetOffset\":" + JVec(settings.targetOffset) +
			",\"vmdOffset\":" + JVec(settings.vmdOffset) +
			",\"vmdRangeScale\":" + JNum(settings.vmdRangeScale) +
			",\"followFactor\":" + JVec(settings.followFactor) +
			",\"followRate\":" + JVec(settings.followRate) +
			",\"followFov\":" + JNum(settings.followFov) +
			",\"srpFinalOverwriteFallback\":" + JBool(settings.srpFinalOverwriteFallback) +
			",\"actorShadowExperimentMode\":" + JNum(settings.actorShadowExperimentMode) +
			",\"transformTrack\":" + CameraTransformJson(settings.transformTrack) +
			",\"transformFollowMove\":" + CameraTransformJson(settings.transformFollowMove) +
			",\"transformLookAt\":" + CameraTransformJson(settings.transformLookAt) +
			",\"usePerModeTransforms\":" + JBool(settings.usePerModeTransforms) + "}";
	}

	inline std::string Bridge::CameraJsonLocked() const
	{
		const auto snapshot = core_.camera.ReadSnapshot();
		return "{\"settings\":" + CameraSettingsJson(snapshot.settings) +
			",\"available\":" + JBool(snapshot.available) +
			",\"owned\":" + JBool(snapshot.owned) +
			",\"hasTrack\":" + JBool(snapshot.hasTrack) +
			",\"error\":" + JString(snapshot.error) +
			",\"followTargetError\":" + JString(snapshot.followTargetError) +
			",\"notice\":" + JString(core_.cameraNotice) + "}";
	}

	inline std::string Bridge::StageSettingsJson(const StageSettings& settings)
	{
		std::string color = "[";
		for (int i = 0; i < 4; ++i)
		{
			if (i) color += ",";
			color += JNum(settings.greenScreenColor[i]);
		}
		color += "]";
		return "{\"hideBackground\":" + JBool(settings.hideBackground) +
			",\"greenScreenActive\":" + JBool(settings.greenScreenActive) +
			",\"greenScreenColor\":" + color +
			",\"screenDistance\":" + JNum(settings.screenDistance) +
			",\"screenScale\":" + JNum(settings.screenScale) +
			",\"screenHeightOffset\":" + JNum(settings.screenHeightOffset) +
			",\"overrideCameraClear\":" + JBool(settings.overrideCameraClear) +
			",\"selectedPresetIndex\":" + JNum(settings.selectedPresetIndex) +
			",\"customStageKey\":" + JString(settings.customStageKey) +
			",\"loadReplacementKey\":" + JString(settings.loadReplacementKey) +
			",\"stageFloorYOffset\":" + JNum(settings.stageFloorYOffset) +
			",\"stageYawOffset\":" + JNum(settings.stageYawOffset) +
			",\"stagePosXOffset\":" + JNum(settings.stagePosXOffset) +
			",\"stagePosZOffset\":" + JNum(settings.stagePosZOffset) +
			",\"lightingEffectsEnabled\":" + JBool(settings.lightingEffectsEnabled) +
			",\"customNodeControl\":" + JBool(settings.customNodeControl) + "}";
	}

	inline std::string Bridge::StageNodesJsonLocked() const
	{
		std::vector<StageCandidate> candidates;
		{
			std::lock_guard<std::mutex> stageLock(core_.stageManager.stageMutex);
			candidates = core_.stageManager.candidates;
		}
		std::string nodes = "[";
		for (size_t i = 0; i < candidates.size(); ++i)
		{
			const auto& candidate = candidates[i];
			if (i) nodes += ",";
			nodes += "{\"index\":" + JNum(static_cast<double>(i)) +
				",\"instanceId\":" + JNum(candidate.instanceId) +
				",\"name\":" + JString(candidate.name) +
				",\"displayName\":" + JString(candidate.displayName) +
				",\"parentIndex\":" + JNum(candidate.parentIndex) +
				",\"depth\":" + JNum(candidate.depth) +
				",\"hasChildren\":" + JBool(candidate.hasChildren) +
				",\"originalActive\":" + JBool(candidate.originalActive) +
				",\"currentlyActive\":" + JBool(candidate.currentlyActive) +
				",\"userMuted\":" + JBool(candidate.userMuted) +
				",\"isIdentifiedBackground\":" + JBool(candidate.isIdentifiedBackground) +
				",\"isColliderLocked\":" + JBool(candidate.isColliderLocked) +
				",\"isLightingRoot\":" + JBool(candidate.isLightingRoot) +
				",\"isLightingNode\":" + JBool(candidate.isLightingNode) + "}";
		}
		nodes += "]";
		return nodes;
	}

	inline std::string Bridge::StageJsonWithNodesLocked(const std::string& nodesJson) const
	{
		StageSettings settings;
		std::string sceneName, statusNotice, mountedKey;
		{
			std::lock_guard<std::mutex> stageLock(core_.stageManager.stageMutex);
			settings = core_.stageManager.settings;
			sceneName = core_.stageManager.currentSceneName;
			statusNotice = core_.stageManager.statusNotice;
			mountedKey = core_.stageManager.mountedStageKey;
		}
		return "{\"settings\":" + StageSettingsJson(settings) +
			",\"sceneName\":" + JString(sceneName) +
			",\"statusNotice\":" + JString(statusNotice) +
			",\"mountedKey\":" + JString(mountedKey) +
			",\"loadNotice\":" + JString(core_.stageLoadNotice) +
			",\"candidates\":" + nodesJson + "}";
	}

	inline std::string Bridge::StageJsonLocked() const
	{
		return StageJsonWithNodesLocked(StageNodesJsonLocked());
	}
} // namespace GakuMotion::webpanel
