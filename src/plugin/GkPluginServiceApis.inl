// The loader copies this domain service table. No new loader ABI or DLL export
// is needed, and the resident functions reject mutations after shutdown.
template<class Function>
GakuMotion::control::v0::Error ControlCall(Function&& function) noexcept
{
	try { return function(GakuMotion::Core::Instance()); }
	catch (...) { return GakuMotion::control::v0::Error::Failed; }
}

GakuStatus PublishControlApi()
{
	using namespace GakuMotion::control::v0;
	const Api service{Version, sizeof(Api),
		[](uint32_t size, State* output) { return ControlCall([&](auto& core) { return core.ReadControlState(size, output); }); },
		[](uint32_t size, Capabilities* output) { return ControlCall([&](auto& core) { return core.ReadControlCapabilities(size, output); }); },
		[](uint64_t revision, uint32_t index, uint32_t size, Motion* output) {
			return ControlCall([&](auto& core) { return core.ReadControlMotion(revision, index, size, output); }); },
		[](const Command* command, uint64_t* ticket) {
			if (ticket) *ticket = 0;
			return ControlCall([&](auto& core) { return core.SubmitControl(command, ticket); }); },
		[](uint64_t ticket, uint32_t size, Result* output) { return ControlCall([&](auto& core) { return core.PollControl(ticket, size, output); }); },
		[](uint64_t ticket) { return ControlCall([&](auto& core) { return core.ReleaseControl(ticket); }); }};
	return GakuMotion::host::api.registerService(GakuMotion::host::api.hostContext, ServiceName, sizeof(service), &service);
}

GakuStatus PublishActorApi()
{
	const GakuMotionActorService service{GAKU_MOTION_ACTOR_VERSION, sizeof(service),
		[](uint32_t size, GakuMotionActorSnapshot* output) -> GakuStatus {
			try { return GakuMotion::Core::Instance().ReadSelectedActor(size, output); }
			catch (...) { return GAKU_E_INTERNAL; }
		}};
	return GakuMotion::host::api.registerService(GakuMotion::host::api.hostContext,
		GAKU_MOTION_ACTOR_SERVICE, sizeof(service), &service);
}

template<class Function>
GakuMotion::scene_nodes::v1::Status SceneNodesCall(bool mutation, Function&& function) noexcept
{
	using GakuMotion::scene_nodes::v1::Status;
	if (mutation && (!GakuMotion::host::mainThread.isMainThread ||
		!GakuMotion::host::mainThread.isMainThread(GakuMotion::host::mainThread.context)))
		return Status::WrongThread;
	try { return function(GakuMotion::Core::Instance()); }
	catch (...) { return Status::Failed; }
}

GakuStatus PublishSceneNodesApi()
{
	using namespace GakuMotion::scene_nodes::v1;
	const Api service{Version, sizeof(Api),
		[](uint32_t size, State* output) {
			return SceneNodesCall(false, [&](auto& core) { return core.stageManager.ReadSceneNodesState(size, output); }); },
		[](uint64_t revision, uint32_t index, uint32_t size, Node* output) {
			return SceneNodesCall(false, [&](auto& core) { return core.stageManager.ReadSceneNode(revision, index, size, output); }); },
		[](uint64_t revision, uint64_t* lease) {
			if (lease) *lease = 0;
			return SceneNodesCall(true, [&](auto& core) { return core.AcquireSceneNodes(revision, lease); }); },
		[](uint64_t lease, uint64_t revision, uint32_t count, const Visibility* values) {
			return SceneNodesCall(true, [&](auto& core) { return core.ApplySceneNodes(lease, revision, count, values); }); },
		[](uint64_t lease) {
			return SceneNodesCall(true, [&](auto& core) { return core.stageManager.ReleaseSceneNodes(lease); }); }};
	return GakuMotion::host::api.registerService(GakuMotion::host::api.hostContext,
		ServiceName, sizeof(service), &service);
}

