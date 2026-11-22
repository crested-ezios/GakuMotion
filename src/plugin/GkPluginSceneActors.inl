// =============================================================================
// 热键采样辅助
// =============================================================================
// 实机故障：暂停一段时间后无人操作，日志却出现
//   [Config] switch prev -> like JENNIE
//   [Config] switch next -> Fighting My Way
// 即`.` / `,` 在「无人按键」时触发。根因是旧写法`GetAsyncKeyState(vk) & 1`：
//低位 bit 的语义是「自上次调用以来该键是否被按下过」，它有两个致命性质：
//   (a) 粘滞——按键状态是在你读它的时候才被清掉的。掉帧/切场景/窗口失焦期间
//       本函数可能几十秒不被调用，用户在别的程序里敲的键全部堆积在bit 里，
//       帧循环恢复后的第一次读取就会一次性「补发」历史按键；
//   (b) 不区分前台窗口——GetAsyncKeyState 报的是物理键态，与「发给谁」无关。
// 此外长按的系统自动重复会被当成一串新按下，导致按住不放连环切歌。
//
// 修法（见 GkHotkey.hpp）：只用 0x8000 高位（真实物理按下态）自己做上升沿，
// 焦点不在本进程时一律不触发，且焦点切换瞬间清空沿基线，使失焦期间的按键
// 永不补发。下面两个函数把 Win32 状态翻译成 HotkeyGate 的输入。
static bool IsGameForeground()
{
	HWND fg = GetForegroundWindow();
	if (!fg) return false;
	DWORD fgPid = 0;
	GetWindowThreadProcessId(fg, &fgPid);
	return fgPid == GetCurrentProcessId();
}

static GakuMotion::HotkeyGate g_hotkeys;

// Materialises the deferred managed System.Type handles. Runtime-ready may run
// on a loader worker thread, where creating managed objects is not allowed, so
// every il2cpp_type_get_object call waits for the Unity main thread.
static void ResolveDeferredManagedTypes()
{
	if (!il2cpp_class_get_type || !il2cpp_type_get_object) return;
	if (g_pendingAnimatorTypeObj.load(std::memory_order_acquire) && g_animatorKlass)
	{
		if (void* typePtr = il2cpp_class_get_type(g_animatorKlass))
		{
			g_animatorTypeObj = il2cpp_type_get_object(typePtr);
			g_pendingAnimatorTypeObj.store(false, std::memory_order_release);
		}
	}
	if (g_pendingTransformTypeObj.load(std::memory_order_acquire) && g_transformKlass)
	{
		if (void* typePtr = il2cpp_class_get_type(g_transformKlass))
		{
			g_transformTypeObj = il2cpp_type_get_object(typePtr);
			g_pendingTransformTypeObj.store(false, std::memory_order_release);
		}
	}
	if (g_pendingFaceModelTypeObj.load(std::memory_order_acquire) && g_faceModelClass)
	{
		if (void* typePtr = il2cpp_class_get_type(g_faceModelClass))
		{
			g_faceModelTypeObj = il2cpp_type_get_object(typePtr);
			g_pendingFaceModelTypeObj.store(false, std::memory_order_release);
		}
	}
}

static bool EnsureRuntimeReadyForActor(void* activeActor, void (*initialize)(void*))
{
	if (g_runtimeReady.load(std::memory_order_acquire)) return true;
	if (!activeActor || !initialize) return false;
	// The loader may not have delivered runtime-ready yet; the first actor
	// callback is a safe Unity-thread retry and is a no-op once it has run.
	initialize(nullptr);
	return g_runtimeReady.load(std::memory_order_acquire);
}

// Reuse the exact typed Unity finder already validated for the scene bridge.
// Search only an admitted owner scene, at most four times per second unless a
// scene boundary or a target-selection command requires a fresh admission check.
// No callbacks, managed pointers or speculative metadata are exposed to ImGui.
static int g_lastActorCallbackFrame = INT_MIN;

