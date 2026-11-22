// Synthetic headless contract tests for gakumotion.web-panel/v1.
// No game, no real user assets: a local Core plus fake host hooks drive the
// bridge exactly like the Loader's UI thread would. Wire shapes mirror
// frontend/src/motion/protocol.ts (v1).
//
// Build/run: tests/run_web_panel_tests.ps1 (MSVC, /std:c++20 /utf-8).

#include "GkWebPanelBridge.hpp"
#include "GkAudioPluginClient.hpp"
#include "GkCoverCache.hpp"

#include <cstdio>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <vector>

using namespace GakuMotion;
using namespace GakuMotion::webpanel;

// The offline binary links this TU alone; provide the log facade symbol.
namespace GakuMotion::Log
{
	void Write(Level, const char*, ...) {}
}

static int g_pass = 0;
static int g_fail = 0;
static std::string g_current;

static void Check(bool condition, const std::string& what)
{
	if (condition) ++g_pass;
	else
	{
		++g_fail;
		std::printf("  [FAIL] %s :: %s\n", g_current.c_str(), what.c_str());
	}
}

static void BeginTest(const char* name)
{
	g_current = name;
	std::printf("[TEST] %s\n", name);
}

static jsonlite::Value ParseJson(const std::string& text)
{
	jsonlite::Value value;
	std::string error;
	if (!jsonlite::ParseText(text, value, error))
		std::printf("  [WARN] parse error: %s\n", error.c_str());
	return value;
}

// -----------------------------------------------------------------------------
// Fixture: fresh Core + bridge + captured event sink per test group.
// -----------------------------------------------------------------------------
struct Fixture
{
	Core core;
	std::vector<std::string> events;
	std::unique_ptr<Bridge> bridge;

	Fixture()
	{
		core.enforceInjectionPolicy = true;
		core.injectionPolicy.SetSceneAllowed(true);
		core.injectionPolicy.SetCaptureHooksReady(true);
		core.motionsRoot = "synthetic/motions";
		HostHooks hooks;
		hooks.releaseFileDialog = [] {};
		hooks.pluginVersion = "0.1.1-test";
		bridge = std::make_unique<Bridge>(core, hooks);
		bridge->SetEventSink(
			[](void* userData, const char* json) {
				static_cast<std::vector<std::string>*>(userData)->push_back(json);
			},
			&events);
		bridge->OnShow();
	}

	std::string Req(const std::string& method, const std::string& params = "{}")
	{
		return bridge->HandleMessageForTest(
			"{\"v\":1,\"id\":7,\"type\":\"request\",\"method\":\"" + method + "\",\"params\":" + params + "}");
	}

	jsonlite::Value ReqJson(const std::string& method, const std::string& params = "{}")
	{
		return ParseJson(Req(method, params));
	}

	bool Ok(const std::string& response)
	{
		const auto value = ParseJson(response);
		const auto* ok = value.Find("ok");
		return ok && ok->IsBool() && ok->boolValue;
	}

	std::string ErrorCode(const std::string& response)
	{
		const auto value = ParseJson(response);
		const auto* error = value.Find("error");
		const auto* code = error ? error->Find("code") : nullptr;
		return code && code->IsString() ? code->stringValue : "";
	}

	const Core::GuiCommand* FindCommand(Core::GuiAction action) const
	{
		for (const auto& command : core.guiCommands)
			if (command.action == action) return &command;
		return nullptr;
	}

	void ClearCommands() { core.guiCommands.clear(); }

	int CountEvents(const std::string& name) const
	{
		int count = 0;
		for (const auto& json : events)
		{
			const auto value = ParseJson(json);
			const auto* eventName = value.Find("name");
			if (eventName && eventName->IsString() && eventName->stringValue == name) ++count;
		}
		return count;
	}

	void LoadSyntheticClip()
	{
		core.player.clip.total_frames = 120;
		core.player.clip.fps = 30.0f;
		core.player.clip.duration_seconds = 4.0f;
		core.player.loadedMotionFile = "synthetic.gkmotion";
		core.loadedMotionKey = "synthetic";
		core.loadedMotionGeneration = 3;
		core.injectionSession = 2;
	}

	void AddSyntheticLibrary()
	{
		MotionEntry first;
		first.name = "alpha";
		first.dir = "synthetic/motions/alpha";
		first.title = "Alpha";
		first.durationSeconds = 4.0f;
		first.totalFrames = 120;
		MotionEntry second = first;
		second.name = "beta";
		second.dir = "synthetic/motions/beta";
		second.title = "Beta";
		core.motions = {first, second};
		core.libraryRevision = 5;
	}
};

static void TestWireFraming()
{
	BeginTest("wire_framing");
	Fixture f;
	const auto malformed = ParseJson(f.bridge->HandleMessageForTest("{not json"));
	Check(malformed.Find("ok") && !malformed.Find("ok")->boolValue, "malformed JSON rejected");
	Check(malformed.Find("error") && malformed.Find("error")->Find("code") &&
		malformed.Find("error")->Find("code")->stringValue == "invalid_params", "malformed -> invalid_params");

	const auto badVersion = ParseJson(f.bridge->HandleMessageForTest(
		"{\"v\":2,\"id\":1,\"type\":\"request\",\"method\":\"bridge.ping\"}"));
	Check(badVersion.Find("error") && badVersion.Find("error")->Find("code")->stringValue == "invalid_params",
		"v!=1 rejected");

	const auto unknown = f.ReqJson("no.such.method");
	Check(unknown.Find("error") && unknown.Find("error")->Find("code")->stringValue == "unknown_method",
		"unknown method -> unknown_method");
	const auto* id = unknown.Find("id");
	Check(id && id->numberValue == 7, "response id correlates with request");

	Check(f.ErrorCode(f.Req("gaze.setMode")) == "invalid_params",
		"gaze domain validates params instead of returning not_implemented");

	const auto badParams = ParseJson(f.bridge->HandleMessageForTest(
		"{\"v\":1,\"id\":3,\"type\":\"request\",\"method\":\"bridge.ping\",\"params\":42}"));
	Check(badParams.Find("error") && badParams.Find("error")->Find("code")->stringValue == "invalid_params",
		"non-object params rejected");
}

static void TestHelloCapabilities()
{
	BeginTest("hello_capabilities");
	Fixture f;
	const auto hello = f.ReqJson("bridge.hello");
	Check(f.Ok(f.Req("bridge.hello")), "bridge.hello ok");
	const auto* result = hello.Find("result");
	Check(result && result->Find("protocol") && result->Find("protocol")->numberValue == 1, "protocol=1");
	Check(result && result->Find("sessionId") && result->Find("sessionId")->numberValue > 0, "sessionId present");
	Check(f.ReqJson("bridge.ping").Find("result")->Find("pong")->boolValue, "bridge.ping pong");

	const auto motionHello = f.ReqJson("motion.hello");
	const auto* methods = motionHello.Find("result")->Find("supportedMethods");
	Check(methods && methods->IsArray(), "supportedMethods array");
	const auto hasMethod = [&](const char* name) {
		for (size_t i = 0; i < methods->Size(); ++i)
			if (methods->At(i)->stringValue == name) return true;
		return false;
	};
	Check(hasMethod("player.play") && hasMethod("state.get") && hasMethod("library.delete") &&
		hasMethod("camera.set") && hasMethod("stage.set"), "core methods advertised");
	Check(hasMethod("dialog.pickFile") && hasMethod("dialog.poll") && hasMethod("drop.setTarget") &&
		hasMethod("drop.poll") && hasMethod("drop.peek") && hasMethod("library.import") &&
		hasMethod("library.probeFiles") && hasMethod("library.startEdit") &&
		hasMethod("actors.getPortrait") && hasMethod("formation.selectSlot") &&
		hasMethod("formation.swapSlots") && hasMethod("formation.setSlotOffset") &&
		hasMethod("formation.resetSlotOffset") && hasMethod("formation.saveSlotOffset") &&
		hasMethod("formation.setSlotSource") &&
		hasMethod("stage.scan") && hasMethod("stage.toggleNode") && hasMethod("stage.setLoadReplacement") &&
		hasMethod("bone.get") && hasMethod("bone.set") && hasMethod("bone.reset") &&
		hasMethod("bone.resetAll") && hasMethod("bone.setMaster") && hasMethod("bone.reload") &&
		hasMethod("bone.saveToMotion") && hasMethod("bone.saveToGlobal") &&
		hasMethod("gaze.get") && hasMethod("gaze.set") && hasMethod("gaze.cycle") &&
		hasMethod("sole.get") && hasMethod("sole.set") && hasMethod("sole.save") && hasMethod("sole.load") &&
		hasMethod("solePitch.get") && hasMethod("solePitch.set") &&
		hasMethod("studio.get") && hasMethod("studio.set") && hasMethod("studio.reset") &&
		hasMethod("experiments.get") && hasMethod("experiments.set") && hasMethod("telemetry.get"),
		"phase-2 protocol methods advertised");
	Check(!hasMethod("ensemble.set") && !hasMethod("ensemble.probe") &&
		!hasMethod("ensemble.presetSave") && !hasMethod("ensemble.presetLoad"),
		"legacy ensemble methods removed");
	const auto* events = motionHello.Find("result")->Find("supportedEvents");
	const auto hasEvent = [&](const char* name) {
		for (size_t i = 0; i < events->Size(); ++i)
			if (events->At(i)->stringValue == name) return true;
		return false;
	};
	Check(hasEvent("player.stateChanged") && hasEvent("bridge.closed") && hasEvent("library.changed") &&
		hasEvent("stage.nodesChanged") && hasEvent("formation.changed") && !hasEvent("ensemble.changed"),
		"events advertised");
	Check(motionHello.Find("result")->Find("pluginVersion")->stringValue == "0.1.1-test",
		"pluginVersion forwarded from hooks");
}

static void TestStateSnapshot()
{
	BeginTest("state_snapshot");
	Fixture f;
	f.AddSyntheticLibrary();
	const auto snapshot = f.ReqJson("state.get");
	Check(f.Ok(f.Req("state.get")), "state.get ok");
	const auto* result = snapshot.Find("result");
	for (const char* key : {"permission", "player", "library", "loaded", "playlist", "actors", "formation", "camera",
		"stage", "facial", "position", "physicsBackend", "musicVolumeSupport",
		"musicVolumeNotice", "importProgress", "motionsRoot", "gaze", "bone", "sole", "solePitch",
		"studio", "experiments"})
		Check(result && result->Find(key), std::string("snapshot carries ") + key);
	Check(result->Find("player")->Find("state")->stringValue == "stopped", "idle player stopped");
	Check(result->Find("player")->Find("title")->stringValue == "未加载曲目", "idle title");
	Check(result->Find("library")->Find("revision")->numberValue == 5, "library revision exposed");
	Check(result->Find("library")->Find("entries")->Size() == 2, "library entries exposed");
	Check(result->Find("library")->Find("entries")->At(0)->Find("id")->stringValue == "alpha", "entry id is key");
	Check(result->Find("importProgress")->IsNull(), "no import -> null progress");
	Check(result->Find("motionsRoot")->stringValue == "synthetic/motions", "motionsRoot exposed");
	Check(result->Find("permission")->Find("injectionAllowed")->boolValue, "permission allowed");
}

static void TestTransportQueue()
{
	BeginTest("transport_queue");
	Fixture f;
	f.LoadSyntheticClip();
	f.AddSyntheticLibrary();

	Check(f.Ok(f.Req("player.play")), "play accepted");
	Check(f.FindCommand(Core::GuiAction::Play) != nullptr, "play queued as GuiAction::Play");
	Check(f.ErrorCode(f.Req("player.pause")) == "not_ready", "pause while stopped -> not_ready");
	f.core.player.state = PlaybackState::Playing;
	Check(f.Ok(f.Req("player.pause")), "pause while playing accepted");
	Check(f.FindCommand(Core::GuiAction::Pause) != nullptr, "pause queued");
	Check(f.Ok(f.Req("player.resume")), "resume accepted");
	Check(f.Ok(f.Req("player.stop")), "stop accepted without clip requirement");

	f.ClearCommands();
	Check(f.Ok(f.Req("player.seek", "{\"seconds\":1.5}")), "seek accepted");
	const auto* seek = f.FindCommand(Core::GuiAction::Seek);
	Check(seek && seek->value == 1.5f, "seek value forwarded");
	Check(seek && seek->key == "synthetic.gkmotion" && seek->loadedGeneration == 3 && seek->sceneSession == 2,
		"seek carries position-command revision guards");
	Check(f.ErrorCode(f.Req("player.seek", "{\"seconds\":99}")) == "invalid_params", "seek beyond duration rejected");
	Check(f.ErrorCode(f.Req("player.seek", "{\"seconds\":-1}")) == "invalid_params", "negative seek rejected");

	Check(f.Ok(f.Req("player.seekFrame", "{\"frame\":119}")), "last frame accepted");
	const auto* seekFrame = f.FindCommand(Core::GuiAction::SeekFrame);
	Check(seekFrame && seekFrame->frame == 119, "seekFrame 0-based frame forwarded");
	Check(f.ErrorCode(f.Req("player.seekFrame", "{\"frame\":120}")) == "invalid_params", "frame out of range rejected");
	Check(f.Ok(f.Req("player.stepFrame", "{\"delta\":1}")), "stepFrame +1 accepted");
	Check(f.ErrorCode(f.Req("player.stepFrame", "{\"delta\":2}")) == "invalid_params", "stepFrame delta must be ±1");

	Check(f.Ok(f.Req("player.setSpeed", "{\"speed\":1.5}")), "speed accepted");
	const auto* speed = f.FindCommand(Core::GuiAction::Speed);
	Check(speed && speed->value == 1.5f, "speed value forwarded");
	Check(f.ErrorCode(f.Req("player.setSpeed", "{\"speed\":0.05}")) == "invalid_params", "speed below range rejected");
	Check(f.Ok(f.Req("player.setVolume", "{\"volume\":0.4}")), "volume accepted");
	const auto* volume = f.FindCommand(Core::GuiAction::MusicVolume);
	Check(volume && volume->value == 0.4f, "volume value forwarded");
	Check(f.ErrorCode(f.Req("player.setVolume", "{\"volume\":2}")) == "invalid_params", "volume above 1 rejected");
	Check(f.Ok(f.Req("player.saveVolume")), "saveVolume accepted");
	Check(f.FindCommand(Core::GuiAction::SaveMusicVolume) != nullptr, "saveVolume queued");

	Check(f.Ok(f.Req("player.cycle", "{\"delta\":1}")), "cycle accepted");
	const auto* cycle = f.FindCommand(Core::GuiAction::Cycle);
	Check(cycle && cycle->value == 1.0f, "cycle delta forwarded");
	Check(f.ErrorCode(f.Req("player.cycle", "{\"delta\":0}")) == "invalid_params", "cycle zero rejected");
	Check(f.Ok(f.Req("player.reload")), "reload accepted");
	Check(f.FindCommand(Core::GuiAction::ReloadMotion) != nullptr, "reload queued");

	Check(f.Ok(f.Req("playlist.setMode", "{\"mode\":2}")), "setMode accepted");
	const auto* mode = f.FindCommand(Core::GuiAction::Mode);
	Check(mode && mode->value == 2.0f, "mode forwarded");
	f.ClearCommands();
	Check(f.Ok(f.Req("playlist.setMode", "{\"mode\":9}")), "invalid mode clamped per protocol");
	const auto* clampedMode = f.FindCommand(Core::GuiAction::Mode);
	Check(clampedMode && clampedMode->value == 1.0f, "invalid mode -> 1");

	Check(f.Ok(f.Req("player.scrubBegin", "{\"seconds\":0.5}")), "scrubBegin accepted");
	Check(f.Ok(f.Req("player.scrubUpdate", "{\"seconds\":1.0}")), "scrubUpdate accepted");
	Check(f.Ok(f.Req("player.scrubEnd", "{\"seconds\":1.0}")), "scrubEnd accepted");
	Check(f.FindCommand(Core::GuiAction::ScrubBegin) && f.FindCommand(Core::GuiAction::ScrubUpdate) &&
		f.FindCommand(Core::GuiAction::ScrubEnd), "scrub trio queued in order");
}

