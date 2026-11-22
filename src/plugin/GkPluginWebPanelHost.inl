	// ---- gakumotion.web-panel/v1 provider ------------------------------------
	// The Loader owns the WebView2 controller and calls these on its desktop UI
	// thread (onUiTick shares the onDesktopUi callback gate, so the host
	// file-drop services are legal here). Domain semantics live in
	// GkWebPanelBridge.hpp; this block only wires Windows/host services to it.

	void WebPanelModuleAnchor() {}

	std::wstring WebPanelModuleAssetsDir()
	{
		static std::wstring cached;
		static bool resolved = false;
		if (resolved) return cached;
		resolved = true;
		HMODULE module = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&WebPanelModuleAnchor), &module) || !module)
			return {};
		wchar_t buffer[32768]{};
		const DWORD length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
		if (!length || length >= std::size(buffer)) return {};
		const std::filesystem::path modulePath(buffer, buffer + length);
		cached = (modulePath.parent_path() / L"GakuMotion.UI").wstring();
		return cached;
	}

	// GakuMotion owns its React/WebView2 UI bundle and ships GakuMotion.UI beside
	// GakuMotion.dll in the plugin directory (for example GakuLoader/plugins/GakuMotion.UI).
	// Never read a host-global UI folder outside the plugin's own directory.
	std::vector<std::wstring> WebPanelAssetsDirs()
	{
		std::vector<std::wstring> candidates;
		const std::wstring moduleDir = WebPanelModuleAssetsDir();
		if (!moduleDir.empty()) candidates.push_back(moduleDir);
		return candidates;
	}

	bool WebPanelOpenLibraryFolder(std::string& notice)
	{
		std::string folder;
		{
			auto& core = GakuMotion::Core::Instance();
			std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
			folder = core.motionsRoot;
		}
		std::wstring wide;
		if (folder.empty() || !GakuMotion::windows_path::FromUtf8(folder, wide))
		{
			notice = "曲库目录不可用";
			return false;
		}
		const auto result = reinterpret_cast<INT_PTR>(
			ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
		if (result <= 32)
		{
			notice = "无法打开曲库文件夹，请检查目录是否存在（错误 " + std::to_string(result) + "）";
			return false;
		}
		return true;
	}

	bool WebPanelDropSetTarget(bool enabled, std::string& notice)
	{
		const auto& service = GakuMotion::host::fileDrop;
		if (!service.setTarget)
		{
			notice = "宿主文件拖放服务不可用";
			return false;
		}
		if (!enabled)
		{
			service.setTarget(service.context, nullptr);
			return true;
		}
		HWND window = GakuMotion::host::desktopWindow.getWindow
			? static_cast<HWND>(GakuMotion::host::desktopWindow.getWindow()) : nullptr;
		RECT rect{0, 0, 1920, 1080};
		if (window) GetClientRect(window, &rect);
		const GakuFileDropTarget target{GAKU_SERVICE_VERSION, sizeof(GakuFileDropTarget),
			static_cast<float>(rect.left), static_cast<float>(rect.top),
			static_cast<float>(rect.right), static_cast<float>(rect.bottom)};
		if (service.setTarget(service.context, &target) != GAKU_OK)
		{
			notice = "拖放目标注册失败";
			return false;
		}
		return true;
	}

	bool WebPanelBeginFileDialog(int webKind, bool /*multiple*/, std::string& notice)
	{
		const auto& service = GakuMotion::host::fileDialog;
		if (!service.beginOpen || !service.poll || !service.release)
		{
			notice = "GakuLoader 文件选择服务不可用，请更新宿主或输入完整文件路径";
			return false;
		}
		if (g_fileDialogTicket)
		{
			notice = "已有文件选择请求正在进行";
			return false;
		}
		GakuFileFilter filter{L"VMD 动作", L"*.vmd"};
		const wchar_t* title = L"选择文件";
		switch (webKind)
		{
		case 0:
			title = L"选择演出与导入素材";
			filter = GakuFileFilter{
				L"演出与导入素材 (*.vmd;*.gkcamera;*.wav;*.mp3;*.flac;*.ogg;*.m4a;*.aac;*.opus)",
				L"*.vmd;*.gkcamera;*.wav;*.mp3;*.flac;*.ogg;*.oga;*.m4a;*.aac;*.opus"};
			break;
		case 1:
			title = L"选择身体动作 VMD";
			filter = GakuFileFilter{L"VMD 身体动作", L"*.vmd"};
			break;
		case 2:
			title = L"选择表情/口型 VMD";
			filter = GakuFileFilter{L"VMD 表情动作", L"*.vmd"};
			break;
		case 3:
			title = L"选择镜头文件";
			filter = GakuFileFilter{L"镜头 VMD / GKCAM", L"*.vmd;*.gkcamera"};
			break;
		case 4:
			title = L"选择音频文件";
			filter = GakuFileFilter{
				L"音频文件 (*.wav;*.mp3;*.flac;*.ogg;*.m4a;*.aac;*.opus)",
				L"*.wav;*.mp3;*.flac;*.ogg;*.oga;*.m4a;*.aac;*.opus"};
			break;
		case 5:
			title = L"选择本地视频";
			filter = GakuFileFilter{L"本地视频", L"*.mp4;*.m4v;*.mov;*.webm;*.avi;*.wmv"};
			break;
		case 6:
			title = L"选择封面图片";
			filter = GakuFileFilter{L"图片文件 (*.png;*.jpg;*.jpeg;*.webp;*.bmp)", L"*.png;*.jpg;*.jpeg;*.webp;*.bmp"};
			break;
		default:
			notice = "不支持的文件选择类型";
			return false;
		}
		const GakuFileDialogRequest request{GAKU_SERVICE_VERSION, sizeof(GakuFileDialogRequest), title, 1, &filter};
		uint64_t ticket = 0;
		const auto status = service.beginOpen(service.context, &request, &ticket);
		if (status != GAKU_OK || !ticket)
		{
			char detail[128]{};
			std::snprintf(detail, sizeof(detail), "文件选择请求失败 (status=%d)", status);
			notice = detail;
			return false;
		}
		g_fileDialogTicket = ticket;
		notice.clear();
		return true;
	}

	GakuMotion::webpanel::DropPeek WebPanelDropPeek()
	{
		GakuMotion::webpanel::DropPeek peek;
		const auto& drop = GakuMotion::host::fileDrop;
		const auto& drag = GakuMotion::host::fileDrag;
		if (!drop.poll) return peek;
		GakuFileDropResult result{};
		if (drop.poll(drop.context, sizeof(result), &result, 0, nullptr) == GAKU_OK)
			peek.pendingCount = result.fileCount;
		if (drag.peek)
		{
			GakuFileDragState state{};
			const auto dragStatus = drag.peek(drag.context, sizeof(state), &state, 0, nullptr);
			if (dragStatus == GAKU_OK || dragStatus == GAKU_E_SIZE)
			{
				peek.hostBlocked = (state.flags & GAKU_FILE_DRAG_HOST_BLOCKED) != 0;
				peek.hostElevated = (state.flags & GAKU_FILE_DRAG_HOST_ELEVATED) != 0;
				if (state.flags & GAKU_FILE_DRAG_INSIDE_TARGET) peek.visual = "hover";
				else if (state.flags & GAKU_FILE_DRAG_ACTIVE) peek.visual = "armed";
				if (state.fileCount > 0) peek.fileCount = state.fileCount;
				if (state.requiredBytes > 0 && state.requiredBytes <= GAKU_FILE_DROP_MAX_BYTES)
				{
					std::string buffer(state.requiredBytes, '\0');
					if (drag.peek(drag.context, sizeof(state), &state,
						static_cast<uint32_t>(buffer.size()), buffer.data()) == GAKU_OK)
					{
						size_t start = 0;
						while (start < buffer.size())
						{
							const size_t end = buffer.find('\0', start);
							if (end == std::string::npos) break;
							if (end > start) peek.dragPaths.push_back(buffer.substr(start, end - start));
							start = end + 1;
						}
						if (peek.fileCount == 0 && !peek.dragPaths.empty())
							peek.fileCount = static_cast<uint32_t>(peek.dragPaths.size());
					}
				}
			}
		}
		else if (peek.pendingCount > 0) peek.visual = "armed";
		peek.ok = true;
		return peek;
	}

	GakuMotion::webpanel::DropBatch WebPanelDropConsume()
	{
		GakuMotion::webpanel::DropBatch batch;
		const auto& service = GakuMotion::host::fileDrop;
		if (!service.poll) return batch;
		GakuFileDropResult result{};
		if (service.poll(service.context, sizeof(result), &result, 0, nullptr) != GAKU_OK ||
			result.fileCount == 0)
		{
			batch.ok = true;
			return batch;
		}
		if (result.requiredBytes == 0 || result.requiredBytes > GAKU_FILE_DROP_MAX_BYTES)
		{
			batch.notice = "拖放批次超出宿主限制";
			return batch;
		}
		std::string buffer(result.requiredBytes, '\0');
		if (service.poll(service.context, sizeof(result), &result,
			static_cast<uint32_t>(buffer.size()), buffer.data()) != GAKU_OK)
		{
			batch.notice = "拖放读取失败";
			return batch;
		}
		size_t start = 0;
		while (start < buffer.size() && batch.paths.size() < result.fileCount)
		{
			const size_t end = buffer.find('\0', start);
			if (end == std::string::npos) break;
			if (end > start) batch.paths.push_back(buffer.substr(start, end - start));
			start = end + 1;
		}
		batch.ok = true;
		return batch;
	}

	GakuMotion::webpanel::AppearanceInfo WebPanelAppearance()
	{
		GakuMotion::webpanel::AppearanceInfo info;
		const auto& service = GakuMotion::host::appearance;
		if (!service.getAppearance) return info;
		GakuUiAppearanceV1 data{};
		if (service.getAppearance(sizeof(data), &data) != GAKU_OK) return info;
		info.ok = true;
		info.theme = static_cast<int>(data.theme);
		info.uiScale = data.uiScale;
		return info;
	}

	GakuMotion::webpanel::AudioProbeView WebPanelProbeAudio(const std::string& pathUtf8, bool includeCover)
	{
		GakuMotion::webpanel::AudioProbeView out;
		GakuMotion::audio::AudioProbeResult probed;
		{
			std::lock_guard<std::mutex> probeLock(g_audioProbeMutex);
			probed = g_audioPlugin.ProbeFile(pathUtf8, includeCover);
		}
		out.valid = probed.valid;
		out.sampleRate = probed.sampleRate;
		out.channels = probed.channels;
		out.hasCover = probed.hasCover;
		out.durationSeconds = probed.durationSeconds;
		out.codec = probed.codec;
		out.title = probed.title;
		out.artist = probed.artist;
		out.album = probed.album;
		if (includeCover && !probed.coverBytes.empty())
		{
			out.coverDataUri = GakuMotion::cover::BytesToThumbnailDataUri(
				probed.coverBytes.data(), probed.coverBytes.size(), probed.coverMime, 512);
			if (out.coverDataUri.empty()) out.hasCover = false;
		}
		return out;
	}

	std::string WebPanelGetTrackCoverDataUri(const std::string& trackDirUtf8)
	{
		if (trackDirUtf8.empty()) return {};
		const auto dirPath = GakuMotion::MotionPathFromUtf8(trackDirUtf8);
		const std::string coverFile = GakuMotion::audio::ResolveTrackCoverFileName(dirPath);
		if (!coverFile.empty())
		{
			std::string uri = GakuMotion::cover::LoadCoverAsDataUri(
				dirPath / GakuMotion::MotionPathFromUtf8(coverFile), 512);
			if (!uri.empty()) return uri;
		}
		const std::string audioFile = GakuMotion::audio::ResolveTrackAudioFileName(dirPath);
		if (!audioFile.empty())
		{
			GakuMotion::audio::AudioProbeResult probed;
			{
				std::lock_guard<std::mutex> probeLock(g_audioProbeMutex);
				probed = g_audioPlugin.ProbeFile(
					GakuMotion::MotionPathToUtf8(dirPath / GakuMotion::MotionPathFromUtf8(audioFile)), true);
			}
			if (!probed.coverBytes.empty())
			{
				return GakuMotion::cover::BytesToThumbnailDataUri(
					probed.coverBytes.data(), probed.coverBytes.size(), probed.coverMime, 512);
			}
		}
		return {};
	}

	std::string WebPanelReadImagePreviewDataUri(const std::string& pathUtf8)
	{
		if (pathUtf8.empty()) return {};
		return GakuMotion::cover::LoadCoverAsDataUri(GakuMotion::MotionPathFromUtf8(pathUtf8), 1024);
	}
