namespace GakuMotion
{
	inline bool Core::ApplyCameraSettings(const GakuMotion::CameraSettings& settings, bool persistMode)
	{
		if (!settings.IsValid()) return false;
		const auto prevMode = camera.settings.mode;
		const auto prevBehavior = camera.settings.followBehavior;
		const bool prevEnabled = camera.settings.enabled;
		camera.settings = settings;
		if (!camera.settings.enabled) camera.Release();
		camera.Publish();
		if (persistMode && (prevEnabled != settings.enabled || prevMode != settings.mode || prevBehavior != settings.followBehavior) &&
			!configRoot.empty() && std::filesystem::exists(MotionPathFromUtf8(configRoot + "/camera.json")))
			SaveCameraSettings();
		return true;
	}

	inline bool Core::SetSoleSettings(const SoleMeasurementSettings& settings)
	{
		if (!settings.IsValid())
		{
			soleNotice = "鞋底测量设置无效，保留已应用设置";
			return false;
		}
		soleSettings = settings;
		soleSnapshot = {};
		soleNotice = "已应用测量设置（姿态不变，尚未保存）";
		return true;
	}

	inline bool Core::LoadStageLoadSettings()
	{
		stage_load::Settings saved;
		std::string error;
		const auto path = configRoot + "/stage.json";
		std::error_code ec;
		const bool exists = std::filesystem::exists(MotionPathFromUtf8(path), ec);
		const bool loaded = exists && !ec && saved.Load(path, error);
		StageSettings settings = stageManager.settings;
		settings.loadReplacementKey = loaded ? saved.key : "";
		stageManager.ApplySettings(settings);
		if (ec || (exists && !loaded))
		{
			stageLoadNotice = "舞台设置读取失败，已使用原生背景: " + (ec ? ec.message() : error);
			LogWarn("[GakuMotion][StageLoad] saved settings rejected: " + (ec ? ec.message() : error));
			return false;
		}
		stageLoadNotice = loaded ? "摄影背景选择已载入；下次进入偶像摄影时生效" :
			"将使用原生背景；可预设下次进入偶像摄影时使用的舞台";
		return loaded;
	}

	inline bool Core::SetStageLoadReplacement(const std::string& key)
	{
		if ((!key.empty() && !stage_load::IsTarget(key)) || configRoot.empty())
		{
			stageLoadNotice = "舞台选择未保存：选项无效或配置目录尚未就绪";
			return false;
		}
		stage_load::Settings saved{key};
		std::string error;
		if (!saved.Save(configRoot + "/stage.json", error))
		{
			stageLoadNotice = "舞台选择保存失败，保留原有选择: " + error;
			LogWarn("[GakuMotion][StageLoad] cannot save settings: " + error);
			return false;
		}
		StageSettings settings = stageManager.settings;
		settings.loadReplacementKey = key;
		stageManager.ApplySettings(settings);
		stageLoadNotice = key.empty() ? "已记住恢复原生背景；下次进入偶像摄影时生效" :
			"舞台选择已保存；下次进入偶像摄影时生效（跨区域和重启保留）";
		LogInfo("[GakuMotion][StageLoad] saved next Photography key=" + (key.empty() ? std::string("native") : key));
		return true;
	}

	inline bool Core::LoadStudioSettings()
	{
		studio::Settings loaded;
		std::string error;
		const auto path = configRoot + "/stage_studio.json";
		std::error_code ec;
		const bool exists = std::filesystem::exists(MotionPathFromUtf8(path), ec);
		const bool accepted = exists && !ec && loaded.Load(path, error);
		studioSettings = accepted ? loaded : studio::Settings{};
		++studioRevision;
		if (ec || (exists && !accepted))
		{
			studioLightingNotice = studioVideoNotice = "布光与视频设置读取失败，实验保持关闭: " + (ec ? ec.message() : error);
			LogWarn("[GakuMotion][Studio] saved settings rejected: " + (ec ? ec.message() : error));
			return false;
		}
		studioLightingNotice = studioSettings.lighting.enabled ? "已载入布光预设；等待有效摄影场景应用" : "自定义布光已关闭";
		studioVideoNotice = studioSettings.video.enabled ? "已载入视频设置；等待有效摄影场景应用" : "视频输入已关闭";
		return accepted;
	}

	inline bool Core::SetStudioSettings(const studio::Settings& settings)
	{
		std::string error;
		if (configRoot.empty() || !settings.Save(configRoot + "/stage_studio.json", error))
		{
			studioLightingNotice = studioVideoNotice = "布光与视频设置保存失败，保留原设置: " +
				(configRoot.empty() ? std::string("配置目录尚未就绪") : error);
			LogWarn("[GakuMotion][Studio] cannot save settings: " + error);
			return false;
		}
		studioSettings = settings;
		++studioRevision;
		studioLightingNotice = settings.lighting.enabled ? "布光预设已保存；将在有效摄影场景应用" : "已保存停用自定义布光；等待恢复结果";
		studioVideoNotice = settings.video.enabled ? "视频设置已保存；将在有效摄影场景应用" : "已保存停用视频输入；等待恢复结果";
		return true;
	}