static void TestPermissionGate()
{
	BeginTest("permission_gate");
	Fixture f;
	f.LoadSyntheticClip();
	f.core.injectionPolicy.SetSceneAllowed(false);
	Check(f.ErrorCode(f.Req("player.play")) == "not_allowed", "transport gated by injection policy");
	Check(f.ErrorCode(f.Req("camera.save")) == "not_allowed", "camera gated by injection policy");
	Check(f.Ok(f.Req("library.refresh")), "library refresh not gated");
	Check(f.Ok(f.Req("state.get")), "state.get not gated");
	f.core.injectionPolicy.SetSceneAllowed(true);
	Check(f.Ok(f.Req("player.play")), "transport restored with permission");
}

static void TestLibraryGuards()
{
	BeginTest("library_guards");
	Fixture f;
	f.AddSyntheticLibrary();

	Check(f.ErrorCode(f.Req("library.select", "{\"key\":\"alpha\",\"autoplay\":true,\"revision\":4}")) ==
		"stale_revision", "select with stale revision rejected");
	Check(f.ErrorCode(f.Req("library.select", "{\"key\":\"missing\",\"autoplay\":true,\"revision\":5}")) ==
		"invalid_params", "select unknown key rejected");
	const auto selected = f.ReqJson("library.select", "{\"key\":\"alpha\",\"autoplay\":true,\"revision\":5}");
	Check(f.Ok(f.Req("library.select", "{\"key\":\"alpha\",\"autoplay\":true,\"revision\":5}")), "select accepted");
	Check(selected.Find("result")->Find("key")->stringValue == "alpha", "select result echoes key");
	const auto* select = f.FindCommand(Core::GuiAction::Select);
	Check(select && select->key == "alpha" && select->flag, "select queued with autoplay");

	Check(f.ErrorCode(f.Req("library.delete", "{\"key\":\"alpha\",\"revision\":9}")) == "stale_revision",
		"delete with stale revision rejected");
	Check(f.Ok(f.Req("library.delete", "{\"key\":\"alpha\",\"revision\":5}")), "delete accepted");
	const auto* deleteCommand = f.FindCommand(Core::GuiAction::DeleteFiles);
	Check(deleteCommand && deleteCommand->key == "alpha" && deleteCommand->flag &&
		deleteCommand->libraryRevision == 5, "delete queued confirmed with revision guard");

	f.core.importBusy = true;
	Check(f.ErrorCode(f.Req("library.delete", "{\"key\":\"beta\",\"revision\":5}")) == "busy",
		"delete during import -> busy");
	f.core.importBusy = false;

	f.ClearCommands();
	const auto refresh = f.ReqJson("library.refresh");
	Check(refresh.Find("result")->Find("started")->boolValue, "refresh started");
	Check(f.FindCommand(Core::GuiAction::Refresh) != nullptr, "refresh queued");
	f.core.libraryScan = std::async(std::launch::deferred, [] { return LibraryScanSnapshot{}; });
	const auto refreshAgain = f.ReqJson("library.refresh");
	Check(!refreshAgain.Find("result")->Find("started")->boolValue, "refresh while scanning -> started:false");
}

static void TestCameraStage()
{
	BeginTest("camera_stage");
	Fixture f;
	Check(f.Ok(f.Req("camera.set", "{\"settings\":{\"mode\":2,\"followActor\":true,\"followFov\":50}}")),
		"camera.set accepted");
	const auto* camera = f.FindCommand(Core::GuiAction::CameraSettings);
	Check(camera && camera->cameraSettings.mode == CameraMode::Follow && camera->cameraSettings.followActor &&
		camera->cameraSettings.followFov == 50.0f, "camera patch applied over current settings");
	Check(f.ErrorCode(f.Req("camera.set", "{\"settings\":{\"mode\":9}}")) == "invalid_params",
		"camera mode out of range rejected");
	Check(f.ErrorCode(f.Req("camera.set", "{\"settings\":{\"followRate\":{\"x\":2,\"y\":0,\"z\":0}}")) ==
		"invalid_params", "camera rate out of range rejected");
	Check(f.Ok(f.Req("camera.save")), "camera.save accepted");
	Check(f.FindCommand(Core::GuiAction::SaveCameraSettings) != nullptr, "camera.save queued");

	f.ClearCommands();
	Check(f.Ok(f.Req("stage.set", "{\"settings\":{\"lightingEffectsEnabled\":false}}")), "stage.set accepted");
	const auto* stage = f.FindCommand(Core::GuiAction::StageSettings);
	Check(stage && !stage->stageSettings.lightingEffectsEnabled, "stage settings queued");
	const auto* cameraLink = f.FindCommand(Core::GuiAction::CameraSettings);
	Check(cameraLink && cameraLink->cameraSettings.actorShadowExperimentMode == 2,
		"lighting off links camera shadow mode 2");

	f.ClearCommands();
	Check(f.Ok(f.Req("stage.set", "{\"settings\":{\"hideBackground\":true}}")), "hideBackground accepted");
	const auto* hidden = f.FindCommand(Core::GuiAction::StageSettings);
	Check(hidden && hidden->stageSettings.hideBackground && hidden->stageSettings.greenScreenActive &&
		hidden->stageSettings.overrideCameraClear, "hideBackground forces green screen + camera clear");

	// Simulate the Unity drain having applied the previous command.
	f.core.stageManager.settings.hideBackground = true;
	f.ClearCommands();
	Check(f.Ok(f.Req("stage.set", "{\"settings\":{\"hideBackground\":false}}")), "show background accepted");
	const auto* shown = f.FindCommand(Core::GuiAction::StageSettings);
	Check(shown && !shown->stageSettings.greenScreenActive && !shown->stageSettings.overrideCameraClear,
		"show background clears green screen");
	Check(f.FindCommand(Core::GuiAction::RestoreCameraSkybox) != nullptr, "skybox restore queued");
}

static void TestFacialPositionSettings()
{
	BeginTest("facial_position_settings");
	Fixture f;
	Check(f.Ok(f.Req("facial.setWeights", "{\"overall\":1.5,\"brow\":0.5,\"eye\":1,\"mouth\":2}")),
		"facial.setWeights accepted");
	Check(f.core.player.controller.facialWeights.overall == 1.5f &&
		f.core.player.controller.facialWeights.brow == 0.5f, "facial weights written under stateMutex");
	Check(f.ErrorCode(f.Req("facial.setWeights", "{\"overall\":2.5,\"brow\":1,\"eye\":1,\"mouth\":1}")) ==
		"invalid_params", "facial weight above 2 rejected");
	Check(f.Ok(f.Req("facial.save")), "facial.save accepted");
	Check(f.FindCommand(Core::GuiAction::SaveFacialWeights) != nullptr, "facial.save queued");

	Check(f.Ok(f.Req("position.set", "{\"x\":0.5,\"y\":1,\"z\":-2}")), "position.set accepted");
	const auto* position = f.FindCommand(Core::GuiAction::InitialPosition);
	Check(position && position->position.y == 1.0f && position->position.z == -2.0f, "position forwarded");
	Check(f.ErrorCode(f.Req("position.set", "{\"x\":11,\"y\":0,\"z\":0}")) == "invalid_params",
		"position beyond 10m rejected");
	Check(f.Ok(f.Req("position.save")), "position.save accepted");
	Check(f.FindCommand(Core::GuiAction::SaveInitialPosition) != nullptr, "position.save queued");

	Check(f.Ok(f.Req("settings.setPhysics", "{\"backend\":1}")), "setPhysics accepted");
	const auto* physics = f.FindCommand(Core::GuiAction::PhysicsBackend);
	Check(physics && physics->value == 1.0f, "physics backend forwarded");
	Check(f.ErrorCode(f.Req("settings.setPhysics", "{\"backend\":3}")) == "invalid_params",
		"physics backend out of range rejected");

	Check(f.Ok(f.Req("shell.setTextInputActive", "{\"active\":true}")), "setTextInputActive accepted");
	Check(f.core.guiTextInputActive.load(), "text input flag stored");
	Check(f.Ok(f.Req("shell.setTextInputActive", "{\"active\":false}")), "setTextInputActive false accepted");
	Check(!f.core.guiTextInputActive.load(), "text input flag cleared");

	const auto appearance = f.ReqJson("appearance.get");
	Check(appearance.Find("result")->Find("theme") != nullptr &&
		appearance.Find("result")->Find("uiScale") != nullptr, "appearance defaults without host service");
}

static void TestActorsToken()
{
	BeginTest("actors_token");
	Fixture f;
	SceneActorObservation observation;
	observation.actor = reinterpret_cast<void*>(0x1234);
	observation.instanceId = 1;
	observation.character = "saki";
	observation.displayName = "咲季";
	f.core.sceneActorRegistry.Publish({observation});
	const uint64_t epoch = f.core.sceneActorRegistry.Epoch();
	const uint64_t token = f.core.sceneActorRegistry.Entries().front().token;

	Check(f.ErrorCode(f.Req("actors.selectTarget", "{\"token\":1,\"epoch\":99}")) == "stale_token",
		"stale epoch rejected");
	Check(f.ErrorCode(f.Req("actors.selectTarget",
		"{\"token\":999,\"epoch\":" + std::to_string(epoch) + "}")) == "stale_token", "unknown token rejected");
	Check(f.Ok(f.Req("actors.selectTarget",
		"{\"token\":" + std::to_string(token) + ",\"epoch\":" + std::to_string(epoch) + "}")),
		"valid token accepted");
	const auto* select = f.FindCommand(Core::GuiAction::SelectTarget);
	Check(select && select->actorToken == token && select->actorEpoch == epoch, "token+epoch queued");

	const auto snapshot = f.ReqJson("state.get");
	const auto* actors = snapshot.Find("result")->Find("actors");
	Check(actors->Find("epoch")->numberValue == static_cast<double>(epoch), "actors epoch exposed");
	Check(actors->Find("actors")->Size() == 1, "actor roster exposed");

	observation.presetName = "自定义演出";
	f.core.sceneActorRegistry.Publish({observation});
	const auto presetSnapshot = f.ReqJson("state.get");
	const auto* presetActors = presetSnapshot.Find("result")->Find("actors")->Find("actors");
	Check(presetActors && presetActors->Size() == 1 &&
		presetActors->At(0)->Find("label")->stringValue == "自定义演出 · 1",
		"actor presetName overrides display name in web-panel roster");
}

static void TestBufferContract()
{
	BeginTest("buffer_contract");
	Fixture f;
	const char* request = "{\"v\":1,\"id\":1,\"type\":\"request\",\"method\":\"bridge.hello\"}";
	char tinyBuffer[8]; // "small" is a Windows RPC macro
	std::memset(tinyBuffer, 0x7E, sizeof(tinyBuffer));
	uint32_t required = 0;
	Check(f.bridge->HandleMessage(request, sizeof(tinyBuffer), tinyBuffer, &required) == GAKU_E_SIZE,
		"short buffer -> GAKU_E_SIZE");
	Check(required > sizeof(tinyBuffer), "requiredBytes reports full size");
	Check(tinyBuffer[0] == 0x7E, "short buffer leaves output untouched");

	std::vector<char> buffer(required, 0);
	Check(f.bridge->HandleMessage(request, static_cast<uint32_t>(buffer.size()), buffer.data(), &required) == GAKU_OK,
		"sized buffer -> GAKU_OK");
	Check(buffer[required - 1] == '\0', "requiredBytes includes NUL");
	Check(std::strlen(buffer.data()) == required - 1, "response NUL-terminated");
	const auto response = ParseJson(buffer.data());
	Check(response.Find("ok") && response.Find("ok")->boolValue, "buffered response valid");

	Check(f.bridge->HandleMessage(request, 0, nullptr, &required) == GAKU_E_SIZE,
		"metadata query returns size");
	Check(f.bridge->HandleMessage(nullptr, 0, nullptr, &required) == GAKU_E_INVALID_ARGUMENT,
		"null request rejected");
}

