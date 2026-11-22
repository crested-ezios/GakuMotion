namespace GakuMotion::webpanel
{
	inline CmdResult Bridge::MutationGuardLocked()
	{
		if (!core_.InjectionAllowed())
			return CmdResult::Err("not_allowed",
				core_.injectionStatus.empty() ? "Injection is not allowed in the current scene" : core_.injectionStatus);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::TransportSimple(GuiAction action, bool requiresClip)
	{
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		if (requiresClip && !core_.player.clip.total_frames)
			return CmdResult::Err("not_ready", "No motion is loaded");
		if (action == GuiAction::Pause &&
			core_.player.state != PlaybackState::Playing && core_.player.state != PlaybackState::Paused)
			return CmdResult::Err("not_ready", "Playback is not active");
		core_.QueueGui({action});
		ForcePlayerEvent();
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::SeekCommand(const jsonlite::Value& params, GuiAction action)
	{
		double seconds = 0;
		if (!NumberParam(params, "seconds", seconds) || seconds < 0)
			return CmdResult::Err("invalid_params", "seconds must be a finite number >= 0");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		if (!core_.player.clip.total_frames) return CmdResult::Err("not_ready", "No motion is loaded");
		const float duration = core_.player.TimelineDuration();
		if (seconds > duration)
			return CmdResult::Err("invalid_params", "seconds exceeds the timeline duration");
		switch (action)
		{
		case GuiAction::Seek: core_.QueueSeek(static_cast<float>(seconds)); break;
		case GuiAction::ScrubBegin: core_.QueueScrubBegin(static_cast<float>(seconds)); break;
		case GuiAction::ScrubUpdate: core_.QueueScrubUpdate(static_cast<float>(seconds)); break;
		case GuiAction::ScrubEnd: core_.QueueScrubEnd(static_cast<float>(seconds)); break;
		default: break;
		}
		ForcePlayerEvent();
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::StageSet(const jsonlite::Value& params)
	{
		const jsonlite::Value* settings = Param(params, "settings");
		if (!settings || !settings->IsObject())
			return CmdResult::Err("invalid_params", "settings object is required");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		StageSettings parsed;
		{
			std::lock_guard<std::mutex> stageLock(core_.stageManager.stageMutex);
			parsed = core_.stageManager.settings;
		}
		std::string error;
		if (!ParseStageSettings(*settings, parsed, error))
			return CmdResult::Err("invalid_params", error);
		// Cross-domain side effects formerly owned by the ImGui stage page.
		bool lightingChanged = false;
		bool restoreSkybox = false;
		{
			std::lock_guard<std::mutex> stageLock(core_.stageManager.stageMutex);
			lightingChanged = parsed.lightingEffectsEnabled != core_.stageManager.settings.lightingEffectsEnabled;
			restoreSkybox = core_.stageManager.settings.hideBackground && !parsed.hideBackground;
		}
		if (parsed.hideBackground)
		{
			parsed.greenScreenActive = true;
			parsed.overrideCameraClear = true;
		}
		else
		{
			parsed.greenScreenActive = false;
			parsed.overrideCameraClear = false;
		}
		if (lightingChanged)
		{
			CameraSettings cameraSettings = core_.camera.settings;
			cameraSettings.actorShadowExperimentMode = parsed.lightingEffectsEnabled ? 1 : 2;
			core_.QueueCameraSettings(cameraSettings);
		}
		core_.QueueStageSettings(parsed);
		if (restoreSkybox) core_.QueueRestoreCameraSkybox();
		return CmdResult::Ok();
	}

	inline void Bridge::EnsureFormationSyncedLocked()
	{
		if (!core_.fnEnsembleActorEligible && !core_.player.controller.fnCanDriveActor &&
			core_.formation.actorEpoch != core_.sceneActorRegistry.Epoch())
		{
			core_.SyncFormationWithRoster();
		}
	}

	inline CmdResult Bridge::ActorsGetPortrait(const jsonlite::Value& params)
	{
		const jsonlite::Value* charField = Param(params, "character");
		const jsonlite::Value* tokenField = Param(params, "token");
		if (!charField && !tokenField)
			return CmdResult::Err("invalid_params", "character or token is required");

		std::string normCode;
		if (charField)
		{
			if (!charField->IsString() || !portrait::IsSafeCharacterCode(charField->stringValue))
				return CmdResult::Err("invalid_params", "character code is invalid");
			normCode = portrait::NormalizeCharacterCode(charField->stringValue);
		}
		else
		{
			int64_t token = 0;
			if (!IntParam(params, "token", token) || token <= 0)
				return CmdResult::Err("invalid_params", "token must be a positive integer");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			for (const auto& item : core_.sceneActorRegistry.Entries())
			{
				if (item.token == static_cast<uint64_t>(token))
				{
					if (portrait::IsSafeCharacterCode(item.character))
						normCode = portrait::NormalizeCharacterCode(item.character);
					break;
				}
			}
			if (normCode.empty())
			{
				return CmdResult::Ok("{\"ok\":true,\"character\":\"\",\"cached\":false,\"dataUri\":null}");
			}
		}

		std::string dataUri;
		bool cached = false;
		if (hooks_.getActorPortraitDataUri)
		{
			hooks_.getActorPortraitDataUri(normCode, dataUri, cached);
		}
		else
		{
			std::string configRoot;
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				configRoot = core_.configRoot;
			}
			const auto path = portrait::PortraitCachePath(configRoot, normCode);
			std::error_code ec;
			if (!path.empty() && std::filesystem::is_regular_file(path, ec) && !ec)
			{
				std::ifstream ifs(path, std::ios::binary | std::ios::ate);
				if (ifs)
				{
					const auto len = ifs.tellg();
					if (len >= 45 && len <= static_cast<std::streamoff>(16LL * 1024LL * 1024LL))
					{
						ifs.seekg(0);
						std::vector<uint8_t> bytes(static_cast<size_t>(len));
						if (ifs.read(reinterpret_cast<char*>(bytes.data()), len) &&
							portrait::IsValidPngBytes(bytes))
						{
							dataUri = cover::ToDataUri(bytes, "image/png");
							cached = !dataUri.empty();
						}
					}
				}
			}
		}

		return CmdResult::Ok("{\"ok\":true,\"character\":" + JString(normCode) +
			",\"cached\":" + JBool(cached) +
			",\"dataUri\":" + (cached && !dataUri.empty() ? JString(dataUri) : "null") + "}");
	}

	inline CmdResult Bridge::FormationSelectSlot(const jsonlite::Value& params)
	{
		int64_t slotIndex = -1;
		if (!IntParam(params, "slotIndex", slotIndex) || slotIndex < 0)
			return CmdResult::Err("invalid_params", "slotIndex must be a non-negative integer");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		EnsureFormationSyncedLocked();
		if (slotIndex >= core_.formation.slotCount)
			return CmdResult::Err("invalid_params", "slotIndex is out of range");
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		core_.QueueFormationSelectSlot(static_cast<int>(slotIndex));
		forceDomainRefresh_.store(true, std::memory_order_release);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::FormationSwapSlots(const jsonlite::Value& params)
	{
		int64_t fromSlotIndex = -1, toSlotIndex = -1;
		if (!IntParam(params, "fromSlotIndex", fromSlotIndex) || fromSlotIndex < 0 ||
			!IntParam(params, "toSlotIndex", toSlotIndex) || toSlotIndex < 0 ||
			fromSlotIndex == toSlotIndex)
			return CmdResult::Err("invalid_params", "fromSlotIndex and toSlotIndex must be distinct non-negative integers");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		EnsureFormationSyncedLocked();
		if (fromSlotIndex >= core_.formation.slotCount || toSlotIndex >= core_.formation.slotCount)
			return CmdResult::Err("invalid_params", "slot indices are out of range");
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		core_.QueueFormationSwapSlots(static_cast<int>(fromSlotIndex), static_cast<int>(toSlotIndex));
		forceDomainRefresh_.store(true, std::memory_order_release);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::FormationSetSlotOffset(const jsonlite::Value& params)
	{
		int64_t slotIndex = -1;
		double x = 0.0, y = 0.0, z = 0.0, yawDegrees = 0.0;
		if (!IntParam(params, "slotIndex", slotIndex) || slotIndex < 0 ||
			!NumberParam(params, "x", x) || !NumberParam(params, "y", y) || !NumberParam(params, "z", z))
			return CmdResult::Err("invalid_params", "slotIndex, x, y, z are required");
		for (const double v : {x, y, z})
			if (v < -10.0 || v > 10.0)
				return CmdResult::Err("invalid_params", "offset x/y/z must be within -10..10");
		if (params.Find("yawDegrees"))
		{
			if (!NumberParam(params, "yawDegrees", yawDegrees) || yawDegrees < -360.0 || yawDegrees > 360.0)
				return CmdResult::Err("invalid_params", "yawDegrees must be within -360..360");
		}
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		EnsureFormationSyncedLocked();
		if (slotIndex >= core_.formation.slotCount)
			return CmdResult::Err("invalid_params", "slotIndex is out of range");
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		core_.QueueFormationSetSlotOffset(static_cast<int>(slotIndex),
			Vector3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)},
			static_cast<float>(yawDegrees));
		forceDomainRefresh_.store(true, std::memory_order_release);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::FormationResetSlotOffset(const jsonlite::Value& params)
	{
		int64_t slotIndex = -1;
		if (!IntParam(params, "slotIndex", slotIndex) || slotIndex < 0)
			return CmdResult::Err("invalid_params", "slotIndex must be a non-negative integer");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		EnsureFormationSyncedLocked();
		if (slotIndex >= core_.formation.slotCount)
			return CmdResult::Err("invalid_params", "slotIndex is out of range");
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		core_.QueueFormationResetSlotOffset(static_cast<int>(slotIndex));
		forceDomainRefresh_.store(true, std::memory_order_release);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::FormationSaveSlotOffset(const jsonlite::Value& params)
	{
		int64_t slotIndex = -1;
		if (!IntParam(params, "slotIndex", slotIndex) || slotIndex < 0)
			return CmdResult::Err("invalid_params", "slotIndex must be a non-negative integer");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		EnsureFormationSyncedLocked();
		if (slotIndex >= core_.formation.slotCount)
			return CmdResult::Err("invalid_params", "slotIndex is out of range");
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		core_.QueueFormationSaveSlotOffset(static_cast<int>(slotIndex));
		forceDomainRefresh_.store(true, std::memory_order_release);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::FormationSetSlotSource(const jsonlite::Value& params)
	{
		int64_t slotIndex = -1, sourceSlotIndex = -1;
		if (!IntParam(params, "slotIndex", slotIndex) || slotIndex < 0 ||
			!IntParam(params, "sourceSlotIndex", sourceSlotIndex) || sourceSlotIndex < 0)
			return CmdResult::Err("invalid_params", "slotIndex and sourceSlotIndex must be non-negative integers");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		EnsureFormationSyncedLocked();
		const int maxSource = (std::max)(1, core_.formation.trackPositionCount);
		if (slotIndex >= core_.formation.slotCount || sourceSlotIndex >= maxSource)
			return CmdResult::Err("invalid_params", "slotIndex or sourceSlotIndex is out of range");
		if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
		core_.QueueFormationSetSlotSource(static_cast<int>(slotIndex), static_cast<int>(sourceSlotIndex));
		forceDomainRefresh_.store(true, std::memory_order_release);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::StageToggleNode(const jsonlite::Value& params)
	{
		int64_t index = -1;
		bool active = false;
		if (!IntParam(params, "index", index) || !BoolParam(params, "active", active))
			return CmdResult::Err("invalid_params", "index and active are required");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		std::lock_guard<std::mutex> stageLock(core_.stageManager.stageMutex);
		const auto& candidates = core_.stageManager.candidates;
		if (index < 0 || index >= static_cast<int64_t>(candidates.size()))
			return CmdResult::Err("invalid_params", "场景节点索引无效，请重新扫描");
		const StageCandidate& candidate = candidates[static_cast<size_t>(index)];
		if (candidate.isColliderLocked)
			return CmdResult::Err("protected", "安全保护：碰撞与相机活动边界锁定开启，禁止修改");
		int ancestor = candidate.parentIndex;
		int depth = 0;
		while (ancestor >= 0 && ancestor < static_cast<int>(candidates.size()) && depth++ < static_cast<int>(candidates.size()))
		{
			if (!candidates[static_cast<size_t>(ancestor)].currentlyActive)
				return CmdResult::Err("protected", "父级节点已隐藏，请先恢复父级再修改子节点");
			ancestor = candidates[static_cast<size_t>(ancestor)].parentIndex;
		}
		core_.QueueToggleStageCandidate(static_cast<int>(index), active);
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::StageSetLoadReplacement(const jsonlite::Value& params)
	{
		std::string key;
		if (!StringParam(params, "key", key))
			return CmdResult::Err("invalid_params", "key must be a string");
		if (!key.empty() && !stage_load::IsTarget(key))
			return CmdResult::Err("invalid_params", "舞台选择无效");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (core_.configRoot.empty())
			return CmdResult::Err("not_ready", "配置目录尚未就绪");
		if (!core_.SetStageLoadReplacement(key))
			return CmdResult::Err("failed", core_.stageLoadNotice);
		return CmdResult::Ok("{\"notice\":" + JString(core_.stageLoadNotice) + "}");
	}
} // namespace GakuMotion::webpanel
