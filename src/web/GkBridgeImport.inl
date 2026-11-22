namespace GakuMotion::webpanel
{
	inline std::string Bridge::ImportSlotString(const jsonlite::Value& draft, const char* fileField, const char* modeField, const char* pathField)
	{
		const jsonlite::Value* mode = draft.Find(modeField);
		if (mode && mode->IsString())
		{
			const std::string& token = mode->stringValue;
			if (token == "keep" || token == "keepTrack") return "[保持当前已导入轨道]";
			if (token == "keepMotion") return "[保持当前动作]";
			if (token == "clear") return "（留空 / 清除）";
			if (token == "skip") return "（不导入）";
			if (token == "auto") return "";
			const jsonlite::Value* path = draft.Find(pathField);
			if (path && path->IsString()) return path->stringValue;
			return "";
		}
		const jsonlite::Value* file = draft.Find(fileField);
		return file && file->IsString() ? file->stringValue : "";
	}

	inline bool Bridge::IsImportSlotFile(const std::string& value)
	{
		return !value.empty() && value != "（留空）" && value != "（留空 / 清除）" &&
			value != "[保持当前已导入轨道]" && value != "[保持当前动作]" &&
			value != "（自动：沿用动作 VMD 内表情）" &&
			value != "（自动：同名 WAV 或 song.wav）" &&
			value != "（自动：同名音频或 song.*）" &&
			value != "（不导入）";
	}

	inline CmdResult Bridge::ValidateImportTrackFile(const std::string& path, const char* label, int kind)
	{
		if (path.size() > 32767) return CmdResult::Err("invalid_params", std::string(label) + "路径过长");
		if (kind == 3)
		{
			if (IsNetworkAudioPath(path))
				return CmdResult::Err("invalid_params", "不支持网络或流媒体路径，请选择本地音频文件");
			if (!IsSupportedAudioExtensionPath(path))
				return CmdResult::Err("invalid_params", "音频轨道仅支持 .wav / .mp3 / .flac / .ogg / .m4a / .aac / .opus 本地文件");
			return CmdResult::Ok();
		}
		const VmdProbeInfo info = ProbeCameraOrVmdFile(path);
		if (kind == 0 && !info.CanUseAsMotion())
			return CmdResult::Err("invalid_params", "所选动作文件不包含身体骨骼动作关键帧");
		if (kind == 1 && !info.CanUseAsFacial())
			return CmdResult::Err("invalid_params", "所选表情文件不包含表情或眼神关键帧");
		if (kind == 2 && !info.CanUseAsCamera())
			return CmdResult::Err("invalid_params", "所选镜头文件不包含镜头关键帧");
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::LibraryImport(const jsonlite::Value& params)
	{
		const jsonlite::Value* requestObj = Param(params, "request");
		const jsonlite::Value* entries = Param(params, "entries");
		const bool usingRequestSchema = requestObj != nullptr;
		const jsonlite::Value* draft = requestObj;
		if (!usingRequestSchema)
		{
			if (!entries || !entries->IsArray() || entries->Size() != 1)
				return CmdResult::Err("invalid_params", "request object or single entry is required");
			draft = entries->At(0);
		}
		if (!draft || !draft->IsObject())
			return CmdResult::Err("invalid_params", "import request must be an object");

		std::string title, editKey;
		StringParam(*draft, "title", title);
		StringParam(*draft, "editKey", editKey);
		bool hasTitle = false;
		for (unsigned char ch : title) if (!std::isspace(ch)) { hasTitle = true; break; }
		if (!hasTitle) return CmdResult::Err("invalid_params", "请输入有效的演出曲目名称");

		float resolvedFps = 0.0f;
		if (const jsonlite::Value* fpsVal = draft->Find("fps"))
		{
			double fpsNum = 0.0;
			if (!fpsVal->IsNumber() || !std::isfinite(fpsVal->numberValue) || fpsVal->numberValue < 0.0)
				return CmdResult::Err("invalid_params", "fps must be a non-negative finite number");
			fpsNum = fpsVal->numberValue;
			resolvedFps = static_cast<float>(fpsNum);
		}
		else
		{
			int64_t fpsIndex = 0;
			if (!IntParam(*draft, "fpsIndex", fpsIndex) || fpsIndex < 0 || fpsIndex > 2)
				return CmdResult::Err("invalid_params", "fpsIndex must be 0 (auto), 1 (30) or 2 (60)");
			resolvedFps = fpsIndex == 1 ? 30.0f : fpsIndex == 2 ? 60.0f : 0.0f;
		}

		float resolvedSourceFps = 0.0f;
		if (const jsonlite::Value* sourceFpsVal = draft->Find("sourceFps"))
		{
			if (!sourceFpsVal->IsNumber() || !std::isfinite(sourceFpsVal->numberValue) || sourceFpsVal->numberValue < 0.0)
				return CmdResult::Err("invalid_params", "sourceFps must be a non-negative finite number");
			resolvedSourceFps = static_cast<float>(sourceFpsVal->numberValue);
		}
		else if (draft->Find("sourceFpsIndex"))
		{
			int64_t sourceFpsIndex = 0;
			if (!IntParam(*draft, "sourceFpsIndex", sourceFpsIndex) || sourceFpsIndex < 0 || sourceFpsIndex > 2)
				return CmdResult::Err("invalid_params", "sourceFpsIndex must be 0 (auto), 1 (30) or 2 (60)");
			resolvedSourceFps = sourceFpsIndex == 1 ? 30.0f : sourceFpsIndex == 2 ? 60.0f : 0.0f;
		}
		if (resolvedSourceFps != 0.0f && resolvedSourceFps != 25.0f && resolvedSourceFps != 30.0f && resolvedSourceFps != 60.0f)
			return CmdResult::Err("invalid_params", "源动作帧率只支持自动、25、30 或 60 fps");

		PerformanceTiming timing;
		std::string error;
		const jsonlite::Value* timingValue = draft->Find("timing");
		if (timingValue && !ParseTiming(*timingValue, timing, error))
			return CmdResult::Err("invalid_params", error);
		if (!ValidPerformanceTiming(timing))
			return CmdResult::Err("invalid_params", "轨道速度必须 >= 0，偏移必须为有限数值");

		const bool edit = !editKey.empty();
		bool rebakeOutput = edit && resolvedFps > 0.0f;
		if (draft->Find("rebakeOutput") && !BoolParam(*draft, "rebakeOutput", rebakeOutput))
			return CmdResult::Err("invalid_params", "rebakeOutput must be a boolean");
		std::string motion = ImportSlotString(*draft, "motionFile", "motionMode", "motionPath");
		if (motion.empty()) StringParam(*draft, "motionVmd", motion);
		const std::string facial = ImportSlotString(*draft, "facialFile", "facialMode", "facialPath");
		const std::string camera = ImportSlotString(*draft, "cameraFile", "cameraMode", "cameraPath");
		const std::string audio = ImportSlotString(*draft, "audioFile", "audioMode", "audioPath");

		MotionImportRequest request;
		request.title = title;
		std::string subtitle;
		if (draft->Find("subtitle"))
		{
			if (!StringParam(*draft, "subtitle", subtitle))
				return CmdResult::Err("invalid_params", "subtitle must be a string");
			request.hasSubtitle = true;
			request.subtitle = subtitle;
		}
		request.fps = resolvedFps;
		request.sourceFps = resolvedSourceFps;
		request.rebakeOutput = rebakeOutput;
		request.timing = timing;
		request.editKey = editKey;

		if (const jsonlite::Value* positionsVal = draft->Find("positions"))
		{
			if (!positionsVal->IsArray() || positionsVal->Size() == 0 || positionsVal->Size() > 64)
				return CmdResult::Err("invalid_params", "positions must be a non-empty array (max 64)");
			request.positionsExplicit = true;
			for (size_t i = 0; i < positionsVal->Size(); ++i)
			{
				const jsonlite::Value* posObj = positionsVal->At(i);
				if (!posObj || !posObj->IsObject())
					return CmdResult::Err("invalid_params", "each position entry must be an object");
				int64_t slotNum = static_cast<int64_t>(i + 1);
				if (posObj->Find("slot") && (!IntParam(*posObj, "slot", slotNum) || slotNum < 1 || slotNum > 64))
					return CmdResult::Err("invalid_params", "position.slot must be within 1..64");
				int64_t sourceSlotNum = 0;
				if (posObj->Find("sourceSlot") && (!IntParam(*posObj, "sourceSlot", sourceSlotNum) || sourceSlotNum < 0 || sourceSlotNum > 64))
					return CmdResult::Err("invalid_params", "position.sourceSlot must be >= 0");
				double px = 0.0, py = 0.0, pz = 0.0, pyaw = 0.0, pSourceFps = 0.0;
				if ((posObj->Find("x") && (!NumberParam(*posObj, "x", px) || px < -100.0 || px > 100.0)) ||
					(posObj->Find("y") && (!NumberParam(*posObj, "y", py) || py < -100.0 || py > 100.0)) ||
					(posObj->Find("z") && (!NumberParam(*posObj, "z", pz) || pz < -100.0 || pz > 100.0)) ||
					(posObj->Find("yawDegrees") && (!NumberParam(*posObj, "yawDegrees", pyaw) || pyaw < -360.0 || pyaw > 360.0)))
					return CmdResult::Err("invalid_params", "position placement coordinates/yaw are out of range");
				if (posObj->Find("sourceFps") && !NumberParam(*posObj, "sourceFps", pSourceFps))
					return CmdResult::Err("invalid_params", "position.sourceFps must be finite");
				if (pSourceFps != 0.0 && pSourceFps != 25.0 && pSourceFps != 30.0 && pSourceFps != 60.0)
					return CmdResult::Err("invalid_params", "position.sourceFps must be 0, 25, 30, or 60");
				bool pDisableMorph = false, pClearMorph = false, pRebake = false, pPreserve = false;
				if ((posObj->Find("disableMorph") && !BoolParam(*posObj, "disableMorph", pDisableMorph)) ||
					(posObj->Find("clearMorph") && !BoolParam(*posObj, "clearMorph", pClearMorph)) ||
					(posObj->Find("rebakeOutput") && !BoolParam(*posObj, "rebakeOutput", pRebake)) ||
					(posObj->Find("preserveExisting") && !BoolParam(*posObj, "preserveExisting", pPreserve)))
					return CmdResult::Err("invalid_params", "position boolean flags must be booleans");

				std::string pMotion = ImportSlotString(*posObj, "motionFile", "motionMode", "motionPath");
				if (pMotion.empty()) StringParam(*posObj, "motionVmd", pMotion);
				if (pMotion == "[保持当前动作]") pPreserve = true;
				std::string pFacial = ImportSlotString(*posObj, "facialFile", "facialMode", "facialPath");
				if (pFacial.empty()) StringParam(*posObj, "morphVmd", pFacial);
				if (pFacial == "（不导入）" || pFacial == "（留空 / 清除）") pDisableMorph = true;

				ImportPositionSlot ps;
				ps.slot = static_cast<int>(slotNum);
				ps.sourceSlot = static_cast<int>(sourceSlotNum);
				ps.disableMorph = pDisableMorph;
				ps.clearMorph = pClearMorph;
				ps.sourceFps = static_cast<float>(pSourceFps);
				ps.rebakeOutput = pRebake;
				ps.preserveExisting = pPreserve;
				ps.x = static_cast<float>(px);
				ps.y = static_cast<float>(py);
				ps.z = static_cast<float>(pz);
				ps.yawDegrees = static_cast<float>(pyaw);

				if (!ps.preserveExisting && IsImportSlotFile(pMotion))
				{
					if (auto check = ValidateImportTrackFile(pMotion, "动作", 0); !check.ok) return check;
					ps.motionVmd = pMotion;
				}
				else if (!edit && !ps.preserveExisting)
				{
					return CmdResult::Err("invalid_params", "请为每个位置选择有效的身体动作 (.vmd) 文件");
				}

				if (!ps.disableMorph && !ps.clearMorph && IsImportSlotFile(pFacial))
				{
					if (auto check = ValidateImportTrackFile(pFacial, "表情", 1); !check.ok) return check;
					ps.morphVmd = pFacial;
				}
				request.positions.push_back(std::move(ps));
			}
		}

		if (usingRequestSchema)
		{
			bool disableMorph = false, clearCamera = false, clearAudio = false;
			if (draft->Find("disableMorph") && !BoolParam(*draft, "disableMorph", disableMorph))
				return CmdResult::Err("invalid_params", "disableMorph must be a boolean");
			if (draft->Find("clearCamera") && !BoolParam(*draft, "clearCamera", clearCamera))
				return CmdResult::Err("invalid_params", "clearCamera must be a boolean");
			if (draft->Find("clearAudio") && !BoolParam(*draft, "clearAudio", clearAudio))
				return CmdResult::Err("invalid_params", "clearAudio must be a boolean");

			if (edit)
			{
				if (IsImportSlotFile(motion))
				{
					if (auto check = ValidateImportTrackFile(motion, "动作", 0); !check.ok) return check;
					request.vmd = motion;
				}
				request.disableMorph = disableMorph;
				if (!disableMorph && IsImportSlotFile(facial))
				{
					if (auto check = ValidateImportTrackFile(facial, "表情", 1); !check.ok) return check;
					request.morphVmd = facial;
				}
				request.clearCamera = clearCamera;
				if (!clearCamera && IsImportSlotFile(camera))
				{
					if (auto check = ValidateImportTrackFile(camera, "镜头", 2); !check.ok) return check;
					request.cameraVmd = camera;
				}
				request.clearAudio = clearAudio;
				if (!clearAudio && IsImportSlotFile(audio))
				{
					if (auto check = ValidateImportTrackFile(audio, "音频", 3); !check.ok) return check;
					request.audio = audio;
				}
			}
			else
			{
				if (IsImportSlotFile(motion))
				{
					if (auto check = ValidateImportTrackFile(motion, "动作", 0); !check.ok) return check;
					request.vmd = motion;
				}
				else if (!request.positions.empty() && !request.positions.front().motionVmd.empty())
				{
					request.vmd = request.positions.front().motionVmd;
				}
				else
				{
					return CmdResult::Err("invalid_params", "请选择有效的身体动作 (.vmd) 文件");
				}
				request.disableMorph = disableMorph || facial == "（不导入）" || facial == "（留空 / 清除）";
				if (!request.disableMorph && IsImportSlotFile(facial))
				{
					if (auto check = ValidateImportTrackFile(facial, "表情", 1); !check.ok) return check;
					request.morphVmd = facial;
				}
				else if (!request.disableMorph && !request.positions.empty() && !request.positions.front().morphVmd.empty())
				{
					request.morphVmd = request.positions.front().morphVmd;
				}
				if (IsImportSlotFile(camera))
				{
					if (auto check = ValidateImportTrackFile(camera, "镜头", 2); !check.ok) return check;
					request.cameraVmd = camera;
				}
				request.clearAudio = clearAudio || audio == "（不导入）" || audio == "（留空 / 清除）";
				if (!request.clearAudio && IsImportSlotFile(audio))
				{
					if (auto check = ValidateImportTrackFile(audio, "音频", 3); !check.ok) return check;
					request.audio = audio;
				}
			}
		}
		else
		{
			if (edit)
			{
				if (IsImportSlotFile(motion)) { if (auto check = ValidateImportTrackFile(motion, "动作", 0); !check.ok) return check; request.vmd = motion; }
				if (facial == "[保持当前已导入轨道]") {}
				else if (IsImportSlotFile(facial)) { if (auto check = ValidateImportTrackFile(facial, "表情", 1); !check.ok) return check; request.morphVmd = facial; }
				else request.disableMorph = true;
				if (camera == "[保持当前已导入轨道]") {}
				else if (IsImportSlotFile(camera)) { if (auto check = ValidateImportTrackFile(camera, "镜头", 2); !check.ok) return check; request.cameraVmd = camera; }
				else request.clearCamera = true;
				if (audio == "[保持当前已导入轨道]") {}
				else if (IsImportSlotFile(audio)) { if (auto check = ValidateImportTrackFile(audio, "音频", 3); !check.ok) return check; request.audio = audio; }
				else request.clearAudio = true;
			}
			else
			{
				if (!IsImportSlotFile(motion)) return CmdResult::Err("invalid_params", "请选择有效的身体动作 (.vmd) 文件");
				if (auto check = ValidateImportTrackFile(motion, "动作", 0); !check.ok) return check;
				request.vmd = motion;
				if (facial == "（不导入）" || facial == "（留空 / 清除）") request.disableMorph = true;
				else if (IsImportSlotFile(facial)) { if (auto check = ValidateImportTrackFile(facial, "表情", 1); !check.ok) return check; request.morphVmd = facial; }
				if (IsImportSlotFile(camera)) { if (auto check = ValidateImportTrackFile(camera, "镜头", 2); !check.ok) return check; request.cameraVmd = camera; }
				if (audio == "（不导入）" || audio == "（留空 / 清除）") request.clearAudio = true;
				else if (IsImportSlotFile(audio)) { if (auto check = ValidateImportTrackFile(audio, "音频", 3); !check.ok) return check; request.audio = audio; }
			}
		}

		bool clearCover = false;
		if (draft->Find("clearCover") && !BoolParam(*draft, "clearCover", clearCover))
			return CmdResult::Err("invalid_params", "clearCover must be a boolean");
		std::string coverDataUri;
		bool hasCoverDataUri = false;
		if (draft->Find("coverDataUri"))
		{
			if (!StringParam(*draft, "coverDataUri", coverDataUri))
				return CmdResult::Err("invalid_params", "coverDataUri must be a string");
			if (!coverDataUri.empty())
			{
				if (coverDataUri.size() > 16ULL * 1024ULL * 1024ULL ||
					coverDataUri.rfind("data:image/", 0) != 0)
					return CmdResult::Err("invalid_params", "coverDataUri must be a valid image data URI");
				hasCoverDataUri = true;
			}
		}

		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (core_.importBusy || core_.cameraImportBusy || core_.libraryScan.valid())
			return CmdResult::Err("busy", "正在导入，请等待完成后保存");
		if (edit)
		{
			int64_t revision = -1;
			const bool hasRevision = IntParam(*draft, "revision", revision) || IntParam(params, "revision", revision);
			if (!usingRequestSchema && !hasRevision)
				return CmdResult::Err("invalid_params", "编辑已有曲目必须携带 revision");
			if (hasRevision && static_cast<uint64_t>(revision) != core_.libraryRevision)
				return CmdResult::Err("stale_revision", "曲库已变化，请刷新后重试");
			const int index = FindPlaylistIndex(core_.motions, editKey);
			if (index < 0) return CmdResult::Err("invalid_params", "原曲目已不在曲库中；请刷新后重试");
			const MotionEntry& entry = core_.motions[index];
			const bool editingActiveTrack =
				(core_.loadedMotionKey == editKey ||
					(!core_.player.currentMotionDir.empty() && MotionPathEqualsUtf8(core_.player.currentMotionDir, entry.dir))) &&
				(core_.player.state == PlaybackState::Playing || core_.player.state == PlaybackState::Paused);
			if (editingActiveTrack)
				return CmdResult::Err("protected", "当前曲目正在播放，无法替换其资源文件。请停止播放后重试。你的修改尚未丢失。");
		}
		GuiCommand command{GuiAction::Import};
		command.import = std::move(request);
		command.importCoverSet = hasCoverDataUri && !clearCover;
		command.importCoverClear = clearCover;
		command.importCoverDataUri = clearCover ? std::string() : std::move(coverDataUri);
		const std::string invalidateKey = edit ? editKey : command.import.title;
		if (!invalidateKey.empty())
		{
			if (hooks_.invalidateTrackCover) hooks_.invalidateTrackCover(invalidateKey);
			std::lock_guard<std::mutex> coverLock(pendingCoverInvalidateMutex_);
			pendingImportCoverInvalidateKey_ = invalidateKey;
		}
		core_.QueueGui(std::move(command));
		return CmdResult::Ok("{\"started\":true}");
	}
} // namespace GakuMotion::webpanel