static void TestEventsEnvelope()
{
	BeginTest("events_envelope");
	Fixture f;
	f.AddSyntheticLibrary();
	f.bridge->OnUiTick();
	Check(!f.events.empty(), "baseline tick emits events");
	uint64_t lastSeq = 0;
	bool envelopeOk = true;
	for (const auto& json : f.events)
	{
		const auto value = ParseJson(json);
		const auto* v = value.Find("v");
		const auto* type = value.Find("type");
		const auto* seq = value.Find("seq");
		const auto* sid = value.Find("sid");
		if (!v || v->numberValue != 1 || !type || type->stringValue != "event" || !seq || !sid ||
			seq->numberValue <= static_cast<double>(lastSeq) ||
			sid->numberValue != static_cast<double>(f.bridge->SessionId()))
			envelopeOk = false;
		lastSeq = static_cast<uint64_t>(seq->numberValue);
	}
	Check(envelopeOk, "every event carries v=1/type=event/monotonic seq/session sid");
	Check(f.CountEvents("player.stateChanged") >= 1, "baseline player event");
	Check(f.CountEvents("permission.changed") >= 1, "baseline permission event");
	Check(f.CountEvents("library.changed") >= 1, "baseline library event");

	const int libraryBefore = f.CountEvents("library.changed");
	++f.core.libraryRevision;
	f.bridge->OnUiTick();
	Check(f.CountEvents("library.changed") == libraryBefore + 1, "library revision change pushes event");

	f.core.player.state = PlaybackState::Playing;
	f.bridge->OnUiTick();
	const int playerBefore = f.CountEvents("player.stateChanged");
	f.bridge->OnUiTick();
	Check(f.CountEvents("player.stateChanged") == playerBefore, "playing state throttled to ~10Hz");

	Fixture second;
	Check(second.bridge->SessionId() > f.bridge->SessionId(), "new bridge advances sessionId");
}

static void TestDialogHostService()
{
	BeginTest("dialog_host_service");
	Fixture f;
	// Core's pick-file hooks are plain function pointers: use file-scope state.
	static int beginCalls = 0;
	static int pollState = GAKU_FILE_DIALOG_PENDING;
	static std::string pollPath;
	beginCalls = 0;
	pollState = GAKU_FILE_DIALOG_PENDING;
	pollPath.clear();
	f.core.fnBeginPickFile = [](int, std::string& notice) -> bool {
		++beginCalls;
		notice.clear();
		return true;
	};
	f.core.fnPollPickFile = [](std::string& path, std::string&) -> int {
		path = pollPath;
		return pollState;
	};

	Check(f.ErrorCode(f.Req("dialog.pickFile", "{\"kind\":9,\"multiple\":false}")) == "invalid_params",
		"invalid kind rejected");
	Check(f.Ok(f.Req("dialog.pickFile", "{\"kind\":5,\"multiple\":true}")),
		"kind=5 and multiple=true accepted");
	f.bridge->OnShutdown();
	const auto picked = f.ReqJson("dialog.pickFile", "{\"kind\":1,\"multiple\":false}");
	Check(picked.Find("ok") && picked.Find("ok")->boolValue, "pickFile begins host dialog");
	Check(picked.Find("result") && picked.Find("result")->Find("dialogId") &&
		picked.Find("result")->Find("dialogId")->numberValue == 2, "dialogId advances across dialogs");
	Check(beginCalls == 2, "host beginOpen called for web kinds");
	Check(f.ErrorCode(f.Req("dialog.pickFile", "{\"kind\":4,\"multiple\":false}")) == "busy",
		"second dialog while pending -> busy");

	auto poll = f.ReqJson("dialog.poll", "{\"dialogId\":2}");
	Check(poll.Find("result")->Find("state")->stringValue == "pending", "poll pending");
	pollState = GAKU_FILE_DIALOG_ACCEPTED;
	pollPath = "C:\\synthetic\\motion.vmd";
	f.bridge->OnUiTick();
	poll = f.ReqJson("dialog.poll", "{\"dialogId\":2}");
	Check(poll.Find("result")->Find("state")->stringValue == "accepted", "onUiTick drives dialog completion");
	Check(poll.Find("result")->Find("paths")->At(0)->stringValue == "C:\\synthetic\\motion.vmd",
		"accepted path returned");
	Check(f.ErrorCode(f.Req("dialog.poll", "{\"dialogId\":42}")) == "invalid_params", "wrong dialogId rejected");
}

static void TestShutdownInvalidation()
{
	BeginTest("shutdown_invalidation");
	namespace fs = std::filesystem;
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path assetsRoot = fs::temp_directory_path() / ("gaku_webpanel_reattach_" + std::to_string(stamp));
	fs::create_directories(assetsRoot);
	{ std::ofstream out(assetsRoot / "index.html", std::ios::binary); out << "<!doctype html>"; }

	Core core;
	HostHooks hooks;
	hooks.assetsDirs = [&] { return std::vector<std::wstring>{assetsRoot.wstring()}; };
	std::vector<std::string> events;
	Bridge bridge(core, hooks);
	auto sink = [](void* user, const char* json) {
		static_cast<std::vector<std::string>*>(user)->emplace_back(json ? json : "");
	};
	bridge.SetEventSink(sink, &events);
	bridge.OnShow();
	bridge.OnUiTick();
	const uint64_t initialSid = bridge.SessionId();
	const size_t beforeShutdown = events.size();

	// UI detach (OnShutdown) notifies bridge.closed, detaches the sink, advances
	// sessionId, and leaves the bridge reusable when re-shown later.
	bridge.OnShutdown();
	Check(!bridge.Closed(), "onShutdown detaches session without permanent plugin close");
	Check(events.size() == beforeShutdown + 1, "bridge.closed emitted once");
	const auto closed = ParseJson(events.back());
	Check(closed.Find("name")->stringValue == "bridge.closed", "last event is bridge.closed");
	Check(closed.Find("payload")->Find("reason") != nullptr, "closure reason present");
	Check(bridge.SessionId() > initialSid, "sessionId advances across UI detach");

	GakuWebPanelAssetsV1 assets{};
	Check(bridge.GetAssets(&assets) == GAKU_OK, "bridge remains queryable after UI detach");
	events.clear();
	bridge.SetEventSink(sink, &events);
	bridge.OnShow();
	bridge.OnUiTick();
	Check(!events.empty(), "re-attached UI session receives fresh baseline events");
	const auto firstReattached = ParseJson(events.front());
	Check(firstReattached.Find("seq")->numberValue == 1 &&
		firstReattached.Find("sid")->numberValue == static_cast<double>(bridge.SessionId()),
		"new UI session resets seq=1 with new sessionId");

	// Permanent unload (Invalidate on plugin shutdown) refuses everything silently.
	Fixture silent;
	silent.bridge->Invalidate("plugin-shutdown");
	Check(silent.bridge->Closed(), "silent invalidate closes bridge permanently");
	Check(silent.events.empty(), "Unity-thread invalidate never calls the sink");
	Check(silent.ErrorCode(silent.Req("bridge.ping")) == "not_allowed", "requests after Invalidate rejected");
	GakuWebPanelAssetsV1 dummyAssets{};
	Check(silent.bridge->GetAssets(&dummyAssets) == GAKU_E_SHUTTING_DOWN, "getAssets after Invalidate -> SHUTTING_DOWN");
	const int eventCount = static_cast<int>(silent.events.size());
	silent.bridge->OnUiTick();
	Check(static_cast<int>(silent.events.size()) == eventCount, "no events after Invalidate");

	std::error_code ec;
	fs::remove_all(assetsRoot, ec);
}

static void TestScrubResetOnPermissionLoss()
{
	BeginTest("scrub_reset_on_permission_loss");
	Fixture f;
	f.LoadSyntheticClip();
	f.core.player.state = PlaybackState::Playing;
	f.core.player.scrubbing = true;
	f.core.player.currentTime = 1.25f;
	f.bridge->OnUiTick();
	f.core.injectionPolicy.SetSceneAllowed(false);
	f.bridge->OnUiTick();
	Check(f.CountEvents("permission.changed") >= 2, "permission flip pushed");
	const auto* scrubEnd = f.FindCommand(Core::GuiAction::ScrubEnd);
	Check(scrubEnd != nullptr, "native scrub finished through the queue on permission loss");
	Check(scrubEnd && scrubEnd->value == 1.25f, "scrub end preserves current time");
}

static void TestGetAssetsCandidates()
{
	BeginTest("get_assets_candidates");
	namespace fs = std::filesystem;
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("gaku_webpanel_assets_" + std::to_string(stamp));
	const fs::path pluginUi = root / "plugins" / "GakuMotion" / "GakuMotion.UI";
	const fs::path fallbackUi = root / "fallback" / "GakuMotion.UI";
	fs::create_directories(pluginUi);
	fs::create_directories(fallbackUi);
	const auto writeIndex = [](const fs::path& dir) {
		std::ofstream out(dir / "index.html", std::ios::binary | std::ios::trunc);
		out << "<!doctype html><title>synthetic</title>";
	};

	Core core;
	HostHooks hooks;
	hooks.assetsDirs = [&] {
		return std::vector<std::wstring>{pluginUi.wstring(), fallbackUi.wstring()};
	};
	Bridge bridge(core, hooks);
	GakuWebPanelAssetsV1 assets{};

	Check(bridge.GetAssets(nullptr) == GAKU_E_INVALID_ARGUMENT, "null output rejected");
	Check(bridge.GetAssets(&assets) == GAKU_E_UNAVAILABLE, "no entry file anywhere -> UNAVAILABLE");

	writeIndex(fallbackUi);
	Check(bridge.GetAssets(&assets) == GAKU_OK, "secondary candidate accepted when primary missing");
	Check(assets.assetsDir == fallbackUi.wstring(), "resolves to first existing candidate");
	Check(std::wstring(assets.entryPath) == L"index.html", "entryPath is index.html");
	Check(std::wstring(assets.virtualHost) == L"gakumotion.invalid", "virtualHost gakumotion.invalid");
	Check(assets.abiVersion == GAKU_WEB_PANEL_VERSION && assets.structSize == sizeof(GakuWebPanelAssetsV1),
		"assets ABI header filled");

	writeIndex(pluginUi);
	Check(bridge.GetAssets(&assets) == GAKU_OK, "plugin module dir candidate accepted");
	Check(assets.assetsDir == pluginUi.wstring(), "plugin module dir preferred first");

	bridge.Invalidate("test");
	Check(bridge.GetAssets(&assets) == GAKU_E_SHUTTING_DOWN, "closed bridge refuses getAssets");

	Core bareCore;
	Bridge bareBridge(bareCore, HostHooks{});
	Check(bareBridge.GetAssets(&assets) == GAKU_E_UNAVAILABLE, "missing hook -> UNAVAILABLE");

	std::error_code ec;
	fs::remove_all(root, ec);
}

static void WriteSyntheticVmd(const std::filesystem::path& path, uint32_t bodyFrames,
	uint32_t morphFrames, uint32_t cameraFrames)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	char header[30]{};
	std::memcpy(header, "Vocaloid Motion Data 0002", 25);
	out.write(header, 30);
	char model[20]{};
	out.write(model, 20);
	out.write(reinterpret_cast<const char*>(&bodyFrames), 4);
	for (uint32_t i = 0; i < bodyFrames; ++i)
	{
		char record[111]{};
		std::memcpy(record, "Center", 6);
		out.write(record, 111);
	}
	out.write(reinterpret_cast<const char*>(&morphFrames), 4);
	for (uint32_t i = 0; i < morphFrames; ++i)
	{
		char record[23]{};
		std::memcpy(record, "mouth", 5);
		out.write(record, 23);
	}
	out.write(reinterpret_cast<const char*>(&cameraFrames), 4);
	for (uint32_t i = 0; i < cameraFrames; ++i)
	{
		char record[61]{};
		out.write(record, 61);
	}
}

