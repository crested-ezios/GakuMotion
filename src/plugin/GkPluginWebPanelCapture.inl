	namespace
	{
		struct GkScreenshotCapabilitiesV1
		{
			uint32_t abiVersion;
			uint32_t structSize;
			uint32_t runtimeReady;
			uint32_t cameraAvailable;
			uint32_t greenScreenDetected;
			uint32_t maxDimension;
			uint32_t windowWidth;
			uint32_t windowHeight;
		};
		struct GkScreenshotFrameRequestV1
		{
			uint32_t abiVersion;
			uint32_t structSize;
			uint32_t width;
			uint32_t height;
			uint32_t pixelFormat;
			uint32_t alphaMode;
		};
		struct GkScreenshotFrameInfoV1
		{
			uint32_t abiVersion;
			uint32_t structSize;
			uint32_t width;
			uint32_t height;
			uint32_t rowPitch;
			uint32_t hasAlpha;
			uint64_t unityFrame;
		};
		struct GkScreenshotServiceV1
		{
			uint32_t abiVersion;
			uint32_t structSize;
			void* context;
			GakuStatus (*getCapabilities)(void* context, GkScreenshotCapabilitiesV1* output);
			GakuStatus (*requestFileCapture)(void* context, const void* request, uint64_t* outTicket);
			GakuStatus (*pollCapture)(void* context, uint64_t ticket, void* output);
			GakuStatus (*releaseTicket)(void* context, uint64_t ticket);
			GakuStatus (*captureFrameImmediate)(void* context, const GkScreenshotFrameRequestV1* request,
				void* outPixels, size_t capacity, GkScreenshotFrameInfoV1* outInfo,
				char* errorBuffer, uint32_t errorCapacity);
			GakuStatus (*armPresentCapture)(void* context, uint64_t token, uint32_t width, uint32_t height,
				uint32_t pixelFormat);
			GakuStatus (*consumePresentCapture)(void* context, uint64_t* outToken, void* outPixels, size_t capacity,
				GkScreenshotFrameInfoV1* outInfo, char* errorBuffer, uint32_t errorCapacity);
			GakuStatus (*cancelPresentCapture)(void* context);
		};

		std::mutex g_screenshotSvcMutex;
		GkScreenshotServiceV1 g_screenshotSvc{};
		bool g_screenshotSvcResolved{false};

		bool ResolveScreenshotService(GkScreenshotServiceV1& out)
		{
			std::lock_guard<std::mutex> lock(g_screenshotSvcMutex);
			if (!g_screenshotSvcResolved)
			{
				if (!GakuMotion::host::active || !GakuMotion::host::api.queryService) return false;
				GkScreenshotServiceV1 candidate{};
				if (GakuMotion::host::Query("screenshotmanager.capture/v1", candidate) != GAKU_OK) return false;
				if (!candidate.getCapabilities || !candidate.captureFrameImmediate) return false;
				g_screenshotSvc = candidate;
				g_screenshotSvcResolved = true;
			}
			out = g_screenshotSvc;
			return true;
		}
	} // namespace

	GakuMotion::webpanel::ScreenshotCapabilitiesView WebPanelGetScreenshotCapabilities()
	{
		GakuMotion::webpanel::ScreenshotCapabilitiesView view;
		GkScreenshotServiceV1 svc{};
		if (!ResolveScreenshotService(svc))
		{
			view.available = false;
			view.notice = "未检测到 ScreenshotManager 联动服务";
			return view;
		}
		GkScreenshotCapabilitiesV1 caps{};
		caps.abiVersion = 1u;
		caps.structSize = sizeof(caps);
		if (svc.getCapabilities(svc.context, &caps) != GAKU_OK)
		{
			view.available = false;
			view.notice = "无法读取 ScreenshotManager 状态";
			return view;
		}
		view.available = true;
		view.runtimeReady = caps.runtimeReady != 0;
		view.cameraAvailable = caps.cameraAvailable != 0;
		view.windowWidth = caps.windowWidth;
		view.windowHeight = caps.windowHeight;
		if (!view.runtimeReady) view.notice = "ScreenshotManager 运行时尚未就绪";
		else if (!view.cameraAvailable) view.notice = "当前场景暂无可用摄影机";
		return view;
	}

	GakuMotion::webpanel::ScreenshotCaptureView WebPanelCaptureScreenshot()
	{
		GakuMotion::webpanel::ScreenshotCaptureView out;
		if (!GakuMotion::host::active || !GakuMotion::host::mainThread.enqueue)
		{
			out.error = "宿主主线程调度服务不可用";
			return out;
		}

		struct CaptureJob
		{
			std::mutex mtx;
			std::condition_variable cv;
			bool done{false};
			bool ok{false};
			uint32_t width{0};
			uint32_t height{0};
			uint32_t rowPitch{0};
			std::vector<uint8_t> pixels;
			std::string error;
		};

		auto executeOnMainThread = [](CaptureJob& job) {
			GkScreenshotServiceV1 svc{};
			if (!ResolveScreenshotService(svc))
			{
				std::lock_guard<std::mutex> lock(job.mtx);
				job.error = "未检测到 ScreenshotManager 联动服务";
				job.done = true;
				job.cv.notify_one();
				return;
			}
			GkScreenshotCapabilitiesV1 caps{};
			caps.abiVersion = 1u;
			caps.structSize = sizeof(caps);
			if (svc.getCapabilities(svc.context, &caps) != GAKU_OK)
			{
				std::lock_guard<std::mutex> lock(job.mtx);
				job.error = "无法查询 ScreenshotManager 能力";
				job.done = true;
				job.cv.notify_one();
				return;
			}
			if (!caps.runtimeReady)
			{
				std::lock_guard<std::mutex> lock(job.mtx);
				job.error = "ScreenshotManager 运行时尚未就绪";
				job.done = true;
				job.cv.notify_one();
				return;
			}
			if (!caps.cameraAvailable)
			{
				std::lock_guard<std::mutex> lock(job.mtx);
				job.error = "当前场景暂无可用摄影机，无法拍照";
				job.done = true;
				job.cv.notify_one();
				return;
			}

			uint32_t reqW = caps.windowWidth;
			uint32_t reqH = caps.windowHeight;
			constexpr uint32_t kMaxCaptureDim = 1024u;
			if (reqW > 0 && reqH > 0 && (reqW > kMaxCaptureDim || reqH > kMaxCaptureDim))
			{
				if (reqW >= reqH)
				{
					reqH = (std::max)(1u, static_cast<uint32_t>(static_cast<uint64_t>(reqH) * kMaxCaptureDim / reqW));
					reqW = kMaxCaptureDim;
				}
				else
				{
					reqW = (std::max)(1u, static_cast<uint32_t>(static_cast<uint64_t>(reqW) * kMaxCaptureDim / reqH));
					reqH = kMaxCaptureDim;
				}
			}

			GkScreenshotFrameRequestV1 req{};
			req.abiVersion = 1u;
			req.structSize = sizeof(req);
			req.width = reqW;
			req.height = reqH;
			req.pixelFormat = 1u; // GAKU_PIXEL_BGRA8_SRGB_TOPDOWN
			req.alphaMode = 2u;   // GAKU_SCREENSHOT_ALPHA_OPAQUE

			const size_t initialBytes = (reqW > 0 && reqH > 0)
				? static_cast<size_t>(reqW) * reqH * 4u
				: static_cast<size_t>(1920u) * 1080u * 4u;
			std::vector<uint8_t> pixels(initialBytes);
			GkScreenshotFrameInfoV1 info{};
			info.abiVersion = 1u;
			info.structSize = sizeof(info);
			char errBuf[256]{};
			GakuStatus st = svc.captureFrameImmediate(
				svc.context, &req, pixels.data(), pixels.size(), &info, errBuf, static_cast<uint32_t>(sizeof(errBuf)));
			if (st == GAKU_E_SIZE && info.width > 0 && info.height > 0 && info.rowPitch >= info.width * 4u)
			{
				const size_t needed = static_cast<size_t>(info.rowPitch) * info.height;
				if (needed <= 128ULL * 1024ULL * 1024ULL)
				{
					pixels.resize(needed);
					std::memset(errBuf, 0, sizeof(errBuf));
					st = svc.captureFrameImmediate(
						svc.context, &req, pixels.data(), pixels.size(), &info, errBuf, static_cast<uint32_t>(sizeof(errBuf)));
				}
			}

			std::lock_guard<std::mutex> lock(job.mtx);
			if (st == GAKU_OK && info.width > 0 && info.height > 0)
			{
				job.ok = true;
				job.width = info.width;
				job.height = info.height;
				job.rowPitch = info.rowPitch;
				job.pixels = std::move(pixels);
			}
			else
			{
				job.ok = false;
				job.error = errBuf[0] != '\0' ? std::string(errBuf) : "ScreenshotManager 即时抓帧失败";
			}
			job.done = true;
			job.cv.notify_one();
		};

		auto job = std::make_shared<CaptureJob>();
		if (GakuMotion::host::mainThread.isMainThread &&
			GakuMotion::host::mainThread.isMainThread(GakuMotion::host::mainThread.context))
		{
			executeOnMainThread(*job);
		}
		else
		{
			struct EnqueuePayload
			{
				std::shared_ptr<CaptureJob> job;
				void (*fn)(CaptureJob&);
			};
			auto* payload = new EnqueuePayload{job, executeOnMainThread};
			const GakuStatus enq = GakuMotion::host::mainThread.enqueue(
				GakuMotion::host::mainThread.context,
				[](void* ud) {
					auto* p = static_cast<EnqueuePayload*>(ud);
					if (p)
					{
						p->fn(*p->job);
						delete p;
					}
				},
				payload);
			if (enq != GAKU_OK)
			{
				delete payload;
				out.error = "无法向 Unity 主线程提交拍照任务";
				return out;
			}
			std::unique_lock<std::mutex> lock(job->mtx);
			if (!job->cv.wait_for(lock, std::chrono::milliseconds(3000), [&]() { return job->done; }))
			{
				out.error = "等待 Unity 主线程拍照超时";
				return out;
			}
		}

		if (!job->ok)
		{
			out.error = job->error.empty() ? "拍照失败" : job->error;
			return out;
		}
		out.dataUri = GakuMotion::cover::EncodeBgraTopDownToDataUri(
			job->pixels.data(), job->width, job->height, job->rowPitch, 1024);
		if (out.dataUri.empty())
		{
			out.error = "拍照图像编码失败";
			return out;
		}
		out.ok = true;
		out.width = job->width;
		out.height = job->height;
		return out;
	}

	GakuMotion::webpanel::AudioEffectState WebPanelGetAudioEffectsState()
	{
		GakuMotion::webpanel::AudioEffectState st;
		st.available = g_audioPlugin.Available();
		st.activeId = g_audioPlugin.ActivePresetId();
		for (const auto& p : g_audioPlugin.Presets())
		{
			st.presets.push_back({ p.id, p.name, p.category, p.description });
		}
		return st;
	}

	bool WebPanelSetAudioEffectPreset(const std::string& presetId, std::string& error)
	{
		return g_audioPlugin.SetActivePreset(presetId, &error);
	}

	GakuMotion::webpanel::HostHooks MakeWebPanelHooks()
	{
		GakuMotion::webpanel::HostHooks hooks;
		hooks.assetsDirs = WebPanelAssetsDirs;
		hooks.openLibraryFolder = WebPanelOpenLibraryFolder;
		hooks.beginWebFileDialog = WebPanelBeginFileDialog;
		hooks.releaseFileDialog = ReleasePendingFileDialog;
		hooks.dropSetTarget = WebPanelDropSetTarget;
		hooks.dropPeek = WebPanelDropPeek;
		hooks.dropConsume = WebPanelDropConsume;
		hooks.appearance = WebPanelAppearance;
		hooks.probeAudio = WebPanelProbeAudio;
		hooks.getTrackCoverDataUri = WebPanelGetTrackCoverDataUri;
		hooks.queryTrackCoverAsync = [](const std::string& key, const std::string& trackDirUtf8,
										std::string& outDataUri, bool& outPending) -> bool {
			return g_trackCoverCache.QueryOrEnqueue(key, trackDirUtf8, outDataUri, outPending);
		};
		hooks.drainReadyCovers = [](size_t maxCount) -> std::vector<GakuMotion::webpanel::AsyncTrackCoverResult> {
			const auto ready = g_trackCoverCache.DrainCompleted(maxCount);
			std::vector<GakuMotion::webpanel::AsyncTrackCoverResult> out;
			out.reserve(ready.size());
			for (const auto& item : ready)
			{
				GakuMotion::webpanel::AsyncTrackCoverResult entry;
				entry.key = item.key;
				entry.hasCover = item.hasCover;
				entry.coverDataUri = item.coverDataUri;
				out.push_back(std::move(entry));
			}
			return out;
		};
		hooks.invalidateTrackCover = [](const std::string& key) {
			if (key.empty()) g_trackCoverCache.InvalidateAll();
			else g_trackCoverCache.Invalidate(key);
		};
		hooks.readImagePreviewDataUri = WebPanelReadImagePreviewDataUri;
		hooks.getActorPortraitDataUri = [](const std::string& normCharCode, std::string& outDataUri, bool& outCached) -> bool {
			return g_portraitCache.QueryOrEnqueue(normCharCode, outDataUri, outCached);
		};
		hooks.getScreenshotCapabilities = WebPanelGetScreenshotCapabilities;
		hooks.captureScreenshot = WebPanelCaptureScreenshot;
		hooks.getAudioEffectsState = WebPanelGetAudioEffectsState;
		hooks.setAudioEffectPreset = WebPanelSetAudioEffectPreset;
		hooks.pluginVersion = "0.1.1";
		return hooks;
	}

	GakuStatus WebPanelGetAssets(void* context, GakuWebPanelAssetsV1* output)
	{
		auto* bridge = static_cast<GakuMotion::webpanel::Bridge*>(context);
		if (!bridge) return GAKU_E_UNAVAILABLE;
		try { return bridge->GetAssets(output); }
		catch (...) { return GAKU_E_INTERNAL; }
	}
	void WebPanelOnShow(void* context)
	{
		if (auto* bridge = static_cast<GakuMotion::webpanel::Bridge*>(context)) bridge->OnShow();
	}
	void WebPanelOnHide(void* context)
	{
		if (auto* bridge = static_cast<GakuMotion::webpanel::Bridge*>(context)) bridge->OnHide();
	}
	void WebPanelOnUiTick(void* context)
	{
		if (auto* bridge = static_cast<GakuMotion::webpanel::Bridge*>(context)) bridge->OnUiTick();
	}
	void WebPanelSetEventSink(void* context, GakuWebPanelEventSink sink, void* userData)
	{
		if (auto* bridge = static_cast<GakuMotion::webpanel::Bridge*>(context)) bridge->SetEventSink(sink, userData);
	}
	GakuStatus WebPanelHandleMessage(void* context, const char* requestJson,
		uint32_t responseCapacity, char* responseJson, uint32_t* requiredBytes)
	{
		auto* bridge = static_cast<GakuMotion::webpanel::Bridge*>(context);
		if (!bridge) return GAKU_E_UNAVAILABLE;
		return bridge->HandleMessage(requestJson, responseCapacity, responseJson, requiredBytes);
	}
	void WebPanelOnShutdown(void* context)
	{
		if (auto* bridge = static_cast<GakuMotion::webpanel::Bridge*>(context)) bridge->OnShutdown();
	}

	GakuStatus PublishWebPanelApi()
	{
		if (!g_webPanelBridge) return GAKU_E_UNAVAILABLE;
		const GakuWebPanelServiceV1 service{GAKU_WEB_PANEL_VERSION, sizeof(service), g_webPanelBridge,
			WebPanelGetAssets, WebPanelOnShow, WebPanelOnHide, WebPanelOnUiTick,
			WebPanelSetEventSink, WebPanelHandleMessage, WebPanelOnShutdown};
		return GakuMotion::host::api.registerService(GakuMotion::host::api.hostContext,
			"gakumotion" GAKU_WEB_PANEL_SERVICE_SUFFIX, sizeof(service), &service);
	}
