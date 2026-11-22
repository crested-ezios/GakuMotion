namespace GakuMotion
{
	inline void Core::ResetSoleObservation()
	{
		solePitchSnapshotPending = false;
		experimentalSolePitchSettings.experimentalSolePitchCorrection = false;
		experimentalSolePitchSnapshot = {};
		experimentalSolePitchConsumedSerial = 0;
		experimentalSolePitchConsumedActor = nullptr;
		soleSnapshot = {};
		soleObservedActor = nullptr;
		++soleCalibrationGeneration;
		// A saved shape remains a draft when the actor/scene changes. Neither the
		// costume identity nor the physical floor is inferred from actor pivot.
		soleSettings.shoeCalibrated = false;
		soleSettings.planeCalibrated = false;
	}

	inline void Core::WriteRequestedSolePitchSnapshot(void* actor)
	{
		if (!solePitchSnapshotPending || configRoot.empty() || !actor ||
			player.state == PlaybackState::Stopped || soleSnapshot.actor != actor ||
			!soleSnapshot.available || soleSnapshot.clipTime != player.currentTime)
			return;
		auto& controller = player.controller;
		const auto& stamp = controller.footPoseWriteStamp;
		if (!controller.CanDriveActor(actor) || stamp.actor != actor || (stamp.mask & 3u) != 3u ||
			!stamp.transforms[0] || !stamp.transforms[1] || !controller.fnGetWorldPosition ||
			!controller.fnGetWorldRotation)
			return;
		if (experimentalSolePitchSettings.experimentalSolePitchCorrection &&
			(experimentalSolePitchSnapshot.actor != actor || experimentalSolePitchSnapshot.poseSerial != stamp.
				serial ||
				experimentalSolePitchSnapshot.clipTime != player.currentTime))
			return;
		std::array<SoleFootMeasurement, 2> after;
		for (int side = 0; side < 2; ++side)
		{
			auto pose = soleSnapshot.feet[side].pose;
			float p[3]{}, q[4]{};
			controller.fnGetWorldPosition(stamp.transforms[side], p);
			controller.fnGetWorldRotation(stamp.transforms[side], q);
			pose.ankle = {p[0], p[1], p[2]};
			pose.footRotation = {q[0], q[1], q[2], q[3]};
			after[side] = MeasureSole(pose, soleSettings, side);
			if (!after[side].valid) return;
		}
		auto number = [](double v) { return std::isfinite(v) ? jsonlite::FormatNumber(v, 9) : "null"; };
		auto boolean = [](bool v) { return v ? "true" : "false"; };
		auto vector = [&](const Vector3& v)
		{
			return "[" + number(v.x) + "," + number(v.y) + "," + number(v.z) + "]";
		};
		auto rotation = [&](const Quaternion& q)
		{
			return "[" + number(q.x) + "," + number(q.y) + "," + number(q.z) + "," + number(q.w) + "]";
		};
		auto measurement = [&](const SoleFootMeasurement& foot)
		{
			std::string out = "{\"valid\":" + std::string(boolean(foot.valid)) + ",\"minimum_distance_m\":" +
				number(foot.minimumDistanceM) + ",\"maximum_penetration_m\":" + number(foot.maximumPenetrationM) +
				",\"ankle_world_m\":" + vector(foot.pose.ankle) + ",\"foot_world_quaternion_xyzw\":" + rotation(
					foot.pose.footRotation) +
				",\"proxy_corner_world_m\":[";
			for (int patch = 0; patch < 2; ++patch)
				for (int corner = 0; corner < 4; ++corner)
				{
					if (patch || corner) out += ",";
					out += vector(foot.patches[patch].corners[corner].world);
				}
			return out + "]}";
		};
		char actorId[40]{};
		std::snprintf(actorId, sizeof(actorId), "%p", actor);
		std::string content = "{\"schema_version\":1,\"command_seq\":" + std::to_string(solePitchSnapshotCommandSeq)
			+
			",\"actor_identity\":\"" + actorId + "\",\"state\":\"" + PlaybackStateName(player.state) +
			"\",\"motion\":\"" + jsonlite::EscapeString(CurrentMotion() ? CurrentMotion()->name : "") +
			"\",\"motion_name\":\"" + jsonlite::EscapeString(CurrentMotion() ? CurrentMotion()->name : "") +
			"\",\"motion_dir\":\"" + jsonlite::EscapeString(player.currentMotionDir) +
			"\",\"clip_time_s\":" + number(player.currentTime) + ",\"pose_serial\":" + std::to_string(stamp.serial)
			+
			",\"initial_position_offset_m\":" + vector(controller.initialPositionOffset) +
			",\"plane_normal\":" + vector(soleSettings.planeNormal) + ",\"plane_offset_m\":" + number(
				soleSettings.planeOffsetM) +
			",\"plane_calibrated\":" + boolean(soleSettings.planeCalibrated) +
			",\"use_boot_proxy\":" + boolean(soleSettings.useBootProxy) +
			",\"experimental_enabled\":" + boolean(experimentalSolePitchSettings.experimentalSolePitchCorrection) +
			",\"use_mesh_envelope\":" + boolean(experimentalSolePitchSettings.useMeshEnvelope) +
			",\"max_pitch_degrees\":" + number(experimentalSolePitchSettings.maxPitchDegrees) + ",\"feet\":[";
		for (int side = 0; side < 2; ++side)
		{
			if (side) content += ",";
			const auto& trace = experimentalSolePitchSnapshot.feet[side];
			content += "{\"side\":\"" + std::string(side ? "right" : "left") + "\",\"before\":" + measurement(
					soleSnapshot.feet[side]) +
				",\"after\":" + measurement(after[side]) + ",\"solver_valid\":" + boolean(trace.solution.valid) +
				",\"fresh_pose\":" + boolean(trace.freshPose) + ",\"pitch_degrees\":" + number(
					trace.solution.pitchDegrees) +
				",\"applied\":" + boolean(trace.applied) + ",\"setter_readback_matches\":" + boolean(
					trace.setterReadbackMatches) +
				",\"solver_used_mesh_envelope\":" + boolean(trace.solution.usedMeshEnvelope) +
				",\"envelope_readback_valid\":" + boolean(trace.measuredAfterValid) +
				",\"envelope_after_minimum_distance_m\":" + (trace.measuredAfterValid
					                                             ? number(trace.measuredAfterMinimumDistanceM)
					                                             : "null") +
				",\"status\":\"" + jsonlite::EscapeString(trace.status ? trace.status : "off") + "\"}";
		}
		content += "]}\n";
		std::error_code ec;
		std::filesystem::create_directories(MotionPathFromUtf8(diagnosticsRoot), ec);
		if (ec) return;
		const auto path = diagnosticsRoot + "/sole_pitch_snapshot.json", temporary = path + ".tmp";
		{
			std::ofstream out(MotionPathFromUtf8(temporary), std::ios::binary | std::ios::trunc);
			out << content;
			out.flush();
			if (!out) return;
		}
		std::filesystem::copy_file(MotionPathFromUtf8(temporary), MotionPathFromUtf8(path), std::filesystem::copy_options::overwrite_existing, ec);
		if (ec) return;
		std::filesystem::remove(MotionPathFromUtf8(temporary), ec);
		solePitchSnapshotPending = false;
		LogInfo("[GakuMotion][SolePitchCheck] snapshot saved seq=" + std::to_string(solePitchSnapshotCommandSeq));
	}

	inline void Core::ObserveSoles(void* actor)
	{
		if (!soleSettings.enabled)
		{
			soleSnapshot = {};
			return;
		}
		auto& controller = player.controller;
		if (!actor || !controller.CanDriveActor(actor))
		{
			if (actor == soleObservedActor) soleSnapshot = {};
			return;
		}
		void* selected = controller.targetActor;
		if (!selected) selected = sceneActors.empty() ? actor : sceneActors.front();
		if (actor != selected) return;
		if (soleObservedActor && soleObservedActor != actor) ResetSoleObservation();
		soleObservedActor = actor;
		soleSnapshot = {};
		soleSnapshot.actor = actor;
		soleSnapshot.clipTime = player.currentTime;
		if (!controller.fnGetBoneTransform || !controller.fnGetWorldPosition || !controller.fnGetWorldRotation)
			return;
		const int hids[2][4] = {{1, 3, 5, 19}, {2, 4, 6, 20}};
		for (int side = 0; side < 2; ++side)
		{
			SoleFootPose pose;
			Vector3* positions[] = {&pose.hip, &pose.knee, &pose.ankle, &pose.toe};
			bool valid = true;
			for (int i = 0; i < 4; ++i)
			{
				void* transform = controller.fnGetBoneTransform(actor, hids[side][i]);
				if (!transform)
				{
					valid = false;
					break;
				}
				float p[3]{};
				controller.fnGetWorldPosition(transform, p);
				*positions[i] = {p[0], p[1], p[2]};
				if (i == 2)
				{
					float q[4]{};
					controller.fnGetWorldRotation(transform, q);
					pose.footRotation = {q[0], q[1], q[2], q[3]};
				}
			}
			pose.valid = valid;
			soleSnapshot.feet[side] = MeasureSole(pose, soleSettings, side);
		}
		soleSnapshot.available = soleSnapshot.feet[0].valid && soleSnapshot.feet[1].valid;
		soleSnapshot.calibrated = soleSnapshot.available && soleSettings.planeCalibrated && soleSettings.
			shoeCalibrated;
	}

	inline void Core::ApplyExperimentalSolePitch(void* actor)
	{
		if (!experimentalSolePitchSettings.experimentalSolePitchCorrection)
		{
			experimentalSolePitchSnapshot = {};
			return;
		}
		auto& controller = player.controller;
		if (soleSnapshot.actor != actor) return; // Other actors never replace target telemetry.
		const auto& stamp = controller.footPoseWriteStamp;
		if (stamp.actor == actor && stamp.mask && experimentalSolePitchConsumedSerial == stamp.serial &&
			experimentalSolePitchConsumedActor == actor)
			return;
		if (stamp.actor == actor && stamp.mask)
		{
			experimentalSolePitchConsumedSerial = stamp.serial;
			experimentalSolePitchConsumedActor = actor;
		}
		experimentalSolePitchSnapshot = {};
		experimentalSolePitchSnapshot.actor = actor;
		experimentalSolePitchSnapshot.clipTime = player.currentTime;
		experimentalSolePitchSnapshot.poseSerial = stamp.serial;
		for (int side = 0; side < 2; ++side)
		{
			auto& trace = experimentalSolePitchSnapshot.feet[side];
			trace.status = "等待本回调原动作 Foot 写入";
			if (player.state == PlaybackState::Stopped || !actor || !controller.CanDriveActor(actor) ||
				stamp.actor != actor || !(stamp.mask & (1U << side)) || !stamp.transforms[side])
				continue;
			trace.freshPose = true;
			if (!controller.fnGetLocalRotation || !controller.fnSetLocalRotation || !controller.fnGetWorldRotation)
			{
				trace.status = "Foot rotation API 不可用";
				continue;
			}
			const auto& original = soleSnapshot.feet[side];
			trace.solution = sole_pitch::Solve(original.pose, soleSettings, side, experimentalSolePitchSettings);
			if (!trace.solution.valid)
			{
				trace.status = "代理 / 已校准水平地面 / 姿态条件不满足";
				continue;
			}
			trace.after = original;
			trace.measuredAfterMinimumDistanceM = trace.solution.beforeMinimumDistanceM;
			trace.measuredAfterValid = true;
			if (!trace.solution.actionable)
			{
				trace.status = trace.solution.fullyCleared ? "无需修正" : "角限内纯 pitch 无法改善";
				continue;
			}
			float local[4]{};
			controller.fnGetLocalRotation(stamp.transforms[side], local);
			const Quaternion base(local[0], local[1], local[2], local[3]);
			if (!sole::ValidRotation(base))
			{
				trace.status = "原动作 Foot local rotation 无效";
				continue;
			}
			const Quaternion output = (base.Normalized() * trace.solution.localDelta).Normalized();
			controller.fnSetLocalRotation(stamp.transforms[side], output.x, output.y, output.z, output.w);
			trace.applied = true;
			float readback[4]{}, world[4]{};
			controller.fnGetLocalRotation(stamp.transforms[side], readback);
			controller.fnGetWorldRotation(stamp.transforms[side], world);
			const Quaternion read(readback[0], readback[1], readback[2], readback[3]);
			auto sameRotation = [](const Quaternion& a, const Quaternion& b)
			{
				const auto q = a.Normalized(), p = b.Normalized();
				const float minus = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) + (q.z - p.z) * (q.z - p.
					z) + (q.w - p.w) * (q.w - p.w);
				const float plus = (q.x + p.x) * (q.x + p.x) + (q.y + p.y) * (q.y + p.y) + (q.z + p.z) * (q.z + p.z)
					+ (q.w + p.w) * (q.w + p.w);
				return (std::min)(minus, plus) < 1e-10f;
			};
			const Quaternion worldRead(world[0], world[1], world[2], world[3]);
			trace.setterReadbackMatches = sole::ValidRotation(read) && sameRotation(read, output) &&
				sole::ValidRotation(worldRead) && sameRotation(worldRead, trace.solution.correctedWorldRotation);
			auto correctedPose = original.pose;
			correctedPose.footRotation = {world[0], world[1], world[2], world[3]};
			trace.after = MeasureSole(correctedPose, soleSettings, side);
			trace.measuredAfterMinimumDistanceM = sole_pitch::MinimumDistance(
				correctedPose, soleSettings, side, experimentalSolePitchSettings);
			trace.measuredAfterValid = trace.measuredAfterMinimumDistanceM != -(std::numeric_limits<float>::max)();
			trace.status = trace.setterReadbackMatches && trace.after.valid ? "已施加实验 pitch" : "已尝试 setter，读回条件未满足";
		}
	}

	inline bool Core::LoadSoleSettings()
	{
		const auto path = configRoot + "/sole_measurement.json";
		if (!std::filesystem::exists(MotionPathFromUtf8(path))) return false;
		jsonlite::Value root;
		std::string error;
		SoleMeasurementSettings next;
		if (!jsonlite::ParseFile(path, root, error) || !ParseSoleSettings(root, next, error))
		{
			soleNotice = "鞋底测量载入失败: " + error;
			return false;
		}
		// World floor and costume must be confirmed in this session; saved point
		// vectors are retained, but previous calibration is not scene detection.
		next.planeCalibrated = false;
		next.shoeCalibrated = false;
		SetSoleSettings(next);
		soleNotice = next.useBootProxy ? "已载入培育靴粗略代理；请核对本场景地面和代理范围" : "已载入代理点；请重新确认本场景地面和靴子";
		return true;
	}

	inline bool Core::SaveSoleSettings()
	{
		const auto content = SerializeSoleSettings(soleSettings);
		if (content.empty())
		{
			soleNotice = "鞋底测量设置无效，无法保存";
			return false;
		}
		std::error_code ec;
		std::filesystem::create_directories(MotionPathFromUtf8(configRoot), ec);
		if (ec)
		{
			soleNotice = "鞋底测量保存失败: " + ec.message();
			return false;
		}
		const auto path = configRoot + "/sole_measurement.json", temporary = path + ".tmp";
		std::ofstream out(MotionPathFromUtf8(temporary), std::ios::binary | std::ios::trunc);
		out << content;
		out.close();
		if (!out)
		{
			soleNotice = "鞋底测量保存失败：无法写入文件";
			return false;
		}
		std::filesystem::copy_file(MotionPathFromUtf8(temporary), MotionPathFromUtf8(path), std::filesystem::copy_options::overwrite_existing, ec);
		if (ec)
		{
			soleNotice = "鞋底测量保存失败: " + ec.message();
			return false;
		}
		std::filesystem::remove(MotionPathFromUtf8(temporary), ec);
		soleNotice = "鞋底测量设置已保存";
		return true;
	}
} // namespace GakuMotion