static void TestLibraryImportProbe()
{
	BeginTest("library_import_probe");
	namespace fs = std::filesystem;
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("gaku_webpanel_import_" + std::to_string(stamp));
	fs::create_directories(root);
	const fs::path motion = root / "synthetic_motion.vmd";
	const fs::path facial = root / "synthetic_facial.vmd";
	const fs::path camera = root / "synthetic_camera.vmd";
	const fs::path audio = root / "synthetic.wav";
	WriteSyntheticVmd(motion, 1, 1, 0);
	WriteSyntheticVmd(facial, 0, 1, 0);
	WriteSyntheticVmd(camera, 0, 0, 1);
	{ std::ofstream out(audio, std::ios::binary | std::ios::trunc); out << "RIFF"; }

	Fixture f;
	const std::string paths = "[" + JString(MotionPathToUtf8(motion)) + "," +
		JString(MotionPathToUtf8(facial)) + "," + JString(MotionPathToUtf8(camera)) + "," +
		JString(MotionPathToUtf8(audio)) + "]";
	const auto probe = f.ReqJson("library.probeFiles", "{\"paths\":" + paths + "}");
	Check(f.Ok(f.Req("library.probeFiles", "{\"paths\":" + paths + "}")), "probeFiles accepted");
	const auto* classification = probe.Find("result")->Find("classification");
	Check(classification && classification->IsObject(), "probeFiles returns classification object");
	Check(classification->Find("motionCandidate")->stringValue == MotionPathToUtf8(motion), "motion candidate classified");
	Check(classification->Find("facialCandidate")->stringValue == MotionPathToUtf8(facial), "facial candidate classified");
	Check(classification->Find("cameraCandidate")->stringValue == MotionPathToUtf8(camera), "camera candidate classified");
	Check(classification->Find("audioCandidate")->stringValue == MotionPathToUtf8(audio), "audio candidate classified");
	const auto* probes = probe.Find("result")->Find("probes");
	Check(probes && probes->Size() == 4, "per-file probes returned");
	Check(probes->At(0)->Find("tendency")->stringValue == "motionAndFacial", "probe tendency string enum");
	Check(probes->At(1)->Find("tendency")->stringValue == "facialOnly", "facial tendency string enum");
	Check(probes->At(2)->Find("tendency")->stringValue == "cameraOnly", "camera tendency string enum");

	const std::string reqBody = "{\"request\":{\"title\":\"Synthetic\",\"editKey\":\"\","
		"\"motionFile\":" + JString(MotionPathToUtf8(motion)) +
		",\"facialFile\":" + JString(MotionPathToUtf8(facial)) +
		",\"cameraFile\":" + JString(MotionPathToUtf8(camera)) +
		",\"audioFile\":" + JString(MotionPathToUtf8(audio)) +
		",\"disableMorph\":false,\"clearCamera\":false,\"clearAudio\":false,"
		"\"fps\":30,\"timing\":{\"motion\":{\"speed\":1.5,\"offsetSeconds\":0.2}}}}";
	const auto imported = f.ReqJson("library.import", reqBody);
	Check(imported.Find("result")->Find("started")->boolValue, "import started");
	const auto* command = f.FindCommand(Core::GuiAction::Import);
	Check(command && command->import.vmd == MotionPathToUtf8(motion), "import queues motion path");
	Check(command && command->import.morphVmd == MotionPathToUtf8(facial), "import queues facial path");
	Check(command && command->import.cameraVmd == MotionPathToUtf8(camera), "import queues camera path");
	Check(command && command->import.audio == MotionPathToUtf8(audio), "import queues audio path");
	Check(command && command->import.fps == 30.0f, "fps 30 preserved");
	Check(command && command->import.timing.motion.speed == 1.5f, "track speed preserved");
	Check(command && command->import.timing.motion.offsetSeconds == 0.2f, "track offset preserved");

	f.core.importBusy = true;
	Check(f.ErrorCode(f.Req("library.import", reqBody)) == "busy",
		"duplicate import while busy rejected");
	f.core.importBusy = false;

	f.AddSyntheticLibrary();
	f.core.motions[0].hasFacial = true;
	f.core.motions[0].hasCamera = true;
	f.core.motions[0].hasAudio = true;
	const auto edit = f.ReqJson("library.startEdit", "{\"key\":\"alpha\",\"revision\":5}");
	Check(f.Ok(f.Req("library.startEdit", "{\"key\":\"alpha\",\"revision\":5}")), "startEdit accepted");
	Check(edit.Find("result")->Find("key")->stringValue == "alpha", "startEdit returns flat edit key");
	Check(edit.Find("result")->Find("title") != nullptr &&
		edit.Find("result")->Find("hasFacial")->boolValue &&
		edit.Find("result")->Find("hasCamera")->boolValue &&
		edit.Find("result")->Find("hasAudio")->boolValue &&
		edit.Find("result")->Find("timing") != nullptr,
		"startEdit returns EditEntryInfo contract");
	Check(f.ErrorCode(f.Req("library.startEdit", "{\"key\":\"alpha\",\"revision\":9}")) == "stale_revision",
		"startEdit revision guard");

	f.LoadSyntheticClip();
	f.core.loadedMotionKey = "alpha";
	f.core.player.state = PlaybackState::Playing;
	const std::string editReq = "{\"request\":{\"title\":\"Alpha\",\"editKey\":\"alpha\","
		"\"motionFile\":\"\",\"facialFile\":\"\",\"cameraFile\":\"\",\"audioFile\":\"\","
		"\"disableMorph\":false,\"clearCamera\":false,\"clearAudio\":false,\"fps\":0}}";
	Check(f.ErrorCode(f.Req("library.import", editReq)) == "protected",
		"editing playing track rejected");
	f.core.player.state = PlaybackState::Stopped;
	f.ClearCommands();
	Check(f.Ok(f.Req("library.import", editReq)), "editing stopped track accepted");
	const auto* editCommand = f.FindCommand(Core::GuiAction::Import);
	Check(editCommand && editCommand->import.editKey == "alpha", "edit key queued");
	Check(editCommand && !editCommand->import.disableMorph && editCommand->import.morphVmd.empty(),
		"keep facial track does not clear or replace");

	f.ClearCommands();
	const std::string badFpsReq = "{\"request\":{\"title\":\"Alpha\",\"editKey\":\"alpha\","
		"\"motionFile\":\"\",\"facialFile\":\"\",\"cameraFile\":\"\",\"audioFile\":\"\","
		"\"disableMorph\":false,\"clearCamera\":false,\"clearAudio\":false,\"fps\":30,\"sourceFps\":42}}";
	Check(f.ErrorCode(f.Req("library.import", badFpsReq)) == "invalid_params",
		"unsupported sourceFps rejected");
	const std::string fpsReq = "{\"request\":{\"title\":\"Alpha\",\"editKey\":\"alpha\","
		"\"motionFile\":\"\",\"facialFile\":\"\",\"cameraFile\":\"\",\"audioFile\":\"\","
		"\"disableMorph\":false,\"clearCamera\":false,\"clearAudio\":false,\"fps\":30,\"sourceFps\":60}}";
	Check(f.Ok(f.Req("library.import", fpsReq)), "explicit sourceFps and output fps accepted");
	const auto* fpsCommand = f.FindCommand(Core::GuiAction::Import);
	Check(fpsCommand && fpsCommand->import.sourceFps == 60.0f && fpsCommand->import.fps == 30.0f &&
		fpsCommand->import.rebakeOutput,
		"sourceFps, output fps and edit rebakeOutput forwarded");

	std::error_code ec;
	fs::remove_all(root, ec);
}

static void TestStageNodes()
{
	BeginTest("stage_nodes");
	namespace fs = std::filesystem;
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("gaku_webpanel_stage_" + std::to_string(stamp));
	fs::create_directories(root);

	Fixture f;
	f.core.configRoot = MotionPathToUtf8(root);
	const auto scanEmpty = f.ReqJson("stage.scan");
	Check(scanEmpty.Find("ok") && scanEmpty.Find("ok")->boolValue, "stage.scan accepted");
	Check(scanEmpty.Find("result")->Find("nodes") && scanEmpty.Find("result")->Find("nodes")->IsArray(),
		"stage.scan returns {nodes: []}");
	Check(f.FindCommand(Core::GuiAction::ScanStageCandidates) != nullptr, "scan queued");
	{
		std::lock_guard<std::mutex> lock(f.core.stageManager.stageMutex);
		StageCandidate parent;
		parent.name = "Background";
		parent.currentlyActive = true;
		parent.hasChildren = true;
		StageCandidate child;
		child.name = "Mesh";
		child.parentIndex = 0;
		child.currentlyActive = true;
		StageCandidate locked;
		locked.name = "Collider";
		locked.isColliderLocked = true;
		f.core.stageManager.candidates = {parent, child, locked};
	}
	f.bridge->OnUiTick();
	Check(f.CountEvents("stage.nodesChanged") >= 1, "stage.nodesChanged emitted when candidates update");
	const auto scanWithNodes = f.ReqJson("stage.scan");
	Check(scanWithNodes.Find("result")->Find("nodes")->Size() == 3, "stage.scan returns current nodes");
	Check(f.ErrorCode(f.Req("stage.toggleNode", "{\"index\":2,\"active\":false}")) == "protected",
		"collider node locked");
	{
		std::lock_guard<std::mutex> lock(f.core.stageManager.stageMutex);
		f.core.stageManager.candidates[0].currentlyActive = false;
	}
	Check(f.ErrorCode(f.Req("stage.toggleNode", "{\"index\":1,\"active\":false}")) == "protected",
		"ancestor inactive blocks child");
	{
		std::lock_guard<std::mutex> lock(f.core.stageManager.stageMutex);
		f.core.stageManager.candidates[0].currentlyActive = true;
	}
	f.ClearCommands();
	Check(f.Ok(f.Req("stage.toggleNode", "{\"index\":1,\"active\":false}")), "valid node toggle accepted");
	const auto* toggle = f.FindCommand(Core::GuiAction::ToggleStageCandidate);
	Check(toggle && toggle->candidateIndex == 1 && !toggle->flag, "toggle index and flag queued");
	Check(f.ErrorCode(f.Req("stage.toggleNode", "{\"index\":99,\"active\":true}")) == "invalid_params",
		"invalid node index");
	Check(f.ErrorCode(f.Req("stage.setLoadReplacement", "{\"key\":\"bad\"}")) == "invalid_params",
		"invalid replacement key");
	const auto replacement = f.ReqJson("stage.setLoadReplacement", "{\"key\":\"env_3d_live_courtyard-00-00-noon\"}");
	Check(replacement.Find("ok") && replacement.Find("ok")->boolValue, "valid replacement key accepted");
	Check(replacement.Find("result")->Find("notice") && replacement.Find("result")->Find("notice")->IsString(),
		"stage.setLoadReplacement returns {notice}");
	Check(fs::exists(root / "stage.json"), "stage.json saved immediately");
	const auto stage = f.ReqJson("state.get");
	Check(stage.Find("result")->Find("stage")->Find("candidates")->Size() == 3, "stage snapshot exposes candidates");

	std::error_code ec;
	fs::remove_all(root, ec);
}

static void TestBoneOverrides()
{
	BeginTest("bone_overrides");
	Fixture f;
	Check(f.Ok(f.Req("bone.set", "{\"humanoidId\":13,\"boneName\":\"LeftUpperArm\",\"pitch\":5,\"yaw\":1,\"roll\":2,"
		"\"spaceWorld\":false,\"enabled\":true,\"symmetrical\":true}")), "bone.set accepted");
	const auto allBones = f.ReqJson("bone.get");
	Check(allBones.Find("ok") && allBones.Find("result")->Find("masterEnabled")->boolValue, "bone.get returns BoneState");
	const auto* overrides = allBones.Find("result")->Find("overrides");
	Check(overrides && overrides->Size() == 2, "symmetrical bone.set created both overrides");
	const auto mirrored = f.ReqJson("bone.get", "{\"hid\":14}");
	Check(mirrored.Find("result")->Find("found")->boolValue, "symmetrical override created");
	Check(mirrored.Find("result")->Find("pitch")->numberValue == 5 &&
		mirrored.Find("result")->Find("yaw")->numberValue == -1 &&
		mirrored.Find("result")->Find("roll")->numberValue == -2, "MMD mirror formula preserved");
	Check(f.Ok(f.Req("bone.reset", "{\"humanoidId\":13,\"boneName\":\"LeftUpperArm\",\"symmetrical\":true}")), "bone.reset accepted");
	Check(!f.ReqJson("bone.get", "{\"hid\":13}").Find("result")->Find("found")->boolValue, "reset removes override");
	Check(f.Ok(f.Req("bone.setMaster", "{\"enabled\":false}")), "bone.setMaster accepted");
	Check(!f.core.masterOverridesEnabled, "bone.setMaster updates master switch");
	namespace fs = std::filesystem;
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("gaku_webpanel_bone_" + std::to_string(stamp));
	fs::create_directories(root);
	f.core.globalOverridePath = MotionPathToUtf8(root / "bone_overrides.json");
	Check(f.Ok(f.Req("bone.saveToGlobal")), "saveToGlobal accepted");
	Check(fs::exists(root / "bone_overrides.json"), "global override file written");
	std::error_code ec;
	fs::remove_all(root, ec);
}

static void NoopLookAtTarget(void*, void*) {}
static void NoopLookAtWeights(void*, float, float, float) {}
static void NoopLookAtEnabled(void*, bool) {}

static void TestGazeSettings()
{
	BeginTest("gaze_settings");
	Fixture f;
	Check(f.ErrorCode(f.Req("gaze.set", "{\"settings\":{\"mode\":2,\"desktopLookAtEnabled\":false,\"eyeWeight\":1,\"headWeight\":0.5,\"bodyWeight\":0.25}}")) == "not_ready",
		"gaze unavailable without runtime callbacks");
	f.core.lookAt.fnSetTarget = NoopLookAtTarget;
	f.core.lookAt.fnSetWeights = NoopLookAtWeights;
	f.core.lookAt.fnSetEnabled = NoopLookAtEnabled;
	Check(f.Ok(f.Req("gaze.set", "{\"settings\":{\"mode\":2,\"desktopLookAtEnabled\":false,\"eyeWeight\":0.5,\"headWeight\":0.2,\"bodyWeight\":0.1}}")),
		"gaze.set accepted");
	Check(f.core.config.gazeMode == 2 && !f.core.lookAt.desktopLookAtEnabled.load() &&
		f.core.lookAt.eyeWeight.load() == 0.5f && f.core.config.headWeight == 0.2f,
		"gaze.set applied all fields");
	Check(f.ErrorCode(f.Req("gaze.set", "{\"settings\":{\"eyeWeight\":1.5}}")) == "invalid_params",
		"gaze weight range validated");
	const auto cycle = f.ReqJson("gaze.cycle");
	Check(cycle.Find("result")->Find("mode")->numberValue == 0, "gaze cycle wraps");
	const auto gaze = f.ReqJson("gaze.get");
	const auto* gazeResult = gaze.Find("result");
	Check(gazeResult && gazeResult->Find("runtimeAvailable")->boolValue &&
		gazeResult->Find("vrifyQueryAvailable") != nullptr &&
		gazeResult->Find("settings")->Find("mode")->numberValue == 0,
		"gaze.get returns protocol GazeState shape");
}

