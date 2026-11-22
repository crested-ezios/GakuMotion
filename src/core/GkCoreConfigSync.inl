namespace GakuMotion
{
	inline void Core::ApplyDeclarativeConfig()
	{
		if (config.secondaryPhysicsBackend != 2) config.beforeQuartzExperiment = false;
		guiPhysicsBackend.store(config.secondaryPhysicsBackend);
		guiBeforeQuartzExperiment.store(config.beforeQuartzExperiment);
		if (config.cameraDeclared && (!cameraConfigApplied || !(config.camera == appliedCamera)))
		{
			camera.settings = config.camera;
			appliedCamera = config.camera;
			cameraConfigApplied = true;
			camera.Publish();
		}
		// ---- 1. 选曲（只在「文件里的值发生变化」时动作，避免快捷键切歌被旧值拉回） ----
		if (!config.selectedMotion.empty() &&
			config.selectedMotion != applied.selectedMotion)
		{
			const int idx = FindMotionIndex(motions, config.selectedMotion);
			if (idx >= 0)
			{
				SelectMotionByIndex(idx, false);
				applied.selectedMotion = config.selectedMotion;
			}
			else
			{
				LogWarn("[GakuMotion][Config] selected_motion not found: " + config.selectedMotion);
			}
		}

		// ---- 2. 视线 ----
		bool gazeChanged = false;
		if (config.gazeMode >= 0 && config.gazeMode != applied.gazeMode)
		{
			lookAt.gazeMode.store(static_cast<GazeMode>(config.gazeMode));
			applied.gazeMode = config.gazeMode;
			gazeChanged = true;
		}
		if (config.desktopLookAtEnabled >= 0 && config.desktopLookAtEnabled != applied.desktopLookAt)
		{
			lookAt.desktopLookAtEnabled.store(config.desktopLookAtEnabled != 0);
			applied.desktopLookAt = config.desktopLookAtEnabled;
			gazeChanged = true;
		}
		if (config.eyeWeight >= 0.0f && config.eyeWeight != applied.eyeWeight)
		{
			lookAt.eyeWeight.store(config.eyeWeight);
			applied.eyeWeight = config.eyeWeight;
			gazeChanged = true;
		}
		if (config.headWeight >= 0.0f && config.headWeight != applied.headWeight)
		{
			lookAt.headWeight.store(config.headWeight);
			applied.headWeight = config.headWeight;
			gazeChanged = true;
		}
		if (config.bodyWeight >= 0.0f && config.bodyWeight != applied.bodyWeight)
		{
			lookAt.bodyWeight.store(config.bodyWeight);
			applied.bodyWeight = config.bodyWeight;
			gazeChanged = true;
		}
		if (gazeChanged)
		{
			LogInfo(detail::FormatMsg(
				"[GakuMotion][Gaze] mode=%s desktop=%s eye=%.2f head=%.2f body=%.2f",
				GazeModeName(static_cast<int>(lookAt.gazeMode.load())),
				lookAt.desktopLookAtEnabled.load() ? "on" : "off",
				lookAt.eyeWeight.load(), lookAt.headWeight.load(), lookAt.bodyWeight.load()));
		}

		// ---- 3. 静息姿态相对校准 ----
		if (config.useRestRotations >= 0 && config.useRestRotations != applied.useRestRotations)
		{
			player.controller.useRestRotations = (config.useRestRotations != 0);
			applied.useRestRotations = config.useRestRotations;
			LogInfo(detail::FormatMsg("[GakuMotion][Config] use_rest_rotations=%s (applies on next bind)",
			                          player.controller.useRestRotations ? "true" : "false"));
		}

		// ---- 3b. gkclip 腿部静息基准（2026-10-07 A/B 实验）----
		if (config.legRestBasis >= 0 && config.legRestBasis != applied.legRestBasis)
		{
			player.controller.legRestBasis = (config.legRestBasis != 0);
			applied.legRestBasis = config.legRestBasis;
			LogInfo(detail::FormatMsg("[GakuMotion][Config] leg_rest_basis=%s",
			                          player.controller.legRestBasis ? "ON" : "off"));
		}

		// ---- 3b-2. gkclip 眼骨几何相对缩放（#150 §8）----
		// Gain comes from the measured per-character table, never from this file;
		// the switch only chooses between the table and the raw 1:1 control.
		if (config.eyePoseRelativeGain >= 0 && config.eyePoseRelativeGain != applied.eyePoseRelativeGain)
		{
			player.controller.eyePoseRelativeGain = (config.eyePoseRelativeGain != 0);
			applied.eyePoseRelativeGain = config.eyePoseRelativeGain;
			LogInfo(detail::FormatMsg("[GakuMotion][Config] eye_pose_relative_gain=%s",
			                          player.controller.eyePoseRelativeGain ? "geometry-relative" : "raw 1:1"));
		}

		// ---- 3c. 辅助骨驱动模式（实机靶向对照实验）----
		const int preserveAnimator = config.secondaryPhysicsBackend == 2
			                             ? 1
			                             : (config.preserveAnimatorForSecondaryPhysics >= 0
				                                ? config.preserveAnimatorForSecondaryPhysics
				                                : 0);
		if (preserveAnimator >= 0 && preserveAnimator != applied.preserveAnimatorForSecondaryPhysics)
		{
			applied.preserveAnimatorForSecondaryPhysics = preserveAnimator;
			player.controller.preserveAnimatorForSecondaryPhysics =
				preserveAnimator != 0;
			LogInfo(detail::FormatMsg(
				"[GakuMotion][SecondaryPhysics] preserve_animator=%s (experimental; device validation required)",
				player.controller.preserveAnimatorForSecondaryPhysics ? "ON" : "off"));
		}

		// ---- Auxiliary bone drive mode ----
		if (config.auxMode >= 0 && config.auxMode != applied.auxMode)
		{
			applied.auxMode = config.auxMode;
			player.controller.SetAuxDriveMode(config.auxMode);
		}
		if (config.experimentalWaistSupport >= 0 &&
			config.experimentalWaistSupport != applied.experimentalWaistSupport)
		{
			player.controller.SetExperimentalWaistSupportEnabled(config.experimentalWaistSupport != 0);
			applied.experimentalWaistSupport = config.experimentalWaistSupport;
			LogInfo(std::string("[GakuMotion][WaistSupport] experimental ") +
				(config.experimentalWaistSupport
					 ? "ON (scope-specific acceptance passed; broader scope unverified)"
					 : "off"));
		}
		guiExperimentalWaistSupport.store(player.controller.IsExperimentalWaistSupportEnabled());

		if (config.muteNativeSoundEffects >= 0 &&
			config.muteNativeSoundEffects != applied.muteNativeSoundEffects)
		{
			applied.muteNativeSoundEffects = config.muteNativeSoundEffects;
			guiMuteNativeSoundEffects.store(config.muteNativeSoundEffects != 0, std::memory_order_release);
		}

		// ---- Character facial default prior (adapter v2) ----
		if (config.facialMikuMouthFit >= 0 && config.facialMikuMouthFit != applied.facialMikuMouthFit)
		{
			applied.facialMikuMouthFit = config.facialMikuMouthFit;
			player.controller.facialMikuMouthFit = config.facialMikuMouthFit != 0;
			for (auto& pair : player.controller.bindings)
				pair.second.face.enableMikuMouthFit = player.controller.facialMikuMouthFit;
			player.controller.currentBinding.face.enableMikuMouthFit = player.controller.facialMikuMouthFit;
		}
		if (config.facialMikuEyeFit >= 0 && config.facialMikuEyeFit != applied.facialMikuEyeFit)
		{
			applied.facialMikuEyeFit = config.facialMikuEyeFit;
			player.controller.facialMikuEyeFit = config.facialMikuEyeFit != 0;
			for (auto& pair : player.controller.bindings)
				pair.second.face.enableMikuEyeFit = player.controller.facialMikuEyeFit;
			player.controller.currentBinding.face.enableMikuEyeFit = player.controller.facialMikuEyeFit;
		}
		if (config.facialMikuFitAllowUnverified >= 0 &&
			config.facialMikuFitAllowUnverified != applied.facialMikuFitAllowUnverified)
		{
			applied.facialMikuFitAllowUnverified = config.facialMikuFitAllowUnverified;
			player.controller.facialMikuFitAllowUnverified = config.facialMikuFitAllowUnverified != 0;
			for (auto& pair : player.controller.bindings)
				pair.second.face.facialFitAllowUnverified = player.controller.facialMikuFitAllowUnverified;
			player.controller.currentBinding.face.facialFitAllowUnverified = player.controller.facialMikuFitAllowUnverified;
		}
		if (config.facialEnableBaseline >= 0 && config.facialEnableBaseline != applied.facialEnableBaseline)
		{
			applied.facialEnableBaseline = config.facialEnableBaseline;
			player.controller.facialEnableBaseline = (config.facialEnableBaseline != 0);
			for (auto& pair : player.controller.bindings)
			{
				pair.second.face.enableBaseline = player.controller.facialEnableBaseline;
			}
			player.controller.currentBinding.face.enableBaseline = player.controller.facialEnableBaseline;
		}
		if (config.facialBaselineMultiplier >= 0.0f && config.facialBaselineMultiplier != applied.
			facialBaselineMultiplier)
		{
			applied.facialBaselineMultiplier = config.facialBaselineMultiplier;
			player.controller.facialBaselineMultiplier = config.facialBaselineMultiplier;
			for (auto& pair : player.controller.bindings)
			{
				pair.second.face.baselineMultiplier = player.controller.facialBaselineMultiplier;
			}
			player.controller.currentBinding.face.baselineMultiplier = player.controller.facialBaselineMultiplier;
		}
		// Facial identity belongs to the bound actor, never an old manual override.
		if (player.controller.facialCharacterOverride != "auto" || applied.facialCharacterOverride != "auto")
		{
			applied.facialCharacterOverride = "auto";
			player.controller.facialCharacterOverride = "auto";
			for (auto& pair : player.controller.bindings) pair.second.face.characterOverride = "auto";
			player.controller.currentBinding.face.characterOverride = "auto";
		}

		// ---- 3b. Global transport rate (multiplied by each file's own speed). ----
		if (config.playbackSpeedMultiplier > 0.0f && config.playbackSpeedMultiplier != applied.playbackSpeed)
		{
			applied.playbackSpeed = config.playbackSpeedMultiplier;
			player.SetPlaybackSpeed(config.playbackSpeedMultiplier);
			LogInfo(detail::FormatMsg("[GakuMotion][Config] playback_speed_multiplier=%.3f",
			                          player.playbackSpeedMultiplier));
		}

		// ---- 4. 骨骼覆盖：总开关与可选路径 ----
		if (config.overridesEnabled >= 0 && config.overridesEnabled != applied.overridesEnabled)
		{
			masterOverridesEnabled = (config.overridesEnabled != 0);
			applied.overridesEnabled = config.overridesEnabled;
			RebuildEffectiveOverrides();
		}
		if (!config.overrideGlobalFile.empty() && config.overrideGlobalFile != applied.globalFile)
		{
			applied.globalFile = config.overrideGlobalFile;
			globalOverridePath = ResolvePath(config.overrideGlobalFile, globalOverridePath);
			LoadOverrides(true);
		}
		if (config.overrideMotionFile != applied.motionFile)
		{
			applied.motionFile = config.overrideMotionFile;
			LoadOverrides(true);
		}
	}

	inline void Core::ConsumeConfigCommands()
	{
		if (!config.HasPendingCommands()) return;
		if (!InjectionAllowed() || ((config.cmdPlay || config.cmdSeek || (config.cmdSolePitchCheck && !formation.active)) &&
			!PrepareInjection())) { config.ClearCommands(); return; }
		if (config.cmdBurstTrace) burstTraceRequestedSeq = config.appliedCommandSeq;

		if (config.cmdSolePitchCheck && formation.active)
			LogWarn("[GakuMotion][SolePitchCheck] unavailable during multi-actor playback; configure placement through actor slots");

		if (config.cmdSolePitchCheck && !formation.active)
		{
			const auto& command = config.cmdSolePitchSettings;
			auto offset = player.controller.initialPositionOffset;
			offset.y = command.initialYM;
			auto settings = soleSettings;
			settings.enabled = true;
			settings.useBootProxy = true;
			if (command.hasPlane)
			{
				settings.planeNormal = {0, 1, 0};
				settings.planeOffsetM = command.planeOffsetM;
				settings.planeCalibrated = true;
			}
			if (settings.IsValid() && sole::Finite(offset) && player.controller.SetInitialPositionOffset(offset))
			{
				SetSoleSettings(settings);
				soleObservedActor = nullptr; // The explicit check starts a new observation on the current target.
				experimentalSolePitchSettings = {command.enabled, command.maxPitchDegrees};
				experimentalSolePitchSettings.useMeshEnvelope = command.useMeshEnvelope;
				experimentalSolePitchSnapshot = {};
				experimentalSolePitchConsumedSerial = 0;
				experimentalSolePitchConsumedActor = nullptr;
				LogInfo("[GakuMotion][SolePitchCheck] applied seq=" + std::to_string(config.appliedCommandSeq));
			}
			else LogWarn("[GakuMotion][SolePitchCheck] rejected runtime settings; experiment unchanged");
		}
		if (config.cmdSolePitchSnapshot)
		{
			solePitchSnapshotCommandSeq = config.appliedCommandSeq;
			solePitchSnapshotPending = true;
		}

		if (config.cmdPlay)
		{
			stageStoppedByTransport = false;
			player.Play();
			for (auto& kv : formationTracks) if (kv.second) { kv.second->SyncClockState(player.currentTime); kv.second->state = player.state; }
			NotifyMotionPlaying(true);
			LogInfo("[GakuMotion][Config] command: play");
		}
		if (config.cmdPause)
		{
			player.Pause();
			for (auto& kv : formationTracks) if (kv.second) { kv.second->SyncClockState(player.currentTime); kv.second->state = player.state; }
			NotifyMotionPlaying(false);
			LogInfo("[GakuMotion][Config] command: pause");
		}
		if (config.cmdStop)
		{
			StopPerformance();
			NotifyMotionPlaying(false);
			LogInfo("[GakuMotion][Config] command: stop");
		}
		if (config.cmdSeek)
		{
			player.Seek(config.cmdSeekSeconds);
			for (auto& kv : formationTracks) if (kv.second) { kv.second->SyncClockState(player.currentTime); kv.second->state = player.state; }
			LogInfo(detail::FormatMsg("[GakuMotion][Config] command: seek %.2fs", config.cmdSeekSeconds));
		}
		if (config.cmdReloadOverrides)
		{
			LoadOverrides(true);
			LogInfo("[GakuMotion][Config] command: reload_overrides");
		}
		if (config.cmdToggleOverrides)
		{
			ToggleOverrides();
			LogInfo("[GakuMotion][Config] command: toggle_overrides");
		}
		config.ClearCommands();
	}
} // namespace GakuMotion