	inline bool Core::LoadCameraSettings()
	{
		const auto path = configRoot + "/camera.json";
		if (!std::filesystem::exists(MotionPathFromUtf8(path))) return false;
		RuntimeConfig savedConfig;
		savedConfig.path = path;
		if (!savedConfig.Reload() || !savedConfig.cameraDeclared || !savedConfig.camera.IsValid())
		{
			cameraNotice = "镜头设置载入失败: " + savedConfig.lastError;
			return false;
		}
		camera.settings = savedConfig.camera;
		camera.Publish();
		cameraNotice = "已载入保存的镜头设置";
		return true;
	}

	inline bool Core::SaveCameraSettings()
	{
		const auto& s = camera.settings;
		if (!s.IsValid())
		{
			cameraNotice = "镜头设置无效，无法保存";
			return false;
		}
		std::error_code ec;
		std::filesystem::create_directories(MotionPathFromUtf8(configRoot), ec);
		if (ec)
		{
			cameraNotice = "镜头设置保存失败: " + ec.message();
			return false;
		}
		const auto path = configRoot + "/camera.json", temporary = path + ".tmp";
		std::ofstream out(MotionPathFromUtf8(temporary), std::ios::binary | std::ios::trunc);
		auto vector = [&](const Vector3& v)
		{
			out << '[' << jsonlite::FormatNumber(v.x, 6) << ", " << jsonlite::FormatNumber(v.y, 6)
				<< ", " << jsonlite::FormatNumber(v.z, 6) << ']';
		};
		auto transform = [&](const char* key, const CameraModeTransform& t)
		{
			out << ",\n    \"" << key << "\": {\"pos\": ["
				<< jsonlite::FormatNumber(t.position.x, 6) << ", "
				<< jsonlite::FormatNumber(t.position.y, 6) << ", "
				<< jsonlite::FormatNumber(t.position.z, 6) << "], \"rot\": ["
				<< jsonlite::FormatNumber(t.rotation.x, 6) << ", "
				<< jsonlite::FormatNumber(t.rotation.y, 6) << ", "
				<< jsonlite::FormatNumber(t.rotation.z, 6) << "]}";
		};
		out << "{\n  \"camera\": {\n    \"enabled\": " << (s.enabled ? "true" : "false")
			<< ",\n    \"mode\": " << static_cast<int>(s.mode)
			<< ",\n    \"follow_behavior\": " << static_cast<int>(s.followBehavior)
			<< ",\n    \"follow_target\": \"" << CameraFollowTargetName(s.followTarget) << '"'
			<< ",\n    \"follow_slot_index\": " << s.followSlotIndex
			<< ",\n    \"follow_actor\": " << (s.followActor ? "true" : "false")
			<< ",\n    \"limit_to_scene\": " << (s.limitToScene ? "true" : "false") << ",\n    \"bounds_min\": ";
		vector(s.boundsMin);
		out << ",\n    \"bounds_max\": ";
		vector(s.boundsMax);
		out << ",\n    \"follow_offset\": ";
		vector(s.followOffset);
		out << ",\n    \"target_offset\": ";
		vector(s.targetOffset);
		out << ",\n    \"vmd_offset\": ";
		vector(s.vmdOffset);
		out << ",\n    \"follow_factor\": ";
		vector(s.followFactor);
		out << ",\n    \"follow_rate\": ";
		vector(s.followRate);
		out << ",\n    \"follow_fov\": " << jsonlite::FormatNumber(s.followFov, 6)
			<< ",\n    \"vmd_range_scale\": " << jsonlite::FormatNumber(s.vmdRangeScale, 6)
			<< ",\n    \"actor_shadow_experiment_mode\": " << s.actorShadowExperimentMode
			<< ",\n    \"use_per_mode_transforms\": " << (s.usePerModeTransforms ? "true" : "false");
		transform("transform_track", s.transformTrack);
		transform("transform_follow_move", s.transformFollowMove);
		transform("transform_look_at", s.transformLookAt);
		out << "\n  }\n}\n";
		out.close();
		if (!out)
		{
			cameraNotice = "镜头设置保存失败：无法写入文件";
			return false;
		}
		std::filesystem::copy_file(MotionPathFromUtf8(temporary), MotionPathFromUtf8(path), std::filesystem::copy_options::overwrite_existing, ec);
		if (ec)
		{
			cameraNotice = "镜头设置保存失败: " + ec.message();
			return false;
		}
		std::filesystem::remove(MotionPathFromUtf8(temporary), ec);
		cameraNotice = "镜头设置已保存，下次启动自动载入";
		return true;
	}
} // namespace GakuMotion