static bool RefreshSceneActors(GakuMotion::Core& core, bool force = false, void* callbackActor = nullptr)
{
	static auto lastScan = std::chrono::steady_clock::time_point{};
	static int lastScanFrame = INT_MIN;
	const auto now = std::chrono::steady_clock::now();
	const int unityFrame = g_faceUnityFrame ? g_faceUnityFrame() : -1;
	if (!core.InjectionAllowed() || !g_sceneOwner.Valid() || g_currentSceneHandle != g_sceneOwner.sceneHandle)
		return false;
	if (callbackActor && !core.sceneActorRegistry.Find(callbackActor))
		force = true;
	if (!force && unityFrame >= 0 && unityFrame == lastScanFrame)
		return true;
	if (!force && !core.HasPendingTargetSelection() && !core.sceneActorRegistry.Entries().empty())
	{
		// Avoid periodic 250 ms FindObjectsOfType full-scene scans and managed
		// heap allocations while playback is active and actor ticks arrive cleanly.
		if (core.player.state == GakuMotion::PlaybackState::Playing &&
			unityFrame >= 0 && g_lastActorCallbackFrame >= 0 &&
			unityFrame >= g_lastActorCallbackFrame &&
			(unityFrame - g_lastActorCallbackFrame) <= 2)
		{
			return true;
		}
		if (now - lastScan < std::chrono::milliseconds(250))
			return true;
	}
	lastScan = now;
	lastScanFrame = unityFrame;
	auto unavailable = [&]
	{
		// A failed fresh check must not admit a command through yesterday's token.
		if (core.PublishSceneActors({}))
		{
			ClearSecondaryPhysics(true);
			HelperClearActorAnimatorCache();
			NativeAudio::Release(GakuMotion::bgm::ReleaseReason::SceneLost);
		}
		return false;
	};
	if (!StageLoader::findAll || !StageLoader::componentGo || !StageLoader::goScene || !StageLoader::instanceId ||
		!StageLoader::arrayLength || !StageLoader::newHandle || !StageLoader::freeHandle ||
		(!StageLoader::actorTypes[0] && !StageLoader::actorTypes[1])) return unavailable();
	std::vector<GakuMotion::SceneActorObservation> observations;
	for (void* actorType : StageLoader::actorTypes)
	{
		if (!actorType) continue;
		bool includeInactive = false;
		void* args[]{actorType, &includeInactive};
		void* array = StageLoader::Invoke(StageLoader::findAll, nullptr, args);
		if (!array) return unavailable();
		StageLoader::Handle pin = StageLoader::newHandle(array, false);
		if (!pin) return unavailable();
		struct Release { StageLoader::Handle value; ~Release() { StageLoader::freeHandle(value); } } release{pin};
		const uintptr_t count = StageLoader::arrayLength(array);
		if (count > 4096) return unavailable();
		void** items = reinterpret_cast<void**>(static_cast<char*>(array) + 32);
		for (uintptr_t i = 0; i < count; ++i)
		{
			void* actor = items[i];
			if (!actor || std::any_of(observations.begin(), observations.end(), [&](const auto& item) { return item.actor == actor; })) continue;
			void* go = StageLoader::Invoke(StageLoader::componentGo, actor);
			if (!go || StageLoader::SceneOfFreshGo(go) != g_sceneOwner.sceneHandle) continue;
			const int id = StageLoader::ReadInt(StageLoader::instanceId, actor);
			if (!id) continue;
			// Prefer the loaded face identity, including a replacement's actual face.
			void* face = HelperGetFaceModel(actor);
			std::string faceName = face ? HelperGetFaceModelName(face) : "";
			const auto identity = GakuMotion::facial_fit::ResolveIdentity(faceName);
			std::string character = identity.character;
			if (character.empty()) character = GakuMotion::DetectIdolCharacterCode(faceName);
			const auto actorName = HelperGetActorName(actor);
			const auto presetName = GakuMotion::GakuActorsPresetName(actorName);
			if (character.empty()) character = GakuMotion::DetectIdolCharacterCode(
				presetName.empty() ? actorName : actorName.substr(0, actorName.find(" | GakuActors[")));
			if (!character.empty()) g_portraitCache.EnqueueIfUncached(character);
			observations.push_back({actor, id, std::move(character), identity.displayName,
				GakuMotion::facial_fit::StripCloneSuffix(std::move(faceName)), presetName});
		}
	}
	const bool hadActors = !core.sceneActorRegistry.Entries().empty();
	const auto oldEpoch = core.sceneActorRegistry.Epoch();
	if (core.PublishSceneActors(observations))
	{
		ClearSecondaryPhysics(true);
		HelperClearActorAnimatorCache();
		NativeAudio::Release(GakuMotion::bgm::ReleaseReason::SceneLost);
		core.PickBoundActorAsTarget(); // Old physics has been abandoned; bind only a fresh actor.
	}
	if (hadActors && oldEpoch != core.sceneActorRegistry.Epoch())
	{
		void* current = observations.empty() ? nullptr : observations.front().actor;
		if (current) StageLoader::BindContext(g_sceneOwner.sceneHandle, current);
		if (!core.stageManager.OnActorSetChanged(current))
			ProtectedCaptureFault("stage restoration failed during actor replacement");
	}
	return true;
}

static void TickPhotographyEnsemble(GakuMotion::Core& core, int nativeFrame, void* callbackActor = nullptr)
{
	const bool probeEnabled = core.InjectionAllowed();
	PhotographyEnsembleProbe::SetEnabled(probeEnabled);
	if (nativeFrame > 0 && probeEnabled) PhotographyEnsembleProbe::Tick(static_cast<uint64_t>(nativeFrame), callbackActor);
	else if (!probeEnabled) PhotographyEnsembleProbe::Tick(0);
	const auto& observed = PhotographyEnsembleProbe::CurrentFrame();
	core.formation.available = nativeFrame > 0 && PhotographyEnsembleProbe::Eligible() &&
		PhotographyEnsembleProbe::resolved && observed.available && observed.number == static_cast<uint64_t>(nativeFrame);
	core.formation.verified = core.formation.available && PhotographyEnsembleProbe::ReadyForInjection();
	if (core.formation.active && !probeEnabled && HasEnsemblePhysics())
	{
		ClearSecondaryPhysics(true);
	}
}
