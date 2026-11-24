namespace GakuMotion
{
	inline std::string RuntimeConfig::DefaultTemplate()
	{
		return
			"// GakuMotion 运行时控制文件（热加载，改动后约 0.5 秒生效）\n"
			"// ---------------------------------------------------------------\n"
			"// 声明式字段：写什么就是什么，持续生效。\n"
			"// 一次性命令：command.seq 必须比上一次更大才会执行。\n"
			"{\n"
			"  \"poll_interval_seconds\": 0.5,\n"
			"  \"playback_speed_multiplier\": 1.0,   // 播放速度倍率 [0.1, 5.0]；有音频时以音频为主时钟保持同步\n"
			"\n"
			"  \"selected_motion\": \"\",   // 留空 = 启动时不选曲；填入曲库内的曲目名可自动选中\n"
			"  \"import_backend\": \"csharp\",      // VMD 导入后端：csharp（原生导入，需配套 DLL）\n"
			"  \"autoplay\": false,\n"
			"  \"camera\": {\n"
			"    \"enabled\": true, \"mode\": 0, \"follow_behavior\": 1, \"follow_actor\": false,\n"
			"    \"follow_target\": \"hips\", // Follow mode target: hips, chest or head\n"
			"    \"limit_to_scene\": false,\n"
			"    \"bounds_min\": [-5, 0.05, -5], \"bounds_max\": [5, 5, 5],\n"
			"    \"follow_offset\": [0, 0.6, 3], \"target_offset\": [0, 0.4, 0], \"vmd_offset\": [0, 0, 0],\n"
			"    \"follow_factor\": [1.0, 1.0, 1.0], // Captured camera-local displacement fractions\n"
			"    \"follow_rate\": [0.1, 0.1, 0.1], // 0: hold, 0.1: smooth pursuit, 1: immediate\n"
			"    \"follow_fov\": 45, \"vmd_range_scale\": 1.0,\n"
			"    // 可选分模式镜头位姿：\"transform_track\", \"transform_follow_move\", \"transform_look_at\": {\"pos\": [x,y,z], \"rot\": [rx,ry,rz]}\n"
			"    \"use_per_mode_transforms\": false\n"
			"  },\n"
			"\n"
			"  \"gaze\": {\n"
			"    \"mode\": 0,                 // 0=Auto(VR 时避让 VRify) 1=ForceOff(纯 VMD 头动) 2=DesktopOn\n"
			"    \"desktop_enabled\": true,\n"
			"    \"eye_weight\": 0.7,\n"
			"    \"head_weight\": 0.4,\n"
			"    \"body_weight\": 0.0\n"
			"  },\n"
			"\n"
			"  \"use_rest_rotations\": true,\n"
			"  // gkclip 眼骨：按角色实测眼区响应比缩放（#150 §8）；false = raw 1:1 对照\n"
			"  \"eye_pose_relative_gain\": true,\n"
			"  // Experimental: keep the native animation graph for hair/cloth; needs device acceptance.\n"
			"  \"secondary_physics\": { \"backend\": \"native\", \"photography_native_only\": false, \"preserve_animator\": false, \"sleeve_muscle_diagnostics\": false },\n"
			"  // 可选：\"native_audio\": { \"mute_sound_effects\": true }（默认开启；界面切换持久化于 native_audio.json）\n"
			"\n"
			"  // 面部表情默认基线与动态让位（Ducking）：\n"
			"  //   enable_baseline: 无明确表情时使用角色官方默认；明确开口时让位（默认 true）\n"
			"  //   baseline_multiplier: 默认表情强度倍率 [0.0, 2.0]（默认 1.0）\n"
			"  //   角色身份由实际绑定对象自动识别，旧 character 覆盖字段不再生效。\n"
			"  \"facial\": {\n"
			"    \"enable_baseline\": true,\n"
			"    \"baseline_multiplier\": 1.0,\n"
			"    \"miku_mouth_fit\": true,\n"
			"    \"miku_eye_fit\": true,\n"
			"    \"miku_fit_allow_unverified\": false\n"
			"  },\n"
			"\n"
			"  // 辅助骨（_Roll_H / _H）驱动模式 —— 实机靶向对照实验\n"
			"  //   0 或 \"normal\"   = 现行 Quartz 半角/半扭转注入（默认）\n"
			"  //   1 或 \"silence\"  = 硬锁初始绑定局部姿态（绝对静默，刚性随父跟动）\n"
			"  //   2 或 \"identity\" = 局部旋转强制归零\n"
			"  //   3 或 \"replay\"   = 直灌官方录制真值（读 <曲目>/aux_replay.json）\n"
			"  \"aux\": { \"mode\": 0 },\n"
			"\n"
			"  \"overrides\": {\n"
			"    \"enabled\": true,\n"
			"    \"global_file\": \"GakuMotion/bone_overrides.json\",\n"
			"    \"motion_file\": \"\"         // 留空 = 使用 <曲目目录>/bone_overrides.json\n"
			"  },\n"
			"\n"
			"  \"command\": {\n"
			"    \"seq\": 0,                  // 每次要执行命令时请 +1\n"
			"    \"play\": false,\n"
			"    \"pause\": false,\n"
			"    \"stop\": false,\n"
			"    \"seek_seconds\": -1,        // >=0 时生效\n"
			"    \"reload_overrides\": false,\n"
			"    \"toggle_overrides\": false\n"
			"  }\n"
			"}\n";
	}

	inline bool RuntimeConfig::EnsureFileExists()
	{
		std::error_code ec;
		if (path.empty()) return false;
		if (std::filesystem::exists(MotionPathFromUtf8(path), ec))
		{
			fileFound = true;
			return false;
		}
		std::filesystem::create_directories(MotionPathFromUtf8(path).parent_path(), ec);
		std::ofstream out(MotionPathFromUtf8(path), std::ios::binary);
		if (!out.is_open())
		{
			lastError = "cannot create control file: " + path;
			return false;
		}
		out << DefaultTemplate();
		out.close();
		fileFound = true;
		return true;
	}

	inline bool RuntimeConfig::Poll(float deltaTime, bool forceReload)
	{
		pollInterval = (pollIntervalSeconds > 0.05f) ? pollIntervalSeconds : 0.05f;

		bool doReload = forceReload || forceReload_;
		forceReload_ = false;

		if (!doReload)
		{
			timer_ += (deltaTime > 0.0f) ? deltaTime : 0.0167f;
			if (timer_ < pollInterval) return false;
		}
		timer_ = 0.0f;

		if (path.empty()) return false;

		FileSignature current = StatFile(path);
		if (!current.exists)
		{
			if (fileFound || loadedOnce)
			{
				lastError = "control file disappeared: " + path;
				fileFound = false;
			}
			return false;
		}
		fileFound = true;

		if (!doReload && loadedOnce && current == signature)
		{
			return false;
		}

		signature = current;
		loadedOnce = true;
		return Reload();
	}
} // namespace GakuMotion
