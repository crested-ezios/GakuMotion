namespace GakuMotion::webpanel
{
	inline CmdResult Bridge::BoneTree()
	{
		struct BoneItem { int hid; const char* label; };
		struct BoneGroup { const char* title; std::vector<BoneItem> items; };
		static const BoneGroup groups[] = {
			{"躯干 (Torso)", {{0,"骨盆 (Hips)"},{7,"脊柱 (Spine)"},{8,"胸腔 (Chest)"}}},
			{"头颈 (Head & Neck)", {{9,"颈部 (Neck)"},{10,"头部 (Head)"}}},
			{"上肢双臂 (Arms)", {{11,"左肩 (L Shoulder)"},{12,"右肩 (R Shoulder)"},{13,"左上臂 (L UpperArm)"},{14,"右上臂 (R UpperArm)"},{15,"左小臂/手肘 (L Forearm)"},{16,"右小臂/手肘 (R Forearm)"},{17,"左手 (L Hand)"},{18,"右手 (R Hand)"}}},
			{"下肢双腿 (Legs)", {{1,"左大腿 (L Thigh)"},{2,"右大腿 (R Thigh)"},{3,"左小腿/膝盖 (L Calf)"},{4,"右小腿/膝盖 (R Calf)"},{5,"左足踝 (L Foot)"},{6,"右足踝 (R Foot)"},{19,"左脚趾 (L Toes)"},{20,"右脚趾 (R Toes)"}}},
			{"左手手指 (L Fingers)", {{24,"左拇指根"},{25,"左拇指中"},{26,"左拇指尖"},{27,"左食指根"},{28,"左食指中"},{29,"左食指尖"},{30,"左中指根"},{31,"左中指中"},{32,"左中指尖"},{33,"左无名根"},{34,"左无名中"},{35,"左无名尖"},{36,"左小指根"},{37,"左小指中"},{38,"左小指尖"}}},
			{"右手手指 (R Fingers)", {{39,"右拇指根"},{40,"右拇指中"},{41,"右拇指尖"},{42,"右食指根"},{43,"右食指中"},{44,"右食指尖"},{45,"右中指根"},{46,"右中指中"},{47,"右中指尖"},{48,"右无名根"},{49,"右无名中"},{50,"右无名尖"},{51,"右小指根"},{52,"右小指中"},{53,"右小指尖"}}},
		};
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		std::string json = "{\"masterEnabled\":" + JBool(core_.masterOverridesEnabled) +
			",\"revision\":" + JNum(static_cast<double>(core_.effectiveOverrides.revision)) + ",\"groups\":[";
		for (size_t g = 0; g < std::size(groups); ++g)
		{
			if (g) json += ",";
			json += "{\"title\":" + JString(groups[g].title) + ",\"bones\":[";
			for (size_t i = 0; i < groups[g].items.size(); ++i)
			{
				const auto& item = groups[g].items[i];
				if (i) json += ",";
				const int symm = BoneOverrideSet::GetSymmetricalHumanoidId(item.hid);
				float pitch = 0, yaw = 0, roll = 0;
				bool world = false, enabled = true;
				const bool found = core_.GetLiveBoneOverride(item.hid, BoneOverrideSet::GetBoneNameByHumanoidId(item.hid),
					&pitch, &yaw, &roll, &world, &enabled);
				json += "{\"hid\":" + JNum(item.hid) +
					",\"name\":" + JString(BoneOverrideSet::GetBoneNameByHumanoidId(item.hid)) +
					",\"label\":" + JString(item.label) +
					",\"symmetricalHid\":" + JNum(symm) +
					",\"hasOverride\":" + JBool(found) + "}";
			}
			json += "]}";
		}
		json += "]}";
		return CmdResult::Ok(json);
	}

	inline bool Bridge::BoneNameParam(const jsonlite::Value& params, int64_t& hid, std::string& name)
	{
		if (!IntParam(params, "humanoidId", hid) && !IntParam(params, "hid", hid)) return false;
		if (!StringParam(params, "boneName", name)) StringParam(params, "name", name);
		if (name.empty()) name = BoneOverrideSet::GetBoneNameByHumanoidId(static_cast<int>(hid));
		return !name.empty();
	}

	inline CmdResult Bridge::BoneGet(const jsonlite::Value& params)
	{
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (!params.Find("humanoidId") && !params.Find("hid"))
			return CmdResult::Ok(BoneJsonLocked());
		int64_t hid = -1;
		std::string name;
		if (!BoneNameParam(params, hid, name))
			return CmdResult::Err("invalid_params", "hid/humanoidId and a known bone name are required");
		float pitch = 0, yaw = 0, roll = 0;
		bool world = false, enabled = true;
		const bool found = core_.GetLiveBoneOverride(static_cast<int>(hid), name.c_str(), &pitch, &yaw, &roll, &world, &enabled);
		return CmdResult::Ok("{\"humanoidId\":" + JNum(static_cast<double>(hid)) +
			",\"hid\":" + JNum(static_cast<double>(hid)) +
			",\"boneName\":" + JString(name) +
			",\"name\":" + JString(name) +
			",\"found\":" + JBool(found) +
			",\"pitch\":" + JNum(pitch) + ",\"yaw\":" + JNum(yaw) + ",\"roll\":" + JNum(roll) +
			",\"spaceWorld\":" + JBool(world) + ",\"enabled\":" + JBool(enabled) +
			",\"symmetricalHid\":" + JNum(BoneOverrideSet::GetSymmetricalHumanoidId(static_cast<int>(hid))) + "}");
	}

	inline CmdResult Bridge::BoneSet(const jsonlite::Value& params)
	{
		int64_t hid = -1;
		std::string name;
		double pitch = 0, yaw = 0, roll = 0;
		bool world = false, enabled = true, symmetrical = false;
		if (!BoneNameParam(params, hid, name) ||
			!NumberParam(params, "pitch", pitch) || !NumberParam(params, "yaw", yaw) || !NumberParam(params, "roll", roll) ||
			!BoolParam(params, "spaceWorld", world) || !BoolParam(params, "enabled", enabled) ||
			!BoolParam(params, "symmetrical", symmetrical))
			return CmdResult::Err("invalid_params", "humanoidId/boneName/pitch/yaw/roll/spaceWorld/enabled/symmetrical are required");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		core_.SetLiveBoneOverride(static_cast<int>(hid), name.c_str(), static_cast<float>(pitch),
			static_cast<float>(yaw), static_cast<float>(roll), world, enabled, symmetrical);
		boneNotice_.clear();
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::BoneReset(const jsonlite::Value& params)
	{
		int64_t hid = -1;
		std::string name;
		bool symmetrical = false;
		if (!BoneNameParam(params, hid, name) || !BoolParam(params, "symmetrical", symmetrical))
			return CmdResult::Err("invalid_params", "humanoidId/boneName and symmetrical are required");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		core_.ResetLiveBoneOverride(static_cast<int>(hid), name.c_str(), symmetrical);
		boneNotice_.clear();
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::BoneResetAll()
	{
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		core_.ResetAllLiveOverrides();
		boneNotice_ = "已清空全部运行时骨骼调整";
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::BoneSetMaster(const jsonlite::Value& params)
	{
		bool enabled = false;
		if (!BoolParam(params, "enabled", enabled))
			return CmdResult::Err("invalid_params", "enabled must be a boolean");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		if (core_.masterOverridesEnabled != enabled) core_.ToggleOverrides();
		boneNotice_ = core_.masterOverridesEnabled ? "骨骼覆盖总开关已开启" : "骨骼覆盖总开关已关闭";
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::BoneToggle()
	{
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		const bool enabled = core_.ToggleOverrides();
		boneNotice_ = enabled ? "骨骼覆盖总开关已开启" : "骨骼覆盖总开关已关闭";
		return CmdResult::Ok("{\"enabled\":" + JBool(enabled) + "}");
	}

	inline CmdResult Bridge::BoneReload()
	{
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		core_.LoadOverrides(true);
		boneNotice_ = "已重新加载磁盘骨骼覆盖配置";
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::BoneSaveToMotion()
	{
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (!core_.SaveLiveOverridesToMotion())
			return CmdResult::Err("failed", "保存失败：无法写入当前曲目目录");
		boneNotice_ = "已保存到当前曲目目录";
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::BoneSaveToGlobal()
	{
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (!core_.SaveLiveOverridesToGlobal())
			return CmdResult::Err("failed", "保存失败：无法写入全局文件");
		boneNotice_ = "已保存到全局默认配置";
		return CmdResult::Ok();
	}

	inline bool Bridge::GazeRuntimeAvailable() const
	{
		return core_.lookAt.fnSetTarget && core_.lookAt.fnSetWeights && core_.lookAt.fnSetEnabled;
	}

	inline CmdResult Bridge::GazeSet(const jsonlite::Value& params)
	{
		const jsonlite::Value* settingsValue = Param(params, "settings");
		if (!settingsValue || !settingsValue->IsObject())
			return CmdResult::Err("invalid_params", "settings object is required");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		int64_t mode = static_cast<int64_t>(core_.lookAt.gazeMode.load());
		bool desktop = core_.lookAt.desktopLookAtEnabled.load();
		double eye = core_.lookAt.eyeWeight.load();
		double head = core_.lookAt.headWeight.load();
		double body = core_.lookAt.bodyWeight.load();
		if (settingsValue->Find("mode") && (!IntParam(*settingsValue, "mode", mode) || mode < 0 || mode > 2))
			return CmdResult::Err("invalid_params", "mode must be 0 (Auto), 1 (ForceOff) or 2 (DesktopOn)");
		if (settingsValue->Find("desktopLookAtEnabled") && !BoolParam(*settingsValue, "desktopLookAtEnabled", desktop))
			return CmdResult::Err("invalid_params", "desktopLookAtEnabled must be a boolean");
		if (settingsValue->Find("eyeWeight") && !NumberParam(*settingsValue, "eyeWeight", eye))
			return CmdResult::Err("invalid_params", "eyeWeight must be a finite number");
		if (settingsValue->Find("headWeight") && !NumberParam(*settingsValue, "headWeight", head))
			return CmdResult::Err("invalid_params", "headWeight must be a finite number");
		if (settingsValue->Find("bodyWeight") && !NumberParam(*settingsValue, "bodyWeight", body))
			return CmdResult::Err("invalid_params", "bodyWeight must be a finite number");
		for (double value : {eye, head, body})
			if (value < 0.0 || value > 1.0)
				return CmdResult::Err("invalid_params", "视线权重必须在 0..1 范围内");
		if (!GazeRuntimeAvailable()) return CmdResult::Err("not_ready", "视线控制器接口尚未就绪");
		core_.lookAt.gazeMode.store(static_cast<GazeMode>(mode));
		core_.config.gazeMode = static_cast<int>(mode);
		core_.lookAt.desktopLookAtEnabled.store(desktop);
		core_.config.desktopLookAtEnabled = desktop ? 1 : 0;
		core_.lookAt.eyeWeight.store(static_cast<float>(eye));
		core_.lookAt.headWeight.store(static_cast<float>(head));
		core_.lookAt.bodyWeight.store(static_cast<float>(body));
		core_.config.eyeWeight = static_cast<float>(eye);
		core_.config.headWeight = static_cast<float>(head);
		core_.config.bodyWeight = static_cast<float>(body);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::GazeSetMode(const jsonlite::Value& params)
	{
		int64_t mode = -1;
		if (!IntParam(params, "mode", mode) || mode < 0 || mode > 2)
			return CmdResult::Err("invalid_params", "mode must be 0 (Auto), 1 (ForceOff) or 2 (DesktopOn)");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (!GazeRuntimeAvailable()) return CmdResult::Err("not_ready", "视线控制器接口尚未就绪");
		core_.lookAt.gazeMode.store(static_cast<GazeMode>(mode));
		core_.config.gazeMode = static_cast<int>(mode);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::GazeSetDesktop(const jsonlite::Value& params)
	{
		bool enabled = false;
		if (!BoolParam(params, "enabled", enabled))
			return CmdResult::Err("invalid_params", "enabled must be a boolean");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (!GazeRuntimeAvailable()) return CmdResult::Err("not_ready", "视线控制器接口尚未就绪");
		core_.lookAt.desktopLookAtEnabled.store(enabled);
		core_.config.desktopLookAtEnabled = enabled ? 1 : 0;
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::GazeSetWeights(const jsonlite::Value& params)
	{
		double eye = 0, head = 0, body = 0;
		if (!NumberParam(params, "eye", eye) || !NumberParam(params, "head", head) || !NumberParam(params, "body", body))
			return CmdResult::Err("invalid_params", "eye/head/body numbers are required");
		for (double value : {eye, head, body})
			if (value < 0.0 || value > 1.0)
				return CmdResult::Err("invalid_params", "视线权重必须在 0..1 范围内");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (!GazeRuntimeAvailable()) return CmdResult::Err("not_ready", "视线控制器接口尚未就绪");
		core_.lookAt.eyeWeight.store(static_cast<float>(eye));
		core_.lookAt.headWeight.store(static_cast<float>(head));
		core_.lookAt.bodyWeight.store(static_cast<float>(body));
		core_.config.eyeWeight = static_cast<float>(eye);
		core_.config.headWeight = static_cast<float>(head);
		core_.config.bodyWeight = static_cast<float>(body);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::GazeCycle()
	{
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (!GazeRuntimeAvailable()) return CmdResult::Err("not_ready", "视线控制器接口尚未就绪");
		const int mode = core_.CycleGazeMode();
		return CmdResult::Ok("{\"mode\":" + JNum(mode) + "}");
	}

	inline CmdResult Bridge::SoleSet(const jsonlite::Value& params)
	{
		const jsonlite::Value* settingsValue = Param(params, "settings");
		if (!settingsValue) return CmdResult::Err("invalid_params", "settings object is required");
		SoleMeasurementSettings settings;
		std::string error;
		if (!ParseSoleSettingsValue(*settingsValue, settings, error))
			return CmdResult::Err("invalid_params", error);
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		core_.QueueSoleSettings(settings);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::SolePitchSet(const jsonlite::Value& params)
	{
		const jsonlite::Value* settingsValue = Param(params, "settings");
		if (!settingsValue || !settingsValue->IsObject())
			return CmdResult::Err("invalid_params", "settings object is required");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		sole_pitch::Settings settings = core_.experimentalSolePitchSettings;
		if (const jsonlite::Value* enabled = settingsValue->Find("enabled"))
		{
			if (!enabled->IsBool()) return CmdResult::Err("invalid_params", "enabled must be a boolean");
			settings.experimentalSolePitchCorrection = enabled->boolValue;
		}
		else if (const jsonlite::Value* correction = settingsValue->Find("experimentalSolePitchCorrection"))
		{
			if (!correction->IsBool()) return CmdResult::Err("invalid_params", "experimentalSolePitchCorrection must be a boolean");
			settings.experimentalSolePitchCorrection = correction->boolValue;
		}
		if (const jsonlite::Value* envelope = settingsValue->Find("useMeshEnvelope"))
		{
			if (!envelope->IsBool()) return CmdResult::Err("invalid_params", "useMeshEnvelope must be a boolean");
			settings.useMeshEnvelope = envelope->boolValue;
		}
		if (const jsonlite::Value* field = settingsValue->Find("maxPitchDegrees"))
		{
			if (!field->IsNumber() || !std::isfinite(field->numberValue))
				return CmdResult::Err("invalid_params", "maxPitchDegrees must be a finite number");
			settings.maxPitchDegrees = static_cast<float>(field->numberValue);
		}
		if (!settings.IsValid())
			return CmdResult::Err("invalid_params", "maxPitchDegrees 必须在 0..35 范围内");
		core_.QueueExperimentalSolePitch(settings);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::StudioSet(const jsonlite::Value& params)
	{
		const jsonlite::Value* settingsValue = Param(params, "settings");
		if (!settingsValue) return CmdResult::Err("invalid_params", "settings object is required");
		studio::Settings settings;
		std::string error;
		if (!ParseStudioSettings(*settingsValue, settings, error))
			return CmdResult::Err("invalid_params", error);
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (!core_.QueueStudioSettings(settings))
			return CmdResult::Err("invalid_params", "布光与视频设置无效");
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::StudioSetLighting(const jsonlite::Value& params)
	{
		const jsonlite::Value* lighting = Param(params, "lighting");
		if (!lighting || !lighting->IsObject())
			return CmdResult::Err("invalid_params", "lighting object is required");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		studio::Settings settings = core_.studioSettings;
		std::string error;
		jsonlite::Value wrapper;
		wrapper.type = jsonlite::Type::Object;
		wrapper.objectMembers.emplace_back("lighting", *lighting);
		if (!ParseStudioSettings(wrapper, settings, error))
			return CmdResult::Err("invalid_params", error);
		if (!core_.QueueStudioSettings(settings))
			return CmdResult::Err("invalid_params", "布光设置无效");
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::StudioSetVideo(const jsonlite::Value& params)
	{
		const jsonlite::Value* video = Param(params, "video");
		if (!video || !video->IsObject())
			return CmdResult::Err("invalid_params", "video object is required");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		studio::Settings settings = core_.studioSettings;
		std::string error;
		jsonlite::Value wrapper;
		wrapper.type = jsonlite::Type::Object;
		wrapper.objectMembers.emplace_back("video", *video);
		if (!ParseStudioSettings(wrapper, settings, error))
			return CmdResult::Err("invalid_params", error);
		if (!core_.QueueStudioSettings(settings))
			return CmdResult::Err("invalid_params", "视频设置无效");
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::ExperimentsSet(const jsonlite::Value& params)
	{
		const jsonlite::Value* patch = Param(params, "patch");
		if (!patch || !patch->IsObject())
			return CmdResult::Err("invalid_params", "patch object is required");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		int mouth = core_.config.facialMikuMouthFit;
		int eye = core_.config.facialMikuEyeFit;
		int allow = core_.config.facialMikuFitAllowUnverified;
		if (!TriStateParam(*patch, "mikuMouthFit", mouth) ||
			!TriStateParam(*patch, "mikuEyeFit", eye) ||
			!TriStateParam(*patch, "mikuFitAllowUnverified", allow))
			return CmdResult::Err("invalid_params", "Miku 拟合开关必须为 boolean 或 -1/0/1");
		bool hasWaist = patch->Find("waistSupport") != nullptr;
		bool waist = core_.guiExperimentalWaistSupport.load();
		if (hasWaist && !BoolParam(*patch, "waistSupport", waist))
			return CmdResult::Err("invalid_params", "waistSupport must be a boolean");
		bool hasBeforeQuartz = patch->Find("beforeQuartz") != nullptr;
		bool beforeQuartz = core_.guiBeforeQuartzExperiment.load();
		if (hasBeforeQuartz && !BoolParam(*patch, "beforeQuartz", beforeQuartz))
			return CmdResult::Err("invalid_params", "beforeQuartz must be a boolean");
		if (hasBeforeQuartz && beforeQuartz && core_.guiPhysicsBackend.load() != 2)
			return CmdResult::Err("invalid_params", "BeforeQuartz 仅在原生注入后端可用");
		bool hasMuteSe = patch->Find("muteNativeSoundEffects") != nullptr;
		bool muteSe = core_.guiMuteNativeSoundEffects.load();
		if (hasMuteSe && !BoolParam(*patch, "muteNativeSoundEffects", muteSe))
			return CmdResult::Err("invalid_params", "muteNativeSoundEffects must be a boolean");
		core_.config.facialMikuMouthFit = mouth;
		core_.config.facialMikuEyeFit = eye;
		core_.config.facialMikuFitAllowUnverified = allow;
		if (hasWaist)
		{
			core_.guiExperimentalWaistSupport.store(waist);
			core_.QueueExperimentalWaistSupport(waist);
		}
		if (hasBeforeQuartz)
		{
			core_.guiBeforeQuartzExperiment.store(beforeQuartz);
			core_.QueueBeforeQuartzExperiment(beforeQuartz);
		}
		if (hasMuteSe)
		{
			core_.guiMuteNativeSoundEffects.store(muteSe);
			core_.QueueMuteNativeSoundEffects(muteSe);
		}
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::ExperimentsSetMiku(const jsonlite::Value& params)
	{
		int mouth = core_.config.facialMikuMouthFit;
		int eye = core_.config.facialMikuEyeFit;
		int allow = core_.config.facialMikuFitAllowUnverified;
		if (!TriStateParam(params, "mouthFit", mouth) || !TriStateParam(params, "eyeFit", eye) ||
			!TriStateParam(params, "allowUnverified", allow))
			return CmdResult::Err("invalid_params", "Miku 拟合开关必须为 true/false/null 或 -1/0/1");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		core_.config.facialMikuMouthFit = mouth;
		core_.config.facialMikuEyeFit = eye;
		core_.config.facialMikuFitAllowUnverified = allow;
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::ExperimentsSetWaistSupport(const jsonlite::Value& params)
	{
		bool enabled = false;
		if (!BoolParam(params, "enabled", enabled))
			return CmdResult::Err("invalid_params", "enabled must be a boolean");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		core_.QueueExperimentalWaistSupport(enabled);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::ExperimentsSetBeforeQuartz(const jsonlite::Value& params)
	{
		bool enabled = false;
		if (!BoolParam(params, "enabled", enabled))
			return CmdResult::Err("invalid_params", "enabled must be a boolean");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (enabled && core_.guiPhysicsBackend.load() != 2)
			return CmdResult::Err("invalid_params", "BeforeQuartz 仅在原生注入后端可用");
		core_.QueueBeforeQuartzExperiment(enabled);
		return CmdResult::Ok();
	}
} // namespace GakuMotion::webpanel
