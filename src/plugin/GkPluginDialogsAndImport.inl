	// -----------------------------------------------------------------------------
	// The host owns UTF-16 paths; all Motion string paths are UTF-8. Filesystem
	// streams use MotionPathFromUtf8 rather than the Windows ANSI code page.
	// -----------------------------------------------------------------------------
	std::string WidePathToNarrow(const wchar_t* wide)
	{
		if (!wide || !*wide) return std::string();
		std::string out;
		if (!GakuMotion::windows_path::ToUtf8(wide, out)) return {};
		return out;
	}

	std::wstring ToWide(const std::string& text, UINT codePage)
	{
		if (text.empty()) return {};
		if (codePage != CP_UTF8) return {};
		std::wstring out;
		if (!GakuMotion::windows_path::FromUtf8(text, out)) return {};
		return out;
	}

	std::string g_importLog;
	uint64_t g_importLogReadOffset{0};
	std::string g_importPendingLine;
	uint64_t g_fileDialogTicket{0};

	// Resident web-panel bridge (gakumotion.web-panel/v1). Created during
	// MotionActivate, silently invalidated by MotionShutdown on the Unity
	// thread; bridge.closed is emitted from the UI-thread onShutdown callback.
	// Like every other provider table here it lives until process exit.
	GakuMotion::webpanel::Bridge* g_webPanelBridge = nullptr;

	void ReleasePendingFileDialog()
	{
		const auto ticket = g_fileDialogTicket;
		g_fileDialogTicket = 0;
		if (ticket && GakuMotion::host::fileDialog.release)
			GakuMotion::host::fileDialog.release(GakuMotion::host::fileDialog.context, ticket);
	}

	bool BeginMotionFile(int kind, std::string& notice)
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
		const GakuFileFilter filter = kind == 0 ? GakuFileFilter{L"VMD motion", L"*.vmd"}
			: kind == 2 ? GakuFileFilter{L"Camera VMD / GKCAM", L"*.vmd;*.gkcamera"}
			: kind == 3 ? GakuFileFilter{L"Video file", L"*.mp4;*.m4v;*.mov;*.webm;*.avi;*.wmv"}
			: GakuFileFilter{L"WAV audio", L"*.wav"};
		const GakuFileDialogRequest request{GAKU_SERVICE_VERSION, sizeof(GakuFileDialogRequest),
			kind == 0 ? L"选择 VMD 动作" : kind == 2 ? L"选择镜头文件" : kind == 3 ? L"选择本地视频" : L"选择 WAV 音频", 1, &filter};
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

	int PollMotionFile(std::string& path, std::string& notice)
	{
		const auto& service = GakuMotion::host::fileDialog;
		if (!g_fileDialogTicket || !service.poll)
		{
			notice = "文件选择请求已失效";
			ReleasePendingFileDialog();
			return GAKU_FILE_DIALOG_FAILED;
		}
		GakuFileDialogResult result{};
		auto status = service.poll(service.context, g_fileDialogTicket, sizeof(result), &result, 0, nullptr);
		if (status == GAKU_OK && result.state == GAKU_FILE_DIALOG_PENDING) return GAKU_FILE_DIALOG_PENDING;
		if (status == GAKU_OK && result.state == GAKU_FILE_DIALOG_ACCEPTED)
		{
			// The host supplies UTF-8 including NUL. Bound allocation even for an
			// invalid provider result; the GUI separately refuses truncation.
			if (result.requiredBytes == 0 || result.requiredBytes > 1024 * 1024) status = GAKU_E_SIZE;
			else
			{
				std::string selected(result.requiredBytes, '\0');
				status = service.poll(service.context, g_fileDialogTicket, sizeof(result), &result,
					static_cast<uint32_t>(selected.size()), selected.data());
				if (status == GAKU_OK && result.state == GAKU_FILE_DIALOG_ACCEPTED && selected.back() == '\0')
				{
					selected.resize(std::strlen(selected.c_str()));
					path = std::move(selected);
					notice.clear();
					ReleasePendingFileDialog();
					return GAKU_FILE_DIALOG_ACCEPTED;
				}
				if (status == GAKU_OK) status = GAKU_E_INTERNAL;
			}
		}
		if (status == GAKU_OK && result.state == GAKU_FILE_DIALOG_CANCELLED)
		{
			notice.clear();
			ReleasePendingFileDialog();
			return GAKU_FILE_DIALOG_CANCELLED;
		}
		char detail[160]{};
		std::snprintf(detail, sizeof(detail), "文件选择失败 (status=%d, HRESULT=0x%08X)",
			status, static_cast<unsigned int>(result.osError));
		notice = detail;
		GakuMotion::host::Warn("[GakuMotion][FileDialog] %s", detail);
		ReleasePendingFileDialog();
		return GAKU_FILE_DIALOG_FAILED;
	}

	bool PickMotionFile(int kind, std::string& result)
	{
		wchar_t buffer[32768]{};
		OPENFILENAMEW dialog{};
		dialog.lStructSize = sizeof(dialog);
		// Legacy Localify owns a real desktop window. Avoid accidentally owning
		// the dialog to another process if focus changed before this callback.
		dialog.hwndOwner = FindWindowW(L"Gakumas Localify", L"Gakumas Config GUI");
		if (!dialog.hwndOwner)
		{
			HWND foreground = GetForegroundWindow();
			DWORD process = 0;
			if (foreground) GetWindowThreadProcessId(foreground, &process);
			if (process == GetCurrentProcessId()) dialog.hwndOwner = foreground;
		}
		dialog.lpstrFile = buffer;
		dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
		dialog.lpstrFilter = kind == 0
			                     ? L"VMD motion\0*.vmd\0\0"
			                     : kind == 2
			                     ? L"Camera VMD / GKCAM\0*.vmd;*.gkcamera\0\0"
			                     : kind == 3
			                     ? L"Video file\0*.mp4;*.m4v;*.mov;*.webm;*.avi;*.wmv\0\0"
			                     : L"WAV audio\0*.wav\0\0";
		dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (!GetOpenFileNameW(&dialog))
		{
			const DWORD error = CommDlgExtendedError();
			if (error && g_hostContext.logWarn)
				g_hostContext.logWarn("[GakuMotion][FileDialog] legacy open failed error=0x%08lx owner=%p thread=%lu", error, dialog.hwndOwner, GetCurrentThreadId());
			else if (!error && g_hostContext.logInfo)
				g_hostContext.logInfo("[GakuMotion][FileDialog] legacy open cancelled");
			return false;
		}
		const int n = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
		result.resize(static_cast<size_t>(n));
		WideCharToMultiByte(CP_UTF8, 0, buffer, -1, result.data(), n, nullptr, nullptr);
		result.resize(static_cast<size_t>(n - 1));
		return true;
	}

	void DrainImportProgressLog(GakuMotion::ImportProgressState* progress)
	{
		if (g_importLog.empty()) return;
		HANDLE h = CreateFileW(ToWide(g_importLog, CP_UTF8).c_str(), GENERIC_READ,
		                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return;

		LARGE_INTEGER offset;
		offset.QuadPart = static_cast<LONGLONG>(g_importLogReadOffset);
		if (!SetFilePointerEx(h, offset, nullptr, FILE_BEGIN))
		{
			CloseHandle(h);
			return;
		}

		char buffer[4096];
		DWORD bytesRead = 0;
		while (ReadFile(h, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
		{
			g_importLogReadOffset += bytesRead;
			g_importPendingLine.append(buffer, bytesRead);
		}
		CloseHandle(h);

		size_t pos = 0;
		while ((pos = g_importPendingLine.find('\n')) != std::string::npos)
		{
			std::string line = g_importPendingLine.substr(0, pos);
			g_importPendingLine.erase(0, pos + 1);
			if (!line.empty() && line.back() == '\r') line.pop_back();

			if (line.rfind("@PROGRESS:", 0) == 0)
			{
				const size_t c1 = line.find(':', 10);
				if (c1 != std::string::npos)
				{
					const size_t c2 = line.find(':', c1 + 1);
					if (c2 != std::string::npos)
					{
						const std::string pctStr = line.substr(10, c1 - 10);
						const std::string stage = line.substr(c1 + 1, c2 - (c1 + 1));
						const std::string msg = line.substr(c2 + 1);
						char* endPtr = nullptr;
						const float pct = std::strtof(pctStr.c_str(), &endPtr);
						if (endPtr != pctStr.c_str() && progress)
						{
							progress->active = true;
							progress->progress = std::clamp(pct / 100.0f, 0.0f, 1.0f);
							progress->stage = stage;
							progress->statusText = msg;
						}
					}
				}
			}
		}
	}

	bool StartMotionImport(const GakuMotion::MotionImportRequest& r, std::string& message)
	{
		if (GakuMotion::csharp_import::Busy() || GakuMotion::csharp_import::CameraBusy())
		{
			message = "已有导入任务正在运行";
			return false;
		}
		if (!GakuMotion::csharp_import::CanHandle(r, message)) return false;
		if (!GakuMotion::csharp_import::Begin(r, message)) return false;
		g_importLog = r.log;
		g_importLogReadOffset = 0;
		g_importPendingLine.clear();
		return true;
	}

	int PollMotionImportWithProgress(std::string& message, GakuMotion::ImportProgressState* progress)
	{
		if (!GakuMotion::csharp_import::Busy()) return 0;
		DrainImportProgressLog(progress);
		return GakuMotion::csharp_import::Poll(message, progress);
	}

	int PollMotionImport(std::string& message)
	{
		return PollMotionImportWithProgress(message, nullptr);
	}

	bool StartCameraImport(const GakuMotion::CameraImportRequest& r, std::string& message)
	{
		if (GakuMotion::csharp_import::Busy() || GakuMotion::csharp_import::CameraBusy())
		{
			message = "已有导入任务正在运行";
			return false;
		}
		if (r.source.empty() || r.motionDirectory.empty())
		{
			message = "请选择镜头文件与目标曲目";
			return false;
		}
		if (!std::isfinite(r.fps) || !std::isfinite(r.scale) || r.fps < 30 || r.fps > 240 || r.scale <= 0)
		{
			message = "镜头采样率或缩放无效";
			return false;
		}
		return GakuMotion::csharp_import::BeginCamera(r.source, r.motionDirectory,
			r.fps, r.scale, r.detectCuts, r.log, message);
	}

	int PollCameraImport(std::string& message)
	{
		return GakuMotion::csharp_import::PollCamera(message);
	}
