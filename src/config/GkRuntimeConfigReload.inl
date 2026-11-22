namespace GakuMotion
{
	inline bool RuntimeConfig::Reload()
	{
		jsonlite::Value root;
		std::string err;
		if (!jsonlite::ParseFile(path, root, err))
		{
			lastError = err;
			return false;
		}
		if (!root.IsObject())
		{
			lastError = path + " -> top-level must be an object";
			return false;
		}

		// ---- 变更检测快照 ----
		const std::string prevMotion = selectedMotion;
		const int prevGaze = gazeMode;
		const int prevDesktop = desktopLookAtEnabled;
		const float prevEye = eyeWeight;
		const float prevHead = headWeight;
		const float prevBody = bodyWeight;
		const int prevLegBasis = legRestBasis;
		const int prevEyePoseGain = eyePoseRelativeGain;
		const int prevPreserveAnimator = preserveAnimatorForSecondaryPhysics;
		const int prevPhysicsBackend = secondaryPhysicsBackend;
		const bool prevBeforeQuartz = beforeQuartzExperiment;
		const bool prevSleeveDiagnostics = sleeveMuscleDiagnostics;
		const int prevRest = useRestRotations;
		const int prevOverrides = overridesEnabled;
		const int prevAuxMode = auxMode;
		const std::string prevGlobalFile = overrideGlobalFile;
		const std::string prevMotionFile = overrideMotionFile;
		const float prevPlaybackSpeed = playbackSpeedMultiplier;

		// ---- 声明式字段 ----
		if (const auto* c = root.Find("camera"))
		{
			CameraSettings next = camera;
			auto vec = [&](const char* key, Vector3& out)
			{
				if (const auto* a = c->Find(key))
				{
					if (!a->IsArray() || a->Size() != 3) return false;
					Vector3 v;
					float* fields[] = {&v.x, &v.y, &v.z};
					for (size_t i = 0; i < 3; ++i)
					{
						if (!a->At(i)->IsNumber()) return false;
						*fields[i] = a->At(i)->AsFloat(0);
					}
					if (!CameraFinite(v)) return false;
					out = v;
				}
				return true;
			};
			if (!c->IsObject() || !vec("bounds_min", next.boundsMin) || !vec("bounds_max", next.boundsMax) ||
				!vec("follow_offset", next.followOffset) || !vec("target_offset", next.targetOffset) ||
				!vec("vmd_offset", next.vmdOffset) || !vec("vmd_initial_offset", next.vmdOffset) ||
				!vec("follow_factor", next.followFactor) || !vec("follow_rate", next.followRate))
			{
				lastError = "camera vectors must contain three finite numbers";
				return false;
			}
			if (const auto* target = c->Find("follow_target"))
			{
				if (!target->IsString())
				{
					lastError = "camera follow_target must be hips, chest or head";
					return false;
				}
				if (target->stringValue == "hips") next.followTarget = CameraFollowTarget::Hips;
				else if (target->stringValue == "chest") next.followTarget = CameraFollowTarget::Chest;
				else if (target->stringValue == "head") next.followTarget = CameraFollowTarget::Head;
				else
				{
					lastError = "camera follow_target must be hips, chest or head";
					return false;
				}
			}

			if (!next.usePerModeTransforms)
			{
				next.transformTrack.position = next.vmdOffset;
				next.transformFollowMove.position = next.followOffset;
				next.transformLookAt.position = next.followOffset;
			}

			bool anyTransformFound = false;
			auto parseTransform = [&](const char* key, CameraModeTransform& out, bool& found) -> bool
			{
				const auto* obj = c->Find(key);
				if (!obj) return true;
				if (!obj->IsObject())
				{
					lastError = std::string("camera.") + key + " must be an object";
					return false;
				}
				auto readVec3 = [&](const char* subKey, Vector3& v) -> bool
				{
					const auto* a = obj->Find(subKey);
					if (!a || !a->IsArray() || a->Size() != 3) return false;
					for (size_t i = 0; i < 3; ++i)
					{
						if (!a->At(i)->IsNumber()) return false;
					}
					v.x = a->At(0)->AsFloat(0.0f);
					v.y = a->At(1)->AsFloat(0.0f);
					v.z = a->At(2)->AsFloat(0.0f);
					return CameraFinite(v);
				};
				CameraModeTransform parsed = out;
				if (!readVec3("pos", parsed.position) || !readVec3("rot", parsed.rotation))
				{
					lastError = std::string("camera.") + key + " pos and rot must contain 3 finite numbers";
					return false;
				}
				if (!parsed.IsValid())
				{
					lastError = std::string("invalid transform values in camera.") + key;
					return false;
				}
				out = parsed;
				found = true;
				anyTransformFound = true;
				return true;
			};

			bool foundTrack = false, foundFollowMove = false, foundLookAt = false;
			if (!parseTransform("transform_track", next.transformTrack, foundTrack) ||
			    !parseTransform("transform_follow_move", next.transformFollowMove, foundFollowMove) ||
			    !parseTransform("transform_look_at", next.transformLookAt, foundLookAt))
			{
				return false;
			}
			if (anyTransformFound)
			{
				next.usePerModeTransforms = true;
			}
			next.usePerModeTransforms = c->BoolPath("use_per_mode_transforms", next.usePerModeTransforms);

			next.enabled = c->BoolPath("enabled", next.enabled);
			int mode = c->IntPath("mode", static_cast<int>(next.mode));
			int followBehavior = c->IntPath("follow_behavior", static_cast<int>(next.followBehavior));
			if (const auto* fsi = c->Find("follow_slot_index"))
			{
				if (!fsi->IsNumber()) { lastError = "camera follow_slot_index must be an integer"; return false; }
				const double d = fsi->numberValue;
				if (!std::isfinite(d) || d != std::floor(d) || d < -1.0 || d > 1000000.0)
				{
					lastError = "camera follow_slot_index must be >= -1";
					return false;
				}
				next.followSlotIndex = static_cast<int>(d);
			}
			next.followActor = c->BoolPath("follow_actor", next.followActor);
			next.limitToScene = c->BoolPath("limit_to_scene", next.limitToScene);
			next.followFov = c->FloatPath("follow_fov", next.followFov);
			if (const auto* scale = c->Find("vmd_range_scale"))
			{
				if (!scale->IsNumber()) { lastError = "camera vmd_range_scale must be a number"; return false; }
				next.vmdRangeScale = scale->AsFloat(0);
			}
			next.srpFinalOverwriteFallback = c->BoolPath("srp_final_overwrite_fallback", next.srpFinalOverwriteFallback);
			next.actorShadowExperimentMode = c->IntPath("actor_shadow_experiment_mode", next.actorShadowExperimentMode);
			if (next.actorShadowExperimentMode < 0 || next.actorShadowExperimentMode > 2) next.actorShadowExperimentMode = 0;
			if (mode < 0 || mode > 2 || followBehavior < 0 || followBehavior > 1 ||
				next.boundsMin.x > next.boundsMax.x || next.boundsMin.y > next.boundsMax.y
				||
				next.boundsMin.z > next.boundsMax.z || !CameraFinite(next.vmdOffset) || !CameraFinite(next.followFactor) ||
				next.followFactor.x < 0 || next.followFactor.x > 1.0f ||
				next.followFactor.y < 0 || next.followFactor.y > 1.0f ||
				next.followFactor.z < 0 || next.followFactor.z > 1.0f || !CameraFinite(next.followRate) ||
				next.followRate.x < 0 || next.followRate.x > 1.0f ||
				next.followRate.y < 0 || next.followRate.y > 1.0f ||
				next.followRate.z < 0 || next.followRate.z > 1.0f || !std::isfinite(next.followFov) ||
				next.followFov < 1 || next.followFov > 179 || !std::isfinite(next.vmdRangeScale) ||
				next.vmdRangeScale < 0.3f || next.vmdRangeScale > 1.0f ||
				!next.transformTrack.IsValid() || !next.transformFollowMove.IsValid() || !next.transformLookAt.IsValid())
			{
				lastError = "invalid camera mode, bounds, follow factor, follow rate, FOV or VMD range scale";
				return false;
			}
			// Hold is retired from user camera selection. Preserve its numeric
			// ABI slot, but migrate old files to the VMD mode on load.
			next.mode = mode == static_cast<int>(CameraMode::Hold) ? CameraMode::Track : static_cast<CameraMode>(mode);
			next.followBehavior = static_cast<FollowBehavior>(followBehavior);
			camera = next;
			cameraDeclared = true;
		}
		pollIntervalSeconds = root.FloatPath("poll_interval_seconds", pollIntervalSeconds);
		selectedMotion = root.StringPath("selected_motion", selectedMotion);
		autoPlayOnSelect = root.BoolPath("autoplay", autoPlayOnSelect);
		{
			// 旧 python/cs 配置均迁移到唯一的原生产品后端。
			importBackend = "csharp";
		}

		// 播放速度倍率（方案 C）：夹取到 [0.1, 5.0]，避免手写越界值导致时钟异常
		if (const jsonlite::Value* v = root.Find("playback_speed_multiplier"))
		{
			playbackSpeedMultiplier = v->AsFloat(playbackSpeedMultiplier);
			if (playbackSpeedMultiplier < 0.1f) playbackSpeedMultiplier = 0.1f;
			if (playbackSpeedMultiplier > 5.0f) playbackSpeedMultiplier = 5.0f;
		}

		if (const jsonlite::Value* gaze = root.Find("gaze"))
		{
			gazeMode = gaze->IntPath("mode", gazeMode);
			if (const jsonlite::Value* d = gaze->Find("desktop_enabled"))
			{
				desktopLookAtEnabled = d->AsBool(true) ? 1 : 0;
			}
			if (const jsonlite::Value* w = gaze->Find("eye_weight")) eyeWeight = w->AsFloat(eyeWeight);
			if (const jsonlite::Value* w = gaze->Find("head_weight")) headWeight = w->AsFloat(headWeight);
			if (const jsonlite::Value* w = gaze->Find("body_weight")) bodyWeight = w->AsFloat(bodyWeight);
		}
		if (const jsonlite::Value* v = root.Find("leg_rest_basis"))
		{
			legRestBasis = v->AsBool(false) ? 1 : 0;
			eyePoseRelativeGain = v->AsBool(true) ? 1 : 0;
		}
		if (const jsonlite::Value* v = root.Find("use_rest_rotations"))
		{
			useRestRotations = v->AsBool(true) ? 1 : 0;
		}
		if (const jsonlite::Value* v = root.FindPath("secondary_physics.preserve_animator"))
		{
			if (v->IsBool()) preserveAnimatorForSecondaryPhysics = v->AsBool(false) ? 1 : 0;
		}
		if (const jsonlite::Value* v = root.FindPath("experimental_waist_support.enabled"))
		{
			if (v->IsBool()) experimentalWaistSupport = v->AsBool(false) ? 1 : 0;
		}
		if (const jsonlite::Value* v = root.FindPath("native_audio.mute_sound_effects"))
		{
			if (v->IsBool()) muteNativeSoundEffects = v->AsBool(true) ? 1 : 0;
		}
		if (const jsonlite::Value* v = root.FindPath("secondary_physics.backend"))
		{
			if (v->IsString())
			{
				if (v->stringValue == "off") secondaryPhysicsBackend = 0;
				else if (v->stringValue == "fallback") secondaryPhysicsBackend = 1;
				else if (v->stringValue == "native") secondaryPhysicsBackend = 2;
			}
		}
		beforeQuartzExperiment = false;
		sleeveMuscleDiagnostics = false;
		if (const auto* v = root.FindPath("secondary_physics.before_quartz_experiment"); v && v->IsBool())
			beforeQuartzExperiment = v->boolValue;
		if (const auto* v = root.FindPath("secondary_physics.sleeve_muscle_diagnostics"); v && v->IsBool())
			sleeveMuscleDiagnostics = v->boolValue;
		if (const jsonlite::Value* ov = root.Find("overrides"))
		{
			if (const jsonlite::Value* e = ov->Find("enabled")) overridesEnabled = e->AsBool(true) ? 1 : 0;
			overrideGlobalFile = ov->StringPath("global_file", overrideGlobalFile);
			overrideMotionFile = ov->StringPath("motion_file", overrideMotionFile);
		}

		// 面部表情默认基线与动态让位
		if (const jsonlite::Value* facial = root.Find("facial"))
		{
			if (const jsonlite::Value* fitted = facial->Find("miku_mouth_fit"))
				facialMikuMouthFit = fitted->AsBool(false) ? 1 : 0;
			if (const jsonlite::Value* fitted = facial->Find("miku_eye_fit"))
				facialMikuEyeFit = fitted->AsBool(false) ? 1 : 0;
			if (const jsonlite::Value* allow = facial->Find("miku_fit_allow_unverified"))
				facialMikuFitAllowUnverified = allow->AsBool(false) ? 1 : 0;
			if (const jsonlite::Value* e = facial->Find("enable_baseline"))
			{
				facialEnableBaseline = e->AsBool(true) ? 1 : 0;
			}
			if (const jsonlite::Value* m = facial->Find("baseline_multiplier"))
			{
				facialBaselineMultiplier = m->AsFloat(facialBaselineMultiplier);
			}
			// Ignore legacy manual identity overrides, including those saved by older UIs.
			facialCharacterOverride = "auto";
		}

		// 辅助骨驱动模式（实机靶向对照实验）
		//   0/normal   = 现行 Quartz 半角 / 半扭转注入
		//   1/silence  = 硬锁初始绑定局部姿态（绝对静默）
		//   2/identity = 局部旋转强制归零
		//   3/replay   = 直灌官方录制真值（读 <曲目>/aux_replay.json）
		if (const jsonlite::Value* v = root.Find("aux"))
		{
			if (const jsonlite::Value* m = v->Find("mode")) auxMode = ParseAuxMode(m);
		}
		if (const jsonlite::Value* v = root.Find("aux_mode")) auxMode = ParseAuxMode(v);

		// 数值合法性夹取（避免手写文件里的越界值造成异常姿态）
		if (eyeWeight >= 0.0f) eyeWeight = Clamp01(eyeWeight);
		if (headWeight >= 0.0f) headWeight = Clamp01(headWeight);
		if (bodyWeight >= 0.0f) bodyWeight = Clamp01(bodyWeight);
		if (gazeMode >= 0) gazeMode = (gazeMode > 2) ? 2 : gazeMode;

		// ---- 一次性命令（seq 去重） ----
		bool seqReported = false;
		std::string commandError;
		if (const jsonlite::Value* cmd = root.Find("command"))
		{
			long long seq = 0;
			if (const jsonlite::Value* s = cmd->Find("seq"))
			{
				if (s->IsNumber()) seq = static_cast<long long>(s->numberValue);
				else if (s->IsString()) seq = std::strtoll(s->stringValue.c_str(), nullptr, 10);
			}
			lastSeenCommandSeq = seq;

			if (seq > appliedCommandSeq)
			{
				// Validate the entire one-shot experiment before publishing any
				// command flags or consuming its sequence. Omission is inert.
				SolePitchCheckCommand soleCommand;
				const auto* soleCheck = cmd->Find("sole_pitch_check");
				const auto* snapshot = cmd->Find("sole_pitch_snapshot");
				auto validateSoleCommand = [&]()
				{
					if (soleCheck)
					{
						const auto* y = soleCheck->Find("initial_y_m");
						const auto* enabled = soleCheck->Find("enabled");
						auto number = [](const jsonlite::Value* v, float& out, float limit, float minimum)
						{
							if (!v || !v->IsNumber() || !std::isfinite(v->numberValue) ||
								v->numberValue < minimum || v->numberValue > limit)
								return false;
							out = static_cast<float>(v->numberValue);
							return std::isfinite(out);
						};
						if (!soleCheck->IsObject() || !enabled || !enabled->IsBool() ||
							!number(y, soleCommand.initialYM, 2.f, -2.f))
						{
							commandError =
								"invalid command.sole_pitch_check: initial_y_m [-2,2] and boolean enabled required";
							return false;
						}
						soleCommand.enabled = enabled->boolValue;
						if (const auto* envelope = soleCheck->Find("use_mesh_envelope"))
						{
							if (!envelope->IsBool())
							{
								commandError =
									"invalid command.sole_pitch_check.use_mesh_envelope: boolean required";
								return false;
							}
							soleCommand.useMeshEnvelope = envelope->boolValue;
						}
						if (const auto* plane = soleCheck->Find("plane_offset_m"))
						{
							if (!number(plane, soleCommand.planeOffsetM, 100.f, -100.f))
							{
								commandError = "invalid command.sole_pitch_check.plane_offset_m [-100,100]";
								return false;
							}
							soleCommand.hasPlane = true;
						}
						if (const auto* angle = soleCheck->Find("max_pitch_degrees"))
						{
							if (!number(angle, soleCommand.maxPitchDegrees, 35.f, 0.f))
							{
								commandError = "invalid command.sole_pitch_check.max_pitch_degrees [0,35]";
								return false;
							}
						}
					}
					if (snapshot && !snapshot->IsBool())
					{
						commandError = "command.sole_pitch_snapshot must be boolean";
						return false;
					}
					return true;
				};
				if (!validateSoleCommand())
				{
					// Keep valid declarative edits and report the error on this
					// reload, without replaying any earlier pending command.
					ClearCommands();
				}
				else
				{
					appliedCommandSeq = seq;
					seqReported = true;

					cmdSolePitchCheck = soleCheck != nullptr;
					cmdSolePitchSettings = soleCommand;
					cmdSolePitchSnapshot = snapshot && snapshot->boolValue;

					cmdPlay = cmd->BoolPath("play", false);
					cmdPause = cmd->BoolPath("pause", false);
					cmdStop = cmd->BoolPath("stop", false);
					cmdReloadOverrides = cmd->BoolPath("reload_overrides", false);
					cmdToggleOverrides = cmd->BoolPath("toggle_overrides", false);
					cmdBurstTrace = cmd->BoolPath("burst_trace", false);

					float seek = cmd->FloatPath("seek_seconds", -1.0f);
					cmdSeek = (seek >= 0.0f);
					cmdSeekSeconds = cmdSeek ? seek : 0.0f;
				}
			}
		}

		// ---- 变更日志 ----
		std::string log;
		AppendChange(log, "selected_motion", prevMotion, selectedMotion);
		AppendChange(log, "gaze.mode", prevGaze, gazeMode);
		AppendChange(log, "gaze.desktop_enabled", prevDesktop, desktopLookAtEnabled);
		AppendChange(log, "gaze.eye_weight", prevEye, eyeWeight);
		AppendChange(log, "gaze.head_weight", prevHead, headWeight);
		AppendChange(log, "gaze.body_weight", prevBody, bodyWeight);
		AppendChange(log, "leg_rest_basis", prevLegBasis, legRestBasis);
		AppendChange(log, "eye_pose_relative_gain", prevEyePoseGain, eyePoseRelativeGain);
		AppendChange(log, "secondary_physics.preserve_animator", prevPreserveAnimator,
		             preserveAnimatorForSecondaryPhysics);
		AppendChange(log, "secondary_physics.backend", prevPhysicsBackend, secondaryPhysicsBackend);
		AppendChange(log, "secondary_physics.before_quartz_experiment", prevBeforeQuartz, beforeQuartzExperiment);
		AppendChange(log, "secondary_physics.sleeve_muscle_diagnostics", prevSleeveDiagnostics, sleeveMuscleDiagnostics);
		AppendChange(log, "use_rest_rotations", prevRest, useRestRotations);
		AppendChange(log, "aux.mode", prevAuxMode, auxMode);
		AppendChange(log, "overrides.enabled", prevOverrides, overridesEnabled);
		AppendChange(log, "overrides.global_file", prevGlobalFile, overrideGlobalFile);
		AppendChange(log, "overrides.motion_file", prevMotionFile, overrideMotionFile);
		AppendChange(log, "playback_speed_multiplier", prevPlaybackSpeed, playbackSpeedMultiplier);
		if (seqReported)
		{
			if (!log.empty()) log += " | ";
			log += "command.seq=" + std::to_string(appliedCommandSeq);
		}
		lastChangeLog = log;
		lastError = commandError;
		++revision;
		return true;
	}
} // namespace GakuMotion
