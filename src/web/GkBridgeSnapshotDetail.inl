namespace GakuMotion::webpanel
{
	inline std::string Bridge::FacialJsonLocked() const
	{
		const auto& weights = core_.player.controller.facialWeights;
		return "{\"weights\":{\"overall\":" + JNum(weights.overall) +
			",\"brow\":" + JNum(weights.brow) + ",\"eye\":" + JNum(weights.eye) +
			",\"mouth\":" + JNum(weights.mouth) + "},\"notice\":" + JString(core_.facialWeightsNotice) + "}";
	}

	inline std::string Bridge::PositionJsonLocked() const
	{
		return "{\"offset\":" + JVec(core_.player.controller.initialPositionOffset) +
			",\"notice\":" + JString(core_.initialPositionNotice) + "}";
	}

	inline std::string Bridge::ImportProgressJsonLocked() const
	{
		const auto& progress = core_.importProgress;
		if (!progress.active && !progress.finished) return "null";
		std::string json = "{\"active\":" + JBool(progress.active) +
			",\"finished\":" + JBool(progress.finished) +
			",\"succeeded\":" + JBool(progress.succeeded) +
			",\"progress\":" + JNum(progress.progress) +
			",\"stage\":" + JString(progress.stage) +
			",\"statusText\":" + JString(progress.statusText) +
			",\"errorDetail\":" + JString(progress.errorDetail);
		if (progress.finished && progress.succeeded && !core_.lastImportedKey.empty())
		{
			json += ",\"importedKey\":" + JString(core_.lastImportedKey);
		}
		json += "}";
		return json;
	}

	inline std::string Bridge::GazeJsonLocked() const
	{
		const auto& lookAt = core_.lookAt;
		const std::string settings = "{\"mode\":" + JNum(static_cast<int>(lookAt.gazeMode.load())) +
			",\"desktopLookAtEnabled\":" + JBool(lookAt.desktopLookAtEnabled.load()) +
			",\"eyeWeight\":" + JNum(lookAt.eyeWeight.load()) +
			",\"headWeight\":" + JNum(lookAt.headWeight.load()) +
			",\"bodyWeight\":" + JNum(lookAt.bodyWeight.load()) + "}";
		return "{\"settings\":" + settings +
			",\"runtimeAvailable\":" + JBool(GazeRuntimeAvailable()) +
			",\"vrifyQueryAvailable\":" + JBool(lookAt.fnQueryVRifyGaze != nullptr) +
			",\"vrifyLiveGazeActive\":" + JBool(lookAt.IsVRifyLiveGazeActive()) +
			",\"notice\":" + JString(gazeNotice_) + "}";
	}

	inline std::string Bridge::BoneJsonLocked() const
	{
		const auto& overrides = core_.effectiveOverrides;
		std::string entries = "[";
		for (size_t i = 0; i < overrides.offsets.size(); ++i)
		{
			const auto& entry = overrides.offsets[i];
			if (i) entries += ",";
			entries += "{\"humanoidId\":" + JNum(entry.humanoidId) +
				",\"boneName\":" + JString(entry.boneName) +
				",\"enabled\":" + JBool(entry.enabled) +
				",\"spaceWorld\":" + JBool(entry.spaceWorld) +
				",\"eulerDeg\":" + JVec(entry.eulerDeg) +
				",\"isDynamic\":" + JBool(entry.isDynamic) + "}";
		}
		entries += "]";
		return "{\"masterEnabled\":" + JBool(core_.masterOverridesEnabled) +
			",\"overrides\":" + entries +
			",\"notice\":" + JString(boneNotice_) + "}";
	}

	inline std::string Bridge::SoleSettingsJson(const SoleMeasurementSettings& settings)
	{
		std::string points = "[";
		for (size_t side = 0; side < 2; ++side)
		{
			if (side) points += ",";
			points += "[";
			for (size_t i = 0; i < 3; ++i)
			{
				if (i) points += ",";
				points += JVec(settings.points[side][i]);
			}
			points += "]";
		}
		points += "]";
		return "{\"enabled\":" + JBool(settings.enabled) +
			",\"planeCalibrated\":" + JBool(settings.planeCalibrated) +
			",\"shoeCalibrated\":" + JBool(settings.shoeCalibrated) +
			",\"useBootProxy\":" + JBool(settings.useBootProxy) +
			",\"planeNormal\":" + JVec(settings.planeNormal) +
			",\"planeOffsetM\":" + JNum(settings.planeOffsetM) +
			",\"displayBandM\":" + JNum(settings.displayBandM) +
			",\"points\":" + points + "}";
	}

	inline std::string Bridge::SoleFootJson(const SoleFootMeasurement& foot)
	{
		std::string points = "[";
		for (size_t i = 0; i < foot.points.size(); ++i)
		{
			if (i) points += ",";
			const auto& point = foot.points[i];
			points += "{\"world\":" + JVec(point.world) +
				",\"signedDistanceM\":" + JNum(point.signedDistanceM) +
				",\"penetrationM\":" + JNum(point.penetrationM) + "}";
		}
		points += "]";
		std::string patches = "[";
		for (size_t p = 0; p < foot.patches.size(); ++p)
		{
			if (p) patches += ",";
			const auto& patch = foot.patches[p];
			std::string corners = "[";
			for (size_t c = 0; c < patch.corners.size(); ++c)
			{
				if (c) corners += ",";
				const auto& corner = patch.corners[c];
				corners += "{\"world\":" + JVec(corner.world) +
					",\"signedDistanceM\":" + JNum(corner.signedDistanceM) +
					",\"penetrationM\":" + JNum(corner.penetrationM) + "}";
			}
			corners += "]";
			patches += "{\"corners\":" + corners +
				",\"minimumDistanceM\":" + JNum(patch.minimumDistanceM) +
				",\"maximumDistanceM\":" + JNum(patch.maximumDistanceM) +
				",\"maximumPenetrationM\":" + JNum(patch.maximumPenetrationM) + "}";
		}
		patches += "]";
		return "{\"valid\":" + JBool(foot.valid) +
			",\"usingBootProxy\":" + JBool(foot.usingBootProxy) +
			",\"pose\":{\"valid\":" + JBool(foot.pose.valid) +
			",\"hip\":" + JVec(foot.pose.hip) +
			",\"knee\":" + JVec(foot.pose.knee) +
			",\"ankle\":" + JVec(foot.pose.ankle) +
			",\"toe\":" + JVec(foot.pose.toe) +
			",\"footRotation\":" + JQuat(foot.pose.footRotation) + "}" +
			",\"points\":" + points + ",\"patches\":" + patches +
			",\"minimumDistanceM\":" + JNum(foot.minimumDistanceM) +
			",\"maximumPenetrationM\":" + JNum(foot.maximumPenetrationM) + "}";
	}

	inline std::string Bridge::SoleJsonLocked() const
	{
		const auto& snapshot = core_.soleSnapshot;
		const auto* actor = core_.sceneActorRegistry.Find(snapshot.actor);
		std::string feet = "[";
		for (size_t i = 0; i < snapshot.feet.size(); ++i)
		{
			if (i) feet += ",";
			feet += SoleFootJson(snapshot.feet[i]);
		}
		feet += "]";
		return "{\"settings\":" + SoleSettingsJson(core_.soleSettings) +
			",\"generation\":" + JNum(static_cast<double>(core_.soleCalibrationGeneration)) +
			",\"notice\":" + JString(core_.soleNotice) +
			",\"snapshot\":{\"available\":" + JBool(snapshot.available) +
			",\"calibrated\":" + JBool(snapshot.calibrated) +
			",\"actorToken\":" + JNum(static_cast<double>(actor ? actor->token : 0)) +
			",\"clipTime\":" + JNum(snapshot.clipTime) +
			",\"feet\":" + feet + "}}";
	}

	inline std::string Bridge::SolePitchSettingsJson(const sole_pitch::Settings& settings)
	{
		return "{\"enabled\":" + JBool(settings.experimentalSolePitchCorrection) +
			",\"maxPitchDegrees\":" + JNum(settings.maxPitchDegrees) +
			",\"useMeshEnvelope\":" + JBool(settings.useMeshEnvelope) + "}";
	}

	inline std::string Bridge::SolePitchJsonLocked() const
	{
		const auto& snapshot = core_.experimentalSolePitchSnapshot;
		std::string feet = "[";
		for (size_t i = 0; i < snapshot.feet.size(); ++i)
		{
			const auto& foot = snapshot.feet[i];
			if (i) feet += ",";
			const auto& solution = foot.solution;
			feet += "{\"freshPose\":" + JBool(foot.freshPose) +
				",\"applied\":" + JBool(foot.applied) +
				",\"setterReadbackMatches\":" + JBool(foot.setterReadbackMatches) +
				",\"measuredAfterMinimumDistanceM\":" + JNum(foot.measuredAfterMinimumDistanceM) +
				",\"measuredAfterValid\":" + JBool(foot.measuredAfterValid) +
				",\"status\":" + JString(foot.status ? foot.status : "") +
				",\"solution\":{\"valid\":" + JBool(solution.valid) +
				",\"actionable\":" + JBool(solution.actionable) +
				",\"fullyCleared\":" + JBool(solution.fullyCleared) +
				",\"usedMeshEnvelope\":" + JBool(solution.usedMeshEnvelope) +
				",\"localAxis\":" + JVec(solution.localAxis) +
				",\"pitchDegrees\":" + JNum(solution.pitchDegrees) +
				",\"localDelta\":" + JQuat(solution.localDelta) +
				",\"correctedWorldRotation\":" + JQuat(solution.correctedWorldRotation) +
				",\"beforeMinimumDistanceM\":" + JNum(solution.beforeMinimumDistanceM) +
				",\"afterMinimumDistanceM\":" + JNum(solution.afterMinimumDistanceM) +
				",\"beforePenetrationM\":" + JNum(solution.beforePenetrationM) +
				",\"afterPenetrationM\":" + JNum(solution.afterPenetrationM) + "}" +
				",\"after\":" + SoleFootJson(foot.after) + "}";
		}
		feet += "]";
		return "{\"settings\":" + SolePitchSettingsJson(core_.experimentalSolePitchSettings) +
			",\"clipTime\":" + JNum(snapshot.clipTime) +
			",\"feet\":" + feet +
			",\"notice\":" + JString(solePitchNotice_) + "}";
	}

	inline std::string Bridge::StudioSettingsJson(const studio::Settings& settings)
	{
		const auto& l = settings.lighting;
		const auto& v = settings.video;
		return "{\"lighting\":{\"enabled\":" + JBool(l.enabled) +
			",\"actorStyle\":" + JBool(l.actorStyle) +
			",\"decalEnabled\":" + JBool(l.decalEnabled) +
			",\"keyColor\":" + JColor3(l.keyColor) +
			",\"fillColor\":" + JColor3(l.fillColor) +
			",\"rimColor\":" + JColor3(l.rimColor) +
			",\"decalTint\":" + JColor3(l.decalTint) +
			",\"keyIntensity\":" + JNum(l.keyIntensity) +
			",\"fillIntensity\":" + JNum(l.fillIntensity) +
			",\"rimIntensity\":" + JNum(l.rimIntensity) +
			",\"keyYaw\":" + JNum(l.keyYaw) + ",\"keyPitch\":" + JNum(l.keyPitch) +
			",\"fillYaw\":" + JNum(l.fillYaw) + ",\"fillPitch\":" + JNum(l.fillPitch) +
			",\"rimYaw\":" + JNum(l.rimYaw) + ",\"rimPitch\":" + JNum(l.rimPitch) +
			",\"actorIntensity\":" + JNum(l.actorIntensity) +
			",\"rimPower\":" + JNum(l.rimPower) +
			",\"decalIntensityScale\":" + JNum(l.decalIntensityScale) + "}" +
			",\"video\":{\"enabled\":" + JBool(v.enabled) +
			",\"paused\":" + JBool(v.paused) + ",\"loop\":" + JBool(v.loop) +
			",\"path\":" + JString(v.path) +
			",\"distance\":" + JNum(v.distance) + ",\"width\":" + JNum(v.width) +
			",\"offsetX\":" + JNum(v.offsetX) + ",\"offsetY\":" + JNum(v.offsetY) + "}}";
	}

	inline std::string Bridge::StudioJsonLocked() const
	{
		return "{\"settings\":" + StudioSettingsJson(core_.studioSettings) +
			",\"revision\":" + JNum(static_cast<double>(core_.studioRevision)) +
			",\"lightingNotice\":" + JString(core_.studioLightingNotice) +
			",\"videoNotice\":" + JString(core_.studioVideoNotice) + "}";
	}

	inline const char* Bridge::MikuStatusText(MikuFitDecision decision)
	{
		switch (decision)
		{
		case MikuFitDecision::Applied: return "已应用";
		case MikuFitDecision::ExternalApplied: return "已应用（Rinha 发布配置）";
		case MikuFitDecision::ExternalUnverified: return "已回退：该配置未经实测验收";
		case MikuFitDecision::MissingRawSource: return "已回退：旧曲目缺原始五元音";
		case MikuFitDecision::MissingRawEyeSource: return "已回退：曲目缺原始眼眉源（需重新导入）";
		case MikuFitDecision::InvalidRawSource: return "已回退：源数据不完整";
		case MikuFitDecision::UnsupportedSource: return "已回退：源嘴形未覆盖";
		case MikuFitDecision::UnsupportedEyeSource: return "已回退：源眼眉未覆盖";
		case MikuFitDecision::NativeAuthored: return "已回退：保留显式原生通道";
		case MikuFitDecision::UnknownFace: return "已回退：实际脸型未匹配";
		case MikuFitDecision::CharacterMismatch: return "已回退：角色身份不一致";
		case MikuFitDecision::MissingOutput: return "已回退：目标通道不完整";
		case MikuFitDecision::OutsideDomain: return "已回退：源混合超出拟合范围";
		case MikuFitDecision::InvalidModel: return "已回退：拟合数据无效";
		case MikuFitDecision::Disabled: return "已关闭";
		}
		return "未知状态";
	}

	inline std::string Bridge::ExperimentsJsonLocked() const
	{
		const auto& controller = core_.player.controller;
		const unsigned snapshot = controller.guiMikuFitSnapshot.load(std::memory_order_relaxed);
		const auto& config = core_.config;
		const bool mouth = config.facialMikuMouthFit >= 0 ? config.facialMikuMouthFit != 0 : controller.facialMikuMouthFit;
		const bool eye = config.facialMikuEyeFit >= 0 ? config.facialMikuEyeFit != 0 : controller.facialMikuEyeFit;
		const bool allow = config.facialMikuFitAllowUnverified >= 0 ? config.facialMikuFitAllowUnverified != 0 : controller.facialMikuFitAllowUnverified;
		const unsigned fittedRole = (snapshot >> 8) & 0xff;
		const auto fittedDecision = static_cast<MikuFitDecision>(snapshot & 0xff);
		const auto eyeDecision = static_cast<MikuFitDecision>((snapshot >> 16) & 0xff);
		const bool playing = core_.player.state != PlaybackState::Stopped;
		const std::string mouthFitStatus = !mouth ? "已关闭" : playing ? MikuStatusText(fittedDecision) : "尚未接管";
		const std::string eyeFitStatus = !eye ? "已关闭" : playing ? MikuStatusText(eyeDecision) : "尚未接管";
		std::string fittedFaceLabel;
		if (fittedRole > 0 && fittedRole <= kMikuMouthFits.size())
			fittedFaceLabel = FormatIdolDisplayName(kMikuMouthFits[fittedRole - 1].character) + "（base-0000）";
		else if (fittedRole == kMikuMouthFits.size() + 1)
			fittedFaceLabel = "发布配置（RinhaReplacement）";
		const std::string settingsJson = "{\"mikuMouthFit\":" + JBool(mouth) +
			",\"mikuEyeFit\":" + JBool(eye) +
			",\"mikuFitAllowUnverified\":" + JBool(allow) +
			",\"waistSupport\":true" +
			",\"beforeQuartz\":" + JBool(core_.guiPhysicsBackend.load() == 2) +
			",\"muteNativeSoundEffects\":" + JBool(core_.guiMuteNativeSoundEffects.load()) + "}";
		return "{\"settings\":" + settingsJson +
			",\"mouthFitStatus\":" + JString(mouthFitStatus) +
			",\"eyeFitStatus\":" + JString(eyeFitStatus) +
			",\"fittedFaceLabel\":" + JString(fittedFaceLabel) +
			",\"notice\":" + JString(experimentsNotice_) + "}";
	}

	inline std::string Bridge::TelemetryJsonLocked() const
	{
		const auto& player = core_.player;
		const auto& controller = player.controller;
		const auto* actor = core_.sceneActorRegistry.Find(controller.currentBinding.actor);
		const auto& entries = core_.sceneActorRegistry.Entries();
		std::string actorLabel = "尚未绑定角色";
		for (size_t i = 0; i < entries.size(); ++i)
		{
			if (entries[i].actor == controller.currentBinding.actor)
			{
				const auto& item = entries[i];
				const std::string name = !item.displayName.empty()
					? item.displayName + " (" + item.character + ")"
					: item.character.empty() ? "未识别角色" : FormatIdolDisplayName(item.character.c_str());
				actorLabel = name + " · " + std::to_string(i + 1);
				break;
			}
		}
		const char* stateName = "已停止";
		switch (player.state)
		{
		case PlaybackState::Playing: stateName = "播放中"; break;
		case PlaybackState::Paused: stateName = "已暂停"; break;
		case PlaybackState::Preview: stateName = "预览中"; break;
		default: break;
		}
		const auto& gui = core_.gui;
		return "{\"statusLine\":" + JString(core_.StatusSummary()) +
			",\"player\":{\"state\":" + JNum(static_cast<int>(player.state)) +
			",\"stateName\":" + JString(stateName) +
			",\"time\":" + JNum(player.currentTime) +
			",\"duration\":" + JNum(player.TimelineDuration()) +
			",\"totalFrames\":" + JNum(player.clip.total_frames) +
			",\"fps\":" + JNum(player.clip.fps) +
			",\"boneCount\":" + JNum(player.clip.bone_count) +
			",\"loadedMotionFile\":" + JString(player.loadedMotionFile) +
			",\"seekCount\":" + JNum(player.SeekCount()) +
			",\"audioSyncOffsetSeconds\":" + JNum(player.AudioSyncOffsetSeconds()) +
			",\"lastClockSource\":" + JString(player.lastClockSource ? player.lastClockSource : "") + "}" +
			",\"actor\":{\"token\":" + JNum(static_cast<double>(actor ? actor->token : 0)) +
			",\"actorLabel\":" + JString(actorLabel) +
			",\"boundBoneCount\":" + JNum(static_cast<double>(controller.currentBinding.boundBoneCount)) +
			",\"totalBoneCount\":" + JNum(static_cast<double>(controller.currentBinding.boneTransforms.size())) + "}" +
			",\"scrub\":{\"begin\":" + JNum(gui.probeScrubBegin) +
			",\"update\":" + JNum(gui.probeScrubUpdate) +
			",\"end\":" + JNum(gui.probeScrubEnd) +
			",\"lastValue\":" + JNum(gui.probeLastScrubValue) +
			",\"active\":" + JBool(gui.scrub.active) + "}" +
			",\"overrides\":{\"masterEnabled\":" + JBool(core_.masterOverridesEnabled) +
			",\"effectiveCount\":" + JNum(static_cast<double>(core_.effectiveOverrides.offsets.size())) + "}" +
			",\"gazeMode\":" + JNum(static_cast<int>(core_.lookAt.gazeMode.load())) +
			",\"physicsBackend\":" + JNum(core_.guiPhysicsBackend.load()) + "}";
	}

	inline std::string Bridge::BuildAppearanceJson() const
	{
		AppearanceInfo info;
		if (hooks_.appearance) info = hooks_.appearance();
		return "{\"theme\":" + JNum(info.theme) +
			",\"uiScale\":" + JNum(info.uiScale) +
			",\"dpiScale\":1,\"scaleLimited\":false}";
	}

	inline std::string Bridge::ClockJsonLocked(std::string* outMeta) const
	{
		GakuMotionClockSnapshotV1 snap{};
		core_.ReadClockSnapshot(&snap, false);
		const char* rm = snap.runMode == GAKUMOTION_RUN_RECORDING ? "recording" : "realtime";
		auto srcName = [](uint32_t s) -> const char*
		{
			switch (s)
			{
			case GAKUMOTION_CLOCK_ENGINE_DELTA: return "engine_delta";
			case GAKUMOTION_CLOCK_FIXED_STEP: return "fixed_step";
			case GAKUMOTION_CLOCK_EXTERNAL: return "external";
			default: return "audio";
			}
		};
		std::string meta = "{\"runMode\":" + JString(rm) +
			",\"configuredSource\":" + JString(srcName(snap.configuredSource)) +
			",\"effectiveSource\":" + JString(srcName(snap.effectiveSource)) +
			",\"fpsNumerator\":" + JNum(snap.fpsNumerator) +
			",\"fpsDenominator\":" + JNum(snap.fpsDenominator) +
			",\"waitingForRenderComplete\":" + JBool(snap.waitingForRenderComplete != 0) +
			",\"leaseId\":" + JNum(static_cast<double>(snap.leaseId)) +
			",\"leaseOwner\":" + JString(snap.leaseOwner);
		if (outMeta) *outMeta = meta;
		return meta + ",\"evaluatedFrameSerial\":" + JNum(static_cast<double>(snap.evaluatedFrameSerial)) + "}";
	}

	inline std::string Bridge::BuildAudioEffectJson() const
	{
		AudioEffectState st;
		if (hooks_.getAudioEffectsState) st = hooks_.getAudioEffectsState();
		if (st.activeId.empty()) st.activeId = "flat";
		if (st.presets.empty())
			st.presets.push_back({"flat", "原声", "flat", "直通输出"});
		std::string list = "[";
		for (size_t i = 0; i < st.presets.size(); ++i)
		{
			if (i) list += ",";
			const auto& p = st.presets[i];
			list += "{\"id\":" + JString(p.id) +
				",\"name\":" + JString(p.name) +
				",\"category\":" + JString(p.category) +
				",\"description\":" + JString(p.description) + "}";
		}
		list += "]";
		return "{\"available\":" + JBool(st.available) +
			",\"activeId\":" + JString(st.activeId) +
			",\"presets\":" + list + "}";
	}

	inline std::string Bridge::BuildSnapshotLocked() const
	{
		const std::string stageNodes = StageNodesJsonLocked();
		return "{\"permission\":" + PermissionJsonLocked() +
			",\"player\":" + PlayerStateJsonLocked() +
			",\"library\":" + LibraryJsonLocked() +
			",\"loaded\":" + LoadedJsonLocked() +
			",\"playlist\":" + PlaylistJsonLocked() +
			",\"actors\":" + ActorsJsonLocked() +
			",\"formation\":" + FormationJsonLocked() +
			",\"camera\":" + CameraJsonLocked() +
			",\"stage\":" + StageJsonWithNodesLocked(stageNodes) +
			",\"stageNodes\":" + stageNodes +
			",\"facial\":" + FacialJsonLocked() +
			",\"position\":" + PositionJsonLocked() +
			",\"physicsBackend\":" + JNum(core_.guiPhysicsBackend.load()) +
			",\"musicVolumeSupport\":" + JNum(core_.player.fnAudioVolumeSupport ? core_.player.fnAudioVolumeSupport() : 0) +
			",\"musicVolumeNotice\":" + JString(core_.musicVolumeNotice) +
			",\"importProgress\":" + ImportProgressJsonLocked() +
			",\"motionsRoot\":" + JString(core_.motionsRoot) +
			",\"gaze\":" + GazeJsonLocked() +
			",\"bone\":" + BoneJsonLocked() +
			",\"sole\":" + SoleJsonLocked() +
			",\"solePitch\":" + SolePitchJsonLocked() +
			",\"studio\":" + StudioJsonLocked() +
			",\"experiments\":" + ExperimentsJsonLocked() +
			",\"clock\":" + ClockJsonLocked() +
			",\"audioEffect\":" + BuildAudioEffectJson() + "}";
	}
} // namespace GakuMotion::webpanel