GakuStatus PublishClockApi()
{
	static GakuMotionClockServiceV1 service{};
	service.abiVersion = GAKU_MOTION_CLOCK_VERSION;
	service.structSize = sizeof(service);
	service.context = &GakuMotion::Core::Instance();
	service.getSnapshot = [](void* ctx, GakuMotionClockSnapshotV1* out) -> GakuStatus
	{
		return ctx ? static_cast<GakuMotion::Core*>(ctx)->ReadClockSnapshot(out) : GAKU_E_INVALID_ARGUMENT;
	};
	service.setRunMode = [](void* ctx, uint32_t runMode) -> GakuStatus
	{
		return ctx ? static_cast<GakuMotion::Core*>(ctx)->SetClockRunMode(runMode) : GAKU_E_INVALID_ARGUMENT;
	};
	service.configureClock = [](void* ctx, uint32_t clockSource, uint32_t fpsNum, uint32_t fpsDen) -> GakuStatus
	{
		return ctx ? static_cast<GakuMotion::Core*>(ctx)->ConfigureClock(clockSource, fpsNum, fpsDen) : GAKU_E_INVALID_ARGUMENT;
	};
	service.acquireRecordingLease = [](void* ctx, const char* ownerName, uint32_t fpsNum, uint32_t fpsDen, uint64_t* outLeaseId) -> GakuStatus
	{
		return ctx ? static_cast<GakuMotion::Core*>(ctx)->AcquireRecordingLease(ownerName, fpsNum, fpsDen, outLeaseId) : GAKU_E_INVALID_ARGUMENT;
	};
	service.releaseRecordingLease = [](void* ctx, uint64_t leaseId) -> GakuStatus
	{
		return ctx ? static_cast<GakuMotion::Core*>(ctx)->ReleaseRecordingLease(leaseId) : GAKU_E_INVALID_ARGUMENT;
	};
	service.recordingBegin = [](void* ctx, uint64_t leaseId, double startSeconds) -> GakuStatus
	{
		return ctx ? static_cast<GakuMotion::Core*>(ctx)->RecordingBegin(leaseId, startSeconds) : GAKU_E_INVALID_ARGUMENT;
	};
	service.recordingStop = [](void* ctx, uint64_t leaseId) -> GakuStatus
	{
		return ctx ? static_cast<GakuMotion::Core*>(ctx)->RecordingStop(leaseId) : GAKU_E_INVALID_ARGUMENT;
	};
	service.notifyFrameRendered = [](void* ctx, uint64_t leaseId, uint64_t evaluatedFrameSerial, uint32_t advanceFrames) -> GakuStatus
	{
		return ctx ? static_cast<GakuMotion::Core*>(ctx)->NotifyFrameRendered(leaseId, evaluatedFrameSerial, advanceFrames) : GAKU_E_INVALID_ARGUMENT;
	};
	service.beginOffscreenCapture = [](void* ctx) -> GakuStatus
	{
		return ctx ? static_cast<GakuMotion::Core*>(ctx)->BeginOffscreenCapture() : GAKU_E_INVALID_ARGUMENT;
	};
	service.endOffscreenCapture = [](void* ctx) -> GakuStatus
	{
		return ctx ? static_cast<GakuMotion::Core*>(ctx)->EndOffscreenCapture() : GAKU_E_INVALID_ARGUMENT;
	};
	return GakuMotion::host::api.registerService(GakuMotion::host::api.hostContext,
		GAKU_SERVICE_MOTION_CLOCK, sizeof(service), &service);
}

GakuStatus PublishStageStateApi()
{
	static GakuMotionStageStateServiceV1 service{};
	service.abiVersion = GAKU_MOTION_STAGE_STATE_VERSION;
	service.structSize = sizeof(service);
	service.context = &GakuMotion::Core::Instance();
	service.getState = [](void* ctx, GakuMotionStageStateV1* out) -> GakuStatus
	{
		return ctx ? static_cast<GakuMotion::Core*>(ctx)->ReadStageState(out) : GAKU_E_INVALID_ARGUMENT;
	};
	return GakuMotion::host::api.registerService(GakuMotion::host::api.hostContext,
		GAKU_SERVICE_MOTION_STAGE_STATE, sizeof(service), &service);
}

