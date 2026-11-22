namespace GakuMotion
{
	inline bool Core::LoadMusicVolume()
	{
		const auto path = configRoot + "/music_volume.json";
		std::error_code ec;
		if (!std::filesystem::exists(MotionPathFromUtf8(path), ec)) return false;
		jsonlite::Value root;
		std::string error;
		if (!jsonlite::ParseFile(path, root, error) || root.IntPath("version", 0) != 1)
		{ musicVolumeNotice = "音乐音量载入失败：配置无效"; return false; }
		const auto* value = root.Find("volume");
		if (!value || !value->IsNumber()) { musicVolumeNotice = "音乐音量载入失败：配置无效"; return false; }
		const float volume = value->AsFloat(-1.0f);
		if (!std::isfinite(volume) || volume < 0.0f || volume > 1.0f)
		{ musicVolumeNotice = "音乐音量载入失败：数值应在 0 到 1 之间"; return false; }
		if (!player.SetMusicVolume(volume)) { musicVolumeNotice = "音乐音量暂不可调"; return false; }
		musicVolumeNotice.clear();
		return true;
	}

	inline bool Core::SaveMusicVolume()
	{
		const float volume = player.musicVolume;
		if (!std::isfinite(volume) || volume < 0.0f || volume > 1.0f)
		{ musicVolumeNotice = "音乐音量保存失败：数值无效"; return false; }
		std::error_code ec;
		std::filesystem::create_directories(MotionPathFromUtf8(configRoot), ec);
		if (ec) { musicVolumeNotice = "音乐音量保存失败: " + ec.message(); return false; }
		const auto path = configRoot + "/music_volume.json", temporary = path + ".tmp";
		std::ofstream out(MotionPathFromUtf8(temporary), std::ios::binary | std::ios::trunc);
		out << "{\n  \"version\": 1,\n  \"volume\": " << jsonlite::FormatNumber(volume, 6) << "\n}\n";
		out.close();
		if (!out) { musicVolumeNotice = "音乐音量保存失败：无法写入文件"; return false; }
		std::filesystem::copy_file(MotionPathFromUtf8(temporary), MotionPathFromUtf8(path),
		                           std::filesystem::copy_options::overwrite_existing, ec);
		if (ec) { musicVolumeNotice = "音乐音量保存失败: " + ec.message(); return false; }
		std::filesystem::remove(MotionPathFromUtf8(temporary), ec);
		return true;
	}

	inline bool Core::LoadNativeAudioSettings()
	{
		const auto path = configRoot + "/native_audio.json";
		std::error_code ec;
		if (!std::filesystem::exists(MotionPathFromUtf8(path), ec)) return false;
		jsonlite::Value root;
		std::string error;
		if (!jsonlite::ParseFile(path, root, error) || root.IntPath("version", 0) != 1)
			return false;
		const auto* value = root.Find("mute_sound_effects");
		if (!value || !value->IsBool()) return false;
		const bool enabled = value->boolValue;
		guiMuteNativeSoundEffects.store(enabled, std::memory_order_release);
		config.muteNativeSoundEffects = enabled ? 1 : 0;
		applied.muteNativeSoundEffects = config.muteNativeSoundEffects;
		return true;
	}

	inline bool Core::SaveNativeAudioSettings()
	{
		if (configRoot.empty()) return false;
		std::error_code ec;
		std::filesystem::create_directories(MotionPathFromUtf8(configRoot), ec);
		if (ec) return false;
		const auto path = configRoot + "/native_audio.json", temporary = path + ".tmp";
		std::ofstream out(MotionPathFromUtf8(temporary), std::ios::binary | std::ios::trunc);
		out << "{\n  \"version\": 1,\n  \"mute_sound_effects\": "
		    << (guiMuteNativeSoundEffects.load(std::memory_order_acquire) ? "true" : "false")
		    << "\n}\n";
		out.close();
		if (!out) return false;
		std::filesystem::copy_file(MotionPathFromUtf8(temporary), MotionPathFromUtf8(path),
		                           std::filesystem::copy_options::overwrite_existing, ec);
		if (ec) return false;
		std::filesystem::remove(MotionPathFromUtf8(temporary), ec);
		return true;
	}

	inline bool Core::LoadFacialWeights()
	{
		const auto path = configRoot + "/facial_weights.json";
		std::error_code ec;
		if (!std::filesystem::exists(MotionPathFromUtf8(path), ec)) return false;
		jsonlite::Value root;
		std::string error;
		if (!jsonlite::ParseFile(path, root, error) || root.IntPath("version", 0) != 1)
		{
			facialWeightsNotice = "表情幅度载入失败：配置无效";
			return false;
		}
		auto parseWeightObject = [&](const jsonlite::Value& obj, FacialWeights& outWeights) -> bool
		{
			if (!obj.IsObject()) return false;
			for (const auto& entry : {std::pair<const char*, float*>{"overall", &outWeights.overall},
			     {"brow", &outWeights.brow}, {"eye", &outWeights.eye}, {"mouth", &outWeights.mouth}})
			{
				const auto* value = obj.Find(entry.first);
				if (!value || !value->IsNumber())
				{
					facialWeightsNotice = "表情幅度载入失败：配置无效";
					return false;
				}
				const float number = value->AsFloat(-1.0f);
				if (!std::isfinite(number) || number < 0.0f || number > 2.0f)
				{
					facialWeightsNotice = "表情幅度载入失败：数值应在 0 到 2 之间";
					return false;
				}
				*entry.second = number;
			}
			return true;
		};

		FacialWeights loadedDefault;
		if (!parseWeightObject(root, loadedDefault)) return false;

		std::map<std::string, FacialWeights> loadedByCharacter;
		if (const auto* byChar = root.Find("by_character"))
		{
			if (!byChar->IsObject())
			{
				facialWeightsNotice = "表情幅度载入失败：by_character 配置无效";
				return false;
			}
			for (const auto& kv : byChar->objectMembers)
			{
				if (kv.first.empty()) continue;
				FacialWeights charWeights;
				if (!parseWeightObject(kv.second, charWeights)) return false;
				loadedByCharacter[kv.first] = charWeights;
			}
		}

		defaultFacialWeights = loadedDefault;
		facialWeightsByCharacter = std::move(loadedByCharacter);

		for (auto& slot : formation.slots)
		{
			if (!slot.occupied || slot.actorToken == 0) continue;
			const FacialWeights resolved = ResolveSavedFacialWeightsForCharacter(slot.character);
			slot.facialWeights = resolved;
			auto& cust = actorCustomizations[slot.actorToken];
			cust.facialWeights = resolved;
			cust.facialCustomized = (!slot.character.empty() && facialWeightsByCharacter.contains(slot.character));
			if (auto it = formationTracks.find(slot.actorToken); it != formationTracks.end() && it->second)
			{
				it->second->controller.facialWeights = resolved;
			}
		}
		SyncPlayerFacialAndOverridesToSelectedSlot();
		facialWeightsNotice = "已载入保存的表情幅度";
		return true;
	}

	inline bool Core::SaveFacialWeights()
	{
		SyncSelectedFacialWeightsFromPlayer();
		const FacialWeights w = player.controller.facialWeights;
		for (const float value : {w.overall, w.brow, w.eye, w.mouth})
			if (!std::isfinite(value) || value < 0.0f || value > 2.0f)
			{ facialWeightsNotice = "表情幅度保存失败：数值无效"; return false; }

		if (const auto* sel = formation.SelectedSlot(); sel && sel->occupied && !sel->character.empty())
		{
			facialWeightsByCharacter[sel->character] = w;
		}
		else
		{
			defaultFacialWeights = w;
		}

		std::error_code ec;
		std::filesystem::create_directories(MotionPathFromUtf8(configRoot), ec);
		if (ec) { facialWeightsNotice = "表情幅度保存失败: " + ec.message(); return false; }
		const auto path = configRoot + "/facial_weights.json", temporary = path + ".tmp";
		std::ofstream out(MotionPathFromUtf8(temporary), std::ios::binary | std::ios::trunc);
		out << "{\n  \"version\": 1,\n  \"overall\": " << jsonlite::FormatNumber(defaultFacialWeights.overall, 6)
			<< ",\n  \"brow\": " << jsonlite::FormatNumber(defaultFacialWeights.brow, 6)
			<< ",\n  \"eye\": " << jsonlite::FormatNumber(defaultFacialWeights.eye, 6)
			<< ",\n  \"mouth\": " << jsonlite::FormatNumber(defaultFacialWeights.mouth, 6)
			<< ",\n  \"by_character\": {";
		bool firstChar = true;
		for (const auto& kv : facialWeightsByCharacter)
		{
			if (kv.first.empty()) continue;
			if (!firstChar) out << ",";
			firstChar = false;
			out << "\n    \"" << jsonlite::EscapeString(kv.first) << "\": {\"overall\": "
				<< jsonlite::FormatNumber(kv.second.overall, 6)
				<< ", \"brow\": " << jsonlite::FormatNumber(kv.second.brow, 6)
				<< ", \"eye\": " << jsonlite::FormatNumber(kv.second.eye, 6)
				<< ", \"mouth\": " << jsonlite::FormatNumber(kv.second.mouth, 6) << "}";
		}
		out << (firstChar ? "}\n}\n" : "\n  }\n}\n");
		out.close();
		if (!out) { facialWeightsNotice = "表情幅度保存失败：无法写入文件"; return false; }
		std::filesystem::copy_file(MotionPathFromUtf8(temporary), MotionPathFromUtf8(path),
		                           std::filesystem::copy_options::overwrite_existing, ec);
		if (ec) { facialWeightsNotice = "表情幅度保存失败: " + ec.message(); return false; }
		std::filesystem::remove(MotionPathFromUtf8(temporary), ec);
		facialWeightsNotice = "表情幅度已保存，下次启动自动载入";
		return true;
	}

	inline bool Core::LoadInitialPosition()
	{
		std::error_code ec;
		if (!std::filesystem::exists(MotionPathFromUtf8(initialPositionPath), ec)) return false;
		jsonlite::Value root;
		std::string error;
		if (!jsonlite::ParseFile(initialPositionPath, root, error))
		{
			initialPositionNotice = "初始位置载入失败: " + error;
			return false;
		}
		const auto* values = root.Find("offset_m");
		Vector3 parsed{};
		if (values && values->IsArray() && values->Size() == 3 &&
			values->At(0)->IsNumber() && values->At(1)->IsNumber() && values->At(2)->IsNumber())
		{
			parsed = Vector3(values->At(0)->AsFloat(0), values->At(1)->AsFloat(0), values->At(2)->AsFloat(0));
		}
		if (root.IntPath("version", 0) != 1 || root.StringPath("space", "") != "world" ||
			!values || !values->IsArray() || values->Size() != 3 ||
			!values->At(0)->IsNumber() || !values->At(1)->IsNumber() || !values->At(2)->IsNumber() ||
			!player.controller.SetInitialPositionOffset(parsed))
		{
			initialPositionNotice = "初始位置文件无效：需要 world 坐标下的三个有限数值";
			return false;
		}
		savedSlot0InitialOffset = parsed;
		if (!formation.slots.empty())
		{
			formation.slots[0].offset = parsed;
			formation.slots[0].placementDirty = false;
			if (formation.active && formation.slots[0].occupied && formation.slots[0].actorToken != 0)
			{
				if (auto it = formationTracks.find(formation.slots[0].actorToken);
					it != formationTracks.end() && it->second)
				{
					it->second->controller.SetInitialPositionOffset(parsed);
				}
			}
		}
		initialPositionNotice = "已载入保存的初始位置";
		return true;
	}

	inline bool Core::SaveInitialPosition()
	{
		const auto offset = player.controller.initialPositionOffset;
		if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z))
		{
			initialPositionNotice = "初始位置保存失败：坐标必须为有限数值";
			return false;
		}
		std::error_code ec;
		std::filesystem::create_directories(MotionPathFromUtf8(configRoot), ec);
		if (ec)
		{
			initialPositionNotice = "初始位置保存失败: " + ec.message();
			return false;
		}
		const auto temporary = initialPositionPath + ".tmp";
		std::ofstream out(MotionPathFromUtf8(temporary), std::ios::binary | std::ios::trunc);
		out << "{\n  \"version\": 1,\n  \"space\": \"world\",\n  \"offset_m\": ["
			<< jsonlite::FormatNumber(offset.x, 6) << ", " << jsonlite::FormatNumber(offset.y, 6) << ", "
			<< jsonlite::FormatNumber(offset.z, 6) << "]\n}\n";
		out.close();
		if (!out)
		{
			initialPositionNotice = "初始位置保存失败：无法写入文件";
			return false;
		}
		std::filesystem::copy_file(MotionPathFromUtf8(temporary), MotionPathFromUtf8(initialPositionPath),
		                           std::filesystem::copy_options::overwrite_existing, ec);
		if (ec)
		{
			initialPositionNotice = "初始位置保存失败: " + ec.message();
			return false;
		}
		std::filesystem::remove(MotionPathFromUtf8(temporary), ec);
		initialPositionNotice = "初始位置已保存，下次启动自动载入";
		return true;
	}
} // namespace GakuMotion
