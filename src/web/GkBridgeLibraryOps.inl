namespace GakuMotion::webpanel
{
	inline CmdResult Bridge::LibraryProbeFiles(const jsonlite::Value& params)
	{
		const jsonlite::Value* paths = Param(params, "paths");
		if (!paths || !paths->IsArray() || paths->Size() == 0 || paths->Size() > 256)
			return CmdResult::Err("invalid_params", "paths must be a non-empty array of at most 256 strings");
		std::vector<std::string> files;
		for (size_t i = 0; i < paths->Size(); ++i)
		{
			const jsonlite::Value* item = paths->At(i);
			if (!item || !item->IsString() || item->stringValue.empty() || item->stringValue.size() > 32767)
				return CmdResult::Err("invalid_params", "each path must be a non-empty string");
			files.push_back(item->stringValue);
		}
		const ImportClassification classification = ClassifyImportFiles(files);
		std::string probes = "[";
		for (size_t i = 0; i < files.size(); ++i)
		{
			const VmdProbeInfo info = ProbeCameraOrVmdFile(files[i]);
			if (i) probes += ",";
			probes += "{\"path\":" + JString(files[i]) +
				",\"valid\":" + JBool(info.valid) +
				",\"isGkCamera\":" + JBool(info.isGkCamera) +
				",\"tendency\":" + JString(VmdTendencyName(info.tendency)) +
				",\"boneFrames\":" + JNum(info.boneFrames) +
				",\"bodyBoneFrames\":" + JNum(info.bodyBoneFrames) +
				",\"eyeBoneFrames\":" + JNum(info.eyeBoneFrames) +
				",\"morphFrames\":" + JNum(info.morphFrames) +
				",\"cameraFrames\":" + JNum(info.cameraFrames) + "}";
		}
		probes += "]";
		std::string motionCandidatesJson = "[";
		for (size_t i = 0; i < classification.motionCandidates.size(); ++i)
		{
			if (i) motionCandidatesJson += ",";
			motionCandidatesJson += JString(classification.motionCandidates[i]);
		}
		motionCandidatesJson += "]";
		const std::string classificationJson = "{\"suggestedTitle\":" + JString(classification.suggestedTitle) +
			",\"motionCandidate\":" + JString(classification.motionCandidate) +
			",\"motionCandidates\":" + motionCandidatesJson +
			",\"facialCandidate\":" + JString(classification.facialCandidate) +
			",\"cameraCandidate\":" + JString(classification.cameraCandidate) +
			",\"audioCandidate\":" + JString(classification.audioCandidate) +
			",\"unsupportedAudioCount\":" + JNum(classification.unsupportedAudioCount) + "}";
		return CmdResult::Ok("{\"classification\":" + classificationJson + ",\"probes\":" + probes + "}");
	}

	inline std::string Bridge::MotionPositionsJson(const std::vector<MotionPositionEntry>& positions)
	{
		std::string out = "[";
		for (size_t j = 0; j < positions.size(); ++j)
		{
			const auto& p = positions[j];
			if (j) out += ",";
			out += "{\"slot\":" + JNum(p.slot) +
				",\"x\":" + JNum(p.x) +
				",\"y\":" + JNum(p.y) +
				",\"z\":" + JNum(p.z) +
				",\"yawDegrees\":" + JNum(p.yawDegrees) +
				",\"hasMotion\":" + JBool(p.hasMotion) +
				",\"hasFacial\":" + JBool(p.hasFacial) + "}";
		}
		out += "]";
		return out;
	}

	inline CmdResult Bridge::LibraryStartEdit(const jsonlite::Value& params)
	{
		std::string key;
		int64_t revision = -1;
		if (!StringParam(params, "key", key) || key.empty() || !IntParam(params, "revision", revision))
			return CmdResult::Err("invalid_params", "key and revision are required");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		if (static_cast<uint64_t>(revision) != core_.libraryRevision)
			return CmdResult::Err("stale_revision", "曲库已变化，请刷新后重新选择曲目");
		if (core_.importBusy || core_.cameraImportBusy || core_.libraryScan.valid())
			return CmdResult::Err("busy", "正在导入或刷新，请等待完成");
		const int index = FindPlaylistIndex(core_.motions, key);
		if (index < 0) return CmdResult::Err("invalid_params", "Motion key not found: " + key);
		const MotionEntry& entry = core_.motions[index];
		const int posCount = entry.isGroup || !entry.positions.empty()
			? (std::max)(entry.positionCount, static_cast<int>(entry.positions.size()))
			: (std::max)(1, entry.positionCount);
		return CmdResult::Ok("{\"key\":" + JString(key) +
			",\"title\":" + JString(entry.title) +
			",\"subtitle\":" + JString(entry.subtitle) +
			",\"hasFacial\":" + JBool(entry.hasFacial) +
			",\"hasCamera\":" + JBool(entry.hasCamera) +
			",\"hasAudio\":" + JBool(entry.hasAudio) +
			",\"audioFile\":" + JString(entry.audioFile) +
			",\"hasCover\":" + JBool(entry.hasCover) +
			",\"isGroup\":" + JBool(entry.isGroup) +
			",\"positionCount\":" + JNum(posCount) +
			",\"positions\":" + MotionPositionsJson(entry.positions) +
			",\"timing\":" + TimingJson(entry.timing) + "}");
	}

	inline CmdResult Bridge::LibraryProbeAudio(const jsonlite::Value& params)
	{
		std::string path;
		if (!StringParam(params, "path", path) || path.empty() || path.size() > 32767)
			return CmdResult::Err("invalid_params", "path must be a non-empty string");
		if (IsNetworkAudioPath(path))
			return CmdResult::Err("invalid_params", "不支持网络或流媒体路径，请选择本地音频文件");
		if (!IsSupportedAudioExtensionPath(path))
			return CmdResult::Err("invalid_params", "音频格式不支持（支持 .wav/.mp3/.flac/.ogg/.m4a/.aac/.opus）");
		bool includeCover = true;
		if (params.Find("includeCover") && !BoolParam(params, "includeCover", includeCover))
			return CmdResult::Err("invalid_params", "includeCover must be a boolean");

		AudioProbeView probed;
		if (hooks_.probeAudio) probed = hooks_.probeAudio(path, includeCover);
		return CmdResult::Ok("{\"valid\":" + JBool(probed.valid) +
			",\"sampleRate\":" + JNum(probed.sampleRate) +
			",\"channels\":" + JNum(probed.channels) +
			",\"durationSeconds\":" + JNum(probed.durationSeconds) +
			",\"codec\":" + JString(probed.codec) +
			",\"title\":" + JString(probed.title) +
			",\"artist\":" + JString(probed.artist) +
			",\"album\":" + JString(probed.album) +
			",\"hasCover\":" + JBool(probed.hasCover) +
			",\"coverDataUri\":" + JString(probed.coverDataUri) + "}");
	}

	inline CmdResult Bridge::LibraryGetCover(const jsonlite::Value& params)
	{
		std::string key, directory;
		StringParam(params, "key", key);
		StringParam(params, "directory", directory);
		if (key.empty() && directory.empty())
			return CmdResult::Err("invalid_params", "key or directory is required");

		std::string resolvedKey, resolvedDir;
		bool hasCoverHint = false;
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			for (const auto& entry : core_.motions)
			{
				if ((!key.empty() && entry.name == key) ||
					(!directory.empty() && entry.dir == directory))
				{
					resolvedKey = entry.name;
					resolvedDir = entry.dir;
					hasCoverHint = entry.hasCover || entry.hasAudio;
					break;
				}
			}
		}
		if (resolvedDir.empty())
			return CmdResult::Err("not_found", "Track not found in library");

		std::string dataUri;
		bool pending = false;
		if (hasCoverHint)
		{
			if (hooks_.queryTrackCoverAsync)
			{
				hooks_.queryTrackCoverAsync(resolvedKey, resolvedDir, dataUri, pending);
			}
			else if (hooks_.getTrackCoverDataUri)
			{
				dataUri = hooks_.getTrackCoverDataUri(resolvedDir);
			}
		}
		return CmdResult::Ok("{\"hasCover\":" + JBool(!dataUri.empty()) +
			",\"coverDataUri\":" + JString(dataUri) +
			",\"pending\":" + JBool(pending) + "}");
	}

	inline CmdResult Bridge::LibrarySetCover(const jsonlite::Value& params)
	{
		std::string key;
		if (!StringParam(params, "key", key) || key.empty())
			return CmdResult::Err("invalid_params", "key is required");
		bool clear = false;
		if (params.Find("clear") && !BoolParam(params, "clear", clear))
			return CmdResult::Err("invalid_params", "clear must be a boolean");
		std::string coverDataUri;
		if (!clear)
		{
			if (!StringParam(params, "coverDataUri", coverDataUri) || coverDataUri.empty())
				return CmdResult::Err("invalid_params", "coverDataUri is required when clear is false");
			if (coverDataUri.size() > 16ULL * 1024ULL * 1024ULL ||
				coverDataUri.rfind("data:image/", 0) != 0)
				return CmdResult::Err("invalid_params", "coverDataUri must be a valid image data URI");
		}

		std::string thumbnailUri;
		std::string error;
		if (!core_.SetTrackCover(key, clear, coverDataUri, thumbnailUri, error))
		{
			if (error.find("正在导入") != std::string::npos)
				return CmdResult::Err("busy", error);
			if (error.find("not found") != std::string::npos)
				return CmdResult::Err("not_found", error);
			return CmdResult::Err("failed", error.empty() ? "Failed to save track cover" : error);
		}
		if (hooks_.invalidateTrackCover) hooks_.invalidateTrackCover(key);
		forceDomainRefresh_.store(true, std::memory_order_release);
		uint64_t rev = 0;
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			rev = core_.libraryRevision;
		}
		return CmdResult::Ok("{\"hasCover\":" + JBool(!clear) +
			",\"coverDataUri\":" + JString(thumbnailUri) +
			",\"revision\":" + JNum(static_cast<double>(rev)) + "}");
	}

	inline CmdResult Bridge::LibraryReadImagePreview(const jsonlite::Value& params)
	{
		std::string path;
		if (!StringParam(params, "path", path) || path.empty() || path.size() > 32767)
			return CmdResult::Err("invalid_params", "path must be a non-empty string");
		if (IsNetworkAudioPath(path))
			return CmdResult::Err("invalid_params", "不支持网络路径，请选择本地图片文件");
		if (!hooks_.readImagePreviewDataUri)
			return CmdResult::Err("not_implemented", "Image preview hook is unavailable");
		const std::string dataUri = hooks_.readImagePreviewDataUri(path);
		if (dataUri.empty())
			return CmdResult::Err("failed", "无法读取或解码所选图片文件");
		return CmdResult::Ok("{\"dataUri\":" + JString(dataUri) + "}");
	}

	inline CmdResult Bridge::ScreenshotCapabilitiesMethod()
	{
		ScreenshotCapabilitiesView caps;
		if (hooks_.getScreenshotCapabilities)
		{
			caps = hooks_.getScreenshotCapabilities();
		}
		else
		{
			caps.available = false;
			caps.notice = "ScreenshotManager 联动服务不可用";
		}
		return CmdResult::Ok("{\"available\":" + JBool(caps.available) +
			",\"runtimeReady\":" + JBool(caps.runtimeReady) +
			",\"cameraAvailable\":" + JBool(caps.cameraAvailable) +
			",\"windowWidth\":" + JNum(caps.windowWidth) +
			",\"windowHeight\":" + JNum(caps.windowHeight) +
			",\"notice\":" + JString(caps.notice) + "}");
	}

	inline CmdResult Bridge::ScreenshotCaptureMethod()
	{
		if (!hooks_.captureScreenshot)
			return CmdResult::Err("not_implemented", "ScreenshotManager 联动拍照服务不可用");
		const ScreenshotCaptureView captured = hooks_.captureScreenshot();
		if (!captured.ok || captured.dataUri.empty())
		{
			return CmdResult::Err("failed",
				captured.error.empty() ? "ScreenshotManager 拍照失败" : captured.error);
		}
		return CmdResult::Ok("{\"dataUri\":" + JString(captured.dataUri) +
			",\"width\":" + JNum(captured.width) +
			",\"height\":" + JNum(captured.height) + "}");
	}

	inline CmdResult Bridge::ClockSetRunMode(const jsonlite::Value& params)
	{
		uint32_t rawMode = GAKUMOTION_RUN_REALTIME;
		if (const auto* rm = params.Find("runMode"))
		{
			if (rm->IsString())
			{
				if (rm->stringValue == "recording") rawMode = GAKUMOTION_RUN_RECORDING;
				else if (rm->stringValue == "realtime") rawMode = GAKUMOTION_RUN_REALTIME;
				else return CmdResult::Err("invalid_params", "runMode must be realtime or recording");
			}
			else if (rm->IsNumber())
			{
				const int v = rm->AsInt(-1);
				if (v != 0 && v != 1) return CmdResult::Err("invalid_params", "runMode must be 0 or 1");
				rawMode = static_cast<uint32_t>(v);
			}
			else return CmdResult::Err("invalid_params", "runMode must be realtime or recording");
		}
		else return CmdResult::Err("invalid_params", "runMode is required");

		if (core_.SetClockRunMode(rawMode) != GAKU_OK)
			return CmdResult::Err("failed", "Failed to update run mode");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		return CmdResult::Ok(ClockJsonLocked());
	}

	inline CmdResult Bridge::ClockConfigure(const jsonlite::Value& params)
	{
		GakuMotionClockSnapshotV1 cur{};
		core_.ReadClockSnapshot(&cur);
		uint32_t source = cur.configuredSource;
		uint32_t fpsNum = cur.fpsNumerator > 0 ? cur.fpsNumerator : 60u;
		uint32_t fpsDen = cur.fpsDenominator > 0 ? cur.fpsDenominator : 1u;

		if (const auto* s = params.Find("source"))
		{
			if (s->IsString())
			{
				if (s->stringValue == "audio") source = GAKUMOTION_CLOCK_AUDIO;
				else if (s->stringValue == "engine_delta") source = GAKUMOTION_CLOCK_ENGINE_DELTA;
				else if (s->stringValue == "fixed_step") source = GAKUMOTION_CLOCK_FIXED_STEP;
				else if (s->stringValue == "external") source = GAKUMOTION_CLOCK_EXTERNAL;
				else return CmdResult::Err("invalid_params", "unknown clock source");
			}
			else if (s->IsNumber())
			{
				const int v = s->AsInt(-1);
				if (v < 0 || v > 3) return CmdResult::Err("invalid_params", "clock source out of range");
				source = static_cast<uint32_t>(v);
			}
			else return CmdResult::Err("invalid_params", "source must be string or number");
		}
		int64_t num = 0, den = 0;
		if (params.Find("fpsNumerator"))
		{
			if (!IntParam(params, "fpsNumerator", num) || num < 1 || num > 1000)
				return CmdResult::Err("invalid_params", "fpsNumerator must be in 1..1000");
			fpsNum = static_cast<uint32_t>(num);
		}
		if (params.Find("fpsDenominator"))
		{
			if (!IntParam(params, "fpsDenominator", den) || den < 1 || den > 100)
				return CmdResult::Err("invalid_params", "fpsDenominator must be in 1..100");
			fpsDen = static_cast<uint32_t>(den);
		}

		if (core_.ConfigureClock(source, fpsNum, fpsDen) != GAKU_OK)
			return CmdResult::Err("failed", "Failed to configure clock");
		std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
		return CmdResult::Ok(ClockJsonLocked());
	}

	inline CmdResult Bridge::AudioEffectSet(const jsonlite::Value& params)
	{
		std::string presetId;
		if (!StringParam(params, "presetId", presetId) || presetId.empty() || presetId.size() > 63)
			return CmdResult::Err("invalid_params", "presetId is required");
		if (!hooks_.setAudioEffectPreset)
			return CmdResult::Err("not_implemented", "GakuAudio service hook unavailable");
		std::string error;
		if (!hooks_.setAudioEffectPreset(presetId, error))
			return CmdResult::Err("failed", error.empty() ? "Failed to switch audio effect" : error);
		forceAudioEffectRefresh_.store(true, std::memory_order_release);
		return CmdResult::Ok(BuildAudioEffectJson());
	}
} // namespace GakuMotion::webpanel