// Native activation: connect the loader services, publish the playback contract,
// fill the host adapter and initialize config/audio/import. Runs before Unity
// exists, so it must not call managed code.
GakuStatus MotionActivate(const GakuHostApi* host, void** pluginState)
{
	if (!pluginState) return GAKU_E_INVALID_ARGUMENT;
	if (g_pluginInitialized) return GAKU_E_CONFLICT;
	const auto status = GakuMotion::host::Connect(host);
	if (status != GAKU_OK) return status;
	g_audioPlugin.BindHost(&GakuMotion::host::api);
	const auto published = GakuMotion::host::PublishPlayback();
	if (published != GAKU_OK) return published;
	const auto controlPublished = PublishControlApi();
	if (controlPublished != GAKU_OK) return controlPublished;
	const auto sceneNodesPublished = PublishSceneNodesApi();
	if (sceneNodesPublished != GAKU_OK) return sceneNodesPublished;
	const auto actorPublished = PublishActorApi();
	if (actorPublished != GAKU_OK)
		MotionLogWarn("[GakuMotion] Selected actor service unavailable (%d).", actorPublished);
	const auto clockPublished = PublishClockApi();
	if (clockPublished != GAKU_OK)
		MotionLogWarn("[GakuMotion] Clock service unavailable (%d).", clockPublished);
	const auto stagePublished = PublishStageStateApi();
	if (stagePublished != GAKU_OK)
		MotionLogWarn("[GakuMotion] Stage state service unavailable (%d).", stagePublished);

	g_hostContext = MotionHostContext{};
	g_hostContext.apiVersion = GAKU_ABI_VERSION;
	g_hostContext.gameRoot = GakuMotion::host::runtime.gameRoot;
	g_hostContext.localRoot = GakuMotion::host::runtime.localRoot;
	g_hostContext.pluginsRoot = GakuMotion::host::runtime.pluginsRoot;
	g_hostContext.gameAssemblyHandle = GakuMotion::host::runtime.getGameAssembly();
	g_hostContext.logInfo = MotionLogInfo;
	g_hostContext.logWarn = MotionLogWarn;
	g_hostContext.logError = MotionLogError;
	// The loader ABI exposes no VR query; those probes stay null and the gaze
	// feature reports itself unavailable.
	g_hostContext.isVRActive = nullptr;
	g_hostContext.isVRifyLiveGazeActive = nullptr;
	g_hostContext.setMotionPlaying = MotionSetMotionPlaying;
	g_hostContext.isMotionPlaying = MotionIsMotionPlaying;

	if (!InitializeMotion()) return GAKU_E_INTERNAL;
	GakuMotion::Core::Instance().SetControlActive(true);
	*pluginState = &GakuMotion::Core::Instance();

	// Optional provider-owned web panel. A registration miss never fails
	// activation: the Loader keeps the ImGui fallback (RenderGmlMotion).
	// Each activation builds a fresh bridge so the frontend sees a new sid;
	// the retired instance stays resident because copied service tables may
	// still reference its context until the Loader re-queries.
	if (g_webPanelBridge) g_webPanelBridge->Invalidate("plugin-reactivation");
	g_webPanelBridge = new GakuMotion::webpanel::Bridge(GakuMotion::Core::Instance(), MakeWebPanelHooks());
	const auto webPanelPublished = PublishWebPanelApi();
	if (webPanelPublished != GAKU_OK)
		MotionLogWarn("[GakuMotion] web-panel service unavailable (%d); ImGui fallback remains", webPanelPublished);
	else
		GakuMotion::host::Info("[GakuMotion] gakumotion.web-panel/v1 published (WebView2 manager panel)");

	const auto hotkey = GakuMotion::host::RegisterHotkey(kShowGuiActionId, "Show Motion controls",
		'M', GAKU_HOTKEY_MOD_CTRL, ShowGuiHotkey);
	if (hotkey == GAKU_OK)
		GakuMotion::host::Info("[GakuMotion] Hotkey Ctrl+M registered for the motion controls panel");
	else
		GakuMotion::host::Warn(
			"[GakuMotion] hotkey service unavailable (status=%d); Ctrl+M disabled", hotkey);
	// Plain F11 belongs to Localify (toggle-fullscreen). Registering it here too
	// made the loader refuse BOTH bindings, so the user lost fullscreen and the
	// reload. Shift+F11 keeps the pair distinct without touching Localify.
	const auto reloadKey = GakuMotion::host::RegisterHotkey(kReloadOverridesActionId,
		"Reload bone overrides", VK_F11, GAKU_HOTKEY_MOD_SHIFT, ReloadOverridesHotkey);
	const auto toggleKey = GakuMotion::host::RegisterHotkey(kToggleOverridesActionId,
		"Toggle bone overrides", VK_F11, GAKU_HOTKEY_MOD_CTRL, ToggleOverridesHotkey);
	if (reloadKey != GAKU_OK || toggleKey != GAKU_OK)
		GakuMotion::host::Warn("[GakuMotion] hotkey registrations: Shift+F11 reload=%d Ctrl+F11 toggle=%d",
			reloadKey, toggleKey);
	GakuMotion::host::Info("[GakuMotion][GakuLoader] ABI v1 services connected; camera hooks leased from the loader; KineTexture consumer active");
	return GAKU_OK;
}

