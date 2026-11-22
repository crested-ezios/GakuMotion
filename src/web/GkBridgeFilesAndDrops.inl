namespace GakuMotion::webpanel
{
	inline CmdResult Bridge::DialogPickFile(const jsonlite::Value& params)
	{
		int64_t kind = -1;
		bool multiple = false;
		if (!IntParam(params, "kind", kind) || kind < 0 || kind > 6)
			return CmdResult::Err("invalid_params", "kind must be 0..6");
		if (!BoolParam(params, "multiple", multiple))
			return CmdResult::Err("invalid_params", "multiple must be a boolean");
		if (!hooks_.beginWebFileDialog && !core_.fnBeginPickFile)
			return CmdResult::Err("not_implemented", "Host file dialog service is unavailable");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (dialogPending_ || core_.gui.browsePendingKind >= 0)
			return CmdResult::Err("busy", "A file dialog is already open");
		std::string notice;
		const int legacyKind = kind == 3 ? 2 : kind == 4 ? 1 : kind == 5 ? 3 : 0;
		const bool started = hooks_.beginWebFileDialog
			? hooks_.beginWebFileDialog(static_cast<int>(kind), multiple, notice)
			: core_.fnBeginPickFile(legacyKind, notice);
		if (!started)
			return CmdResult::Err("failed", notice.empty() ? "Cannot open the host file dialog" : notice);
		dialogPending_ = true;
		dialogKind_ = static_cast<int>(kind);
		dialogId_ += 1;
		dialogState_ = "pending";
		dialogPaths_.clear();
		dialogNotice_.clear();
		return CmdResult::Ok("{\"dialogId\":" + JNum(static_cast<double>(dialogId_)) + "}");
	}

	inline CmdResult Bridge::DialogPoll(const jsonlite::Value& params)
	{
		int64_t dialogId = 0;
		if (!IntParam(params, "dialogId", dialogId) || dialogId != static_cast<int64_t>(dialogId_))
			return CmdResult::Err("invalid_params", "dialogId does not match the current dialog");
		PollBridgeDialog();
		std::string paths = "[";
		for (size_t i = 0; i < dialogPaths_.size(); ++i)
		{
			if (i) paths += ",";
			paths += JString(dialogPaths_[i]);
		}
		paths += "]";
		return CmdResult::Ok("{\"state\":" + JString(dialogState_) + ",\"paths\":" + paths +
			",\"notice\":" + JString(dialogNotice_) + "}");
	}

	inline void Bridge::PollBridgeDialog()
	{
		if (!dialogPending_ || !core_.fnPollPickFile) return;
		std::string path;
		std::string notice;
		const int state = core_.fnPollPickFile(path, notice);
		if (state == GAKU_FILE_DIALOG_PENDING) return;
		dialogPending_ = false;
		dialogNotice_ = notice;
		if (state == GAKU_FILE_DIALOG_ACCEPTED)
		{
			dialogState_ = "accepted";
			dialogPaths_.assign(1, path);
		}
		else if (state == GAKU_FILE_DIALOG_CANCELLED) dialogState_ = "cancelled";
		else dialogState_ = "failed";
	}

	inline bool Bridge::IsSupportedAudioExtensionPath(const std::string& path)
	{
		std::string ext = WideToUtf8(std::filesystem::path(Utf8ToWide(path)).extension().wstring());
		for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return ext == ".wav" || ext == ".mp3" || ext == ".flac" ||
		       ext == ".ogg" || ext == ".oga" || ext == ".m4a" ||
		       ext == ".aac" || ext == ".opus" || ext == ".wma";
	}

	inline bool Bridge::IsNetworkAudioPath(const std::string& path)
	{
		return path.find("://") != std::string::npos ||
		       path.rfind("\\\\", 0) == 0 ||
		       path.rfind("//", 0) == 0;
	}

	inline bool Bridge::IsAcceptedImportFile(const std::string& path)
	{
		std::string ext = WideToUtf8(std::filesystem::path(Utf8ToWide(path)).extension().wstring());
		for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return ext == ".vmd" || ext == ".gkcamera" || IsSupportedAudioExtensionPath(path);
	}

	inline DropPeek Bridge::NormalizeDropPeek(const DropPeek& raw)
	{
		DropPeek peek = raw;
		if (!peek.dragPaths.empty())
		{
			if (peek.fileCount == 0) peek.fileCount = static_cast<uint32_t>(peek.dragPaths.size());
			uint32_t valid = 0;
			for (const auto& path : peek.dragPaths)
				if (IsAcceptedImportFile(path)) ++valid;
			peek.accepted = valid;
			if (peek.visual == "hover" && valid == 0) peek.visual = "invalid";
		}
		else if (peek.fileCount == 0 && peek.pendingCount > 0)
		{
			peek.fileCount = peek.pendingCount;
		}
		return peek;
	}

	inline CmdResult Bridge::DropSetTarget(const jsonlite::Value& params)
	{
		bool enabled = false;
		if (!BoolParam(params, "enabled", enabled))
			return CmdResult::Err("invalid_params", "enabled must be a boolean");
		if (!hooks_.dropSetTarget && !hooks_.dropPeek && !hooks_.dropConsume)
			return CmdResult::Err("not_implemented", "Host file-drop service is unavailable");
		dropEnabled_ = enabled;
		if (!enabled) cachedDropPeek_ = {};
		return CmdResult::Ok();
	}

	inline CmdResult Bridge::DropPoll()
	{
		if (!hooks_.dropConsume && !hooks_.dropPeek)
			return CmdResult::Err("not_implemented", "Host file-drop service is unavailable");
		std::string files = "[";
		for (size_t i = 0; i < pendingDrops_.size(); ++i)
		{
			if (i) files += ",";
			files += JString(pendingDrops_[i]);
		}
		files += "]";
		pendingDrops_.clear();
		return CmdResult::Ok("{\"files\":" + files + "}");
	}

	inline CmdResult Bridge::DropPeekMethod()
	{
		if (!hooks_.dropPeek)
			return CmdResult::Err("not_implemented", "Host file-drop service is unavailable");
		DropPeek peek;
		if (hooks_.dropSetTarget)
		{
			if (dropEnabled_) peek = cachedDropPeek_;
		}
		else
		{
			// Test hooks without dropSetTarget may invoke drop.peek directly.
			peek = NormalizeDropPeek(hooks_.dropPeek());
		}
		if (peek.visual.empty()) peek.visual = "none";
		return CmdResult::Ok("{\"visual\":" + JString(peek.visual) +
			",\"fileCount\":" + JNum(peek.fileCount) +
			",\"accepted\":" + JNum(peek.accepted) +
			",\"hostElevated\":" + JBool(peek.hostElevated) +
			",\"hostBlocked\":" + JBool(peek.hostBlocked) + "}");
	}

	inline const char* Bridge::VmdTendencyName(VmdTendency tendency)
	{
		switch (tendency)
		{
		case VmdTendency::MotionOnly: return "motionOnly";
		case VmdTendency::MotionAndFacial: return "motionAndFacial";
		case VmdTendency::FacialOnly: return "facialOnly";
		case VmdTendency::CameraOnly: return "cameraOnly";
		case VmdTendency::Empty: return "empty";
		default: return "invalid";
		}
	}

	inline std::string Bridge::TimingJson(const PerformanceTiming& timing)
	{
		const auto track = [](const TrackTiming& value) {
			return "{\"speed\":" + JNum(value.speed) + ",\"offsetSeconds\":" + JNum(value.offsetSeconds) + "}";
		};
		return "{\"motion\":" + track(timing.motion) + ",\"facial\":" + track(timing.facial) +
			",\"camera\":" + track(timing.camera) + ",\"audio\":" + track(timing.audio) + "}";
	}

	inline bool Bridge::ParseTiming(const jsonlite::Value& value, PerformanceTiming& timing, std::string& error)
	{
		const auto track = [&](const char* name, TrackTiming& target) {
			const jsonlite::Value* field = value.Find(name);
			if (!field) return true;
			if (!field->IsObject()) { error = std::string(name) + " timing must be an object"; return false; }
			double speed = target.speed, offset = target.offsetSeconds;
			if (!NumberParam(*field, "speed", speed)) { error = std::string(name) + ".speed must be a finite number"; return false; }
			const jsonlite::Value* offsetValue = field->Find("offsetSeconds");
			if (!offsetValue) offsetValue = field->Find("offset_seconds");
			if (offsetValue)
			{
				if (!offsetValue->IsNumber() || !std::isfinite(offsetValue->numberValue))
				{ error = std::string(name) + ".offsetSeconds must be a finite number"; return false; }
				offset = offsetValue->numberValue;
			}
			if (speed < 0.0) { error = std::string(name) + ".speed must be >= 0"; return false; }
			target.speed = static_cast<float>(speed);
			target.offsetSeconds = static_cast<float>(offset);
			return true;
		};
		return track("motion", timing.motion) && track("facial", timing.facial) &&
			track("camera", timing.camera) && track("audio", timing.audio);
	}
} // namespace GakuMotion::webpanel