static void TestSoleStudio()
{
	BeginTest("sole_studio");
	Fixture f;
	const std::string sole = "{\"settings\":{\"enabled\":true,\"useBootProxy\":true,\"planeCalibrated\":true,"
		"\"shoeCalibrated\":true,\"planeNormal\":{\"x\":0,\"y\":1,\"z\":0},\"planeOffsetM\":0,\"displayBandM\":0.002}}";
	Check(f.Ok(f.Req("sole.set", sole)), "sole.set accepted");
	Check(f.FindCommand(Core::GuiAction::SoleSettings) != nullptr, "sole settings queued");
	Check(f.Ok(f.Req("sole.save")), "sole.save accepted");
	Check(f.FindCommand(Core::GuiAction::SaveSoleSettings) != nullptr, "sole save queued");
	Check(f.Ok(f.Req("sole.load")), "sole.load accepted");
	Check(f.FindCommand(Core::GuiAction::LoadSoleSettings) != nullptr, "sole load queued");
	const auto soleState = f.ReqJson("sole.get");
	Check(soleState.Find("result")->Find("generation") != nullptr, "sole.get exposes generation");
	Check(f.ErrorCode(f.Req("solePitch.set", "{\"settings\":{\"maxPitchDegrees\":50}}")) == "invalid_params",
		"solePitch range validated");
	Check(f.Ok(f.Req("solePitch.set", "{\"settings\":{\"enabled\":true,\"maxPitchDegrees\":20,\"useMeshEnvelope\":true}}")),
		"solePitch.set accepted");
	const auto* pitchCmd = f.FindCommand(Core::GuiAction::ExperimentalSolePitch);
	Check(pitchCmd && pitchCmd->experimentalSolePitchSettings.experimentalSolePitchCorrection &&
		pitchCmd->experimentalSolePitchSettings.useMeshEnvelope, "solePitch settings queued");
	const auto pitchState = f.ReqJson("solePitch.get");
	Check(pitchState.Find("result")->Find("settings")->Find("enabled") != nullptr &&
		pitchState.Find("result")->Find("feet") != nullptr, "solePitch.get exposes flat SolePitchState");

	Check(f.Ok(f.Req("studio.set", "{\"settings\":{\"lighting\":{\"enabled\":true},\"video\":{\"enabled\":false,\"path\":\"\"}}}")),
		"studio.set accepted");
	const auto* studio = f.FindCommand(Core::GuiAction::StudioSettings);
	Check(studio && studio->studioSettings.lighting.enabled, "studio lighting queued");
	f.ClearCommands();
	Check(f.Ok(f.Req("studio.setLighting", "{\"lighting\":{\"enabled\":false}}")), "studio.setLighting accepted");
	const auto* lighting = f.FindCommand(Core::GuiAction::StudioSettings);
	Check(lighting && !lighting->studioSettings.lighting.enabled, "lighting-only update queued");
	Check(f.Ok(f.Req("studio.reset")), "studio.reset accepted");
	Check(f.FindCommand(Core::GuiAction::StudioReset) != nullptr, "studio reset queued");
	const auto snapshot = f.ReqJson("studio.get");
	Check(snapshot.Find("result")->Find("revision")->numberValue >= 1, "studio revision exposed");
}

static void TestExperimentsTelemetry()
{
	BeginTest("experiments_telemetry");
	Fixture f;
	Check(f.Ok(f.Req("experiments.set", "{\"patch\":{\"mikuMouthFit\":false,\"mikuEyeFit\":true,\"mikuFitAllowUnverified\":true,\"waistSupport\":false}}")),
		"experiments.set accepted");
	Check(f.core.config.facialMikuMouthFit == 0 && f.core.config.facialMikuEyeFit == 1 &&
		f.core.config.facialMikuFitAllowUnverified == 1, "Miku switches written");
	const auto* waist = f.FindCommand(Core::GuiAction::ExperimentalWaistSupport);
	Check(waist && !waist->flag, "waist support flag queued");
	Check(f.Ok(f.Req("experiments.set", "{\"patch\":{\"beforeQuartz\":true}}")), "BeforeQuartz accepted on native");
	Check(f.FindCommand(Core::GuiAction::BeforeQuartzExperiment) != nullptr, "BeforeQuartz queued");
	f.core.guiPhysicsBackend.store(1);
	Check(f.ErrorCode(f.Req("experiments.set", "{\"patch\":{\"beforeQuartz\":true}}")) == "invalid_params",
		"BeforeQuartz rejected without native backend");
	const auto telemetry = f.ReqJson("telemetry.get");
	Check(telemetry.Find("result")->Find("statusLine")->IsString(), "telemetry status line");
	Check(telemetry.Find("result")->Find("player")->Find("stateName")->IsString(), "telemetry player stateName");
	Check(telemetry.Find("result")->Find("actor")->Find("actorLabel")->IsString(), "telemetry actorLabel");
	const auto experiments = f.ReqJson("experiments.get");
	const auto* expResult = experiments.Find("result");
	Check(expResult && expResult->Find("settings")->Find("mikuMouthFit") != nullptr &&
		expResult->Find("settings")->Find("muteNativeSoundEffects")->boolValue &&
		expResult->Find("mouthFitStatus")->IsString() && expResult->Find("eyeFitStatus")->IsString(),
		"experiments.get exposes ExperimentsState shape with default-on muteNativeSoundEffects");

	f.ClearCommands();
	Check(f.Ok(f.Req("experiments.set", "{\"patch\":{\"muteNativeSoundEffects\":false}}")),
		"muteNativeSoundEffects false accepted");
	const auto* muteSeCmd = f.FindCommand(Core::GuiAction::MuteNativeSoundEffects);
	Check(muteSeCmd && !muteSeCmd->flag && !f.core.guiMuteNativeSoundEffects.load(),
		"muteNativeSoundEffects false queued and reflected");
	const auto updatedExperiments = f.ReqJson("experiments.get");
	Check(!updatedExperiments.Find("result")->Find("settings")->Find("muteNativeSoundEffects")->boolValue,
		"experiments.get reflects updated muteNativeSoundEffects");
}

static void TestDropPeek()
{
	BeginTest("drop_peek");
	Core core;
	HostHooks hooks;
	bool targetActive = false;
	int setTargetCalls = 0;
	int peekCalls = 0;
	std::vector<std::string> nextBatch;
	DropPeek syntheticPeek;
	syntheticPeek.ok = true;
	syntheticPeek.visual = "hover";
	syntheticPeek.dragPaths = {"C:\\temp\\a.vmd", "C:\\temp\\b.txt"};

	hooks.dropSetTarget = [&](bool enabled, std::string&) {
		++setTargetCalls;
		targetActive = enabled;
		return true;
	};
	hooks.dropPeek = [&] {
		++peekCalls;
		return syntheticPeek;
	};
	hooks.dropConsume = [&] {
		DropBatch batch;
		batch.ok = true;
		batch.paths = nextBatch;
		nextBatch.clear();
		syntheticPeek.pendingCount = 0;
		return batch;
	};
	Bridge bridge(core, hooks);
	bridge.OnShow();
	const auto setOk = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":1,\"type\":\"request\",\"method\":\"drop.setTarget\",\"params\":{\"enabled\":true}}"));
	Check(setOk.Find("ok")->boolValue && setTargetCalls == 0,
		"drop.setTarget never calls host outside OnUiTick");
	bridge.OnUiTick();
	Check(targetActive && peekCalls == 1, "OnUiTick refreshes drop target and peeks drag state");

	const auto response = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":2,\"type\":\"request\",\"method\":\"drop.peek\",\"params\":{}}"));
	Check(response.Find("ok")->boolValue, "drop.peek accepted");
	Check(response.Find("result")->Find("visual")->stringValue == "hover", "peek visual hover for valid .vmd");
	Check(response.Find("result")->Find("fileCount")->numberValue == 2, "peek fileCount computed");
	Check(response.Find("result")->Find("accepted")->numberValue == 1, "peek accepted count excludes .txt");
	Check(peekCalls == 1, "drop.peek serves cached Tick state without calling host outside callback");

	syntheticPeek.dragPaths = {"C:\\temp\\only.txt"};
	bridge.OnUiTick();
	const auto invalidPeek = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":3,\"type\":\"request\",\"method\":\"drop.peek\",\"params\":{}}"));
	Check(invalidPeek.Find("result")->Find("visual")->stringValue == "invalid",
		"unsupported files downgrade hover visual to invalid");

	syntheticPeek.pendingCount = 1;
	nextBatch = {"C:\\temp\\dance.vmd"};
	bridge.OnUiTick();
	const auto poll = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":4,\"type\":\"request\",\"method\":\"drop.poll\",\"params\":{}}"));
	Check(poll.Find("result")->Find("files")->Size() == 1 &&
		poll.Find("result")->Find("files")->At(0)->stringValue == "C:\\temp\\dance.vmd",
		"drop.poll drains Tick-consumed files");

	bridge.OnHide();
	bridge.OnUiTick();
	Check(!targetActive, "hiding panel disarms drop target on next OnUiTick");
}

static void TestClockAudioEffectsAndCovers()
{
	BeginTest("clock_audio_effects_and_covers");
	using namespace GakuMotion;
	using namespace GakuMotion::webpanel;
	namespace fs = std::filesystem;

	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("gaku_webpanel_clock_" + std::to_string(stamp));
	fs::create_directories(root);

	Core core;
	core.configRoot = MotionPathToUtf8(root);
	MotionEntry entry;
	entry.name = "luna-track";
	entry.title = "Luna";
	entry.dir = MotionPathToUtf8(root / "luna-track");
	entry.hasAudio = true;
	entry.audioFile = "song.flac";
	entry.hasCover = true;
	entry.coverFile = "cover.jpg";
	core.motions = {entry};

	std::string activePreset = "flat";
	HostHooks hooks;
	hooks.getAudioEffectsState = [&] {
		AudioEffectState st;
		st.available = true;
		st.activeId = activePreset;
		st.presets = {
			{"flat", "原声", "flat", "直通"},
			{"concert_arena", "演唱会现场", "live", "万人场馆"},
			{"livehouse", "LiveHouse现场", "live", "小型舞台"},
		};
		return st;
	};
	hooks.setAudioEffectPreset = [&](const std::string& id, std::string& err) {
		if (id != "flat" && id != "concert_arena" && id != "livehouse")
		{
			err = "unknown preset";
			return false;
		}
		activePreset = id;
		return true;
	};
	hooks.probeAudio = [&](const std::string& path, bool includeCover) {
		AudioProbeView p;
		p.valid = true;
		p.codec = "flac";
		p.sampleRate = 48000;
		p.channels = 2;
		p.durationSeconds = 123.5;
		p.title = "Luna";
		p.artist = "Sakiko";
		p.hasCover = includeCover;
		p.coverDataUri = includeCover ? "data:image/jpeg;base64,/9j/4AAQ" : "";
		(void)path;
		return p;
	};
	hooks.getTrackCoverDataUri = [&](const std::string& dir) {
		return dir == entry.dir ? std::string("data:image/jpeg;base64,LUNA") : std::string();
	};

	Bridge bridge(core, hooks);
	bridge.OnShow();
	const auto snap = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":1,\"type\":\"request\",\"method\":\"state.get\",\"params\":{}}"));
	Check(snap.Find("ok")->boolValue, "state.get succeeds");
	Check(snap.Find("result")->Find("clock")->Find("runMode")->stringValue == "realtime", "default runMode realtime");
	Check(snap.Find("result")->Find("audioEffect")->Find("available")->boolValue &&
		snap.Find("result")->Find("audioEffect")->Find("presets")->Size() == 3,
		"state.get exposes audioEffect presets");
	Check(snap.Find("result")->Find("library")->Find("entries")->At(0)->Find("hasCover")->boolValue &&
		snap.Find("result")->Find("library")->Find("entries")->At(0)->Find("audioFile")->stringValue == "song.flac",
		"library entry exposes hasCover and audioFile");

	const auto setRec = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":2,\"type\":\"request\",\"method\":\"clock.setRunMode\",\"params\":{\"runMode\":\"recording\"}}"));
	Check(setRec.Find("ok")->boolValue &&
		setRec.Find("result")->Find("runMode")->stringValue == "recording" &&
		setRec.Find("result")->Find("effectiveSource")->stringValue == "fixed_step",
		"clock.setRunMode switches to recording with fixed_step");

	const auto cfgClock = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":3,\"type\":\"request\",\"method\":\"clock.configure\",\"params\":{\"source\":\"engine_delta\",\"fpsNumerator\":120,\"fpsDenominator\":1}}"));
	Check(cfgClock.Find("ok")->boolValue &&
		cfgClock.Find("result")->Find("configuredSource")->stringValue == "engine_delta" &&
		cfgClock.Find("result")->Find("fpsNumerator")->numberValue == 120,
		"clock.configure updates source and fps");
	Check(fs::exists(root / "clock.json"), "clock.json persisted");

	const auto setFx = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":4,\"type\":\"request\",\"method\":\"audioEffect.set\",\"params\":{\"presetId\":\"concert_arena\"}}"));
	Check(setFx.Find("ok")->boolValue &&
		setFx.Find("result")->Find("activeId")->stringValue == "concert_arena",
		"audioEffect.set switches preset");

	const auto probeMp3 = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":5,\"type\":\"request\",\"method\":\"library.probeAudio\",\"params\":{\"path\":\"C:\\\\music\\\\song.flac\"}}"));
	Check(probeMp3.Find("ok")->boolValue &&
		probeMp3.Find("result")->Find("artist")->stringValue == "Sakiko" &&
		probeMp3.Find("result")->Find("hasCover")->boolValue,
		"library.probeAudio returns artist and cover data URI");

	const auto rejectNet = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":6,\"type\":\"request\",\"method\":\"library.probeAudio\",\"params\":{\"path\":\"https://example.com/song.mp3\"}}"));
	Check(!rejectNet.Find("ok")->boolValue && rejectNet.Find("error")->Find("code")->stringValue == "invalid_params",
		"library.probeAudio rejects network URLs");

	const auto coverRes = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":7,\"type\":\"request\",\"method\":\"library.getCover\",\"params\":{\"key\":\"luna-track\"}}"));
	Check(coverRes.Find("ok")->boolValue &&
		coverRes.Find("result")->Find("coverDataUri")->stringValue == "data:image/jpeg;base64,LUNA",
		"library.getCover resolves track cover");

	const auto probeFiles = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":8,\"type\":\"request\",\"method\":\"library.probeFiles\",\"params\":{\"paths\":[\"C:\\\\music\\\\song.mp3\"]}}"));
	Check(probeFiles.Find("ok")->boolValue &&
		probeFiles.Find("result")->Find("classification")->Find("audioCandidate")->stringValue == "C:\\music\\song.mp3" &&
		probeFiles.Find("result")->Find("classification")->Find("unsupportedAudioCount")->numberValue == 0,
		"library.probeFiles classifies .mp3 as supported audioCandidate");

	std::error_code ec;
	fs::remove_all(root, ec);
}

