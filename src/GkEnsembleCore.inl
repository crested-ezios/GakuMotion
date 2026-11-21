// Included inside Core. One master MotionPlayer owns transport/audio. Other
// tracks use only SampleAtPerformanceTime; they have no audio callbacks.
		bool EnsembleActive() const { return ensembleSnapshot.active; }
		MotionPlayer* EnsemblePlayer(size_t slot)
		{
			if (slot >= ensembleActiveSettings.slots.size() || !ensembleActiveSettings.slots[slot].enabled) return nullptr;
			return slot == ensembleActiveSettings.masterSlot ? &player : ensembleTracks[slot].get();
		}
		MotionPlayer* PlayerForActor(void* actor)
		{
			if (!actor) return nullptr;
			if (!EnsembleActive()) return IsTargetActor(actor) ? &player : nullptr;
			for (size_t slot = 0; slot < ensembleActiveSettings.slots.size(); ++slot)
				if (auto* track = EnsemblePlayer(slot); track && track->controller.IsTargetActor(actor)) return track;
			return nullptr;
		}
		bool IsPerformanceActor(void* actor) { return PlayerForActor(actor) != nullptr; }
		void* EnsembleCameraActor()
		{
			auto* track = EnsemblePlayer(ensembleActiveSettings.cameraSlot);
			return track ? track->controller.targetActor : player.controller.targetActor;
		}
		void QueueEnsemble(const ensemble::Settings& settings, uint64_t revision, uint64_t epoch)
		{
			GuiCommand cmd{GuiAction::EnsembleSettings}; cmd.ensembleSettings = settings;
			cmd.loadedGeneration = revision; cmd.actorEpoch = epoch; cmd.sceneSession = injectionSession;
			QueueGui(std::move(cmd));
		}
		void QueueEnsemblePreset(bool save, const char* name, const ensemble::Settings& draft)
		{
			GuiCommand cmd{GuiAction::EnsemblePreset}; cmd.flag = save; cmd.key = name ? name : ""; cmd.ensembleSettings = draft;
			QueueGui(std::move(cmd));
		}
		void RestoreEnsembleTracks()
		{
			for (auto& track : ensembleTracks) if (track) track->Stop();
		}
		void RetireEnsembleTracks()
		{
			// Call only after restoring valid identities or abandoning invalid
			// ones. No inactive secondary binding may outlive roster monitoring.
			for (auto& track : ensembleTracks) if (track) { track->AbandonScene(); track.reset(); }
		}
		bool EnsembleActorsValid() const
		{
			if (!EnsembleActive()) return true;
			for (size_t i = 0; i < ensembleActiveSettings.slots.size(); ++i) if (ensembleActiveSettings.slots[i].enabled)
			{
				const auto* track = i == ensembleActiveSettings.masterSlot ? &player : ensembleTracks[i].get();
				const auto* found = track ? sceneActorRegistry.Find(track->controller.targetActor) : nullptr;
				if (!found || found->token != ensembleActiveSettings.slots[i].actorToken || found->instanceId != ensembleInstanceIds[i]) return false;
				const auto binding = track->controller.bindings.find(found->actor);
				if (binding != track->controller.bindings.end() && binding->second.bindPoseCaptured && track->controller.fnGetBoneTransform &&
					binding->second.rootTransform != track->controller.fnGetBoneTransform(found->actor, 0)) return false;
			}
			return true;
		}
		void LoseEnsembleActors()
		{
			// Restore surviving identities only. Never invoke Unity on the missing
			// actor or on a new object reusing its address.
			for (size_t i = 0; i < ensembleActiveSettings.slots.size(); ++i) if (auto* track = EnsemblePlayer(i))
			{
				const auto* found = sceneActorRegistry.Find(track->controller.targetActor);
				const auto binding = track->controller.bindings.find(track->controller.targetActor);
				const bool identityMatches = found && found->token == ensembleActiveSettings.slots[i].actorToken && found->instanceId == ensembleInstanceIds[i];
				const bool rigChanged = identityMatches && binding != track->controller.bindings.end() && binding->second.bindPoseCaptured &&
					track->controller.fnGetBoneTransform && binding->second.rootTransform != track->controller.fnGetBoneTransform(found->actor, 0);
				if (!found || found->token != ensembleActiveSettings.slots[i].actorToken || found->instanceId != ensembleInstanceIds[i] || rigChanged)
					track->AbandonScene();
				else track->Stop();
			}
			RetireEnsembleTracks();
			camera.Release(false); ++injectionSession;
			ensembleSnapshot.active = false; ensembleSnapshot.verified = false;
			ensembleSnapshot.settings.enabled = false; ++ensembleSnapshot.revision;
			player.additionalPerformanceDuration = 0; ensembleLastFrame = 0;
			player.controller.SetPlacementYawDegrees(0);
			player.controller.SetInitialPositionOffset({}); LoadInitialPosition(); LoadOverrides(false);
			ensembleSnapshot.notice = "演员或摄影布局已变化，整场已停止；请观察并重新绑定";
			NotifyMotionPlaying(false);
		}
		bool ApplyEnsemble(const ensemble::Settings& settings, uint64_t revision, uint64_t epoch)
		{
			auto fail = [&](const std::string& why) { ensembleSnapshot.notice = why; return false; };
			player.Pause(); // Editing never silently resumes audio.
			if (revision != ensembleSnapshot.revision || epoch != sceneActorRegistry.Epoch()) return fail("名单或组合已变化，请重新应用");
			if (!settings.enabled)
			{
				if (fnBeforeTargetChange && !fnBeforeTargetChange()) return fail("物理恢复未确认，未关闭多人舞台");
				StopPerformance(); RetireEnsembleTracks();
				ensembleSnapshot.active = false; ensembleSnapshot.settings = settings;
				player.controller.SetPlacementYawDegrees(0);
				player.controller.SetInitialPositionOffset({}); LoadInitialPosition();
				LoadOverrides(false);
				player.additionalPerformanceDuration = 0; ++ensembleSnapshot.revision;
				return true;
			}
			std::string error;
			if (!ensemble::Validate(settings, error)) return fail(error);
			if (!settings.slots[settings.masterSlot].enabled || !settings.slots[settings.cameraSlot].enabled) return fail("主槽和跟随槽必须参与演出");
			if (!ensembleSnapshot.available || !ensembleSnapshot.verified || !InjectionAllowed() || captureSuspended)
				return fail("多人观察尚未通过：请在摄影场景开启观察探针并确认全部演员");
			if (!PrepareInjection()) return fail("摄影保护检查未就绪，保留原组合");
			std::vector<MotionClip> clips(settings.slots.size());
			std::vector<MotionEntry> entries(settings.slots.size());
			std::vector<MotionFileVersion> versions(settings.slots.size());
			std::vector<void*> actors(settings.slots.size());
			for (size_t i = 0; i < settings.slots.size(); ++i) if (settings.slots[i].enabled)
			{
				actors[i] = sceneActorRegistry.Resolve(settings.slots[i].actorToken, epoch);
				if (!actors[i] || !player.controller.CanDriveActor(actors[i])) return fail("槽位演员未绑定或已失效");
				if (enforceInjectionPolicy && (!fnEnsembleActorEligible || !fnEnsembleActorEligible(actors[i])))
					return fail("演员不属于已验证的官方摄影槽位");
				const int index = FindPlaylistIndex(motions, settings.slots[i].motionKey);
				if (index < 0) return fail("曲库动作不存在：" + settings.slots[i].motionKey);
				entries[i] = motions[index]; versions[i] = ReadMotionFileVersion(entries[i].dir);
				const auto file = FindMotionFile(entries[i].dir);
				if (file.empty() || !clips[i].LoadFromFile(file) || !clips[i].total_frames) return fail("动作预检失败：" + settings.slots[i].motionKey);
			}
			// Preflight camera as well, before releasing any live actor.
			CameraTrack cameraCandidate;
			if (!CameraController::ReadTrack(entries[settings.masterSlot].dir, clips[settings.masterSlot], cameraCandidate)) return fail(cameraCandidate.error);
			for (size_t i = 0; i < settings.slots.size(); ++i) if (settings.slots[i].enabled && versions[i] != ReadMotionFileVersion(entries[i].dir))
				return fail("素材在预检期间发生变化，请重试");
			if (fnBeforeTargetChange && !fnBeforeTargetChange()) return fail("旧物理恢复未确认，保留原组合");
			for (size_t i = 0; i < (EnsembleActive() ? ensembleActiveSettings.slots.size() : 1); ++i)
			{
				auto* track = EnsembleActive() ? EnsemblePlayer(i) : i == 0 ? &player : nullptr;
				if (!track) continue;
				const auto restored = track->controller.RestoreForCapture();
				if (!restored.bodyConfirmed || !restored.faceConfirmed || !restored.gazeConfirmed) return fail("旧角色恢复未确认，保留原组合");
			}
			StopPerformance(); RestoreEnsembleTracks();
			player.controller.DiscardBindings();
			for (auto& track : ensembleTracks) track.reset();
			ensembleTracks.resize(settings.slots.size());
			ensembleOverrides.resize(settings.slots.size());
			ensembleInstanceIds.resize(settings.slots.size());
			ensembleSnapshot.physics.assign(settings.slots.size(), {});
			ensembleActiveSettings = settings;
			float duration = 0;
			for (size_t i = 0; i < settings.slots.size(); ++i) if (settings.slots[i].enabled)
			{
				MotionPlayer* track = &player;
				if (i != settings.masterSlot)
				{
					ensembleTracks[i] = std::make_unique<MotionPlayer>(); track = ensembleTracks[i].get();
					track->controller.CopyRuntimeConfigurationFrom(player.controller);
				}
				track->additionalPerformanceDuration = 0;
				track->AdoptLoadedMotion(entries[i].dir, FindMotionFile(entries[i].dir), std::move(clips[i]));
				track->timing = entries[i].timing; track->controller.SetTargetActor(actors[i]);
				track->controller.SetInitialPositionOffset(settings.slots[i].offset);
				track->controller.SetPlacementYawDegrees(settings.slots[i].yawDegrees);
				track->controller.LoadAuxGroundTruth(entries[i].dir + "/aux_replay.json");
				BoneOverrideSet local; local.LoadFromFile(entries[i].dir + "/bone_overrides.json");
				BoneOverrideSet::BuildEffective(globalOverrides, local, masterOverridesEnabled, ensembleOverrides[i]);
				track->controller.SetBoneOverrides(&ensembleOverrides[i]);
				if (i != settings.masterSlot) { track->audioSourceDuration = 0; track->cameraSourceDuration = 0; }
				duration = (std::max)(duration, track->TimelineDuration());
				ensembleInstanceIds[i] = sceneActorRegistry.Find(actors[i])->instanceId;
			}
			player.additionalPerformanceDuration = duration;
			camera.InstallTrack(entries[settings.masterSlot].dir, std::move(cameraCandidate));
			player.cameraSourceDuration = camera.track.frames.empty() ? 0 : static_cast<float>(camera.track.frames.size()) / camera.track.fps;
			currentMotionIndex = FindPlaylistIndex(motions, entries[settings.masterSlot].name);
			loadedMotionEntry = entries[settings.masterSlot]; loadedMotionKey = loadedMotionEntry.name;
			loadedMotionVersion = versions[settings.masterSlot]; loadedSourceMissing = loadedSourceChanged = false;
			++loadedMotionGeneration; ++injectionSession;
			ensembleSnapshot.settings = settings; ensembleSnapshot.active = true; ++ensembleSnapshot.revision;
			ensembleLastFrame = 0; stageStoppedByTransport = false;
			ensembleSnapshot.notice = "多人组合已应用：预览；每位实际物理后端见下方";
			return true;
		}
		void EnsemblePreset(bool save, const std::string& name, const ensemble::Settings& draftToSave)
		{
			if (name.empty() || name.size() > 80 || name == "." || name == ".." || name.find_first_of("/\\:*?\"<>|") != std::string::npos)
			{ ensembleSnapshot.notice = "预设名称无效"; return; }
			const auto path = configRoot + "/ensembles/" + name + ".json";
			std::string error;
			if (save)
			{
				if (!ensemble::Save(path, draftToSave, error)) { ensembleSnapshot.notice = error; return; }
				ensembleSnapshot.notice = "组合预设已保存";
			}
			else
			{
				ensemble::Settings draft;
				if (!ensemble::Load(path, draft, error)) { ensembleSnapshot.notice = error; return; }
				player.Pause(); ensembleSnapshot.settings = std::move(draft); ++ensembleSnapshot.revision;
				ensembleSnapshot.notice = "已载入草稿；请逐槽绑定当前演员后应用，原组合仍保留";
			}
		}
