#pragma once

static void TestCameraCoreWorkflow(const std::string& repoRoot)
{
	using namespace GakuMotion;
	g_current = "camera ordered GUI/save/import integration";
	Core core;
	core.configRoot = repoRoot + "/build/tests/camera-core";
	CameraSettings settings = core.camera.settings;
	settings.mode = CameraMode::Follow;
	settings.limitToScene = true;
	settings.boundsMin = {-2, 0, -3};
	settings.boundsMax = {2, 4, 3};
	settings.followOffset = {0, .7f, 2.5f};
	settings.vmdOffset = {0.1f, -0.2f, 0.3f};
	settings.followFactor = {0.5f, 0.75f, 1.0f};
	settings.followRate = {0.0f, 0.1f, 1.0f};
	settings.followTarget = CameraFollowTarget::Head;
	settings.followFov = 51;
	settings.vmdRangeScale = 0.75f;
	settings.followPlacementYaw = true;
	core.QueueCameraSettings(settings);
	core.QueueGui({Core::GuiAction::SaveCameraSettings});
	Check(core.camera.settings.mode == CameraMode::Track,
	      "render queue does not mutate camera before Unity consumes it");
	core.ProcessPendingGuiCommands();
	Check(core.camera.settings == settings, "ordered camera update precedes save");
	Core reopened;
	reopened.configRoot = core.configRoot;
	Check(reopened.LoadCameraSettings() && reopened.camera.settings == settings,
	      "camera settings survive restart without changing control.json");
	Check(reopened.camera.settings.followRate.x == 0.0f && reopened.camera.settings.followRate.y == 0.1f &&
	      reopened.camera.settings.followRate.z == 1.0f && reopened.camera.settings.followFactor.x == 0.5f,
	      "independent pursuit rates and displacement fractions survive camera.json round-trip");
	Check(reopened.camera.settings.followTarget == CameraFollowTarget::Head,
	      "selected camera follow body part survives camera.json round-trip");
	Check(reopened.camera.settings.vmdRangeScale == 0.75f,
	      "VMD range scale survives camera.json round-trip");
	Check(reopened.camera.settings.followPlacementYaw, "camera placement-facing option survives camera.json round-trip");
	settings.boundsMin.x = 4;
	core.QueueCameraSettings(settings);
	core.ProcessPendingGuiCommands();
	Check(core.camera.settings.boundsMin.x == -2, "invalid queued scene bounds cannot reach clamp");
	settings = core.camera.settings;
	settings.enabled = false;
	core.QueueCameraSettings(settings);
	core.ProcessPendingGuiCommands();
	Check(!core.camera.settings.enabled && !core.camera.owned, "disable releases camera ownership");
	Core gameCameraReopened;
	gameCameraReopened.configRoot = core.configRoot;
	Check(gameCameraReopened.LoadCameraSettings() && !gameCameraReopened.camera.settings.enabled,
	      "game camera selection persists even when mode and follow behavior are unchanged");
	settings.enabled = true;
	core.QueueCameraSettings(settings);
	core.ProcessPendingGuiCommands();
	Check(gameCameraReopened.LoadCameraSettings() && gameCameraReopened.camera.settings.enabled,
	      "returning from game to the same GM camera mode persists ownership selection");
	RuntimeConfig legacyHold;
	legacyHold.path = core.configRoot + "/legacy-hold.json";
	WriteTextFile(legacyHold.path, R"({"camera":{"mode":1,"enabled":true,"follow_fov":51}})");
	Check(legacyHold.Reload() && legacyHold.camera.mode == CameraMode::Track && legacyHold.camera.enabled &&
	      legacyHold.camera.followFov == 51,
	      "retired fixed-camera files migrate to VMD without changing other camera parameters");

	static CameraImportRequest received;
	core.fnStartCameraImport = [](const CameraImportRequest& r, std::string&)-> bool
	{
		received = r;
		return true;
	};
	core.fnPollCameraImport = [](std::string& message)-> int
	{
		message = "test import failed";
		return -1;
	};
	MotionEntry selected;
	selected.name = "song";
	selected.dir = repoRoot + "/Motions/Fighting My Way";
	core.motions = {selected};
	CameraImportRequest request;
	request.source = "example.vmd";
	request.motionKey = "song";
	request.motionDirectory = "untrusted GUI destination";
	core.StartCameraImport(request);
	Check(core.cameraImportBusy && received.motionDirectory == selected.dir,
	      "camera import uses stable song key and trusted library directory");
	Check(received.script.empty() && received.python.empty(),
	      "camera import starts without an installed Python helper or interpreter");
	core.PollCameraImport();
	Check(!core.cameraImportBusy && core.cameraNotice == "test import failed",
	      "camera import failures leave busy state and surface error");
	core.player.state = PlaybackState::Paused;
	core.StartCameraImport(request);
	Check(!core.cameraImportBusy, "paused playback blocks destructive camera replacement");
	core.player.state = PlaybackState::Stopped;
	request.motionKey = "removed song";
	core.StartCameraImport(request);
	Check(!core.cameraImportBusy, "removed song cannot redirect camera import");
	core.player.state = PlaybackState::Preview;
	core.QueueStop();
	core.ProcessPendingGuiCommands();
	Check(core.player.state == PlaybackState::Stopped && !core.camera.owned,
	      "GUI stop releases camera in same command batch");

	// Per-mode Transform settings queued through Core
	CameraSettings perModeSettings = core.camera.settings;
	perModeSettings.usePerModeTransforms = true;
	perModeSettings.transformTrack = {{0.1f, 0.2f, 0.3f}, {5.0f, 10.0f, 15.0f}};
	perModeSettings.transformFollowMove = {{0.4f, 0.7f, 2.5f}, {-5.0f, 0.0f, 10.0f}};
	perModeSettings.transformLookAt = {{0.0f, 0.6f, 3.0f}, {0.0f, 15.0f, 0.0f}};
	perModeSettings.vmdOffset = perModeSettings.transformTrack.position;
	perModeSettings.followOffset = perModeSettings.transformFollowMove.position;
	core.QueueCameraSettings(perModeSettings);
	core.ProcessPendingGuiCommands();
	Check(core.camera.settings.usePerModeTransforms, "usePerModeTransforms queued and applied in core");
	Check(core.camera.settings.transformFollowMove == perModeSettings.transformFollowMove,
	      "per-mode follow move transform preserved through queue");
	Check(core.camera.settings.transformTrack == perModeSettings.transformTrack,
	      "per-mode track transform preserved through queue");
	Check(core.camera.settings.transformLookAt == perModeSettings.transformLookAt,
	      "per-mode look at transform preserved through queue");

	// Per-mode Transform disk persistence round-trip (SaveCameraSettings -> LoadCameraSettings)
	core.QueueGui({Core::GuiAction::SaveCameraSettings});
	core.ProcessPendingGuiCommands();
	Core reopenedPerMode;
	reopenedPerMode.configRoot = core.configRoot;
	Check(reopenedPerMode.LoadCameraSettings(),
	      "camera.json with per-mode transforms loads cleanly");
	Check(reopenedPerMode.camera.settings.usePerModeTransforms,
	      "use_per_mode_transforms survives camera.json round-trip");
	Check(reopenedPerMode.camera.settings.transformTrack == perModeSettings.transformTrack,
	      "transform_track pos and rot survive camera.json round-trip");
	Check(reopenedPerMode.camera.settings.transformFollowMove == perModeSettings.transformFollowMove,
	      "transform_follow_move pos and rot survive camera.json round-trip");
	Check(reopenedPerMode.camera.settings.transformLookAt == perModeSettings.transformLookAt,
	      "transform_look_at pos and rot survive camera.json round-trip");

	BeginTest("camera pursuit rate config validation and legacy fields");
	RuntimeConfig rateConfig;
	rateConfig.path = core.configRoot + "/rate-control.json";
	WriteTextFile(rateConfig.path, R"({"camera":{"follow_factor":[0.25,0.5,0.75]}})");
	Check(rateConfig.Reload() && rateConfig.camera.followFactor.x == 0.25f &&
	      rateConfig.camera.followFactor.y == 0.5f && rateConfig.camera.followFactor.z == 0.75f &&
	      rateConfig.camera.followRate.x == 0.1f && rateConfig.camera.followRate.y == 0.1f &&
	      rateConfig.camera.followRate.z == 0.1f,
	      "legacy displacement fractions retain their meaning and missing rates use smooth defaults");
	Check(rateConfig.camera.vmdRangeScale == 1.0f, "legacy camera config defaults to original VMD range");
	const auto originalCamera = rateConfig.camera;
	for (const char* invalidScale : {R"({"camera":{"vmd_range_scale":0.2}})",
		R"({"camera":{"vmd_range_scale":1.1}})", R"({"camera":{"vmd_range_scale":"0.8"}})",
		R"({"camera":{"vmd_range_scale":1e100}})"})
	{
		WriteTextFile(rateConfig.path, invalidScale);
		Check(!rateConfig.Reload() && rateConfig.camera == originalCamera,
		      "invalid VMD range scales cannot replace camera settings");
	}
	WriteTextFile(rateConfig.path, R"({"camera":{"follow_rate":[0,0.2,1]}})");
	Check(rateConfig.Reload() && rateConfig.camera.followRate.x == 0.0f &&
	      rateConfig.camera.followRate.y == 0.2f && rateConfig.camera.followRate.z == 1.0f &&
	      rateConfig.camera.followFactor.y == 0.5f,
	      "pursuit rate endpoints parse independently of retained displacement fractions");
	const auto previousRates = rateConfig.camera;
	const char* invalidRates[] = {
		R"({"camera":{"follow_rate":[-0.1,0.1,0.1],"follow_factor":[1,1,1]}})",
		R"({"camera":{"follow_rate":[0.1,1.1,0.1]}})",
		R"({"camera":{"follow_rate":[0.1,0.1]}})",
		R"({"camera":{"follow_rate":[0.1,"0.1",0.1]}})",
		R"({"camera":{"follow_rate":[0.1,0.1,1e100]}})"
	};
	for (const char* invalid : invalidRates)
	{
		WriteTextFile(rateConfig.path, invalid);
		Check(!rateConfig.Reload() && rateConfig.camera == previousRates,
		      "invalid pursuit rates cannot partially replace camera settings");
	}
	WriteTextFile(rateConfig.path, R"({"camera":{"follow_fov":60}})");
	Check(rateConfig.Reload() && rateConfig.camera.followRate.y == 0.2f &&
	      rateConfig.camera.followRate.z == 1.0f,
	      "partial camera declarations preserve an existing pursuit rate");
	WriteTextFile(rateConfig.path, RuntimeConfig::DefaultTemplate());
	RuntimeConfig rateDefaults;
	rateDefaults.path = rateConfig.path;
	Check(rateDefaults.Reload() && rateDefaults.camera.followRate.x == 0.1f &&
	      rateDefaults.camera.followRate.y == 0.1f && rateDefaults.camera.followRate.z == 0.1f,
	      "generated control template declares smooth pursuit defaults");

	BeginTest("camera follow body part persistence and strict config validation");
	RuntimeConfig targetConfig;
	targetConfig.path = core.configRoot + "/target-control.json";
	WriteTextFile(targetConfig.path, R"({"camera":{"follow_factor":[0.5,1,1]}})");
	Check(targetConfig.Reload() && targetConfig.camera.followTarget == CameraFollowTarget::Hips,
	      "legacy camera config without a body part defaults to hips");
	for (const auto target : {CameraFollowTarget::Hips, CameraFollowTarget::Chest, CameraFollowTarget::Head})
	{
		const std::string name = CameraFollowTargetName(target);
		WriteTextFile(targetConfig.path, "{\"camera\":{\"follow_target\":\"" + name + "\"}}");
		Check(targetConfig.Reload() && targetConfig.camera.followTarget == target,
		      "supported camera follow body part names parse without changing displacement settings");
		auto bodyPartSettings = core.camera.settings;
		bodyPartSettings.followTarget = target;
		core.QueueCameraSettings(bodyPartSettings);
		core.QueueGui({Core::GuiAction::SaveCameraSettings});
		core.ProcessPendingGuiCommands();
		Core targetReopened;
		targetReopened.configRoot = core.configRoot;
		Check(targetReopened.LoadCameraSettings() && targetReopened.camera.settings.followTarget == target,
		      "each camera follow body part saves and reloads through the real Core workflow");
	}
	const auto previousTargetSettings = targetConfig.camera;
	const char* invalidTargets[] = {
		R"({"camera":{"follow_target":"neck","follow_factor":[1,1,1]}})",
		R"({"camera":{"follow_target":"Head"}})",
		R"({"camera":{"follow_target":1}})",
		R"({"camera":{"follow_target":false}})",
		R"({"camera":{"follow_target":null}})",
		R"({"camera":{"follow_target":["hips"]}})"
	};
	for (const char* invalid : invalidTargets)
	{
		WriteTextFile(targetConfig.path, invalid);
		Check(!targetConfig.Reload() && targetConfig.camera == previousTargetSettings,
		      "unsupported or mistyped camera follow body parts reject the entire camera declaration");
	}
	WriteTextFile(targetConfig.path, R"({"camera":{"follow_rate":[0.2,0.3,0.4]}})");
	Check(targetConfig.Reload() && targetConfig.camera.followTarget == CameraFollowTarget::Head,
	      "camera declarations without a body part preserve the existing selection");
	WriteTextFile(targetConfig.path, RuntimeConfig::DefaultTemplate());
	RuntimeConfig targetDefaults;
	targetDefaults.path = targetConfig.path;
	Check(targetDefaults.Reload() && targetDefaults.camera.followTarget == CameraFollowTarget::Hips,
	      "generated camera template declares the hips follow target");
}