static void TestAudioPluginClientQueryCopiesStruct()
{
	BeginTest("audio_plugin_client_query_copies_struct");
	struct FakeHost
	{
		GakuAudioServiceV1 svc{};
		bool volumeCalled{false};
		std::string lastPreset;
	} fake{};
	fake.svc.abiVersion = GAKU_AUDIO_VERSION;
	fake.svc.structSize = sizeof(GakuAudioServiceV1);
	fake.svc.context = &fake;
	fake.svc.openAndPlay = [](void*, const char*, double) -> GakuStatus { return GAKU_OK; };
	fake.svc.pause = [](void*) -> GakuStatus { return GAKU_OK; };
	fake.svc.resumeAt = [](void*, double) -> GakuStatus { return GAKU_OK; };
	fake.svc.seek = [](void*, double, uint32_t) -> GakuStatus { return GAKU_OK; };
	fake.svc.stop = [](void*) -> GakuStatus { return GAKU_OK; };
	fake.svc.observe = [](void*, GakuAudioObservationV1* out) -> GakuStatus {
		if (!out) return GAKU_E_INVALID_ARGUMENT;
		*out = GakuAudioObservationV1{};
		out->structSize = sizeof(*out);
		return GAKU_OK;
	};
	fake.svc.setVolume = [](void* c, float) -> GakuStatus {
		static_cast<FakeHost*>(c)->volumeCalled = true;
		return GAKU_OK;
	};
	fake.svc.setPlaybackRate = [](void*, float) -> GakuStatus { return GAKU_OK; };
	fake.svc.setActivePreset = [](void* c, const char* id) -> GakuStatus {
		static_cast<FakeHost*>(c)->lastPreset = id ? id : "";
		return GAKU_OK;
	};

	GakuHostApi host{};
	host.abiVersion = GAKU_ABI_VERSION;
	host.structSize = sizeof(GakuHostApi);
	host.hostContext = &fake;
	host.queryService = [](void* hctx, const char* name, uint32_t reqVer, uint32_t outSize, void* outBuf) -> GakuStatus {
		auto* fh = static_cast<FakeHost*>(hctx);
		if (!fh || !name || std::string_view(name) != GAKU_SERVICE_AUDIO_CORE) return GAKU_E_UNAVAILABLE;
		if (reqVer != GAKU_AUDIO_VERSION) return GAKU_E_VERSION;
		if (!outBuf || outSize < sizeof(GakuAudioServiceV1)) return GAKU_E_SIZE;
		std::memcpy(outBuf, &fh->svc, sizeof(GakuAudioServiceV1));
		return GAKU_OK;
	};

	GakuMotion::audio::AudioPluginClient client;
	client.BindHost(&host);
	Check(client.Available(), "Available queries and copies GakuAudioServiceV1");
	Check(client.SetVolume(0.5f) && fake.volumeCalled, "SetVolume invokes queried service");
	Check(client.SetActivePreset("concert_arena") && fake.lastPreset == "concert_arena",
		"SetActivePreset forwards to copied service table");
}

static void TestPlayingTickThrottlingAndClockEvents()
{
	BeginTest("playing_tick_throttling_and_clock_events");
	Core core;
	core.enforceInjectionPolicy = true;
	core.injectionPolicy.SetSceneAllowed(true);
	core.injectionPolicy.SetCaptureHooksReady(true);
	core.motionsRoot = "synthetic/motions";
	int audioEffectQueries = 0;
	HostHooks hooks;
	hooks.releaseFileDialog = [] {};
	hooks.pluginVersion = "0.1.1-test";
	hooks.getAudioEffectsState = [&]() -> AudioEffectState {
		++audioEffectQueries;
		return AudioEffectState{true, "flat", {{"flat", "原声", "flat", "直通输出"}}};
	};
	hooks.setAudioEffectPreset = [&](const std::string&, std::string&) -> bool {
		return true;
	};
	Bridge bridge(core, hooks);
	std::vector<std::string> events;
	bridge.SetEventSink(
		[](void* userData, const char* json) {
			static_cast<std::vector<std::string>*>(userData)->push_back(json);
		},
		&events);
	auto countEvents = [&](const std::string& name) {
		int n = 0;
		for (const auto& e : events)
		{
			const auto v = ParseJson(e);
			const auto* ev = v.Find("name");
			if (ev && ev->stringValue == name) ++n;
		}
		return n;
	};

	bridge.OnShow();
	bridge.OnUiTick(); // baseline
	const int queriesAtBaseline = audioEffectQueries;

	core.player.state = PlaybackState::Playing;
	++core.player.evaluatedFrameSerial;
	bridge.OnUiTick();
	const int clockCountAfterFirst = countEvents("clock.changed");
	++core.player.evaluatedFrameSerial;
	bridge.OnUiTick();
	Check(countEvents("clock.changed") == clockCountAfterFirst,
		"playing evaluatedFrameSerial updates throttle clock.changed to ~10Hz");
	Check(audioEffectQueries == queriesAtBaseline,
		"playing ticks skip repeated 60Hz audioEffect preset queries");

	bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":99,\"type\":\"request\",\"method\":\"clock.configure\",\"params\":{\"source\":\"engine_delta\"}}");
	bridge.OnUiTick();
	Check(countEvents("clock.changed") == clockCountAfterFirst + 1,
		"structural clock.configure emits clock.changed immediately even while playing");
}

namespace Bug5TestFixture
{
	inline std::filesystem::path motionsRoot;
	inline bool importStarted = false;
	inline bool importReady = false;
	inline void WriteTinyClip(const std::filesystem::path& path)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		const auto write = [&](const auto& value) { file.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
		file.write("GKCLIPCL", 8);
		write(uint32_t{4}); write(60.0f); write(uint32_t{2}); write(2.0f / 60.0f); write(uint32_t{1}); write(.083f);
		write(uint16_t{0}); write(uint8_t{4}); write(uint8_t{0}); file.write("Hips", 4);
		for (uint32_t frame = 0; frame < 2; ++frame)
		{
			write(GakuMotion::Vector3{});
			write(GakuMotion::Quaternion::Identity());
		}
		write(uint8_t{0}); write(uint32_t{0});
	}
	inline bool StartImportFn(const GakuMotion::MotionImportRequest&, std::string&)
	{
		importStarted = true;
		std::filesystem::create_directories(motionsRoot / "Gimme_Gimme-70f876a6");
		WriteTinyClip(motionsRoot / "Gimme_Gimme-70f876a6" / "motion.gkclip");
		std::ofstream info(motionsRoot / "Gimme_Gimme-70f876a6" / "info.json", std::ios::binary | std::ios::trunc);
		info << "{\"title\": \"Gimme×Gimme\", \"fps\": 60}\n";
		importReady = true;
		return true;
	}
	inline int PollImportFn(std::string& message, GakuMotion::ImportProgressState* progress)
	{
		if (!importReady) return 0;
		message = "导入完成";
		if (progress) { progress->progress = 1.0f; progress->statusText = "导入完成"; }
		return 1;
	}
}

static void TestCoverEditingScreenshotAndDuplicateTitleImport()
{
	BeginTest("cover_editing_screenshot_and_duplicate_title_import");
	using namespace GakuMotion;
	using namespace GakuMotion::webpanel;
	namespace fs = std::filesystem;

	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("gaku_webpanel_bug5_" + std::to_string(stamp));
	const fs::path motionsRoot = root / "motions";
	const fs::path logsRoot = root / "logs";
	fs::create_directories(motionsRoot / "Gimme x Gimme");
	fs::create_directories(logsRoot);

	Bug5TestFixture::motionsRoot = motionsRoot;
	Bug5TestFixture::importStarted = false;
	Bug5TestFixture::importReady = false;
	Bug5TestFixture::WriteTinyClip(motionsRoot / "Gimme x Gimme" / "motion.gkclip");
	{
		std::ofstream info(motionsRoot / "Gimme x Gimme" / "info.json", std::ios::binary | std::ios::trunc);
		info << "{\"title\": \"Gimme×Gimme\", \"fps\": 60}\n";
	}

	const std::string pngDataUri =
		"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=";

	Core core;
	core.enforceInjectionPolicy = true;
	core.injectionPolicy.SetSceneAllowed(true);
	core.injectionPolicy.SetCaptureHooksReady(true);
	core.motionsRoot = MotionPathToUtf8(motionsRoot);
	core.logsRoot = MotionPathToUtf8(logsRoot);
	core.playlistPath = MotionPathToUtf8(root / "playlist.json");
	core.RefreshMotionLibrary();
	Check(core.motions.size() == 1 && core.motions[0].name == "Gimme x Gimme",
		"initial duplicate-title baseline track scanned");

	core.fnStartImport = Bug5TestFixture::StartImportFn;
	core.fnPollImportProgress = Bug5TestFixture::PollImportFn;

	HostHooks hooks;
	hooks.releaseFileDialog = [] {};
	hooks.readImagePreviewDataUri = [&](const std::string& p) { return p.empty() ? std::string() : pngDataUri; };
	hooks.getScreenshotCapabilities = []() {
		ScreenshotCapabilitiesView v;
		v.available = true; v.runtimeReady = true; v.cameraAvailable = true;
		v.windowWidth = 1280; v.windowHeight = 720;
		return v;
	};
	hooks.captureScreenshot = [&]() {
		ScreenshotCaptureView c;
		c.ok = true; c.width = 1280; c.height = 720; c.dataUri = pngDataUri;
		return c;
	};
	hooks.getTrackCoverDataUri = [&](const std::string& dir) {
		std::error_code ec;
		return fs::is_regular_file(MotionPathFromUtf8(dir) / "cover.png", ec) ? pngDataUri : std::string();
	};

	Bridge bridge(core, hooks);
	std::vector<std::string> events;
	bridge.SetEventSink(
		[](void* ud, const char* json) { static_cast<std::vector<std::string>*>(ud)->push_back(json); },
		&events);
	bridge.OnShow();
	bridge.OnUiTick();

	// 1. Screenshot capabilities & capture
	const auto caps = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":1,\"type\":\"request\",\"method\":\"screenshot.capabilities\",\"params\":{}}"));
	Check(caps.Find("ok")->boolValue && caps.Find("result")->Find("cameraAvailable")->boolValue,
		"screenshot.capabilities reports cameraAvailable");
	const auto shot = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":2,\"type\":\"request\",\"method\":\"screenshot.capture\",\"params\":{}}"));
	Check(shot.Find("ok")->boolValue && shot.Find("result")->Find("dataUri")->stringValue == pngDataUri,
		"screenshot.capture returns encoded dataUri");
	const auto preview = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":3,\"type\":\"request\",\"method\":\"library.readImagePreview\",\"params\":{\"path\":\"C:\\\\img.png\"}}"));
	Check(preview.Find("ok")->boolValue && preview.Find("result")->Find("dataUri")->stringValue == pngDataUri,
		"library.readImagePreview returns dataUri");

	const fs::path synthVmd = root / "dance.vmd";
	WriteSyntheticVmd(synthVmd, 1, 0, 0);

	// 2. Import with duplicate title + custom coverDataUri + .mp3 audio
	const std::string importReq =
		"{\"v\":1,\"id\":4,\"type\":\"request\",\"method\":\"library.import\",\"params\":{\"request\":{"
		"\"title\":\"Gimme×Gimme\",\"motionFile\":" + JString(MotionPathToUtf8(synthVmd)) +
		",\"facialFile\":\"（自动：沿用动作 VMD 内表情）\","
		"\"cameraFile\":\"（留空）\",\"audioFile\":\"C:\\\\song.mp3\",\"disableMorph\":false,\"clearCamera\":false,"
		"\"clearAudio\":false,\"fps\":0,\"coverDataUri\":\"" + pngDataUri + "\","
		"\"timing\":{\"motion\":{\"speed\":1,\"offsetSeconds\":0},\"facial\":{\"speed\":1,\"offsetSeconds\":0},"
		"\"camera\":{\"speed\":1,\"offsetSeconds\":0},\"audio\":{\"speed\":1,\"offsetSeconds\":0}}}}}";
	const auto imp = ParseJson(bridge.HandleMessageForTest(importReq));
	Check(imp.Find("ok")->boolValue, "library.import with mp3 and coverDataUri accepted");

	core.ProcessPendingLibraryCommands(); // starts import
	core.ProcessPendingLibraryCommands(); // polls completion
	bridge.OnUiTick();

	Check(Bug5TestFixture::importStarted && core.motions.size() == 2, "new duplicate-title track added alongside existing entry");
	Check(core.lastImportedKey == "Gimme_Gimme-70f876a6", "lastImportedKey resolves exact new folder id");
	Check(core.loadedMotionKey == "Gimme_Gimme-70f876a6",
		"PollImport auto-selects newly imported folder id instead of duplicate-title old entry");
	const int newIdx = FindPlaylistIndex(core.motions, "Gimme_Gimme-70f876a6");
	Check(newIdx >= 0 && core.motions[newIdx].hasCover && core.motions[newIdx].coverFile == "cover.png",
		"imported track persisted cover.png and hasCover=true");
	Check(fs::is_regular_file(motionsRoot / "Gimme_Gimme-70f876a6" / "cover.png"),
		"cover.png written to newly imported track folder");

	// Check import.progress event carries importedKey
	bool sawImportedKeyEvent = false;
	for (const auto& evJson : events)
	{
		const auto ev = ParseJson(evJson);
		if (ev.Find("name") && ev.Find("name")->stringValue == "import.progress")
		{
			const auto* payload = ev.Find("payload");
			if (payload && payload->Find("importedKey") && payload->Find("importedKey")->stringValue == "Gimme_Gimme-70f876a6")
				sawImportedKeyEvent = true;
		}
	}
	Check(sawImportedKeyEvent, "import.progress event includes importedKey");

	// 3. Direct library.setCover set & clear on old entry
	const uint64_t revBeforeCover = core.libraryRevision;
	const auto setCov = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":5,\"type\":\"request\",\"method\":\"library.setCover\","
		"\"params\":{\"key\":\"Gimme x Gimme\",\"coverDataUri\":\"" + pngDataUri + "\"}}"));
	Check(setCov.Find("ok")->boolValue && setCov.Find("result")->Find("hasCover")->boolValue,
		"library.setCover saves cover.png");
	Check(core.libraryRevision == revBeforeCover + 1, "library.setCover bumps libraryRevision");
	Check(fs::is_regular_file(motionsRoot / "Gimme x Gimme" / "cover.png"),
		"library.setCover writes cover.png to track dir");

	const auto clrCov = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":6,\"type\":\"request\",\"method\":\"library.setCover\","
		"\"params\":{\"key\":\"Gimme x Gimme\",\"clear\":true}}"));
	Check(clrCov.Find("ok")->boolValue && !clrCov.Find("result")->Find("hasCover")->boolValue,
		"library.setCover clear removes cover");
	Check(!fs::exists(motionsRoot / "Gimme x Gimme" / "cover.png"),
		"library.setCover clear deletes cover.png");

	std::error_code ec;
	fs::remove_all(root, ec);
}