// Unity main thread, at most once per frame. Frame-scoped work that needs no
// actor (deferred queue drains, timers) would go here; actor work belongs to
// MotionActorTick.
void MotionTick(void*, float)
{
	// Preferences and file jobs do not need a dancer. The import UI is already
	// usable in Photography preparation, before any actor callback can arrive.
	// Playback and scene commands retain their actor timing and queue order.
	{
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		core.ProcessPendingStageLoadCommands();
		core.ProcessPendingLibraryCommands();
	}
	if (g_runtimeReady.load(std::memory_order_acquire))
	{
		ResolveDeferredManagedTypes();
		StageLoader::ResolveDeferredTypeObjects();
		RefreshProtectedContext();
		auto& core = GakuMotion::Core::Instance();
		std::lock_guard<std::recursive_mutex> lock(core.stateMutex);
		TickPhotographyEnsemble(core, g_faceUnityFrame ? g_faceUnityFrame() : -1);
		RefreshSceneActors(core);
		ReconcileEnsemblePhysicsSlots(core);
		TickStudio(core);
		core.RejectObsoleteControlCommands();
	}
}

void RenderGmlMotion(void*)
{
#if defined(GKMOTION_ENABLE_IMGUI) || defined(HAS_IMGUI)
	if (!g_runtimeReady.load(std::memory_order_acquire) || !GakuMotion::host::desktop.bind) return;
	const GakuImGuiAbi abi{IMGUI_VERSION_NUM, sizeof(ImGuiIO), sizeof(ImGuiStyle),
		sizeof(ImDrawVert), sizeof(ImDrawIdx), sizeof(ImWchar)};
	GakuDesktopUiBinding binding{};
	const auto status = GakuMotion::host::desktop.bind(&abi, sizeof(binding), &binding);
	if (status != GAKU_OK || !binding.imguiContext || !binding.alloc || !binding.free) return;
	// ImGui allocations may persist in plugin UI objects between callbacks. Keep
	// the negotiated host allocator for their entire lifetime, including teardown.
	ImGui::SetAllocatorFunctions(binding.alloc, binding.free, binding.allocatorUserData);
	auto* previous = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(static_cast<ImGuiContext*>(binding.imguiContext));
	GakuMotion::Core::Instance().OnRenderGui(
		GakuMotion::host::desktopWindow.presentationKind == GAKU_DESKTOP_PRESENTATION_PLUGIN_PANEL);
	ImGui::SetCurrentContext(previous);
	static bool logged = false;
	if (!logged) {
		logged = true;
		GakuMotion::host::Info("[GakuMotion][GakuLoader] desktop UI callback rendered with negotiated ImGui context/allocator");
	}
#endif
}
