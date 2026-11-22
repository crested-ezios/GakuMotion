namespace GakuMotion
{
	inline std::string Core::CurrentMotionOverridePath() const
	{
		if (!config.overrideMotionFile.empty())
		{
			return ResolvePath(config.overrideMotionFile, std::string());
		}
		int slot1Based = 1;
		if (const auto* sel = formation.SelectedSlot())
		{
			slot1Based = sel->sourceSlotIndex + 1;
		}
		const MotionEntry* m = CurrentMotion();
		if (m) return m->SlotDirectory(slot1Based) + "/bone_overrides.json";
		if (!player.currentMotionDir.empty())
			return ResolveMotionSlotDirectory(player.currentMotionDir, slot1Based) + "/bone_overrides.json";
		return std::string();
	}

	inline void Core::LoadOverrides(bool verbose)
	{
		// 全局
		globalOverrides.LoadFromFile(globalOverridePath);
		if (!globalOverrides.lastError.empty())
		{
			LogWarn("[GakuMotion][Overrides] global override load failed: " + globalOverrides.lastError);
		}
		for (const auto& w : globalOverrides.warnings)
		{
			LogWarn("[GakuMotion][Overrides] " + w);
		}

		// 曲目级（当前选中槽位对应位置文件）
		motionOverrides.Clear();
		const std::string motionPath = CurrentMotionOverridePath();
		if (!motionPath.empty() && std::filesystem::exists(MotionPathFromUtf8(motionPath)))
		{
			motionOverrides.LoadFromFile(motionPath);
			if (!motionOverrides.lastError.empty())
			{
				LogWarn("[GakuMotion][Overrides] per-motion override load failed: " + motionOverrides.lastError);
			}
			for (const auto& w : motionOverrides.warnings)
			{
				LogWarn("[GakuMotion][Overrides] " + w);
			}
		}

		const MotionEntry* m = CurrentMotion();
		for (auto& slot : formation.slots)
		{
			if (!slot.occupied || slot.actorToken == 0) continue;
			const std::string slotDir = m
				                            ? m->SlotDirectory(slot.sourceSlotIndex + 1)
				                            : (!player.currentMotionDir.empty()
					                               ? ResolveMotionSlotDirectory(player.currentMotionDir, slot.sourceSlotIndex + 1)
					                               : std::string());
			auto& cust = actorCustomizations[slot.actorToken];
			cust.actorMotionOverrides.Clear();
			if (!slotDir.empty())
			{
				const std::string slotPath = slotDir + "/bone_overrides.json";
				if (std::filesystem::exists(MotionPathFromUtf8(slotPath)))
				{
					cust.actorMotionOverrides.LoadFromFile(slotPath);
				}
			}
			cust.loadedSlotDir = slotDir;
			cust.overridesLoaded = true;
			slot.hasActorBoneOverrides = !cust.actorMotionOverrides.Empty();
		}

		RebuildEffectiveOverrides();
		if (verbose)
		{
			LogInfo(detail::FormatMsg(
				"[GakuMotion][Overrides] composed | global: %s | motion: %s | effective=%zu (master=%s)",
				globalOverrides.Describe().c_str(),
				motionPath.empty() ? "<none>" : motionOverrides.Describe().c_str(),
				effectiveOverrides.offsets.size(),
				masterOverridesEnabled ? "on" : "off"));
		}
	}

	inline void Core::RebuildEffectiveOverrides()
	{
		for (auto& slot : formation.slots)
		{
			if (!slot.occupied || slot.actorToken == 0) continue;
			RebuildSlotActorOverrides(slot);
		}
		BoneOverrideSet::BuildEffective(globalOverrides, motionOverrides,
		                                masterOverridesEnabled, effectiveOverrides);
		if (!formation.active)
		{
			player.controller.SetBoneOverrides(&effectiveOverrides);
		}
	}

	inline bool Core::ToggleOverrides()
	{
		masterOverridesEnabled = !masterOverridesEnabled;
		RebuildEffectiveOverrides();
		LogInfo(detail::FormatMsg("[GakuMotion][Overrides] master switch -> %s (effective entries=%zu)",
		                          masterOverridesEnabled ? "ON" : "OFF",
		                          effectiveOverrides.offsets.size()));
		return masterOverridesEnabled;
	}

	inline bool Core::PublishSceneActors(const std::vector<SceneActorObservation>& observations)
	{
		void* oldTarget = player.controller.targetActor;
		const auto* old = sceneActorRegistry.Find(oldTarget);
		const uint64_t oldToken = old ? old->token : 0;
		sceneActorRegistry.Publish(observations);
		sceneActors.clear();
		for (const auto& item : sceneActorRegistry.Entries()) sceneActors.push_back(item.actor);
		const auto* selectedAfterPublish = oldTarget ? sceneActorRegistry.Find(oldTarget) : nullptr;
		const bool targetLostInRegistry = oldTarget && (!selectedAfterPublish || selectedAfterPublish->token != oldToken);
		SyncFormationWithRoster();
		const bool lost = !formation.active && targetLostInRegistry;
		if (lost)
		{
			++injectionSession;
			camera.Release(false);
			player.AbandonScene();
			currentActor = nullptr;
			ResetSoleObservation();
			NotifyMotionPlaying(false);
		}
		if (!formation.active && !lost && !player.controller.targetActor && !sceneActors.empty())
		{
			void* bound = player.controller.currentBinding.actor;
			SetTargetActor(sceneActorRegistry.Find(bound) ? bound : sceneActors.front());
		}
		return lost;
	}

	inline void Core::NoteActor(void* a)
	{
		if (!a || sceneActorRegistry.Find(a)) return;
		std::vector<SceneActorObservation> observations;
		for (const auto& item : sceneActorRegistry.Entries()) observations.push_back(item);
		observations.push_back({a, static_cast<int>(sceneActorRegistry.Entries().size() + 1), {}});
		PublishSceneActors(observations);
	}

	inline void Core::SetTargetActor(void* a)
	{
		if (!a || !sceneActorRegistry.Find(a) || player.controller.targetActor == a) return;
		if (fnBeforeTargetChange && !fnBeforeTargetChange())
		{
			LogWarn("[GakuMotion][Target] selection rejected: secondary physics restore unconfirmed");
			return;
		}
		// Restore only a still-live old target, then release its cached body/face state.
		if (!player.controller.bindings.empty())
		{
			const auto restored = player.controller.RestoreForCapture();
			if (!restored.bodyConfirmed || !restored.faceConfirmed || !restored.gazeConfirmed)
			{
				LogWarn("[GakuMotion][Target] selection rejected: old actor restore unconfirmed");
				return;
			}
		}
		player.controller.DiscardBindings();
		camera.Release();
		++injectionSession;
		ResetSoleObservation();
		player.controller.SetTargetActor(a);
		LogDebug("[GakuMotion][Target] selected one current scene actor");
	}

	inline void Core::CycleTargetActor()
	{
		if (sceneActors.empty()) return;
		const auto found = std::find(sceneActors.begin(), sceneActors.end(), player.controller.targetActor);
		const size_t next = found == sceneActors.end() ? 0 : (std::distance(sceneActors.begin(), found) + 1) % sceneActors.size();
		SetTargetActor(sceneActors[next]);
	}

	inline void Core::PickBoundActorAsTarget()
	{
		void* bound = player.controller.currentBinding.actor;
		if (sceneActorRegistry.Find(bound)) SetTargetActor(bound);
		else if (!sceneActors.empty()) SetTargetActor(sceneActors.front());
	}

	inline int Core::CycleGazeMode()
	{
		const int current = static_cast<int>(lookAt.gazeMode.load());
		const int next = (current + 1) % 3;
		lookAt.gazeMode.store(static_cast<GazeMode>(next));
		LogInfo(detail::FormatMsg("[GakuMotion][Gaze] mode: %s -> %s (VRifyLiveGaze=%s)",
		                          GazeModeName(current), GazeModeName(next),
		                          lookAt.IsVRifyLiveGazeActive() ? "active" : "inactive"));
		return next;
	}

	inline const char* Core::GazeModeName(int mode)
	{
		switch (mode)
		{
		case 0: return "Auto(defer to VRify)";
		case 1: return "ForceOff(pure VMD head)";
		case 2: return "DesktopOn(desktop look-at)";
		default: return "Unknown";
		}
	}

	inline std::string Core::StatusSummary() const
	{
		const MotionEntry* m = CurrentMotion();
		// PROBE S1：把 scrub 累计计数与播放器侧时钟来源并入摘要。这样即使玩家
		// 没注意到拖动瞬间的日志，也能从 5 秒一次的状态行里看出
		// 「BEGIN/UPDATE/END 各发生了几次」以及「当前时钟由谁在推进」。
		return detail::FormatMsg(
			"motion=%s | state=%s | time=%.2f/%.2fs | bones=%zu/%zu | gaze=%s | overrides=%s(%zu) | seeks=%d corr=%.3fs | scrub=%d/%d/%d clk=%s | secondary=%s",
			m ? m->name.c_str() : "<none>",
			PlaybackStateName(player.state),
			player.currentTime,
			player.TimelineDuration(),
			currentActorBoundBones,
			currentActorTotalBones,
			GazeModeName(static_cast<int>(lookAt.gazeMode.load())),
			masterOverridesEnabled ? "ON" : "OFF",
			effectiveOverrides.offsets.size(),
			player.SeekCount(),
			player.AudioSyncOffsetSeconds(),
			gui.probeScrubBegin, gui.probeScrubUpdate, gui.probeScrubEnd,
			player.lastClockSource ? player.lastClockSource : "none",
			config.secondaryPhysicsBackend == 2
				? "native(optional; fallback on failure)"
				: config.secondaryPhysicsBackend == 1
				? "fallback"
				: "off");
	}

	inline const char* Core::PlaybackStateName(PlaybackState s)
	{
		switch (s)
		{
		case PlaybackState::Stopped: return "Stopped";
		case PlaybackState::Playing: return "Playing";
		case PlaybackState::Paused: return "Paused";
		case PlaybackState::Preview: return "Preview(f0)";
		}
		return "?";
	}

	inline void Core::SetLiveBoneOverride(int hid, const char* name, float pitch, float yaw, float roll, bool world, bool enabled, bool symmetrical)
	{
		std::string bname = name ? name : "";
		if (bname.empty() && hid >= 0)
		{
			bname = BoneOverrideSet::GetBoneNameByHumanoidId(hid);
		}
		Vector3 euler(pitch, yaw, roll);

		auto applyOne = [&](int h, const std::string& n, const Vector3& e)
		{
			auto& eff = effectiveOverrides.GetOrAddOffset(h, n);
			eff.eulerDeg = e;
			eff.spaceWorld = world;
			eff.enabled = enabled;
			eff.useQuaternion = false;

			auto& mot = motionOverrides.GetOrAddOffset(h, n);
			mot.eulerDeg = e;
			mot.spaceWorld = world;
			mot.enabled = enabled;
			mot.useQuaternion = false;
		};

		applyOne(hid, bname, euler);

		if (symmetrical)
		{
			int symmHid = BoneOverrideSet::GetSymmetricalHumanoidId(hid);
			if (symmHid >= 0)
			{
				std::string symmName = BoneOverrideSet::GetBoneNameByHumanoidId(symmHid);
				Vector3 symmEuler(euler.x, -euler.y, -euler.z); // MMD mirror formula
				applyOne(symmHid, symmName, symmEuler);
			}
		}

		effectiveOverrides.enabled = true;
		effectiveOverrides.RebuildIndex();
		motionOverrides.enabled = true;
		motionOverrides.RebuildIndex();
		masterOverridesEnabled = true;

		if (auto* sel = formation.SelectedSlot(); sel && sel->occupied && sel->actorToken != 0)
		{
			auto& cust = actorCustomizations[sel->actorToken];
			cust.actorMotionOverrides = motionOverrides;
			cust.actorMotionOverrides.enabled = true;
			cust.actorMotionOverrides.RebuildIndex();
			cust.overridesLoaded = true;
			RebuildSlotActorOverrides(*sel);
		}
	}

	inline void Core::ResetLiveBoneOverride(int hid, const char* name, bool symmetrical)
	{
		std::string bname = name ? name : "";
		effectiveOverrides.RemoveOffset(hid, bname);
		motionOverrides.RemoveOffset(hid, bname);

		if (symmetrical)
		{
			int symmHid = BoneOverrideSet::GetSymmetricalHumanoidId(hid);
			if (symmHid >= 0)
			{
				std::string symmName = BoneOverrideSet::GetBoneNameByHumanoidId(symmHid);
				effectiveOverrides.RemoveOffset(symmHid, symmName);
				motionOverrides.RemoveOffset(symmHid, symmName);
			}
		}
		effectiveOverrides.RebuildIndex();
		motionOverrides.RebuildIndex();

		if (auto* sel = formation.SelectedSlot(); sel && sel->occupied && sel->actorToken != 0)
		{
			auto& cust = actorCustomizations[sel->actorToken];
			cust.actorMotionOverrides = motionOverrides;
			cust.actorMotionOverrides.RebuildIndex();
			cust.overridesLoaded = true;
			RebuildSlotActorOverrides(*sel);
		}
	}

	inline void Core::ResetAllLiveOverrides()
	{
		effectiveOverrides.offsets.clear();
		motionOverrides.offsets.clear();
		effectiveOverrides.RebuildIndex();
		motionOverrides.RebuildIndex();

		if (auto* sel = formation.SelectedSlot(); sel && sel->occupied && sel->actorToken != 0)
		{
			auto& cust = actorCustomizations[sel->actorToken];
			cust.actorMotionOverrides.offsets.clear();
			cust.actorMotionOverrides.RebuildIndex();
			cust.overridesLoaded = true;
			RebuildSlotActorOverrides(*sel);
		}
	}

	inline bool Core::SaveLiveOverridesToMotion()
	{
		std::string targetFile = CurrentMotionOverridePath();
		if (targetFile.empty())
		{
			const MotionEntry* m = CurrentMotion();
			int slot1Based = 1;
			if (const auto* sel = formation.SelectedSlot()) slot1Based = sel->sourceSlotIndex + 1;
			if (m) targetFile = m->SlotDirectory(slot1Based) + "/bone_overrides.json";
			else if (!player.currentMotionDir.empty())
				targetFile = ResolveMotionSlotDirectory(player.currentMotionDir, slot1Based) + "/bone_overrides.json";
		}
		if (targetFile.empty()) return false;
		std::error_code ec;
		const auto parentDir = MotionPathFromUtf8(targetFile).parent_path();
		if (!parentDir.empty()) std::filesystem::create_directories(parentDir, ec);
		bool ok = effectiveOverrides.SaveToFile(targetFile);
		if (ok)
		{
			LogInfo("[GakuMotion] Saved bone overrides to: " + targetFile);
		}
		else
		{
			LogWarn("[GakuMotion] Failed to save bone overrides to: " + targetFile);
		}
		return ok;
	}

	inline bool Core::SaveLiveOverridesToGlobal()
	{
		if (globalOverridePath.empty()) return false;
		bool ok = effectiveOverrides.SaveToFile(globalOverridePath);
		if (ok)
		{
			LogInfo("[GakuMotion] Saved global bone overrides to: " + globalOverridePath);
		}
		else
		{
			LogWarn("[GakuMotion] Failed to save global bone overrides to: " + globalOverridePath);
		}
		return ok;
	}

	inline bool Core::GetLiveBoneOverride(int hid, const char* name, float* outPitch, float* outYaw, float* outRoll, bool* outWorld, bool* outEnabled) const
	{
		std::string bname = name ? name : "";
		const auto* o = effectiveOverrides.Find(hid, bname);
		if (o)
		{
			if (outPitch) *outPitch = o->eulerDeg.x;
			if (outYaw) *outYaw = o->eulerDeg.y;
			if (outRoll) *outRoll = o->eulerDeg.z;
			if (outWorld) *outWorld = o->spaceWorld;
			if (outEnabled) *outEnabled = o->enabled;
			return true;
		}
		if (outPitch) *outPitch = 0.0f;
		if (outYaw) *outYaw = 0.0f;
		if (outRoll) *outRoll = 0.0f;
		if (outWorld) *outWorld = false;
		if (outEnabled) *outEnabled = true;
		return false;
	}
} // namespace GakuMotion