static void TestM4PortraitsAndFormation()
{
	BeginTest("m4_portraits_and_formation");
	Fixture f;
	int a1 = 1, a2 = 2;
	f.core.PublishSceneActors({
		{&a1, 101, "hski", "花海咲季", "chr_hski_face", ""},
		{&a2, 102, "ttmr", "月村手毬", "chr_ttmr_face", ""},
	});
	MotionEntry group;
	group.name = "grp";
	group.title = "Group";
	group.dir = "synthetic/motions/grp";
	group.durationSeconds = 5.0f;
	group.totalFrames = 150;
	group.isGroup = true;
	group.positionCount = 2;
	group.positions = {
		{1, 0.0f, 0.0f, 0.0f, 0.0f, true, true},
		{2, 0.5f, 0.0f, -0.25f, 15.0f, true, false},
	};
	f.core.motions = {group};

	const auto snap = f.ReqJson("state.get");
	Check(f.Ok(f.Req("state.get")), "m4 state.get ok");
	const auto* res = snap.Find("result");
	const auto* actorsArr = res && res->Find("actors") ? res->Find("actors")->Find("actors") : nullptr;
	Check(actorsArr && actorsArr->IsArray() && actorsArr->Size() == 2 &&
		actorsArr->At(0)->Find("character")->stringValue == "hski" &&
		actorsArr->At(0)->Find("shortName")->stringValue == "咲季",
		"SceneActor character/shortName serialized");
	const auto* entriesArr = res && res->Find("library") ? res->Find("library")->Find("entries") : nullptr;
	Check(entriesArr && entriesArr->IsArray() && entriesArr->Size() == 1 &&
		entriesArr->At(0)->Find("isGroup")->boolValue &&
		entriesArr->At(0)->Find("positions")->Size() == 2,
		"LibraryEntry group positions serialized");
	const auto* camSettings = res && res->Find("camera") ? res->Find("camera")->Find("settings") : nullptr;
	Check(camSettings && camSettings->Find("followSlotIndex") &&
		camSettings->Find("followSlotIndex")->numberValue == -1.0,
		"CameraSettings.followSlotIndex defaults to -1");
	Check(!f.Ok(f.Req("camera.set", "{\"settings\":{\"followSlotIndex\":-2}}")),
		"camera.set rejects followSlotIndex < -1");
	Check(!f.Ok(f.Req("actors.getPortrait", "{\"character\":\"../evil\"}")),
		"actors.getPortrait rejects traversal");
	const auto npcPortrait = f.ReqJson("actors.getPortrait", "{\"character\":\"uncached_npc\"}");
	Check(npcPortrait.Find("ok")->boolValue && !npcPortrait.Find("result")->Find("cached")->boolValue,
		"actors.getPortrait misses cleanly when uncached");

	const auto* fm = res ? res->Find("formation") : nullptr;
	Check(fm && fm->Find("slotCount")->numberValue == 2.0, "formation snapshot present with 2 slots");
	Check(f.Ok(f.Req("formation.selectSlot", "{\"slotIndex\":1}")), "formation.selectSlot ok");
	Check(f.Ok(f.Req("formation.setSlotOffset", "{\"slotIndex\":1,\"x\":0.75,\"y\":0,\"z\":-0.5,\"yawDegrees\":30}")),
		"formation.setSlotOffset ok");
	f.core.ProcessPendingGuiCommands();
	f.bridge->OnUiTick();
	bool sawFormationEvent = false;
	for (const auto& evText : f.events)
	{
		const auto ev = ParseJson(evText);
		if (ev.Find("name") && ev.Find("name")->stringValue == "formation.changed") sawFormationEvent = true;
	}
	Check(sawFormationEvent, "formation.changed emitted on tick");
	Check(!f.Ok(f.Req("formation.swapSlots", "{\"fromSlotIndex\":0,\"toSlotIndex\":99}")),
		"formation.swapSlots rejects out-of-range index");
}

static void TestAsyncTrackCoverCache()
{
	BeginTest("async_track_cover_cache");
	Core core;
	core.enforceInjectionPolicy = true;
	core.injectionPolicy.SetSceneAllowed(true);
	core.injectionPolicy.SetCaptureHooksReady(true);

	MotionEntry withCover;
	withCover.name = "track-with-cover";
	withCover.dir = "synthetic/dir-with-cover";
	withCover.hasCover = true;
	withCover.hasAudio = true;

	MotionEntry noCover;
	noCover.name = "track-no-cover";
	noCover.dir = "synthetic/dir-no-cover";
	noCover.hasAudio = true;

	core.motions.push_back(withCover);
	core.motions.push_back(noCover);
	core.libraryRevision = 1;

	std::atomic<int> resolveCalls{0};
	TrackCoverCache cache;
	cache.Configure([&](const std::string& dir) -> std::string {
		resolveCalls.fetch_add(1, std::memory_order_relaxed);
		if (dir == "synthetic/dir-with-cover")
			return "data:image/png;base64,AAA=";
		return "";
	});

	HostHooks hooks;
	hooks.releaseFileDialog = [] {};
	hooks.queryTrackCoverAsync = [&](const std::string& key, const std::string& dir,
									 std::string& outDataUri, bool& outPending) -> bool {
		return cache.QueryOrEnqueue(key, dir, outDataUri, outPending);
	};
	hooks.drainReadyCovers = [&](size_t maxCount) -> std::vector<AsyncTrackCoverResult> {
		const auto items = cache.DrainCompleted(maxCount);
		std::vector<AsyncTrackCoverResult> out;
		out.reserve(items.size());
		for (const auto& item : items)
		{
			out.push_back(AsyncTrackCoverResult{item.key, item.hasCover, item.coverDataUri});
		}
		return out;
	};
	hooks.invalidateTrackCover = [&](const std::string& key) {
		if (key.empty()) cache.InvalidateAll();
		else cache.Invalidate(key);
	};

	Bridge bridge(core, hooks);
	std::vector<std::string> events;
	bridge.SetEventSink(
		[](void* userData, const char* json) {
			static_cast<std::vector<std::string>*>(userData)->push_back(json);
		},
		&events);
	bridge.OnShow();

	const auto hello = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":1,\"type\":\"request\",\"method\":\"motion.hello\",\"params\":{\"client\":\"test\",\"protocol\":1}}"));
	const auto* supportedEv = hello.Find("result") ? hello.Find("result")->Find("supportedEvents") : nullptr;
	bool advertisesCoverReady = false;
	if (supportedEv && supportedEv->IsArray())
	{
		for (size_t i = 0; i < supportedEv->Size(); ++i)
			if (supportedEv->At(i)->stringValue == "library.coverReady") advertisesCoverReady = true;
	}
	Check(advertisesCoverReady, "motion.hello advertises library.coverReady");

	// 1. Positive lookup returns pending=true immediately, then emits library.coverReady.
	const auto firstMiss = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":2,\"type\":\"request\",\"method\":\"library.getCover\",\"params\":{\"key\":\"track-with-cover\"}}"));
	Check(firstMiss.Find("ok")->boolValue &&
		firstMiss.Find("result")->Find("pending")->boolValue &&
		!firstMiss.Find("result")->Find("hasCover")->boolValue,
		"uncached cover returns pending=true immediately");

	cache.WaitForIdleForTest();
	events.clear();
	bridge.OnUiTick();

	bool sawWithCoverReady = false;
	for (const auto& evText : events)
	{
		const auto ev = ParseJson(evText);
		if (ev.Find("name") && ev.Find("name")->stringValue == "library.coverReady")
		{
			const auto* d = ev.Find("payload");
			if (d && d->Find("key")->stringValue == "track-with-cover" &&
				d->Find("hasCover")->boolValue &&
				d->Find("coverDataUri")->stringValue == "data:image/png;base64,AAA=")
			{
				sawWithCoverReady = true;
			}
		}
	}
	Check(sawWithCoverReady, "OnUiTick emits library.coverReady for completed cover");

	const auto firstHit = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":3,\"type\":\"request\",\"method\":\"library.getCover\",\"params\":{\"key\":\"track-with-cover\"}}"));
	Check(firstHit.Find("ok")->boolValue &&
		!firstHit.Find("result")->Find("pending")->boolValue &&
		firstHit.Find("result")->Find("hasCover")->boolValue &&
		firstHit.Find("result")->Find("coverDataUri")->stringValue == "data:image/png;base64,AAA=",
		"second cover lookup returns cached data URI synchronously");
	const int callsAfterPositive = resolveCalls.load();

	// 2. Negative lookup is also cached so repeated requests do not re-probe.
	const auto negMiss = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":4,\"type\":\"request\",\"method\":\"library.getCover\",\"params\":{\"key\":\"track-no-cover\"}}"));
	Check(negMiss.Find("ok")->boolValue && negMiss.Find("result")->Find("pending")->boolValue,
		"uncached track without cover starts as pending=true");
	cache.WaitForIdleForTest();
	events.clear();
	bridge.OnUiTick();
	bool sawNoCoverReady = false;
	for (const auto& evText : events)
	{
		const auto ev = ParseJson(evText);
		if (ev.Find("name") && ev.Find("name")->stringValue == "library.coverReady")
		{
			const auto* d = ev.Find("payload");
			if (d && d->Find("key")->stringValue == "track-no-cover" &&
				!d->Find("hasCover")->boolValue &&
				d->Find("coverDataUri")->stringValue.empty())
			{
				sawNoCoverReady = true;
			}
		}
	}
	Check(sawNoCoverReady, "library.coverReady reports negative cache result");

	const auto negHit = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":5,\"type\":\"request\",\"method\":\"library.getCover\",\"params\":{\"key\":\"track-no-cover\"}}"));
	Check(negHit.Find("ok")->boolValue &&
		!negHit.Find("result")->Find("pending")->boolValue &&
		!negHit.Find("result")->Find("hasCover")->boolValue,
		"repeated lookup for missing cover returns cached negative hit");
	Check(resolveCalls.load() == callsAfterPositive + 1,
		"negative cache avoids redundant resolver invocations");

	// 3. library.refresh invalidates cached entries.
	bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":6,\"type\":\"request\",\"method\":\"library.refresh\",\"params\":{}}");
	const auto afterRefresh = ParseJson(bridge.HandleMessageForTest(
		"{\"v\":1,\"id\":7,\"type\":\"request\",\"method\":\"library.getCover\",\"params\":{\"key\":\"track-with-cover\"}}"));
	Check(afterRefresh.Find("ok")->boolValue && afterRefresh.Find("result")->Find("pending")->boolValue,
		"library.refresh invalidates cached cover entries");
	cache.WaitForIdleForTest();
	cache.Stop();
}

static void WriteRootOriginVmd(const std::filesystem::path& path, uint32_t extraFillerBones,
	bool includeParentRotation, uint32_t morphFrames, uint32_t cameraFrames)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	char header[30]{};
	std::memcpy(header, "Vocaloid Motion Data 0002", 25);
	out.write(header, 30);
	char model[20]{};
	out.write(model, 20);

	auto writeBoneRecord = [&](const char* nameBytes, size_t nameLen, uint32_t frame,
		float px, float py, float pz, float rx, float ry, float rz, float rw) {
		char rec[111]{};
		std::memcpy(rec, nameBytes, (std::min)(nameLen, size_t{15}));
		std::memcpy(rec + 15, &frame, 4);
		float pos[3]{px, py, pz};
		float rot[4]{rx, ry, rz, rw};
		std::memcpy(rec + 19, pos, 12);
		std::memcpy(rec + 31, rot, 16);
		out.write(rec, 111);
	};

	const uint32_t rootRecordCount = 3 + (includeParentRotation ? 1u : 0u);
	const uint32_t boneCount = extraFillerBones + rootRecordCount;
	out.write(reinterpret_cast<const char*>(&boneCount), 4);

	// Out-of-order later frame before frame 0, to verify min-frame selection.
	writeBoneRecord("\x83\x5A\x83\x93\x83\x5E\x81\x5B", 8, 12, 99.0f, 99.0f, 99.0f, 0.0f, 0.0f, 0.0f, 1.0f);
	for (uint32_t i = 0; i < extraFillerBones; ++i)
	{
		writeBoneRecord("UpperBody", 9, i, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
	}
	if (includeParentRotation)
	{
		// Rotate +90 deg around Y in MMD (sin(45) = 0.70710678, cos(45) = 0.70710678)
		const float s = 0.70710678f;
		writeBoneRecord("\x91\x53\x82\xC4\x82\xCC\x90\x65", 8, 0, 2.0f, 0.0f, 0.0f, 0.0f, s, 0.0f, s);
	}
	// センター at frame 0: (10, 2, -5) in MMD
	writeBoneRecord("\x83\x5A\x83\x93\x83\x5E\x81\x5B", 8, 0, 10.0f, 2.0f, -5.0f, 0.0f, 0.0f, 0.0f, 1.0f);
	// グルーブ at frame 0: (0, 1, 0) in MMD
	writeBoneRecord("\x83\x4F\x83\x8B\x81\x5B\x83\x75", 8, 0, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);

	out.write(reinterpret_cast<const char*>(&morphFrames), 4);
	for (uint32_t i = 0; i < morphFrames; ++i)
	{
		char mrec[23]{};
		std::memcpy(mrec, "blink", 5);
		out.write(mrec, 23);
	}
	out.write(reinterpret_cast<const char*>(&cameraFrames), 4);
	for (uint32_t i = 0; i < cameraFrames; ++i)
	{
		char crec[61]{};
		out.write(crec, 61);
	}
}

