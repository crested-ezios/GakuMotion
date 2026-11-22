namespace GakuMotion
{
	inline bool Core::ApplyTrackCoverToDirectory(const std::filesystem::path& trackDir, bool clear, const std::string& coverDataUri, std::string& outCoverFile, std::string& outError) const
	{
		outCoverFile.clear();
		outError.clear();
		if (trackDir.empty())
		{
			outError = "Track directory is empty";
			return false;
		}
		if (clear)
		{
			RemoveTrackCoverFiles(trackDir);
			return UpdateTrackInfoJsonCover(trackDir, false, "", outError);
		}
		std::string mime;
		std::vector<uint8_t> rawBytes;
		if (!DecodeCoverDataUri(coverDataUri, mime, rawBytes))
		{
			outError = "封面图片数据无效（需要 PNG/JPEG/WEBP/BMP Data URI）";
			return false;
		}
		if (fnSaveTrackCoverPng)
		{
			if (!fnSaveTrackCoverPng(rawBytes, mime, trackDir, outCoverFile, outError))
				return false;
		}
		else
		{
			const bool isPngMagic = rawBytes.size() >= 8 &&
				rawBytes[0] == 0x89 && rawBytes[1] == 0x50 && rawBytes[2] == 0x4E && rawBytes[3] == 0x47;
			if (!isPngMagic && mime != "image/png")
			{
				outError = "Cover PNG encoder is unavailable";
				return false;
			}
			if (!WriteRawPngCoverAtomic(rawBytes, trackDir, outCoverFile, outError))
				return false;
		}
		return UpdateTrackInfoJsonCover(trackDir, true, outCoverFile, outError);
	}

	inline bool Core::SetTrackCover(const std::string& key, bool clear, const std::string& coverDataUri, std::string& outThumbnailDataUri, std::string& outError)
	{
		outThumbnailDataUri.clear();
		outError.clear();
		if (key.empty())
		{
			outError = "Track key is required";
			return false;
		}
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		if (importBusy || cameraImportBusy || libraryScan.valid())
		{
			outError = "正在导入或刷新曲库，请稍后再试";
			return false;
		}
		const int index = FindPlaylistIndex(motions, key);
		if (index < 0)
		{
			outError = "Motion key not found: " + key;
			return false;
		}
		const std::filesystem::path trackDir = MotionPathFromUtf8(motions[index].dir);
		std::string savedFile;
		if (!ApplyTrackCoverToDirectory(trackDir, clear, coverDataUri, savedFile, outError))
		{
			return false;
		}
		motions[index].hasCover = !clear;
		motions[index].coverFile = savedFile;
		if (loadedMotionKey == key)
		{
			loadedMotionEntry.hasCover = motions[index].hasCover;
			loadedMotionEntry.coverFile = motions[index].coverFile;
		}
		libraryVersions[key] = ReadMotionFileVersion(motions[index].dir);
		++libraryRevision;
		++libraryMutationGeneration;
		if (!clear && !savedFile.empty())
		{
			if (fnLoadTrackCoverDataUri)
				outThumbnailDataUri = fnLoadTrackCoverDataUri(trackDir / MotionPathFromUtf8(savedFile));
			if (outThumbnailDataUri.empty())
				outThumbnailDataUri = coverDataUri;
		}
		return true;
	}

	inline std::string Core::FindNewlyAddedMotionDirLocked() const
	{
		const auto root = MotionPathFromUtf8(motionsRoot);
		std::error_code ec;
		if (!std::filesystem::is_directory(root, ec)) return {};
		for (std::filesystem::directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec))
		{
			const auto& entry = *it;
			if (entry.path().filename() == ".gakumotion-trash") continue;
			if (!entry.is_directory(ec)) continue;
			const auto utf8Name = entry.path().filename().u8string();
			std::string name(utf8Name.begin(), utf8Name.end());
			if (pendingImportExistingKeys.count(name)) continue;
			for (const char* candidate : {"motion.gkclip", "motion.gk_world_dump", "motion.gkmotion"})
			{
				if (std::filesystem::exists(entry.path() / candidate, ec))
				{
					return MotionPathToUtf8(entry.path());
				}
			}
		}
		return {};
	}

	inline void Core::PollImport()
	{
		if (!importBusy) return;
		std::string result;
		int state = 0;
		{
			std::lock_guard<std::recursive_mutex> lock(stateMutex);
			state = fnPollImportProgress ? fnPollImportProgress(result, &importProgress)
			                             : (fnPollImport ? fnPollImport(result) : 0);
			if (state != 0)
			{
				importBusy = false;
				importProgress.active = false;
				importProgress.finished = true;
				importProgress.succeeded = (state > 0);
				if (state > 0)
				{
					importProgress.progress = 1.0f;
					if (importProgress.statusText.empty()) importProgress.statusText = "导入完成";

					const bool isNewImport = pendingImportEditKey.empty() && activeImportEditDir.empty();
					std::string targetDir = activeImportEditDir;
					if (targetDir.empty())
						targetDir = FindNewlyAddedMotionDirLocked();

					if (!targetDir.empty() && (pendingImportCover.clear || (pendingImportCover.set && !pendingImportCover.dataUri.empty())))
					{
						std::string coverFile, coverErr;
						if (!ApplyTrackCoverToDirectory(
								MotionPathFromUtf8(targetDir),
								pendingImportCover.clear,
								pendingImportCover.dataUri,
								coverFile,
								coverErr))
						{
							LogWarn("[GakuMotion][Cover] Failed to save import cover: " + coverErr);
						}
					}

					RefreshMotionLibrary();
					SavePlaylist();

					std::string importedKey;
					if (!pendingImportEditKey.empty())
					{
						if (FindPlaylistIndex(motions, pendingImportEditKey) >= 0)
							importedKey = pendingImportEditKey;
					}
					else
					{
						for (const auto& entry : motions)
						{
							if (!pendingImportExistingKeys.count(entry.name))
							{
								importedKey = entry.name;
								break;
							}
						}
						if (importedKey.empty() && !targetDir.empty())
						{
							for (const auto& entry : motions)
							{
								if (MotionPathEqualsUtf8(entry.dir, targetDir))
								{
									importedKey = entry.name;
									break;
								}
							}
						}
					}
					lastImportedKey = importedKey;

					if (isNewImport && !importedKey.empty() && player.state != PlaybackState::Playing)
					{
						const int idx = FindPlaylistIndex(motions, importedKey);
						if (idx >= 0) SelectMotionByIndex(idx, false, /*forcePreview=*/true);
					}

					// Keep every loaded channel on one version until explicit reload.
					if (!activeImportEditDir.empty() && player.currentMotionDir == activeImportEditDir)
						loadedSourceChanged = true;
				}
				else
				{
					lastImportedKey.clear();
					if (importProgress.errorDetail.empty()) importProgress.errorDetail = result;
					if (importProgress.statusText.empty()) importProgress.statusText = "导入失败";
				}
				activeImportEditDir.clear();
				pendingImportEditKey.clear();
				pendingImportExistingKeys.clear();
				pendingImportCover = {};
				playlistNotice = result;
			}
		}
	}

	inline bool Core::ApplyLibrarySnapshot(LibraryScanSnapshot snapshot)
	{
		if (!snapshot.result.Succeeded())
		{
			libraryNotice = "刷新失败，仍显示上次结果: " + snapshot.result.error;
			LogWarn("[GakuMotion][Library] " + snapshot.result.error);
			return false;
		}
		std::error_code rootError;
		if (!std::filesystem::is_directory(MotionPathFromUtf8(motionsRoot), rootError))
		{
			libraryNotice = "刷新失败，曲库目录已不可访问；仍显示上次结果";
			return false;
		}
		const std::string current = !loadedMotionKey.empty() ? loadedMotionKey :
			(CurrentMotion() ? CurrentMotion()->name : "");
		auto found = std::move(snapshot.result.motions);
		const bool complete = snapshot.result.Complete();
		if (!complete)
		{
			// Unreadable/half-copied files are not evidence that an old song was deleted.
			for (const auto& old : motions)
				if (FindPlaylistIndex(found, old.name) < 0)
				{
					std::error_code error;
					const bool exists = std::filesystem::exists(MotionPathFromUtf8(old.dir), error);
					if (exists || error)
					{
						found.push_back(old);
						if (libraryVersions.contains(old.name)) snapshot.versions[old.name] = libraryVersions[old.name];
					}
				}
		}
		playlist.Apply(found);
		size_t added = 0, removed = 0, changed = 0;
		for (const auto& entry : found)
		{
			if (FindPlaylistIndex(motions, entry.name) < 0) ++added;
			else if (libraryVersions.contains(entry.name) && snapshot.versions.contains(entry.name) &&
				libraryVersions[entry.name] != snapshot.versions[entry.name]) ++changed;
		}
		for (const auto& entry : motions) if (FindPlaylistIndex(found, entry.name) < 0) ++removed;
		motions = std::move(found);
		libraryVersions = std::move(snapshot.versions);
		currentMotionIndex = current.empty() ? -1 : FindPlaylistIndex(motions, current);
		++libraryRevision;
		++libraryMutationGeneration;
		if (!player.currentMotionDir.empty())
		{
			loadedSourceMissing = FindMotionFile(player.currentMotionDir).empty();
			loadedSourceChanged = !loadedSourceMissing && ReadMotionFileVersion(player.currentMotionDir) != loadedMotionVersion;
		}
		libraryNotice = added || removed || changed
			? detail::FormatMsg("刷新完成：新增 %zu，移除 %zu，更新 %zu", added, removed, changed)
			: "曲库已是最新";
		if (!snapshot.result.warnings.empty())
			libraryNotice += detail::FormatMsg("；%zu 项扫描提示：", snapshot.result.warnings.size()) + snapshot.result.warnings.front();
		return true;
	}

	inline void Core::BeginLibraryRefresh()
	{
		if (libraryScan.valid()) return;
		if (importBusy || cameraImportBusy) { libraryNotice = "正在导入，请在完成后刷新"; return; }
		libraryScanGeneration = libraryMutationGeneration;
		const auto root = motionsRoot;
		try
		{
			libraryScan = std::async(std::launch::async, [root] { return ReadLibrarySnapshot(root); });
			libraryNotice = "正在刷新曲库…";
		}
		catch (const std::exception& error) { libraryNotice = "无法启动刷新: " + std::string(error.what()); }
	}

	inline void Core::PollLibraryRefresh()
	{
		if (!libraryScan.valid() || libraryScan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
		try
		{
			auto snapshot = libraryScan.get();
			if (libraryScanGeneration == libraryMutationGeneration && !importBusy && !cameraImportBusy)
				ApplyLibrarySnapshot(std::move(snapshot));
			else libraryNotice = "曲库在扫描期间已变化，请重新刷新";
		}
		catch (const std::exception& error) { libraryNotice = "刷新失败，仍显示上次结果: " + std::string(error.what()); }
	}

	inline void Core::WaitForLibraryRefresh()
	{
		if (libraryScan.valid()) { libraryScan.wait(); libraryScan = {}; }
	}

	inline bool Core::CanOpenLoadedAudio()
	{
		if (loadedMotionKey.empty() || player.currentMotionDir.empty()) return true;
		if (ReadMotionFileVersion(player.currentMotionDir).files[4] == loadedMotionVersion.files[4]) return true;
		loadedSourceChanged = true;
		libraryNotice = "音乐文件已更新或移除，请先重新加载当前曲目再播放";
		return false;
	}

	inline bool Core::ReloadCurrentMotion()
	{
		if (importBusy || cameraImportBusy) { libraryNotice = "正在导入，完成后可重新加载"; return false; }
		const auto directory = player.currentMotionDir;
		const auto file = FindMotionFile(directory);
		if (file.empty()) { libraryNotice = "重新加载失败：源动作文件已移除"; loadedSourceMissing = true; return false; }
		const auto version = ReadMotionFileVersion(directory);
		MotionClip next;
		CameraTrack nextCamera;
		if (!next.LoadFromFile(file) || !CameraController::ReadTrack(directory, next, nextCamera))
		{
			libraryNotice = "重新加载失败，保留当前版本：" + (nextCamera.error.empty() ? std::string("动作文件无效") : nextCamera.error);
			return false;
		}
		if (version != ReadMotionFileVersion(directory)) { libraryNotice = "文件仍在更新，请稍后重新加载"; return false; }
		const float time = player.currentTime;
		const bool preview = player.state == PlaybackState::Stopped || player.state == PlaybackState::Preview;
		player.AdoptLoadedMotion(directory, file, std::move(next));
		camera.InstallTrack(directory, std::move(nextCamera));
		player.cameraSourceDuration = camera.track.frames.empty() ? 0.0f :
			static_cast<float>(camera.track.frames.size()) / camera.track.fps;
		loadedMotionEntry.timing = player.timing;
		if (formation.active)
		{
			const MotionEntry* m = CurrentMotion();
			for (const auto& s : formation.slots)
			{
				if (s.occupied) ConfigureFormationTrackForSlot(s, m, true);
			}
			UpdateFormationDuration();
		}
		player.Seek(time);
		player.state = preview ? PlaybackState::Preview : PlaybackState::Paused;
		for (auto& kv : formationTracks)
		{
			if (!kv.second) continue;
			kv.second->SyncClockState(player.currentTime);
			kv.second->state = player.state;
		}
		++loadedMotionGeneration;
		loadedMotionVersion = version;
		loadedSourceMissing = loadedSourceChanged = false;
		InvalidateClockAudioProbeCache();
		LoadOverrides(false);
		TryLoadAuxGroundTruth(directory);
		NotifyMotionPlaying(false);
		libraryNotice = "当前曲目已重新加载，保持静止；点击播放继续";
		return true;
	}

	inline void Core::ClearCurrentMotion()
	{
		player.Stop();
		StopAndReleaseFormationTracks(false);
		camera.Release();
		player.clip = MotionClip{};
		player.currentMotionDir.clear();
		player.loadedMotionFile.clear();
		currentMotionIndex = -1;
		loadedMotionKey.clear();
		loadedMotionEntry = {};
		loadedSourceMissing = loadedSourceChanged = false;
		++loadedMotionGeneration;
		InvalidateClockAudioProbeCache();
		motionOverrides = BoneOverrideSet{};
		RebuildFormationForTrack(nullptr);
		LoadOverrides(false);
		NotifyMotionPlaying(false);
	}
} // namespace GakuMotion
