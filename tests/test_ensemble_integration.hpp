#pragma once

namespace EnsembleFixture
{
	inline std::unordered_map<void*, Quaternion> rotations;
	inline std::unordered_map<void*, Vector3> positions;
	inline std::unordered_map<void*, bool> enabled;
	inline std::unordered_map<void*, void*> controllers;
	struct Face { float weight{0}; };
	inline std::unordered_map<void*, Face> faces;
	inline unsigned writes = 0;
	inline std::vector<void*> writtenTransforms;
	inline bool rejectRotations = false;
	inline void* Bone(void* actor, int hid)
	{ return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(actor) + 0x100 + static_cast<unsigned>(hid)); }
	inline void* Transform(void* actor)
	{ return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(actor) + 0x500); }
	inline void* Parent(void* transform)
	{ return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(transform) & ~uintptr_t{0xfffff}) + 0x500); }
	inline void GetRot(void* t, float* out)
	{
		const auto it = rotations.find(t);
		const auto q = it == rotations.end() ? Quaternion::Identity() : it->second;
		out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
	}
	inline void SetRot(void* t, float x, float y, float z, float w)
	{ if (!rejectRotations) rotations[t] = {x,y,z,w}; writtenTransforms.push_back(t); ++writes; }
	inline void GetPos(void* t, float* out)
	{
		const auto it = positions.find(t);
		const auto p = it == positions.end() ? Vector3{} : it->second;
		out[0] = p.x; out[1] = p.y; out[2] = p.z;
	}
	inline void SetPos(void* t, float x, float y, float z) { positions[t] = {x,y,z}; }
	inline bool GetEnabled(void* a) { return enabled.find(a) == enabled.end() || enabled[a]; }
	inline void SetEnabled(void* a, bool value) { enabled[a] = value; }
	inline void* GetController(void* a)
	{ return controllers.find(a) == controllers.end() ? reinterpret_cast<void*>(0x123) : controllers[a]; }
	inline void SetController(void* a, void* value) { controllers[a] = value; }
	inline void Wire(IdolController& c)
	{
		c.fnGetBoneTransform = Bone; c.fnGetComponentTransform = Transform;
		c.fnGetWorldRotation = GetRot; c.fnSetWorldRotation = SetRot;
		c.fnGetTransformParent = Parent; c.fnSetLocalRotation = SetRot;
		c.fnGetWorldPosition = GetPos; c.fnSetWorldPosition = SetPos;
		c.fnGetAnimatorEnabled = GetEnabled; c.fnSetAnimatorEnabled = SetEnabled;
		c.fnGetAnimatorController = GetController; c.fnSetAnimatorController = SetController;
		c.facialEnableBaseline = false; c.facialMikuMouthFit = false;
		c.faceApi.getModel = [](void* actor) -> void* { return &faces[actor]; };
		c.faceApi.getCount = [](void*) { return 1; };
		c.faceApi.getName = [](void*, int) { return std::string("b_mouth.mouth_020"); };
		c.faceApi.getWeight = [](void* face, int) { return static_cast<Face*>(face)->weight; };
		c.faceApi.setWeight = [](void* face, int, float value) { static_cast<Face*>(face)->weight = value; };
	}
	inline void WriteClip(const std::filesystem::path& path, float degrees)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write("GKCLIPCL", 8);
		auto write = [&](auto value) { file.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
		write(uint32_t{4}); write(30.0f); write(uint32_t{2}); write(2.0f / 30.0f);
		write(uint32_t{1}); write(.083f); write(uint16_t{0}); write(uint8_t{4}); write(uint8_t{0}); file.write("Hips", 4);
		for (int i = 0; i < 2; ++i) { write(Vector3{}); write(Quaternion::FromEulerDegreesXYZ(0, 0, degrees)); }
		write(uint8_t{0}); write(uint32_t{1});
		const std::string name = "b_mouth.mouth_020";
		write(static_cast<uint16_t>(name.size())); file.write(name.data(), name.size());
		write(degrees / 100.0f); write(degrees / 100.0f);
	}
}