static void TestPerSlotTimingAndRootOriginProbe()
{
	BeginTest("per_slot_timing_and_root_origin_probe");
	namespace fs = std::filesystem;
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("gaku_webpanel_slot_timing_" + std::to_string(stamp));
	fs::create_directories(root);

	const fs::path vmdLarge = root / "large_center.vmd";
	const fs::path vmdRotated = root / "rotated_parent.vmd";
	const fs::path vmdFacial = root / "facial_only.vmd";
	const fs::path vmdCam = root / "cam_only.vmd";
	WriteRootOriginVmd(vmdLarge, 5000, false, 3, 0);
	WriteRootOriginVmd(vmdRotated, 10, true, 0, 0);
	WriteSyntheticVmd(vmdFacial, 0, 2, 0);
	WriteSyntheticVmd(vmdCam, 0, 0, 4);

	Fixture f;
	const std::string probePaths = "[" + JString(MotionPathToUtf8(vmdLarge)) + "," +
		JString(MotionPathToUtf8(vmdRotated)) + "," + JString(MotionPathToUtf8(vmdFacial)) + "," +
		JString(MotionPathToUtf8(vmdCam)) + "]";
	const auto probeRes = f.ReqJson("library.probeFiles", "{\"paths\":" + probePaths + "}");
	Check(probeRes.Find("ok") && probeRes.Find("ok")->boolValue, "probeFiles succeeds for rootOrigin VMDs");
	const auto* probes = probeRes.Find("result") ? probeRes.Find("result")->Find("probes") : nullptr;
	Check(probes && probes->Size() == 4, "4 probes returned");
	if (probes && probes->Size() == 4)
	{
		const auto* p0 = probes->At(0);
		Check(p0->Find("hasRootOrigin") && p0->Find("hasRootOrigin")->boolValue &&
			p0->Find("morphFrames") && p0->Find("morphFrames")->numberValue == 3.0 &&
			p0->Find("bodyBoneFrames") && p0->Find("bodyBoneFrames")->numberValue == 5003.0,
			"large >4096-frame VMD scans all bones and reads trailing morphFrames");
		const auto* r0 = p0->Find("rootOrigin");
		Check(r0 &&
			std::fabs(r0->Find("x")->numberValue - (-10.0 * 0.07795)) < 1e-4 &&
			std::fabs(r0->Find("y")->numberValue - (3.0 * 0.07795)) < 1e-4 &&
			std::fabs(r0->Find("z")->numberValue - (5.0 * 0.07795)) < 1e-4,
			"frame-0 Center + Groove converts to Unity rootOrigin * 0.07795 (ignoring later frame 12)");

		const auto* p1 = probes->At(1);
		const auto* r1 = p1->Find("rootOrigin");
		// 全ての親 MMD pos=(2,0,0) -> Unity (-2,0,0); +90Y rot rotates Center Unity (-10,2,5) -> (5,2,10); total=(3,3,10)*0.07795
		Check(p1->Find("hasRootOrigin")->boolValue && r1 &&
			std::fabs(r1->Find("x")->numberValue - (3.0 * 0.07795)) < 1e-3 &&
			std::fabs(r1->Find("y")->numberValue - (3.0 * 0.07795)) < 1e-3 &&
			std::fabs(r1->Find("z")->numberValue - (10.0 * 0.07795)) < 1e-3,
			"rotated parent hierarchy composes Center translation into Unity rootOrigin");

		const auto* p2 = probes->At(2);
		const auto* p3 = probes->At(3);
		Check(p2->Find("hasRootOrigin") && !p2->Find("hasRootOrigin")->boolValue &&
			p3->Find("hasRootOrigin") && !p3->Find("hasRootOrigin")->boolValue,
			"facial-only and camera-only VMDs report hasRootOrigin=false");
	}

	// library.import parses per-position timing and hasExplicitPlacement, and syncs slot 1 into request.timing
	f.ClearCommands();
	const std::string groupImportReq = "{\"request\":{\"title\":\"PerSlot\",\"editKey\":\"\","
		"\"fps\":30,\"clearAudio\":true,"
		"\"timing\":{\"motion\":{\"speed\":1.0,\"offsetSeconds\":0.0},\"facial\":{\"speed\":1.0,\"offsetSeconds\":0.0},"
		"\"camera\":{\"speed\":1.25,\"offsetSeconds\":0.5},\"audio\":{\"speed\":0.9,\"offsetSeconds\":-0.25}},"
		"\"positions\":["
		"{\"slot\":1,\"motionFile\":" + JString(MotionPathToUtf8(vmdLarge)) +
		",\"x\":0,\"y\":0,\"z\":0,\"yawDegrees\":0,\"hasExplicitPlacement\":false,"
		"\"timing\":{\"motion\":{\"speed\":1.5,\"offsetSeconds\":0.4},\"facial\":{\"speed\":0.8,\"offset_seconds\":-0.2}}},"
		"{\"slot\":2,\"motionFile\":" + JString(MotionPathToUtf8(vmdRotated)) +
		",\"x\":1.25,\"y\":0,\"z\":-0.5,\"yawDegrees\":30,\"hasExplicitPlacement\":true,"
		"\"timing\":{\"motion\":{\"speed\":0.75,\"offset_seconds\":1.2},\"facial\":{\"speed\":2.0,\"offsetSeconds\":0.1}}},"
		"{\"slot\":3,\"motionFile\":" + JString(MotionPathToUtf8(vmdLarge)) +
		",\"x\":-1.25,\"y\":0,\"z\":-0.5,\"yawDegrees\":-30}"
		"]}}";
	Check(f.Ok(f.Req("library.import", groupImportReq)), "group library.import with per-slot timing accepted");
	const auto* cmd = f.FindCommand(Core::GuiAction::Import);
	Check(cmd && cmd->import.positions.size() == 3, "3 import position slots queued");
	if (cmd && cmd->import.positions.size() == 3)
	{
		const auto& s1 = cmd->import.positions[0];
		const auto& s2 = cmd->import.positions[1];
		const auto& s3 = cmd->import.positions[2];
		Check(s1.hasTiming && !s1.hasExplicitPlacement &&
			std::fabs(s1.motionTiming.speed - 1.5f) < 1e-5f && std::fabs(s1.motionTiming.offsetSeconds - 0.4f) < 1e-5f &&
			std::fabs(s1.facialTiming.speed - 0.8f) < 1e-5f && std::fabs(s1.facialTiming.offsetSeconds - (-0.2f)) < 1e-5f,
			"slot 1 per-slot timing and hasExplicitPlacement parsed");
		Check(s2.hasTiming && s2.hasExplicitPlacement &&
			std::fabs(s2.motionTiming.speed - 0.75f) < 1e-5f && std::fabs(s2.motionTiming.offsetSeconds - 1.2f) < 1e-5f &&
			std::fabs(s2.facialTiming.speed - 2.0f) < 1e-5f && std::fabs(s2.facialTiming.offsetSeconds - 0.1f) < 1e-5f,
			"slot 2 per-slot timing and hasExplicitPlacement=true parsed");
		Check(s3.hasTiming &&
			std::fabs(s3.motionTiming.speed - 1.0f) < 1e-5f && std::fabs(s3.facialTiming.speed - 1.0f) < 1e-5f,
			"slot 3 on new import inherits top-level timing when timingValue supplied");
		Check(std::fabs(cmd->import.timing.motion.speed - 1.5f) < 1e-5f &&
			std::fabs(cmd->import.timing.motion.offsetSeconds - 0.4f) < 1e-5f &&
			std::fabs(cmd->import.timing.facial.speed - 0.8f) < 1e-5f &&
			std::fabs(cmd->import.timing.camera.speed - 1.25f) < 1e-5f &&
			std::fabs(cmd->import.timing.audio.speed - 0.9f) < 1e-5f,
			"positions[0] timing syncs into request.timing motion/facial while camera & audio remain global");
	}

	// Invalid slot timing rejected
	const std::string badSlotTimingReq = "{\"request\":{\"title\":\"BadSlotTiming\",\"editKey\":\"\","
		"\"fps\":30,"
		"\"positions\":[{\"slot\":1,\"motionFile\":" + JString(MotionPathToUtf8(vmdLarge)) +
		",\"timing\":{\"motion\":{\"speed\":-0.5,\"offsetSeconds\":0}}}]}}";
	Check(f.ErrorCode(f.Req("library.import", badSlotTimingReq)) == "invalid_params",
		"negative per-slot speed rejected");

	// Edit mode: omitted slot timing keeps hasTiming=false even if top-level timing is supplied
	f.AddSyntheticLibrary();
	f.core.motions[0].isGroup = true;
	f.core.motions[0].positionCount = 2;
	f.core.motions[0].timing.motion = {1.1f, 0.2f};
	f.core.motions[0].timing.facial = {1.2f, -0.3f};
	MotionPositionEntry pe1{1, 0, 0, 0, 0, true, false};
	pe1.motionTiming = {1.4f, 0.5f}; pe1.facialTiming = {0.9f, 0.1f}; pe1.hasTiming = true;
	MotionPositionEntry pe2{2, 1, 0, 0, 45, true, true};
	pe2.motionTiming = {0.6f, 2.0f}; pe2.facialTiming = {1.8f, -0.5f}; pe2.hasTiming = true;
	MotionPositionEntry pe3{3, -1, 0, 0, -45, true, false}; // hasTiming=false -> fallback to track timing
	f.core.motions[0].positions = {pe1, pe2, pe3};

	f.ClearCommands();
	const std::string editSlotReq = "{\"request\":{\"title\":\"Alpha\",\"editKey\":\"alpha\","
		"\"fps\":0,\"timing\":{\"camera\":{\"speed\":1.5,\"offsetSeconds\":0}},"
		"\"positions\":[{\"slot\":1,\"preserveExisting\":true,"
		"\"timing\":{\"motion\":{\"speed\":2.0,\"offsetSeconds\":0.3},\"facial\":{\"speed\":1.0,\"offsetSeconds\":0}}},"
		"{\"slot\":2,\"preserveExisting\":true}]}}";
	Check(f.Ok(f.Req("library.import", editSlotReq)), "edit with selective slot timing accepted");
	const auto* editCmd = f.FindCommand(Core::GuiAction::Import);
	Check(editCmd && editCmd->import.positions.size() == 2 &&
		editCmd->import.positions[0].hasTiming && !editCmd->import.positions[1].hasTiming,
		"edit mode sets hasTiming=true for explicit slot and false for omitted slot");

	// library.startEdit and state.get return per-slot timing in positions[]
	const auto startEdit = f.ReqJson("library.startEdit", "{\"key\":\"alpha\",\"revision\":5}");
	Check(startEdit.Find("ok") && startEdit.Find("ok")->boolValue, "startEdit succeeds for group track");
	const auto* editPos = startEdit.Find("result") ? startEdit.Find("result")->Find("positions") : nullptr;
	Check(editPos && editPos->Size() == 3, "startEdit returns 3 positions");
	if (editPos && editPos->Size() == 3)
	{
		const auto* t1 = editPos->At(1)->Find("timing");
		const auto* t2 = editPos->At(2)->Find("timing");
		Check(t1 && std::fabs(t1->Find("motion")->Find("speed")->numberValue - 0.6) < 1e-4 &&
			std::fabs(t1->Find("motion")->Find("offsetSeconds")->numberValue - 2.0) < 1e-4 &&
			std::fabs(t1->Find("facial")->Find("speed")->numberValue - 1.8) < 1e-4,
			"startEdit serializes explicit slot 2 timing");
		Check(t2 && std::fabs(t2->Find("motion")->Find("speed")->numberValue - 1.1) < 1e-4 &&
			std::fabs(t2->Find("facial")->Find("speed")->numberValue - 1.2) < 1e-4,
			"startEdit serializes fallback track timing for slot without explicit timing");
	}
	const auto stateSnap = f.ReqJson("state.get");
	const auto* libPos = stateSnap.Find("result")->Find("library")->Find("entries")->At(0)->Find("positions");
	Check(libPos && libPos->Size() == 3 &&
		libPos->At(1)->Find("timing") != nullptr &&
		std::fabs(libPos->At(1)->Find("timing")->Find("motion")->Find("speed")->numberValue - 0.6) < 1e-4,
		"state.get library positions[] exposes per-slot timing");

	std::error_code ec;
	fs::remove_all(root, ec);
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	TestWireFraming();
	TestHelloCapabilities();
	TestStateSnapshot();
	TestTransportQueue();
	TestPermissionGate();
	TestLibraryGuards();
	TestCameraStage();
	TestFacialPositionSettings();
	TestActorsToken();
	TestBufferContract();
	TestEventsEnvelope();
	TestDialogHostService();
	TestShutdownInvalidation();
	TestScrubResetOnPermissionLoss();
	TestGetAssetsCandidates();
	TestLibraryImportProbe();
	TestStageNodes();
	TestBoneOverrides();
	TestGazeSettings();
	TestSoleStudio();
	TestExperimentsTelemetry();
	TestDropPeek();
	TestClockAudioEffectsAndCovers();
	TestAudioPluginClientQueryCopiesStruct();
	TestPlayingTickThrottlingAndClockEvents();
	TestCoverEditingScreenshotAndDuplicateTitleImport();
	TestM4PortraitsAndFormation();
	TestAsyncTrackCoverCache();
	TestPerSlotTimingAndRootOriginProbe();
	std::printf("\nweb-panel bridge contracts: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
