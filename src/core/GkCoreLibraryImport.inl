namespace GakuMotion
{
	inline bool Core::SavePlaylist(const std::string& destination)
	{
		std::string error;
		const bool ok = playlist.Save(destination.empty() ? playlistPath : destination, error);
		playlistNotice = ok ? "播放列表已保存" : "保存失败: " + error;
		return ok;
	}

	inline bool Core::DeleteMotionFiles(const std::string& key, bool confirmed)
	{
		if (!confirmed) return false;
		if (importBusy || cameraImportBusy) { playlistNotice = "正在导入，完成后可删除曲目"; return false; }
		const int index = FindPlaylistIndex(motions, key);
		if (index < 0) return false;
		const auto root = MotionPathFromUtf8(motionsRoot);
		const auto source = MotionPathFromUtf8(motions[index].dir);
		if (source.filename() != MotionPathFromUtf8(key).filename() || MotionPathFromUtf8(key).has_parent_path())
		{ playlistNotice = "删除失败：曲目路径与标识不一致"; return false; }
		std::filesystem::path checkedRoot, checkedSource;
		std::string error;
		if (!LibraryDelete::ResolveRoot(root, checkedRoot, error) || !LibraryDelete::ResolveChild(checkedRoot, source, checkedSource, error))
		{ playlistNotice = "删除失败，原文件保留: " + error; return false; }
		// Verify persistence before irreversible work. A later external write or
		// lock can still fail; that must never be reported as a file rollback.
		if (!playlist.Save(playlistPath, error))
		{ playlistNotice = "删除失败，原文件保留；列表无法保存: " + error; return false; }
		const auto current = CurrentMotion() ? CurrentMotion()->name : "";
		const bool deletingLoaded = loadedMotionKey == key ||
			(!player.currentMotionDir.empty() && DataLayout::SamePath(player.currentMotionDir, motions[index].dir));
		if (deletingLoaded) { player.Stop(); camera.Release(); NotifyMotionPlaying(false); }
		bool changed = false;
		if (!LibraryDelete::PermanentlyDelete(root, source, changed, error))
		{
			if (changed)
			{
				if (deletingLoaded) ClearCurrentMotion();
				++libraryMutationGeneration;
				const bool refreshed = RefreshMotionLibrary();
				playlistNotice = std::string("删除未完成，部分文件可能已永久删除；") +
					(refreshed ? "已刷新曲库: " : "曲库刷新失败，请稍后刷新: ") + error;
			}
			else playlistNotice = "删除失败，原文件保留: " + error;
			return false;
		}
		if (deletingLoaded) ClearCurrentMotion();
		motions.erase(motions.begin() + index);
		playlist.Remember(motions);
		libraryVersions.erase(key);
		currentMotionIndex = deletingLoaded ? -1 : FindPlaylistIndex(motions, current);
		++libraryRevision; ++libraryMutationGeneration;
		if (!playlist.Save(playlistPath, error))
			playlistNotice = "曲目目录已永久删除；列表保存失败，请稍后保存: " + error;
		else playlistNotice = "已永久删除曲目目录: " + MotionPathToUtf8(checkedSource);
		return true;
	}

	inline bool Core::MoveMotion(const std::string& key, int delta)
	{
		const int index = FindPlaylistIndex(motions, key);
		const int target = index + delta;
		if (index < 0 || motions[index].name != key || target < 0 || target >= static_cast<int>(motions.size()))
			return false;
		const std::string current = CurrentMotion() ? CurrentMotion()->name : "";
		std::swap(motions[index], motions[target]);
		++libraryRevision;
		++libraryMutationGeneration;
		playlist.Remember(motions);
		currentMotionIndex = FindPlaylistIndex(motions, current);
		SavePlaylist();
		return true;
	}

	inline void Core::StartImport(MotionImportRequest request)
	{
		std::lock_guard<std::recursive_mutex> lock(stateMutex);
		if (importBusy || cameraImportBusy)
		{
			playlistNotice = "正在导入，请等待完成";
			return;
		}
		++libraryMutationGeneration; // Any in-flight scan predates this import.
		lastImportedKey.clear();
		pendingImportEditKey = request.editKey;
		pendingImportExistingKeys.clear();
		for (const auto& entry : motions)
			pendingImportExistingKeys.insert(entry.name);
		importProgress = {};
		importProgress.active = true;
		importProgress.progress = 0.02f;
		importProgress.stage = "INIT";
		importProgress.statusText = "正在启动原生转换后端...";

		if (!fnStartImport || (!fnPollImport && !fnPollImportProgress))
		{
			importProgress.active = false;
			importProgress.finished = true;
			importProgress.succeeded = false;
			importProgress.statusText = "导入服务未就绪";
			importProgress.errorDetail = "导入服务未就绪";
			playlistNotice = "导入服务未就绪";
			pendingImportEditKey.clear();
			pendingImportExistingKeys.clear();
			pendingImportCover = {};
			return;
		}

		if (!request.editKey.empty())
		{
			const int idx = FindPlaylistIndex(motions, request.editKey);
			if (idx < 0)
			{
				importProgress.active = false;
				importProgress.finished = true;
				importProgress.succeeded = false;
				importProgress.statusText = "目标曲目不存在";
				importProgress.errorDetail = "未找到待编辑曲目: " + request.editKey;
				playlistNotice = importProgress.errorDetail;
				pendingImportEditKey.clear();
				pendingImportExistingKeys.clear();
				pendingImportCover = {};
				return;
			}
			request.editDir = motions[idx].dir;
			activeImportEditDir = request.editDir;
		}
		else
		{
			activeImportEditDir.clear();
		}

		request.motionsRoot = motionsRoot;
		request.transactionRoot = transactionRoot;
		request.log = logsRoot + "/import.log";
		request.useCsharp = true;
		if (request.useCsharp)
		{
			importProgress.statusText = "正在启动原生转换后端...";
			importProgress.stage = "INIT";
		}
		importBusy = fnStartImport(request, playlistNotice);
		if (!importBusy)
		{
			importProgress.active = false;
			importProgress.finished = true;
			importProgress.succeeded = false;
			importProgress.statusText = "启动导入失败";
			importProgress.errorDetail = playlistNotice;
			pendingImportEditKey.clear();
			pendingImportExistingKeys.clear();
			pendingImportCover = {};
		}
		else
		{
			playlistNotice = request.editDir.empty() ? "正在转换 VMD，完成后将自动加入列表…"
			                                        : "正在更新曲目配置与轨道…";
		}
	}

	inline void Core::ReloadCamera()
	{
		ReloadCurrentMotion();
		cameraNotice = libraryNotice;
	}

	inline void Core::StartCameraImport(CameraImportRequest request)
	{
		if (cameraImportBusy || importBusy)
		{
			cameraNotice = "正在导入镜头，请等待完成";
			return;
		}
		if (player.state == PlaybackState::Playing || player.state == PlaybackState::Paused)
		{
			cameraNotice = "请先停止播放，再导入镜头";
			return;
		}
		const int index = FindPlaylistIndex(motions, request.motionKey);
		if (index < 0 || motions[index].name != request.motionKey)
		{
			cameraNotice = "目标曲目已不在列表中";
			return;
		}
		if (!fnStartCameraImport || !fnPollCameraImport)
		{
			cameraNotice = "镜头导入服务未就绪";
			return;
		}
		request.motionDirectory = motions[index].dir;
		request.log = logsRoot + "/camera_import.log";
		cameraImportDirectory = request.motionDirectory;
		++libraryMutationGeneration;
		cameraImportBusy = fnStartCameraImport(request, cameraNotice);
		if (cameraImportBusy) cameraNotice = "正在导入镜头，完成后可重新加载当前曲目…";
	}

	inline void Core::PollCameraImport()
	{
		if (!cameraImportBusy || !fnPollCameraImport) return;
		std::string result;
		const int state = fnPollCameraImport(result);
		if (!state) return;
		cameraImportBusy = false;
		if (state > 0)
		{
			RefreshMotionLibrary();
			if (player.currentMotionDir == cameraImportDirectory) loadedSourceChanged = true;
		}
		cameraNotice = result;
	}

	inline bool Core::SelectMotionByIndex(int index, bool autoPlay, bool forcePreview)
	{
		if (index < 0 || index >= static_cast<int>(motions.size()))
		{
			LogWarn(detail::FormatMsg("[GakuMotion][Config] motion index out of range: %d", index));
			return false;
		}

		const MotionEntry& entry = motions[index];
		const bool wasPlaying = (player.state == PlaybackState::Playing);
		const bool wasPaused = (player.state == PlaybackState::Paused);

		// 切曲前先干净落停：停止音频、恢复官方 Animator，避免残留姿态串台。
		// 注意 LoadMotion 内部会把状态置为 Preview（静止保持第 0 帧），
		// 因此「选中但未播放」不再退回官方待机造型。
		if (player.state != PlaybackState::Stopped)
		{
			player.Stop();
		}
		for (auto& kv : formationTracks)
		{
			if (kv.second && kv.second->state != PlaybackState::Stopped)
			{
				kv.second->Stop();
			}
		}
		if (player.state != PlaybackState::Stopped || wasPlaying || wasPaused)
		{
			NotifyMotionPlaying(false);
		}

		if (!player.LoadMotion(entry.dir))
		{
			LogWarn("[GakuMotion][Config] failed to load motion: " + entry.dir);
			return false;
		}

		currentMotionIndex = index;
		loadedMotionKey = entry.name;
		loadedMotionEntry = entry;
		loadedMotionVersion = ReadMotionFileVersion(entry.dir);
		loadedSourceMissing = loadedSourceChanged = false;
		++loadedMotionGeneration;
		camera.LoadTrack(player.currentMotionDir, player.clip);
		player.cameraSourceDuration = camera.track.frames.empty() ? 0.0f :
			static_cast<float>(camera.track.frames.size()) / camera.track.fps;
		RebuildFormationForTrack(&entry);
		LoadOverrides(false); // 曲目级覆盖随曲目切换

		// 若本曲目目录自带辅助骨官方真值（aux_replay.json）则顺带挂载，供 mode=3 直接对照
		TryLoadAuxGroundTruth(entry.dir);

		LogInfo(detail::FormatMsg("[GakuMotion][Config] selected: %s | %s | overrides: %s",
		                          entry.name.c_str(),
		                          entry.DisplayLabel().c_str(),
		                          effectiveOverrides.Describe().c_str()));

		if (!forcePreview && (autoPlay || wasPlaying || config.autoPlayOnSelect))
		{
			player.Play();
			NotifyMotionPlaying(true);
		}
		else if (wasPaused && !forcePreview)
		{
			// 暂停中切曲：保持暂停语义（停在第 0 帧），不自动恢复播放
			player.state = PlaybackState::Paused; // No audio started for this new clip yet.
			NotifyMotionPlaying(false);
		}
		else
		{
			// Preview：已载入、静止保持第 0 帧姿态，不播音频。
			LogInfo(detail::FormatMsg(
				"[GakuMotion][Config] preview frame 0 of '%s' (loaded, not playing)", entry.name.c_str()));
		}
		for (auto& kv : formationTracks)
		{
			if (!kv.second) continue;
			kv.second->SyncClockState(player.currentTime);
			kv.second->state = player.state;
		}
		return true;
	}

	inline bool Core::CycleMotion(int delta, const char* source)
	{
		if (motions.empty())
		{
			LogWarn("[GakuMotion][Config] motion library is empty; cannot switch");
			return false;
		}
		const bool wasPlaying = (player.state == PlaybackState::Playing);
		int index = currentMotionIndex;
		if (index < 0)
		{
			index = (delta >= 0) ? 0 : static_cast<int>(motions.size()) - 1;
		}
		else
		{
			const int count = static_cast<int>(motions.size());
			index = ((index + delta) % count + count) % count;
		}
		const bool ok = SelectMotionByIndex(index, wasPlaying);
		if (ok)
		{
			LogInfo(detail::FormatMsg("[GakuMotion][Config] switch %s -> %s (source=%s)",
			                          delta > 0 ? "next" : "prev",
			                          motions[index].name.c_str(),
			                          source ? source : "unknown"));
		}
		return ok;
	}

	inline const MotionEntry* Core::CurrentMotion() const
	{
		if (currentMotionIndex < 0 || currentMotionIndex >= static_cast<int>(motions.size()))
			return loadedMotionKey.empty() ? nullptr : &loadedMotionEntry;
		return &motions[currentMotionIndex];
	}
} // namespace GakuMotion