static void TestEnsembleIntegration(const std::string& repoRoot)
{
	BeginTest("three actor binding, shared time and safe restoration");
	namespace fs = std::filesystem;
	const auto root = fs::absolute(fs::path(repoRoot) / "build" / "tests" / "ensemble_synthetic");
	EnsembleFixture::WriteClip(root / "B" / "motion.gkclip", 20);
	EnsembleFixture::WriteClip(root / "C" / "motion.gkclip", 45);
	Core core;
	EnsembleFixture::Wire(core.player.controller);
	void* a = reinterpret_cast<void*>(0x100000), *b = reinterpret_cast<void*>(0x200000),
	    *c = reinterpret_cast<void*>(0x300000), *animA = reinterpret_cast<void*>(0x600000), *animB = reinterpret_cast<void*>(0x400000),
	    *animC = reinterpret_cast<void*>(0x500000);
	core.PublishSceneActors({{a, 1, ""}, {b, 2, ""}, {c, 3, ""}});
	MotionEntry trackB; trackB.name = "B"; trackB.dir = MotionPathToUtf8(root / "B");
	MotionEntry trackC; trackC.name = "C"; trackC.dir = MotionPathToUtf8(root / "C");
	core.motions = {trackB, trackC}; core.libraryRevision = 5;
	core.player.clip = MakeSmallClip(); core.player.state = PlaybackState::Preview;
	const auto view = core.BuildGuiContext();
	const auto tokenB = view.sceneActors[1].token, tokenC = view.sceneActors[2].token;
	view.fnSetEnsembleFollower(view.self, 0, tokenB, view.actorRosterEpoch, "B", .01f, view.libraryRevision);
	view.fnSetEnsembleFollower(view.self, 1, tokenC, view.actorRosterEpoch, "C", -.01f, view.libraryRevision);
	core.ProcessPendingGuiCommands();
	Check(core.ensemble.Followers()[0].player && core.ensemble.Followers()[1].player,
	      "two distinct actors load independent synthetic clips");
	if (!core.ensemble.Followers()[0].player || !core.ensemble.Followers()[1].player) return;
	Check(!core.ensemble.Followers()[0].player->fnPlayAudio && !core.ensemble.Followers()[1].player->fnStopAudio,
	      "follower players cannot start or stop the primary audio device");
	const auto stable = core.ensemble.Followers()[1].actorToken;
	view.fnSetEnsembleFollower(view.self, 1, tokenB, view.actorRosterEpoch, "C", 0, view.libraryRevision);
	core.ProcessPendingGuiCommands();
	Check(core.ensemble.Followers()[1].actorToken == stable, "duplicate actor is rejected without changing the old binding");
	view.fnSetEnsembleFollower(view.self, 1, tokenC, view.actorRosterEpoch, "missing", 0, view.libraryRevision);
	core.ProcessPendingGuiCommands();
	Check(core.ensemble.Followers()[1].actorToken == stable, "missing motion leaves the previous binding intact");
	core.player.state = PlaybackState::Paused; core.player.currentTime = .04f;
	EnsembleFixture::faces[b].weight = 12; EnsembleFixture::faces[c].weight = 17;
	const auto noAnimatorWrites = EnsembleFixture::writes;
	core.OnLateUpdate(b, nullptr, nullptr, .016f, false);
	Check(!core.ensemble.Followers()[0].active && EnsembleFixture::writes == noAnimatorWrites,
	      "an actor without a restorable animator receives no follower writes");
	core.OnLateUpdate(b, animB, nullptr, .016f, false);
	core.OnLateUpdate(c, animC, nullptr, .016f, false);
	const auto& fb = core.ensemble.Followers()[0]; const auto& fc = core.ensemble.Followers()[1];
	Check(fb.active && fc.active && fb.player->controller.bindings.count(b) && fc.player->controller.bindings.count(c),
	      "both follower controllers bind only their own actor");
	Check(!QuatNear(EnsembleFixture::rotations[EnsembleFixture::Bone(b, 0)],
	                EnsembleFixture::rotations[EnsembleFixture::Bone(c, 0)]),
	      "different clips produce different poses on their own actors");
	Check(std::fabs(EnsembleFixture::faces[b].weight - 20) < .001f &&
	      std::fabs(EnsembleFixture::faces[c].weight - 45) < .001f,
	      "separate facial tracks drive the corresponding actors independently");
	const auto positionsView = core.BuildGuiContext();
	const auto savedC = EnsembleFixture::positions[EnsembleFixture::Bone(c, 0)];
	positionsView.fnSetEnsemblePosition(positionsView.self, 0, tokenB, positionsView.actorRosterEpoch,
	                                    positionsView.ensembleFollowers[0].bindingGeneration, {1.0f, .2f, -.5f});
	core.ProcessPendingGuiCommands();
	core.OnLateUpdate(b, animB, nullptr, .016f, false);
	const auto movedB = EnsembleFixture::positions[EnsembleFixture::Bone(b, 0)];
	const auto unchangedC = EnsembleFixture::positions[EnsembleFixture::Bone(c, 0)];
	Check(std::fabs(movedB.x - 1.0f) < 1e-5f && std::fabs(movedB.y - .2f) < 1e-5f &&
	      std::fabs(movedB.z + .5f) < 1e-5f && unchangedC.x == savedC.x && unchangedC.z == savedC.z &&
	      core.player.controller.initialPositionOffset.x == 0,
	      "follower position changes its own actor without moving the main or other actor");
	positionsView.fnSetEnsemblePosition(positionsView.self, 1, tokenC, positionsView.actorRosterEpoch,
	                                    positionsView.ensembleFollowers[1].bindingGeneration, {-2.0f, 0, 1.0f});
	core.ProcessPendingGuiCommands();
	core.OnLateUpdate(c, animC, nullptr, .016f, false);
	const auto movedC = EnsembleFixture::positions[EnsembleFixture::Bone(c, 0)];
	Check(std::fabs(movedC.x + 2.0f) < 1e-5f && std::fabs(movedC.z - 1.0f) < 1e-5f &&
	      std::fabs(core.ensemble.Followers()[0].player->controller.initialPositionOffset.x - 1.0f) < 1e-5f,
	      "both followers retain independent position offsets");
	positionsView.fnSetEnsemblePosition(positionsView.self, 0, tokenB, positionsView.actorRosterEpoch,
	                                    positionsView.ensembleFollowers[0].bindingGeneration + 1, {8, 0, 0});
	positionsView.fnSetEnsemblePosition(positionsView.self, 0, tokenB, positionsView.actorRosterEpoch,
	                                    positionsView.ensembleFollowers[0].bindingGeneration,
	                                    {std::numeric_limits<float>::quiet_NaN(), 0, 0});
	core.ProcessPendingGuiCommands();
	Check(std::fabs(core.ensemble.Followers()[0].player->controller.initialPositionOffset.x - 1.0f) < 1e-5f,
	      "invalid or stale follower position commands leave its current placement unchanged");
	positionsView.fnSetEnsembleYaw(positionsView.self, 0, tokenB, positionsView.actorRosterEpoch,
	                               positionsView.ensembleFollowers[0].bindingGeneration, 90);
	positionsView.fnSetEnsembleYaw(positionsView.self, 1, tokenC, positionsView.actorRosterEpoch,
	                               positionsView.ensembleFollowers[1].bindingGeneration, -90);
	core.ProcessPendingGuiCommands();
	core.OnLateUpdate(b, animB, nullptr, .016f, false);
	core.OnLateUpdate(c, animC, nullptr, .016f, false);
	Check(core.ensemble.Followers()[0].player->controller.initialYawDegrees == 90 &&
	      core.ensemble.Followers()[1].player->controller.initialYawDegrees == -90 &&
	      core.player.controller.initialYawDegrees == 0,
	      "followers keep independent yaw without changing the main actor");
	Check(std::fabs(EnsembleFixture::positions[EnsembleFixture::Bone(b, 0)].x - movedB.x) < 1e-5f &&
	      std::fabs(EnsembleFixture::positions[EnsembleFixture::Bone(c, 0)].x - movedC.x) < 1e-5f,
	      "yaw changes facing without orbiting either actor's current position");
	core.player.state = PlaybackState::Preview;
	const uint64_t oldGeneration = positionsView.ensembleFollowers[0].bindingGeneration;
	positionsView.fnSetEnsembleFollower(positionsView.self, 0, tokenB, positionsView.actorRosterEpoch,
	                                    "C", .01f, positionsView.libraryRevision);
	core.ProcessPendingGuiCommands();
	Check(core.ensemble.Followers()[0].motionKey == "C" &&
	      core.ensemble.Followers()[0].bindingGeneration != oldGeneration &&
	      core.ensemble.Followers()[0].player->controller.initialYawDegrees == 90 &&
	      std::fabs(core.ensemble.Followers()[0].player->controller.initialPositionOffset.x - 1.0f) < 1e-5f,
	      "changing a follower's clip preserves its actor's position and yaw");
	positionsView.fnSetEnsembleYaw(positionsView.self, 0, tokenB, positionsView.actorRosterEpoch, oldGeneration, 10);
	core.ProcessPendingGuiCommands();
	Check(core.ensemble.Followers()[0].player->controller.initialYawDegrees == 90,
	      "queued yaw from the old binding cannot alter the replacement clip");
	core.player.state = PlaybackState::Paused;
	core.OnLateUpdate(b, animB, nullptr, .016f, false);
	Check(std::fabs(fb.player->currentTime - .03f) < 1e-5f &&
	      std::fabs(fc.player->currentTime - .05f) < 1e-5f && core.player.currentTime == .04f,
	      "both sample the shared paused time with independent offsets");
	core.player.state = PlaybackState::Playing; core.player.currentTime = .02f;
	core.OnLateUpdate(a, animA, nullptr, .01f, true);
	core.OnLateUpdate(b, animB, nullptr, .01f, false);
	core.OnLateUpdate(c, animC, nullptr, .01f, false);
	Check(std::fabs(core.player.currentTime - .03f) < 1e-5f &&
	      std::fabs(fb.player->currentTime - .02f) < 1e-5f && std::fabs(fc.player->currentTime - .04f) < 1e-5f,
	      "one physical frame advances the main clock once and both actors sample it");
	core.player.currentTime = .02f;
	core.OnLateUpdate(c, animC, nullptr, .01f, true);
	core.OnLateUpdate(b, animB, nullptr, .01f, false);
	core.OnLateUpdate(a, animA, nullptr, .01f, false);
	Check(std::fabs(core.player.currentTime - .03f) < 1e-5f &&
	      std::fabs(fb.player->currentTime - .02f) < 1e-5f && std::fabs(fc.player->currentTime - .04f) < 1e-5f,
	      "callback order does not change the shared time or advance it twice");
	core.player.state = PlaybackState::Paused;
	Check(core.SuspendForCapture() && !fb.active && !fc.active && EnsembleFixture::GetEnabled(animB) &&
	      EnsembleFixture::GetEnabled(animC), "capture restores both follower animators before native capture");
	Check(std::fabs(EnsembleFixture::faces[b].weight - 12) < .001f &&
	      std::fabs(EnsembleFixture::faces[c].weight - 17) < .001f,
	      "capture restores each follower's own facial state");
	Check(core.ResumeAfterCapture() && core.player.state == PlaybackState::Paused,
	      "capture resumes the same paused transport");
	core.OnLateUpdate(b, animB, nullptr, .016f, false);
	core.OnLateUpdate(c, animC, nullptr, .016f, false);
	core.StopPerformance();
	Check(!fb.active && !fc.active && EnsembleFixture::GetEnabled(animB) && EnsembleFixture::GetEnabled(animC),
	      "stop restores both follower animators");
	Check(std::fabs(EnsembleFixture::positions[EnsembleFixture::Bone(b, 0)].x) < 1e-5f &&
	      std::fabs(EnsembleFixture::positions[EnsembleFixture::Bone(c, 0)].x) < 1e-5f,
	      "stop restores the original positions without baking in follower offsets");
	Check(fb.player->controller.initialYawDegrees == 90 &&
	      std::fabs(fb.player->controller.initialPositionOffset.x - 1.0f) < 1e-5f,
	      "stop restores the scene but retains configured placement for replay");
	const auto resetView = core.BuildGuiContext();
	resetView.fnSetEnsemblePosition(resetView.self, 0, tokenB, resetView.actorRosterEpoch,
	                                resetView.ensembleFollowers[0].bindingGeneration, {});
	resetView.fnSetEnsembleYaw(resetView.self, 0, tokenB, resetView.actorRosterEpoch,
	                           resetView.ensembleFollowers[0].bindingGeneration, 0);
	core.ProcessPendingGuiCommands();
	Check(fb.player->controller.initialPositionOffset.x == 0 && fb.player->controller.initialYawDegrees == 0 &&
	      fc.player->controller.initialYawDegrees == -90,
	      "reset clears only that actor's position and yaw");
	core.player.state = PlaybackState::Preview;
	core.OnLateUpdate(b, animB, nullptr, .016f, false);
	core.OnLateUpdate(c, animC, nullptr, .016f, false);
	const auto before = EnsembleFixture::writes;
	core.PublishSceneActors({{a, 1, ""}, {b, 22, ""}}); // old B address reused; C removed
	Check(core.ensemble.HasFollowers() && core.ensembleRestoreUnconfirmed && EnsembleFixture::writes == before,
	      "removed or replaced identities retain snapshots and block writes without dereferencing stale objects");
	view.fnSetEnsembleFollower(view.self, 0, tokenB, view.actorRosterEpoch, "B", 0, view.libraryRevision);
	core.ProcessPendingGuiCommands();
	Check(core.ensemble.Followers()[0].actorToken == tokenB && core.ensembleRestoreUnconfirmed,
	      "commands from an old roster cannot bind a reused address");
	core.AbandonScene();
	Check(!core.ensemble.HasFollowers() && !core.ensembleRestoreUnconfirmed,
	      "confirmed scene abandonment discards stale snapshots without writes");

	Core changed;
	EnsembleFixture::enabled[animB] = true; EnsembleFixture::enabled[animC] = true;
	EnsembleFixture::controllers[animB] = reinterpret_cast<void*>(0x123);
	EnsembleFixture::controllers[animC] = reinterpret_cast<void*>(0x123);
	EnsembleFixture::Wire(changed.player.controller);
	changed.PublishSceneActors({{a, 1, ""}, {b, 2, ""}, {c, 3, ""}});
	changed.motions = {trackB, trackC}; changed.libraryRevision = 10;
	changed.player.clip = MakeSmallClip(); changed.player.state = PlaybackState::Preview;
	const auto changedView = changed.BuildGuiContext();
	changedView.fnSetEnsembleFollower(changedView.self, 0, changedView.sceneActors[1].token, changedView.actorRosterEpoch, "B", 0, 10);
	changedView.fnSetEnsembleFollower(changedView.self, 1, changedView.sceneActors[2].token, changedView.actorRosterEpoch, "C", 0, 10);
	changed.ProcessPendingGuiCommands();
	changed.OnLateUpdate(b, animB, nullptr, .016f, false);
	changed.OnLateUpdate(c, animC, nullptr, .016f, false);
	EnsembleFixture::writtenTransforms.clear();
	changed.PublishSceneActors({{a, 1, ""}, {b, 2, ""}});
	const bool touchedRemoved = std::any_of(EnsembleFixture::writtenTransforms.begin(), EnsembleFixture::writtenTransforms.end(),
		[&](void* t) { const auto n = reinterpret_cast<uintptr_t>(t); return n >= 0x300000 && n < 0x301000; });
	Check(changed.ensemble.HasFollowers() && changed.ensembleRestoreUnconfirmed &&
	      EnsembleFixture::GetEnabled(animB) && !touchedRemoved,
	      "roster changes restore the surviving actor and fail closed for the missing actor");
	changed.AbandonScene();

	Core failing;
	EnsembleFixture::Wire(failing.player.controller);
	failing.PublishSceneActors({{a, 1, ""}, {b, 2, ""}});
	failing.motions = {trackB}; failing.libraryRevision = 9;
	failing.player.clip = MakeSmallClip(); failing.player.state = PlaybackState::Preview;
	const auto failView = failing.BuildGuiContext();
	failView.fnSetEnsembleFollower(failView.self, 0, failView.sceneActors[1].token,
	                               failView.actorRosterEpoch, "B", 0, failView.libraryRevision);
	failing.ProcessPendingGuiCommands();
	failing.OnLateUpdate(b, animB, nullptr, .016f, false);
	Check(failing.ensemble.HasActiveFollowers(), "failure test owns a live follower before capture");
	EnsembleFixture::rotations[EnsembleFixture::Bone(b, 0)] = Quaternion::FromEulerDegreesXYZ(0, 0, 120);
	EnsembleFixture::rejectRotations = true;
	const bool accepted = failing.SuspendForCapture();
	EnsembleFixture::rejectRotations = false;
	Check(!accepted && failing.captureRestoreReport.body == capture::RestoreState::Unconfirmed &&
	      failing.ensembleRestoreUnconfirmed && !failing.PrepareInjection(),
	      "failed follower restore rejects capture and blocks further injection");

	Core missingAnimator;
	EnsembleFixture::enabled[animB] = true;
	EnsembleFixture::controllers[animB] = reinterpret_cast<void*>(0x123);
	EnsembleFixture::Wire(missingAnimator.player.controller);
	missingAnimator.PublishSceneActors({{a, 1, ""}, {b, 2, ""}});
	missingAnimator.motions = {trackB}; missingAnimator.libraryRevision = 11;
	missingAnimator.player.clip = MakeSmallClip(); missingAnimator.player.state = PlaybackState::Preview;
	const auto missingView = missingAnimator.BuildGuiContext();
	missingView.fnSetEnsembleFollower(missingView.self, 0, missingView.sceneActors[1].token,
	                                  missingView.actorRosterEpoch, "B", 0, missingView.libraryRevision);
	missingAnimator.ProcessPendingGuiCommands();
	missingAnimator.OnLateUpdate(b, animB, nullptr, .016f, false);
	const unsigned priorWrites = EnsembleFixture::writes;
	missingAnimator.OnLateUpdate(b, nullptr, nullptr, .016f, false);
	Check(missingAnimator.ensembleRestoreUnconfirmed && !missingAnimator.PrepareInjection() &&
	      EnsembleFixture::writes == priorWrites,
	      "disappearing animator blocks injection without touching an unconfirmed actor");
	Check(!missingAnimator.SuspendForCapture() && EnsembleFixture::writes == priorWrites,
	      "capture refuses unconfirmed ownership without dereferencing the old animator");

	Core switching;
	switching.PublishSceneActors({{a, 1, ""}, {b, 2, ""}});
	auto mainView = switching.BuildGuiContext();
	mainView.fnSetInitialPosition(mainView.self, {1, 0, 2});
	mainView.fnSetInitialYaw(mainView.self, 30);
	switching.ProcessPendingGuiCommands();
	switching.SetTargetActor(b);
	Check(switching.player.controller.initialPositionOffset.x == 0 && switching.player.controller.initialYawDegrees == 0,
	      "switching to a new main actor does not inherit the prior actor's placement");
	mainView = switching.BuildGuiContext();
	mainView.fnSetInitialPosition(mainView.self, {-2, 0, 0}); mainView.fnSetInitialYaw(mainView.self, -45);
	switching.ProcessPendingGuiCommands();
	switching.SetTargetActor(a);
	Check(switching.player.controller.initialPositionOffset.x == 1 && switching.player.controller.initialYawDegrees == 30,
	      "returning to a main actor restores its own remembered placement");
	switching.SetTargetActor(b);
	Check(switching.player.controller.initialPositionOffset.x == -2 && switching.player.controller.initialYawDegrees == -45,
	      "main actor placements are independent when switching back and forth");

	Core crossRole;
	EnsembleFixture::Wire(crossRole.player.controller);
	crossRole.PublishSceneActors({{a, 1, ""}, {b, 2, ""}});
	crossRole.motions = {trackB}; crossRole.libraryRevision = 13;
	crossRole.player.clip = MakeSmallClip(); crossRole.player.state = PlaybackState::Preview;
	auto roleView = crossRole.BuildGuiContext();
	roleView.fnSetInitialPosition(roleView.self, {2, 0, 0}); roleView.fnSetInitialYaw(roleView.self, 25);
	roleView.fnSetEnsembleFollower(roleView.self, 0, roleView.sceneActors[1].token, roleView.actorRosterEpoch, "B", 0, 13);
	crossRole.ProcessPendingGuiCommands();
	roleView = crossRole.BuildGuiContext();
	roleView.fnSetEnsemblePosition(roleView.self, 0, roleView.ensembleFollowers[0].actorToken, roleView.actorRosterEpoch,
	                               roleView.ensembleFollowers[0].bindingGeneration, {-3, 0, 0});
	roleView.fnSetEnsembleYaw(roleView.self, 0, roleView.ensembleFollowers[0].actorToken, roleView.actorRosterEpoch,
	                          roleView.ensembleFollowers[0].bindingGeneration, -60);
	crossRole.ProcessPendingGuiCommands();
	crossRole.SetTargetActor(b);
	Check(crossRole.player.controller.initialPositionOffset.x == -3 && crossRole.player.controller.initialYawDegrees == -60,
	      "a follower's own placement follows it when selected as the primary actor");
	roleView = crossRole.BuildGuiContext();
	roleView.fnSetEnsembleFollower(roleView.self, 0, roleView.sceneActors[0].token, roleView.actorRosterEpoch, "B", 0, 13);
	crossRole.ProcessPendingGuiCommands();
	Check(crossRole.ensemble.Followers()[0].player &&
	      crossRole.ensemble.Followers()[0].player->controller.initialPositionOffset.x == 2 &&
	      crossRole.ensemble.Followers()[0].player->controller.initialYawDegrees == 25,
	      "the previous main actor keeps its placement when assigned as a follower");
	roleView = crossRole.BuildGuiContext();
	roleView.fnSelectTargetActor(roleView.self, roleView.sceneActors[0].token, roleView.actorRosterEpoch);
	roleView.fnSetInitialYaw(roleView.self, 99);
	crossRole.ProcessPendingGuiCommands();
	Check(crossRole.player.controller.targetActor == a && crossRole.player.controller.initialYawDegrees == 25,
	      "queued yaw for the previous main actor is not applied to a newly selected actor");
}
