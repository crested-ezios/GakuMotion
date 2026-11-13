// Included by test_core.cpp to use the existing assertion and temp-file helpers.
#include <limits>

static void TestCameraController(const std::string& repoRoot)
{
	BeginTest("camera ownership / follow / bounds / restore");
	static CameraFrame actual;
	static Vector3 hips;
	static bool alive = true;
	static int writes = 0;
	actual.position = {0, 2, 4};
	actual.rotation = Quaternion::FromEulerDegreesXYZ(0, 90, 0).Normalized();
	actual.fov = 52;
	const CameraFrame before = actual;
	hips = {10, 1, 20};
	alive = true;
	writes = 0;
	CameraController camera;
	camera.fnGetCamera = []() -> void* { return reinterpret_cast<void*>(0x10); };
	camera.fnGetTransform = [](void*) -> void* { return reinterpret_cast<void*>(0x20); };
	camera.fnAlive = [](void*) { return alive; };
	camera.fnRead = [](void*, void*, CameraFrame& out)
	{
		out = actual;
		return true;
	};
	camera.fnWrite = [](void*, void*, const CameraFrame& frame)
	{
		actual = frame;
		++writes;
	};
	camera.fnActorPose = [](void*, Vector3& origin, Quaternion& rotation, Vector3& hip)
	{
		origin = {10, 0, 20};
		rotation = Quaternion::FromEulerDegreesXYZ(0, 90, 0);
		hip = hips;
		return alive;
	};
	auto actor = reinterpret_cast<void*>(0x30);
	camera.Tick(false, 0, .016f, actor);
	Check(!camera.owned && writes == 0, "preview leaves camera manual");
	camera.Tick(true, 0, .016f, actor);
	Check(camera.owned && camera.BlocksTransform(reinterpret_cast<void*>(0x20)), "play owns camera writes");
	Check(!camera.BlocksTransform(reinterpret_cast<void*>(0x21)), "unrelated transforms remain writable");
	actual.position = {999, 999, 999};
	actual.fov = 120;
	camera.BeforeRender(reinterpret_cast<void*>(0x10));
	Check(actual.position.z == 4 && actual.fov == 52, "manual pose and FOV overwritten before render");
	int priorWrites = writes;
	camera.BeforeRender(reinterpret_cast<void*>(0x11));
	Check(writes == priorWrites, "other render cameras unchanged");
	camera.settings.mode = CameraMode::Follow;
	camera.settings.followFactor = {1, 1, 1};
	camera.settings.followRate = {1, 1, 1}; // isolate displacement and camera-local axis behavior
	camera.Tick(true, 1, .016f, actor);
	Check(std::fabs(actual.position.x - 13) < 1e-5f && std::fabs(actual.position.z - 20) < 1e-5f,
	      "follow offset rotates with actor anchor");
	auto forward = actual.rotation.Rotate({0, 0, 1});
	Check(forward.x < -.99f, "follow looks at target from actor-relative offset");
	const Vector3 followBasePosition = actual.position;
	const Vector3 followBaseLook{10, 1.4f, 20};
	const Quaternion followBaseRotation = actual.rotation;
	const Vector3 localHipMove{4, 8, -6};
	hips = Vector3{10, 1, 20} + camera.saved.rotation.Rotate(localHipMove);
	camera.settings.followFactor = {0.5f, 0.25f, 0.0f};
	camera.settings.followBehavior = FollowBehavior::LookAt;
	camera.Tick(true, 1.1f, .016f, actor);
	const Vector3 lookDelta = camera.saved.rotation.Rotate({2, 2, 0});
	const Vector3 lookTarget = followBaseLook + lookDelta;
	const Vector3 expectedLookDirection = lookTarget - followBasePosition;
	const float lookLength = std::sqrt(expectedLookDirection.x * expectedLookDirection.x +
									   expectedLookDirection.y * expectedLookDirection.y +
									   expectedLookDirection.z * expectedLookDirection.z);
	forward = actual.rotation.Rotate({0, 0, 1});
	Check(std::fabs(actual.position.x - followBasePosition.x) < 1e-5f &&
	      std::fabs(actual.position.y - followBasePosition.y) < 1e-5f &&
	      std::fabs(actual.position.z - followBasePosition.z) < 1e-5f,
	      "LookAt keeps camera position fixed while hips move");
	Check(std::fabs(forward.x - expectedLookDirection.x / lookLength) < 1e-4f &&
	      std::fabs(forward.y - expectedLookDirection.y / lookLength) < 1e-4f &&
	      std::fabs(forward.z - expectedLookDirection.z / lookLength) < 1e-4f,
	      "LookAt moves its target by camera-local factor-scaled displacement");
	camera.settings.followBehavior = FollowBehavior::FollowMove;
	camera.Tick(true, 1.2f, .016f, actor);
	const Vector3 followExpectedPosition = followBasePosition + lookDelta;
	Check(std::fabs(actual.position.x - followExpectedPosition.x) < 1e-5f &&
	      std::fabs(actual.position.y - followExpectedPosition.y) < 1e-5f &&
	      std::fabs(actual.position.z - followExpectedPosition.z) < 1e-5f,
	      "FollowMove translates the camera by the scaled local displacement");
	Check(QuatNear(actual.rotation, followBaseRotation),
	      "FollowMove translates camera and look target together, preserving their relative pose");
	camera.settings.limitToScene = true;
	camera.settings.boundsMin = {0, 0, 0};
	camera.settings.boundsMax = {11, 2, 21};
	camera.Tick(true, 2, .016f, actor);
	Check(actual.position.x == 11 && actual.position.y <= 2, "scene bounds clamp world position");
	camera.settings.limitToScene = false;
	const Vector3 axisTestWorldMove = camera.saved.rotation.Rotate(localHipMove);
	hips = Vector3{10, 1, 20} + axisTestWorldMove;
	camera.settings.followFactor = {0.5f, 0.0f, 0.0f};
	camera.Tick(true, 2.1f, .016f, actor);
	const Vector3 xOnlyExpected = followBasePosition + camera.saved.rotation.Rotate({2, 0, 0});
	Check(std::fabs(actual.position.x - xOnlyExpected.x) < 1e-5f &&
	      std::fabs(actual.position.y - xOnlyExpected.y) < 1e-5f &&
	      std::fabs(actual.position.z - xOnlyExpected.z) < 1e-5f,
	      "camera-local X factor moves only the camera-local X component");
	camera.settings.followFactor = {0.0f, 0.5f, 0.0f};
	camera.Tick(true, 2.2f, .016f, actor);
	const Vector3 yOnlyExpected = followBasePosition + camera.saved.rotation.Rotate({0, 4, 0});
	Check(std::fabs(actual.position.x - yOnlyExpected.x) < 1e-5f &&
	      std::fabs(actual.position.y - yOnlyExpected.y) < 1e-5f &&
	      std::fabs(actual.position.z - yOnlyExpected.z) < 1e-5f,
	      "camera-local Y factor moves only the camera-local Y component");
	camera.settings.followFactor = {0.0f, 0.0f, 0.5f};
	camera.Tick(true, 2.3f, .016f, actor);
	const Vector3 zOnlyExpected = followBasePosition + camera.saved.rotation.Rotate({0, 0, -3});
	Check(std::fabs(actual.position.x - zOnlyExpected.x) < 1e-5f &&
	      std::fabs(actual.position.y - zOnlyExpected.y) < 1e-5f &&
	      std::fabs(actual.position.z - zOnlyExpected.z) < 1e-5f,
	      "camera-local Z factor moves only the camera-local Z component");
	camera.settings.followFactor = {1.0f, 1.0f, 1.0f};
	camera.Tick(true, 20, .016f, actor);
	const Vector3 fullExpectedPosition = followBasePosition + axisTestWorldMove;
	Check(std::fabs(actual.position.x - fullExpectedPosition.x) < 1e-4f &&
	      std::fabs(actual.position.y - fullExpectedPosition.y) < 1e-4f &&
	      std::fabs(actual.position.z - fullExpectedPosition.z) < 1e-4f,
	      "full follow factor reaches the actor displacement in the captured camera basis");
	camera.Tick(true, 0, .016f, actor);
	Check(std::fabs(actual.position.x - fullExpectedPosition.x) < 1e-4f &&
	      std::fabs(actual.position.y - fullExpectedPosition.y) < 1e-4f &&
	      std::fabs(actual.position.z - fullExpectedPosition.z) < 1e-4f,
	      "full follow factor retains the actor position on loop");
	camera.Tick(false, 0, .016f, actor);
	Check(!camera.owned && actual.position.z == before.position.z && actual.fov == before.fov,
	      "stop restores captured camera and unlocks writes");
	camera.Tick(true, 0, .016f, actor);
	priorWrites = writes;
	alive = false;
	camera.Release();
	Check(writes == priorWrites && !camera.owned, "destroyed scene camera never restored");
	alive = true;

	BeginTest("Live VMD track baseline alignment / restore");
	{
		Core core;
		core.player.controller.currentBinding.actor = actor;
		core.player.controller.currentBinding.bindPoseCaptured = true;
		core.player.controller.currentBinding.restRootPos = {2, .9f, -3};
		auto zero = core.LiveCameraTrackAnchor(actor);
		Check(zero.x == 0 && zero.y == 0 && zero.z == 0, "ordinary scenes retain original camera anchor");
		core.liveTakeoverActive = true;
		const auto anchor = core.LiveCameraTrackAnchor(actor);
		Check(anchor.x == 2 && anchor.y == 0 && anchor.z == -3,
		      "Live anchor includes only captured horizontal root baseline");
		Check(core.LiveCameraTrackAnchor(reinterpret_cast<void*>(0x31)).x == 0,
		      "another actor cannot inherit the selected actor baseline");
		actual = before;
		camera.settings = CameraSettings{};
		camera.settings.mode = CameraMode::Track;
		camera.track.fps = 30;
		CameraFrame first; first.position = {0, 1.2f, 1.7f};
		first.rotation = Quaternion::FromEulerDegreesXYZ(0, 180, 0);
		first.fov = 22;
		CameraFrame second = first; second.position.x += 1; second.fov = 30;
		camera.track.frames = {first, second};
		camera.Tick(true, 0, .016f, actor, 1, anchor);
		const Vector3 expected = Vector3{10, 0, 20} + Quaternion::FromEulerDegreesXYZ(0, 90, 0).Rotate(anchor + first.position);
		Check(std::fabs(actual.position.x - expected.x) < 1e-4f &&
		      std::fabs(actual.position.y - expected.y) < 1e-4f && std::fabs(actual.position.z - expected.z) < 1e-4f,
		      "captured Live displacement shifts track into the motion's reference frame");
		const Vector3 motionLocal{.4f, 1.0f, -.2f};
		const auto actorRotation = Quaternion::FromEulerDegreesXYZ(0, 90, 0);
		const Vector3 motionWorld = Vector3{10, 0, 20} + actorRotation.Rotate(anchor + motionLocal);
		const Vector3 relative = motionWorld - actual.position;
		const Vector3 intendedRelative = actorRotation.Rotate(motionLocal - first.position);
		Check(std::fabs(relative.x - intendedRelative.x) < 1e-4f &&
		      std::fabs(relative.y - intendedRelative.y) < 1e-4f &&
		      std::fabs(relative.z - intendedRelative.z) < 1e-4f,
		      "motion and camera retain their source relative framing despite the Live baseline");
		const auto expectedRotation = Quaternion::FromEulerDegreesXYZ(0, 90, 0) * first.rotation;
		Check(QuatNear(actual.rotation, expectedRotation) && actual.fov == 22,
		      "alignment keeps source camera orientation and field of view");
		camera.settings.followPlacementYaw = true;
		camera.Tick(true, 0, .016f, actor, 1, 45.0f, anchor);
		const auto combinedYaw = Quaternion::FromEulerDegreesXYZ(0, 45, 0);
		const Vector3 combinedExpected = Vector3{10,0,20} + actorRotation.Rotate(anchor) +
			combinedYaw.Rotate(actorRotation.Rotate(first.position));
		Check(std::fabs(actual.position.x-combinedExpected.x) < 1e-4f &&
		      std::fabs(actual.position.y-combinedExpected.y) < 1e-4f &&
		      std::fabs(actual.position.z-combinedExpected.z) < 1e-4f &&
		      QuatNear(actual.rotation, combinedYaw * actorRotation * first.rotation),
		      "Live baseline and optional placement yaw compose without losing camera framing");
		camera.settings.followPlacementYaw = false;
		camera.Tick(true, 1.0f / 30, .016f, actor, 2, anchor);
		const Vector3 nextExpected = Vector3{10, 0, 20} + Quaternion::FromEulerDegreesXYZ(0, 90, 0).Rotate(anchor + second.position);
		Check(std::fabs(actual.position.x - nextExpected.x) < 1e-4f &&
		      std::fabs(actual.position.z - nextExpected.z) < 1e-4f && actual.fov == 30,
		      "later camera keyframes retain their movement and field of view");
		const auto restored = camera.ReleaseForCapture(); // Live exit uses the protected-capture restoration path.
		Check(restored.modified && restored.confirmed && !camera.owned &&
		      QuatNear(actual.rotation, before.rotation) && actual.fov == before.fov &&
		      std::fabs(actual.position.x - before.position.x) < 1e-5f &&
		      std::fabs(actual.position.y - before.position.y) < 1e-5f &&
		      std::fabs(actual.position.z - before.position.z) < 1e-5f,
		      "release restores the original camera pose and field of view");
		core.liveTakeoverActive = false;
		Check(core.LiveCameraTrackAnchor(actor).x == 0, "disabling Live removes its alignment");
	}

	BeginTest("camera settings validity / placement anchor");
	{
		auto valid = CameraSettings{};
		Check(valid.IsValid(), "default camera settings are valid");
		valid.mode = static_cast<CameraMode>(3);
		Check(!valid.IsValid(), "unknown camera mode is invalid");
		valid = CameraSettings{};
		valid.boundsMin.x = valid.boundsMax.x + 1;
		Check(!valid.IsValid(), "inverted bounds invalid even while bounds are disabled");
		valid = CameraSettings{};
		valid.vmdOffset.y = std::numeric_limits<float>::quiet_NaN();
		Check(!valid.IsValid(), "NaN VMD offset is invalid");
		valid = CameraSettings{};
		valid.followOffset.y = std::numeric_limits<float>::infinity();
		Check(!valid.IsValid(), "non-finite follow position is invalid");
		valid = CameraSettings{};
		valid.followFactor.x = -0.1f;
		Check(!valid.IsValid(), "negative follow factor is invalid");
		valid = CameraSettings{};
		valid.followFactor.y = 1.5f;
		Check(!valid.IsValid(), "follow factor > 1 is invalid");
		valid = CameraSettings{};
		valid.followRate.x = -0.1f;
		Check(!valid.IsValid(), "negative follow response is invalid");
		valid = CameraSettings{};
		valid.followRate.y = 1.1f;
		Check(!valid.IsValid(), "follow response > 1 is invalid");
		valid = CameraSettings{};
		valid.followRate.z = std::numeric_limits<float>::quiet_NaN();
		Check(!valid.IsValid(), "non-finite follow response is invalid");
		valid = CameraSettings{};
		valid.followBehavior = static_cast<FollowBehavior>(2);
		Check(!valid.IsValid(), "unknown follow behavior is invalid");
		camera.settings = valid;
		priorWrites = writes;
		camera.Tick(true, 0, .016f, actor);
		Check(!camera.owned && writes == priorWrites, "invalid settings do not write the Unity camera");
		camera.settings = CameraSettings{};

		static Vector3 placementAnchor;
		static Vector3 placedHips;
		placementAnchor = {10.4f, .23f, 19.38f};
		placedHips = {10.4f, 1.23f, 19.38f};
		CameraController anchored;
		anchored.settings.followRate = {1, 1, 1}; // placement accounting is independent of response time
		anchored.fnGetCamera = camera.fnGetCamera;
		anchored.fnGetTransform = camera.fnGetTransform;
		anchored.fnAlive = camera.fnAlive;
		anchored.fnRead = camera.fnRead;
		anchored.fnWrite = camera.fnWrite;
		anchored.fnActorPose = [](void*, Vector3& origin, Quaternion& rotation, Vector3& hip)
		{
			origin = placementAnchor;
			rotation = Quaternion::Identity();
			hip = placedHips;
			return true;
		};
		CameraFrame local;
		local.position = {0, 2, -4};
		anchored.track.frames = {local};
		anchored.Tick(true, 0, .016f, actor);
		Check(std::fabs(actual.position.x - 10.4f) < 1e-5f && std::fabs(actual.position.y - 2.23f) < 1e-5f,
		      "VMD path captures the same XYZ placement anchor as the actor");
		Check(anchored.settings.vmdRangeScale == 1.0f && actual.orthographicSize == local.orthographicSize,
		      "default VMD range leaves the original camera path and focus size unchanged");
		const Vector3 beforeYawPosition = actual.position;
		const Quaternion beforeYawRotation = actual.rotation;
		const Quaternion yaw90 = Quaternion::FromEulerDegreesXYZ(0, 90, 0);
		anchored.Tick(true, 0, .016f, actor, 0, 90);
		Check(std::fabs(actual.position.x - beforeYawPosition.x) < 1e-5f && QuatNear(actual.rotation, beforeYawRotation),
		      "disabled facing sync leaves the original track unchanged");
		anchored.settings.followPlacementYaw = true;
		anchored.Tick(true, 0, .016f, actor, 0, 90);
		const Vector3 yawExpected = placementAnchor + yaw90.Rotate(beforeYawPosition - placementAnchor);
		Check(std::fabs(actual.position.x-yawExpected.x) < 1e-5f && std::fabs(actual.position.z-yawExpected.z) < 1e-5f &&
		      QuatNear(actual.rotation, yaw90 * beforeYawRotation),
		      "facing sync rotates VMD position and orientation around the placed actor");
		anchored.Tick(true, 0, .016f, actor, 0, -90);
		const auto negativeExpected = placementAnchor + Quaternion::FromEulerDegreesXYZ(0, -90, 0).Rotate(beforeYawPosition-placementAnchor);
		Check(std::fabs(actual.position.x-negativeExpected.x) < 1e-5f && std::fabs(actual.position.z-negativeExpected.z) < 1e-5f,
		      "camera responds to changed placement yaw without accumulating the previous angle");
		anchored.settings.followPlacementYaw = false;
		anchored.Tick(true, 0, .016f, actor, 0, 90);
		Check(std::fabs(actual.position.x-beforeYawPosition.x) < 1e-5f && QuatNear(actual.rotation, beforeYawRotation),
		      "disabling facing sync restores the original composition");
		anchored.settings.vmdRangeScale = 0.5f;
		anchored.Tick(true, 0, .016f, actor);
		Check(std::fabs(actual.position.x - 10.4f) < 1e-5f &&
		      std::fabs(actual.position.y - 1.73f) < 1e-5f && std::fabs(actual.position.z - 17.38f) < 1e-5f &&
		      std::fabs(actual.orthographicSize - local.orthographicSize * 0.5f) < 1e-5f &&
		      actual.fov == local.fov && std::fabs(Quaternion::Dot(actual.rotation, local.rotation)) > .99999f,
		      "VMD range scales around a fixed actor-local pivot and scales focus distance without changing rotation or FOV");
		anchored.settings.vmdOffset = {0.5f, -0.2f, 1.0f};
		anchored.Tick(true, 0, .016f, actor);
		Check(std::fabs(actual.position.x - 10.9f) < 1e-5f && std::fabs(actual.position.y - 1.53f) < 1e-5f &&
		      std::fabs(actual.position.z - 18.38f) < 1e-5f,
		      "VMD manual position offsets are not scaled with the track");
		anchored.settings.vmdRangeScale = 1.0f;
		anchored.settings.vmdOffset = {0.5f, -0.2f, 1.0f};
		anchored.Tick(true, 0, .016f, actor);
		Check(std::fabs(actual.position.x - 10.9f) < 1e-5f &&
		      std::fabs(actual.position.y - 2.03f) < 1e-5f &&
		      std::fabs(actual.position.z - 16.38f) < 1e-5f,
		      "VMD path incorporates initial XYZ offset (e.g. height adjustment for idols)");
		anchored.settings.vmdOffset = {0, 0, 0};
		placementAnchor.x += 1;
		placedHips.x += 1;
		anchored.Tick(true, .016f, .016f, actor);
		Check(std::fabs(actual.position.x - 11.4f) < 1e-5f, "XYZ changes translate the VMD path while playing");
		anchored.settings.followActor = true;
		anchored.Tick(true, .032f, .016f, actor);
		Check(std::fabs(actual.position.x - 11.4f) < 1e-5f, "full hips following does not count XYZ translation twice");
		placedHips.x += 2;
		anchored.Tick(true, .048f, .016f, actor);
		Check(std::fabs(actual.position.x - 13.4f) < 1e-5f,
		      "hips following includes actual animated root displacement");
		anchored.Release();
		// Both follow modes rotate their anchor offsets while preserving the target.
		for (auto behavior : {FollowBehavior::FollowMove, FollowBehavior::LookAt})
		{
			anchored.settings = CameraSettings{};
			anchored.settings.mode = CameraMode::Follow;
			anchored.settings.followBehavior = behavior;
			anchored.Tick(true, 0, .016f, actor);
			const Vector3 base = actual.position;
			const Quaternion rotation = actual.rotation;
			anchored.settings.followPlacementYaw = true;
			anchored.Tick(true, 0, .016f, actor, 0, 90);
			const Vector3 expected = anchored.followOrigin + yaw90.Rotate(base - anchored.followOrigin);
			Check(std::fabs(actual.position.x-expected.x) < 1e-4f && std::fabs(actual.position.z-expected.z) < 1e-4f &&
			      QuatNear(actual.rotation, yaw90 * rotation), "follow camera rotates with placement yaw around its target");
			anchored.Release();
		}
	}

	BeginTest("camera follow response / time / physical frame / lifecycle");
	{
		constexpr float step = 1.0f / 60.0f;
		const auto nearPosition = [](const Vector3& a, const Vector3& b)
		{
			return std::fabs(a.x - b.x) < 2e-4f && std::fabs(a.y - b.y) < 2e-4f &&
				std::fabs(a.z - b.z) < 2e-4f;
		};
		const auto prepare = [&](CameraController& cam, const Vector3& rate,
			const Quaternion& basis = Quaternion::Identity())
		{
			actual = CameraFrame{};
			actual.rotation = basis;
			hips = {};
			cam.fnGetCamera = camera.fnGetCamera;
			cam.fnGetTransform = camera.fnGetTransform;
			cam.fnAlive = camera.fnAlive;
			cam.fnRead = camera.fnRead;
			cam.fnWrite = camera.fnWrite;
			cam.fnActorPose = [](void*, Vector3& origin, Quaternion& rotation, Vector3& hip)
			{
				origin = {};
				rotation = Quaternion::Identity();
				hip = hips;
				return true;
			};
			cam.settings.mode = CameraMode::Follow;
			cam.settings.followOffset = {};
			cam.settings.followRate = rate;
			cam.Tick(true, 0, step, actor, 1);
		};

		const auto defaults = CameraSettings{};
		Check(nearPosition(defaults.followRate, {.1f, .1f, .1f}),
		      "camera follow response defaults to MMDD-style 0.1 on every axis");
		CameraController response;
		prepare(response, defaults.followRate);
		hips = {10, 0, 0};
		response.Tick(true, 4, step, actor, 2);
		Check(nearPosition(response.desired.position, {1, 0, 0}),
		      "0.1 response first removes ten percent of the remaining displacement");
		response.Tick(true, 4, step, actor, 3);
		Check(nearPosition(response.desired.position, {1.9f, 0, 0}),
		      "paused timeline still approaches the stationary actor using elapsed frame time");
		response.Tick(true, 100, step, actor, 4);
		Check(nearPosition(response.desired.position, {2.71f, 0, 0}),
		      "seek preserves the filtered position without a timeline-sized jump");
		response.Tick(true, 0, step, actor, 5);
		Check(nearPosition(response.desired.position, {3.439f, 0, 0}),
		      "loop to zero preserves response history instead of snapping to the actor");
		const Vector3 beforeInvalidDt = response.desired.position;
		response.settings.followRate = {1, 1, 1};
		hips = {20, 0, 0};
		for (float invalidDt : {0.0f, -step, std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
			response.Tick(true, 0, invalidDt, actor);
		Check(nearPosition(response.desired.position, beforeInvalidDt),
		      "zero, negative and non-finite elapsed time never advance even immediate response");
		response.settings.followRate = defaults.followRate;
		hips = {10, 0, 0};
		for (int i = 0; i < 120; ++i) response.Tick(true, 0, step, actor);
		Check(response.desired.position.x > 9.999f && response.desired.position.x <= 10.0f,
		      "0.1 response keeps converging to the whole displacement rather than a fixed tenth");
		response.Release();

		CameraController freeze;
		prepare(freeze, defaults.followRate);
		hips = {10, 0, 0};
		freeze.Tick(true, 1, step, actor, 2);
		const Vector3 frozenPosition = freeze.desired.position;
		freeze.settings.followRate = {};
		hips = {-20, 7, 9};
		for (int i = 0; i < 10; ++i) freeze.Tick(true, 1, step, actor);
		Check(nearPosition(freeze.desired.position, frozenPosition) && frozenPosition.x > .9f,
		      "changing response to zero freezes the current filtered position instead of resetting it");
		freeze.settings.followRate = {1, 1, 1};
		freeze.Tick(true, 1, step, actor);
		Check(nearPosition(freeze.desired.position, hips),
		      "response one immediately reaches all axes of the current actor displacement");
		freeze.Release();

		Vector3 equalTime[3];
		const int frequencies[] = {30, 60, 120};
		for (int n = 0; n < 3; ++n)
		{
			CameraController timed;
			prepare(timed, defaults.followRate);
			hips = {10, -4, 2};
			for (int i = 0; i < frequencies[n] / 2; ++i)
				timed.Tick(true, 0, 1.0f / frequencies[n], actor);
			equalTime[n] = timed.desired.position;
			timed.Release();
		}
		Check(nearPosition(equalTime[0], equalTime[1]) && nearPosition(equalTime[1], equalTime[2]) &&
		      equalTime[1].x > 9.5f && equalTime[1].x < 9.7f,
		      "30, 60 and 120 Hz reach the same intermediate position after half a second");

		CameraController deduplicated;
		prepare(deduplicated, defaults.followRate);
		hips = {10, 0, 0};
		deduplicated.Tick(true, 1, step, actor, 2);
		const Vector3 oncePerFrame = deduplicated.desired.position;
		for (int i = 0; i < 5; ++i)
		{
			deduplicated.Tick(true, 1, step, actor, 2);
			actual.position = {999, 999, 999};
			deduplicated.BeforeRender(reinterpret_cast<void*>(0x10));
		}
		Check(nearPosition(deduplicated.desired.position, oncePerFrame) &&
		      nearPosition(actual.position, oncePerFrame),
		      "duplicate physical frame and repeated render callbacks reapply without accelerating follow");
		deduplicated.Tick(true, 1, step, actor, 3);
		Check(nearPosition(deduplicated.desired.position, {1.9f, 0, 0}),
		      "the next physical frame advances response once after duplicate callbacks");
		deduplicated.Release();
		hips = {};
		deduplicated.Tick(true, 0, step, actor, 3);
		Check(nearPosition(deduplicated.desired.position, {}),
		      "release clears the previous session's filtered displacement");
		hips = {10, 0, 0};
		deduplicated.Tick(true, 0, step, actor, 4);
		Check(nearPosition(deduplicated.desired.position, {1, 0, 0}),
		      "a new ownership session starts fresh response and physical-frame bookkeeping");
		hips = {30, 0, 0};
		auto otherActor = reinterpret_cast<void*>(0x31);
		deduplicated.Tick(true, 0, step, otherActor, 4);
		Check(deduplicated.actor == otherActor && nearPosition(deduplicated.desired.position, {30, 0, 0}),
		      "actor replacement captures the new anchor without inheriting filtered displacement");
		hips = {40, 0, 0};
		deduplicated.Tick(true, 0, step, otherActor, 5);
		Check(nearPosition(deduplicated.desired.position, {31, 0, 0}),
		      "replacement actor begins chasing from its own captured origin");
		deduplicated.Release();

		const Quaternion rotatedBasis = Quaternion::FromEulerDegreesXYZ(20, 70, -15).Normalized();
		CameraController mixedAxes;
		prepare(mixedAxes, {0, .1f, 1}, rotatedBasis);
		mixedAxes.settings.followFactor = {.5f, .25f, .75f};
		hips = rotatedBasis.Rotate({4, 8, -6});
		mixedAxes.Tick(true, 1, step, actor, 2);
		Check(nearPosition(mixedAxes.desired.position, rotatedBasis.Rotate({0, .2f, -4.5f})),
		      "mixed response and displacement factors act independently in the captured camera-local basis");
		mixedAxes.Tick(true, 1, step, actor, 3);
		Check(nearPosition(mixedAxes.desired.position, rotatedBasis.Rotate({0, .38f, -4.5f})),
		      "local slow axis approaches its factor-scaled goal while frozen and immediate axes stay distinct");
		mixedAxes.settings.followBehavior = FollowBehavior::LookAt;
		mixedAxes.Tick(true, 1, step, actor, 3);
		const Vector3 smoothLookTarget = mixedAxes.settings.targetOffset + rotatedBasis.Rotate({0, .38f, -4.5f});
		const float smoothLookLength = std::sqrt(smoothLookTarget.x * smoothLookTarget.x +
			smoothLookTarget.y * smoothLookTarget.y + smoothLookTarget.z * smoothLookTarget.z);
		const Vector3 smoothLookForward = mixedAxes.desired.rotation.Rotate({0, 0, 1});
		Check(nearPosition(mixedAxes.desired.position, {}) &&
		      nearPosition(smoothLookForward, {smoothLookTarget.x / smoothLookLength,
			      smoothLookTarget.y / smoothLookLength, smoothLookTarget.z / smoothLookLength}),
		      "LookAt aims from its fixed tripod using the same filtered local displacement");
		mixedAxes.Release();

		Core independentClock;
		prepare(independentClock.camera, defaults.followRate);
		independentClock.NoteActor(actor);
		independentClock.NoteActor(otherActor);
		independentClock.SetTargetActor(otherActor);
		independentClock.player.state = PlaybackState::Paused;
		independentClock.OnLateUpdate(otherActor, nullptr, nullptr, 0, false, step, 10);
		hips = {10, 0, 0};
		independentClock.OnLateUpdate(actor, nullptr, nullptr, .5f, true, step, 11);
		independentClock.OnLateUpdate(otherActor, nullptr, nullptr, 0, false, step, 11);
		Check(independentClock.camera.actor == otherActor &&
		      nearPosition(independentClock.camera.desired.position, {1, 0, 0}),
		      "Core advances the selected second actor's camera using real dt when its playback-clock flag is false");
		independentClock.OnLateUpdate(otherActor, nullptr, nullptr, .5f, false, step, 11);
		Check(nearPosition(independentClock.camera.desired.position, {1, 0, 0}),
		      "Core forwards the physical frame id so duplicate selected-actor callbacks cannot accelerate follow");
		independentClock.OnLateUpdate(otherActor, nullptr, nullptr, .5f, false, 0, 12);
		Check(nearPosition(independentClock.camera.desired.position, {1, 0, 0}),
		      "explicit zero camera dt overrides nonzero playback dt in Core");
		independentClock.player.state = PlaybackState::Playing;
		independentClock.player.clip.duration_seconds = 100;
		independentClock.player.playbackSpeedMultiplier = .25f;
		independentClock.OnLateUpdate(otherActor, nullptr, nullptr, .5f, true, step, 13);
		Check(nearPosition(independentClock.camera.desired.position, {1.9f, 0, 0}),
		      "slow playback and a larger motion dt do not alter the independent camera response interval");
		independentClock.player.playbackSpeedMultiplier = 4;
		independentClock.OnLateUpdate(otherActor, nullptr, nullptr, .5f, true, step, 14);
		Check(nearPosition(independentClock.camera.desired.position, {2.71f, 0, 0}),
		      "fast playback keeps the same camera response per real elapsed interval");
		independentClock.StopPerformance();
	}

	BeginTest("camera selected body part / mode / failure recovery");
	{
		constexpr float step = 1.0f / 60.0f;
		static Vector3 partHips, partChest, partHead;
		static bool partAvailable, partNonFinite;
		static int partReads;
		static void* partActor;
		static CameraFollowTarget partReadTarget;
		const auto nearPosition = [](const Vector3& a, const Vector3& b)
		{
			return std::fabs(a.x - b.x) < 2e-4f && std::fabs(a.y - b.y) < 2e-4f &&
				std::fabs(a.z - b.z) < 2e-4f;
		};
		const auto aimsAt = [&](const CameraFrame& frame, const Vector3& target)
		{
			const Vector3 direction = target - frame.position;
			const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
			return length > 0 && nearPosition(frame.rotation.Rotate({0, 0, 1}),
				{direction.x / length, direction.y / length, direction.z / length});
		};
		const auto readPart = [](void* selectedActor, CameraFollowTarget target, Vector3& out)
		{
			++partReads;
			partActor = selectedActor;
			partReadTarget = target;
			if (!partAvailable) return false;
			out = target == CameraFollowTarget::Chest ? partChest : partHead;
			if (partNonFinite) out.x = std::numeric_limits<float>::quiet_NaN();
			return true;
		};
		const auto prepare = [&](CameraController& cam, CameraFollowTarget target,
			FollowBehavior behavior = FollowBehavior::FollowMove)
		{
			actual = CameraFrame{};
			actual.position = {7, 8, 9};
			actual.fov = 51;
			partHips = {0, 1, 0};
			partChest = {0, 2, 0};
			partHead = {0, 3, 0};
			partAvailable = true;
			partNonFinite = false;
			partReads = 0;
			partActor = nullptr;
			cam.fnGetCamera = camera.fnGetCamera;
			cam.fnGetTransform = camera.fnGetTransform;
			cam.fnAlive = camera.fnAlive;
			cam.fnRead = camera.fnRead;
			cam.fnWrite = camera.fnWrite;
			cam.fnActorPose = [](void*, Vector3& origin, Quaternion& rotation, Vector3& hip)
			{
				origin = {};
				rotation = Quaternion::Identity();
				hip = partHips;
				return true;
			};
			cam.fnFollowTarget = readPart;
			cam.settings.mode = CameraMode::Follow;
			cam.settings.followBehavior = behavior;
			cam.settings.followTarget = target;
			cam.settings.followOffset = {0, .5f, 4};
			cam.settings.targetOffset = {.25f, .1f, 0};
			cam.settings.followRate = {1, 1, 1};
		};

		Check(CameraSettings{}.followTarget == CameraFollowTarget::Hips &&
		      CameraFollowTargetBoneId(CameraFollowTarget::Hips) == 0 &&
		      CameraFollowTargetBoneId(CameraFollowTarget::Chest) == 8 &&
		      CameraFollowTargetBoneId(CameraFollowTarget::Head) == 10,
		      "default follow target and explicit hips/chest/head Humanoid ids are stable");
		auto invalidTarget = CameraSettings{};
		invalidTarget.followTarget = static_cast<CameraFollowTarget>(3);
		Check(!invalidTarget.IsValid(), "unknown camera follow target is rejected before any body-part read");

		const CameraFollowTarget targets[] = {CameraFollowTarget::Hips, CameraFollowTarget::Chest, CameraFollowTarget::Head};
		for (FollowBehavior behavior : {FollowBehavior::FollowMove, FollowBehavior::LookAt})
		{
			for (int selected = 0; selected < 3; ++selected)
			{
				CameraController cam;
				prepare(cam, targets[selected], behavior);
				const CameraFrame restorePose = actual;
				cam.Tick(true, 0, step, actor, 1);
				const CameraFrame initialFollow = cam.desired;
				Vector3* positions[] = {&partHips, &partChest, &partHead};
				for (int other = 0; other < 3; ++other)
					if (other != selected) *positions[other] = *positions[other] + Vector3{9, 2, -5};
				cam.Tick(true, 0, step, actor, 2);
				Check(nearPosition(cam.desired.position, initialFollow.position) &&
				      QuatNear(cam.desired.rotation, initialFollow.rotation),
				      "moving unselected body parts leaves the selected part's follow pose unchanged");
				*positions[selected] = *positions[selected] + Vector3{2, .25f, -1};
				cam.Tick(true, 0, step, actor, 3);
				if (behavior == FollowBehavior::FollowMove)
					Check(nearPosition(cam.desired.position, *positions[selected] + cam.settings.followOffset) &&
					      QuatNear(cam.desired.rotation, initialFollow.rotation),
					      "FollowMove translates with the selected hips, chest or head independently of other parts");
				else
					Check(nearPosition(cam.desired.position, {0, 1.5f, 4}) &&
					      aimsAt(cam.desired, *positions[selected] + cam.settings.targetOffset),
					      "LookAt keeps the original hips-based tripod while aiming at the selected moving body part");
				Check(selected == 0 ? partReads == 0 : partReads > 0 && partActor == actor && partReadTarget == targets[selected],
				      "hips use the original actor pose while chest/head query only the requested actor and part");
				cam.Release();
				Check(nearPosition(actual.position, restorePose.position) && QuatNear(actual.rotation, restorePose.rotation) &&
				      actual.fov == restorePose.fov, "stopping any body-part follow restores the original captured camera");
			}
		}

		CameraController switching;
		prepare(switching, CameraFollowTarget::Hips);
		const CameraFrame originalCamera = actual;
		switching.Tick(true, 0, step, actor, 1);
		switching.settings.followRate = {.1f, .1f, .1f};
		switching.settings.followTarget = CameraFollowTarget::Head;
		switching.Tick(true, 0, step, actor, 2);
		Check(nearPosition(switching.desired.position, {0, 1.7f, 4}),
		      "switching from hips to head at response 0.1 approaches its full absolute height without recapture");
		switching.settings.followTarget = CameraFollowTarget::Chest;
		switching.Tick(true, 0, step, actor, 3);
		Check(nearPosition(switching.desired.position, {0, 1.78f, 4}),
		      "changing body part retains existing response history instead of starting from zero");
		switching.settings.followRate = {};
		switching.settings.followTarget = CameraFollowTarget::Head;
		partHead = {5, 4, -2};
		switching.Tick(true, 0, step, actor, 4);
		Check(nearPosition(switching.desired.position, {0, 1.78f, 4}),
		      "response zero freezes the current camera even when both selected part and its position change");
		switching.settings.followRate = {1, 1, 1};
		switching.Tick(true, 0, step, actor, 5);
		Check(nearPosition(switching.desired.position, {5, 4.5f, 2}) &&
		      nearPosition(switching.saved.position, originalCamera.position),
		      "response one immediately follows the newly selected part while preserving the original restore pose");
		const CameraFrame lastGood = switching.desired;
		partAvailable = false;
		partHips = {100, 100, 100};
		partHead = {10, 4, -2};
		switching.Tick(true, 0, step, actor, 6);
		Check(switching.owned && nearPosition(switching.desired.position, lastGood.position) &&
		      !switching.ReadSnapshot().followTargetError.empty(),
		      "a missing selected bone holds the last camera and reports an error without falling back to moving hips");
		partAvailable = true;
		partNonFinite = true;
		switching.Tick(true, 0, step, actor, 7);
		Check(nearPosition(switching.desired.position, lastGood.position) &&
		      !switching.ReadSnapshot().followTargetError.empty(), "non-finite selected bone data keeps the last valid follow pose");
		partNonFinite = false;
		switching.settings.followRate = {.1f, .1f, .1f};
		switching.Tick(true, 0, step, actor, 8);
		Check(nearPosition(switching.desired.position, {5.5f, 4.5f, 2}) &&
		      switching.ReadSnapshot().followTargetError.empty(),
		      "bone recovery resumes from filtered history and the original hips anchor without a snap");
		switching.fnFollowTarget = nullptr;
		switching.Tick(true, 0, step, actor, 9);
		Check(nearPosition(switching.desired.position, {5.5f, 4.5f, 2}) &&
		      !switching.ReadSnapshot().followTargetError.empty(), "missing target callback cannot silently substitute hips after takeover");
		switching.Tick(false, 0, step, actor);
		Check(!switching.owned && nearPosition(actual.position, originalCamera.position) &&
		      switching.ReadSnapshot().followTargetError.empty(), "stop restores the original camera and clears body-part errors");

		CameraController waiting;
		prepare(waiting, CameraFollowTarget::Head);
		const Vector3 manualPosition = actual.position;
		waiting.fnFollowTarget = nullptr;
		const int writesBeforeWaiting = writes;
		waiting.Tick(true, 0, step, actor, 1);
		Check(!waiting.owned && writes == writesBeforeWaiting && nearPosition(actual.position, manualPosition) &&
		      !waiting.ReadSnapshot().followTargetError.empty(), "unavailable head reader leaves the manual camera untouched before initial takeover");
		waiting.fnFollowTarget = readPart;
		partAvailable = false;
		waiting.Tick(true, 0, step, actor, 2);
		Check(!waiting.owned && writes == writesBeforeWaiting, "a missing head bone also prevents initial camera ownership");
		partAvailable = true;
		partNonFinite = true;
		waiting.Tick(true, 0, step, actor, 3);
		Check(!waiting.owned && writes == writesBeforeWaiting,
		      "non-finite head data cannot capture or move the initial manual camera");
		partNonFinite = false;
		waiting.Tick(true, 0, step, actor, 4);
		Check(waiting.owned && nearPosition(waiting.desired.position, {0, 3.5f, 4}) &&
		      waiting.ReadSnapshot().followTargetError.empty(), "initial target recovery takes over at the requested head rather than hips");
		waiting.Release();

		for (CameraMode mode : {CameraMode::Track, CameraMode::Hold})
		{
			CameraController translated;
			prepare(translated, CameraFollowTarget::Head);
			translated.settings.mode = mode;
			translated.settings.followActor = true;
			partAvailable = false;
			translated.Tick(true, 0, step, actor, 1);
			const Vector3 base = translated.desired.position;
			partHead = {100, 200, 300};
			partHips = partHips + Vector3{2, .5f, -1};
			translated.Tick(true, 0, step, actor, 2);
			Check(nearPosition(translated.desired.position, base + Vector3{2, .5f, -1}) && partReads == 0 &&
			      translated.ReadSnapshot().followTargetError.empty(),
			      "Track/Hold advanced actor translation stays hips-based and ignores the follow-mode body-part selector");
			translated.Release();
		}

		Core selectedActor;
		prepare(selectedActor.camera, CameraFollowTarget::Head);
		auto otherActor = reinterpret_cast<void*>(0x31);
		selectedActor.NoteActor(actor);
		selectedActor.NoteActor(otherActor);
		selectedActor.SetTargetActor(otherActor);
		selectedActor.player.state = PlaybackState::Paused;
		selectedActor.OnLateUpdate(actor, nullptr, nullptr, 0, true, step, 1);
		Check(partReads == 0, "a nonselected actor callback never queries its body part for the camera");
		selectedActor.OnLateUpdate(otherActor, nullptr, nullptr, 0, false, step, 1);
		Check(partReads == 1 && partActor == otherActor && partReadTarget == CameraFollowTarget::Head,
		      "Core body-part query is limited to the selected actor on its own update");
		const int readsBeforeRender = partReads;
		for (int i = 0; i < 5; ++i) selectedActor.camera.BeforeRender(reinterpret_cast<void*>(0x10));
		Check(partReads == readsBeforeRender, "render callbacks only reapply the camera and never query actor bones");
		selectedActor.StopPerformance();
	}

	BeginTest("camera Core pause / preview / multiple actors");
	{
		Core core;
		core.camera.fnGetCamera = camera.fnGetCamera;
		core.camera.fnGetTransform = camera.fnGetTransform;
		core.camera.fnAlive = camera.fnAlive;
		core.camera.fnRead = camera.fnRead;
		core.camera.fnWrite = camera.fnWrite;
		core.camera.fnActorPose = camera.fnActorPose;
		auto second = reinterpret_cast<void*>(0x31);
		core.NoteActor(actor);
		core.NoteActor(second);
		core.player.state = PlaybackState::Paused;
		core.OnLateUpdate(actor, nullptr, nullptr, .016f, false);
		Check(core.camera.owned && core.camera.actor == actor, "paused MMD retains camera ownership");
		priorWrites = writes;
		core.OnLateUpdate(second, nullptr, nullptr, .016f, false);
		Check(writes == priorWrites && core.camera.actor == actor, "other actors do not advance or retarget camera");
		core.SetTargetActor(second);
		core.OnLateUpdate(second, nullptr, nullptr, .016f, false);
		Check(core.camera.actor == second, "camera follows selected actor");
		core.SetTargetActor(nullptr);
		core.camera.fnAlive = [](void* p) { return p != reinterpret_cast<void*>(0x30); };
		core.OnLateUpdate(second, nullptr, nullptr, .016f, false);
		Check(core.camera.actor == second, "dead first scene actor cannot strand camera selection");
		core.player.state = PlaybackState::Preview;
		core.OnLateUpdate(second, nullptr, nullptr, .016f, false);
		Check(core.camera.owned && core.camera.actor == second, "preview retains the same camera for precise inspection");
		core.StopPerformance();
		Check(!core.camera.owned, "stop still releases preview camera ownership");
	}

	BeginTest("camera track sampling / cuts / projection / format guard");
	CameraTrack track;
	track.fps = 30;
	CameraFrame a, b;
	a.position = {0, 0, 0};
	a.fov = 30;
	b.position = {10, 0, 0};
	b.rotation = Quaternion::FromEulerDegreesXYZ(0, 90, 40);
	b.fov = 60;
	b.flags = 1;
	track.frames = {a, b};
	CameraFrame sample;
	Check(track.Sample(1.f / 60, sample) && std::fabs(sample.position.x - 5) < 1e-5f && sample.fov == 45,
	      "fractional time samples pose and lens");
	Check(sample.flags == 0, "projection switches discretely at key");
	track.Sample(1, sample);
	Check(sample.flags == 1 && sample.position.x == 10, "past end holds last camera key");
	track.frames[1].flags = 3;
	track.Sample(1.f / 60, sample);
	Check(sample.position.x == 0, "camera cut never flies between keys");
	Check(!track.Sample(std::numeric_limits<float>::quiet_NaN(), sample), "non-finite sample time rejected");

	std::filesystem::path dir = std::filesystem::path(repoRoot) / "build/tests/camera";
	std::filesystem::create_directories(dir);
	{
		const auto path = dir / "embedded.gkmotion";
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write("GKMO", 4);
		uint32_t version = 1, frames = 1, bones = 1;
		float fps = 30, duration = 0, scale = .083f;
		out.write(reinterpret_cast<char*>(&version), 4);
		out.write(reinterpret_cast<char*>(&fps), 4);
		out.write(reinterpret_cast<char*>(&frames), 4);
		out.write(reinterpret_cast<char*>(&duration), 4);
		out.write(reinterpret_cast<char*>(&bones), 4);
		out.write(reinterpret_cast<char*>(&scale), 4);
		uint16_t hid = 0;
		out.write(reinterpret_cast<char*>(&hid), 2);
		out.put(4);
		out.put(1);
		out.write("Hips", 4);
		Vector3 root{};
		Quaternion q;
		out.write(reinterpret_cast<char*>(&root), 12);
		out.write(reinterpret_cast<char*>(&q), 16);
		out.put(1);
		LegacyCameraFrame camera{{1, 2, 3}, {0, 1, 0}, 60};
		out.write(reinterpret_cast<char*>(&camera), 28);
		out.close();
		MotionClip clip;
		Check(clip.LoadFromFile(path.string()) && clip.camera_frames.size() == 1 && clip.camera_frames[0].fov == 60,
		      "existing seven-float embedded camera tail loads");
		CameraController fallback;
		fallback.LoadTrack((dir / "missing-sidecar").string(), clip);
		CameraFrame legacy;
		Check(fallback.track.Sample(0, legacy) && legacy.position.x == 1 && legacy.fov == 60 && legacy.flags == 0,
		      "missing sidecar falls back to embedded camera track");
		std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
		Check(!clip.LoadFromFile(path.string()), "truncated embedded camera tail rejected");
	}
	auto writeTrack = [&](uint32_t version, uint32_t count, bool truncate)
	{
		std::ofstream out(dir / "camera.gkcamera", std::ios::binary | std::ios::trunc);
		out.write("GKCAM\0\0\0", 8);
		out.write(reinterpret_cast<char*>(&version), 4);
		out.write(reinterpret_cast<char*>(&track.fps), 4);
		out.write(reinterpret_cast<char*>(&count), 4);
		if (!truncate) out.write(reinterpret_cast<char*>(track.frames.data()),
		                         sizeof(CameraFrame) * track.frames.size());
	};
	writeTrack(1, 2, false);
	CameraTrack loaded;
	Check(loaded.Load((dir / "camera.gkcamera").string()), "binary camera v1 round trip");
	{
		CameraController reload;
		reload.fnGetCamera = camera.fnGetCamera;
		reload.fnGetTransform = camera.fnGetTransform;
		reload.fnAlive = camera.fnAlive;
		reload.fnRead = camera.fnRead;
		reload.fnWrite = camera.fnWrite;
		reload.fnActorPose = camera.fnActorPose;
		reload.LoadTrack(dir.string(), MotionClip{});
		reload.Tick(true, 0, .016f, actor);
		const CameraFrame originallySaved = reload.saved;
		const Vector3 capturedOrigin = reload.origin;
		track.frames[0].position.x = 25;
		writeTrack(1, 2, false);
		reload.LoadTrack(dir.string(), MotionClip{});
		Check(reload.track.frames[0].position.x == 0, "same-directory track is cached until invalidated");
		reload.InvalidateTrack();
		reload.LoadTrack(dir.string(), MotionClip{});
		Check(reload.track.frames[0].position.x == 25 && reload.owned &&
		      reload.saved.position.x == originallySaved.position.x && reload.origin.x == capturedOrigin.x,
		      "in-place sidecar import reloads without recapturing the restore pose or anchor");
		track.frames[0].position.x = 30;
		writeTrack(1, 2, false);
		reload.LoadTrack(dir.string(), MotionClip{}, true);
		Check(reload.track.frames[0].position.x == 30, "explicit force reload sees same-directory replacement");
		reload.Release();
		Check(actual.position.x == originallySaved.position.x && actual.fov == originallySaved.fov,
		      "Stop after camera replacement still restores the original camera");
		track.frames[0].position.x = 0;
	}
	writeTrack(2, 2, false);
	Check(!loaded.Load((dir / "camera.gkcamera").string()), "unknown camera version rejected");
	writeTrack(1, 2, true);
	Check(!loaded.Load((dir / "camera.gkcamera").string()), "truncated camera rejected");
	writeTrack(1, 1000001, false);
	Check(!loaded.Load((dir / "camera.gkcamera").string()), "oversized camera allocation rejected");
	track.frames[0].position.x = std::numeric_limits<float>::infinity();
	writeTrack(1, 2, false);
	Check(!loaded.Load((dir / "camera.gkcamera").string()), "non-finite camera rejected");

	BeginTest("v4 camera / morph payload ordering");
	{
		const auto path = dir / "camera-and-morph.gkclip";
		auto writeV4 = [&](bool hasCamera, bool invalidCamera = false)
		{
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out.write("GKCLIP\0\0", 8);
			const uint32_t version = 4, frames = 2, bones = 1;
			const float fps = 30, duration = 1.f / 30, scale = .07795f;
			out.write(reinterpret_cast<const char*>(&version), 4);
			out.write(reinterpret_cast<const char*>(&fps), 4);
			out.write(reinterpret_cast<const char*>(&frames), 4);
			out.write(reinterpret_cast<const char*>(&duration), 4);
			out.write(reinterpret_cast<const char*>(&bones), 4);
			out.write(reinterpret_cast<const char*>(&scale), 4);
			const uint16_t hid = 0;
			out.write(reinterpret_cast<const char*>(&hid), 2);
			out.put(4);
			out.put(1);
			out.write("Hips", 4);
			for (int i = 0; i < 2; ++i)
			{
				Vector3 root{static_cast<float>(i), 0, 0};
				Quaternion q;
				out.write(reinterpret_cast<const char*>(&root), 12);
				out.write(reinterpret_cast<const char*>(&q), 16);
			}
			out.put(hasCamera ? 1 : 0);
			if (hasCamera)
			{
				LegacyCameraFrame first{{1, 2, 3}, {0, 1, 0}, invalidCamera ? 180.f : 60.f};
				LegacyCameraFrame second{{7, 8, 9}, {1, 1, 0}, 70};
				out.write(reinterpret_cast<const char*>(&first), 28);
				out.write(reinterpret_cast<const char*>(&second), 28);
			}
			const uint32_t count = 1;
			const uint16_t length = 5;
			const float weights[] = {.25f, .75f};
			out.write(reinterpret_cast<const char*>(&count), 4);
			out.write(reinterpret_cast<const char*>(&length), 2);
			out.write("Smile", 5);
			out.write(reinterpret_cast<const char*>(weights), sizeof(weights));
		};
		MotionClip clip;
		writeV4(true);
		Check(clip.LoadFromFile(path.string()) && clip.camera_frames.size() == 2 &&
		      clip.camera_frames[1].position.x == 7 && clip.blendshape_names == std::vector<std::string>{"Smile"},
		      "v4 reads camera before the named morph table without shifting either stream");
		std::vector<float> weights;
		clip.SampleBlendshapes(1.f / 60, weights);
		Check(weights.size() == 1 && std::fabs(weights[0] - .5f) < 1e-5f,
		      "v4 morph weights retain interpolation after embedded camera data");
		writeV4(false);
		Check(clip.LoadFromFile(path.string()) && clip.camera_frames.empty() && clip.blendshape_weights.size() == 2,
		      "reloading v4 without camera clears previous camera and retains the morph stream");
		writeV4(true, true);
		Check(!clip.LoadFromFile(path.string()), "invalid v4 camera sample is rejected before morph playback");
		writeV4(true);
		std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
		Check(!clip.LoadFromFile(path.string()), "truncated v4 morph table after a camera payload is rejected");
	}

	BeginTest("camera config transaction and GUI queue");
	RuntimeConfig config;
	config.path = (dir / "control.json").string();
	WriteTextFile(config.path, R"({"camera":{"mode":2,"follow_behavior":0,"follow_actor":true,"limit_to_scene":true,
        "bounds_min":[0,1,2],"bounds_max":[3,4,5],"follow_offset":[1,2,3]}})");
	Check(config.Reload() && config.camera.mode == CameraMode::Follow &&
	      config.camera.followBehavior == FollowBehavior::LookAt && config.camera.boundsMax.z == 5,
	      "camera mode and follow behavior declarations parsed");
	auto previous = config.camera;
	WriteTextFile(config.path, R"({"camera":{"bounds_min":[9,9,9],"bounds_max":[1,1,1]}})");
	Check(!config.Reload() && config.camera == previous, "invalid bounds do not partly apply camera settings");
	Core core;
	core.config = config;
	core.ApplyDeclarativeConfig();
	auto guiSettings = previous;
	guiSettings.mode = CameraMode::Hold;
	guiSettings.followBehavior = FollowBehavior::FollowMove;
	core.QueueCameraSettings(guiSettings);
	core.ProcessPendingGuiCommands();
	Check(core.camera.settings.mode == CameraMode::Hold &&
	      core.camera.settings.followBehavior == FollowBehavior::FollowMove,
	      "GUI follow behavior settings applied on main thread");
	core.ApplyDeclarativeConfig();
	Check(core.camera.settings.mode == CameraMode::Hold &&
	      core.camera.settings.followBehavior == FollowBehavior::FollowMove,
	      "unchanged file declarations preserve GUI camera settings");

	BeginTest("camera per-mode transform and local math");
	{
		// 1. Default values check
		const auto defTrack = CameraSettings::DefaultTrackTransform();
		const auto defFollowMove = CameraSettings::DefaultFollowMoveTransform();
		const auto defLookAt = CameraSettings::DefaultLookAtTransform();

		Check(defTrack.position.x == 0.0f && defTrack.position.y == 0.0f && defTrack.position.z == 0.0f &&
		      defTrack.rotation.x == 0.0f && defTrack.rotation.y == 0.0f && defTrack.rotation.z == 0.0f,
		      "Track transform defaults to zero pos and zero rot");
		Check(defFollowMove.position.x == 0.0f && defFollowMove.position.y == 0.6f && defFollowMove.position.z == 3.0f &&
		      defFollowMove.rotation.x == 0.0f && defFollowMove.rotation.y == 0.0f && defFollowMove.rotation.z == 0.0f,
		      "FollowMove transform defaults to {0, 0.6, 3} pos and zero rot");
		Check(defLookAt.position.x == 0.0f && defLookAt.position.y == 0.6f && defLookAt.position.z == 3.0f &&
		      defLookAt.rotation.x == 0.0f && defLookAt.rotation.y == 0.0f && defLookAt.rotation.z == 0.0f,
		      "LookAt transform defaults to {0, 0.6, 3} pos and zero rot");

		CameraSettings defaultSettings;
		Check(!defaultSettings.usePerModeTransforms, "usePerModeTransforms is default false for backward compatibility");
		Check(defaultSettings.transformTrack == defTrack, "settings transformTrack defaults correctly");
		Check(defaultSettings.transformFollowMove == defFollowMove, "settings transformFollowMove defaults correctly");
		Check(defaultSettings.transformLookAt == defLookAt, "settings transformLookAt defaults correctly");
		Check(defaultSettings.EffectiveTrackTransform().position.x == defaultSettings.vmdOffset.x,
		      "legacy EffectiveTrackTransform reflects vmdOffset when usePerModeTransforms is false");
		Check(defaultSettings.EffectiveFollowMoveTransform().position.y == defaultSettings.followOffset.y,
		      "legacy EffectiveFollowMoveTransform reflects followOffset when usePerModeTransforms is false");
		Check(defaultSettings.EffectiveLookAtTransform().position.z == defaultSettings.followOffset.z,
		      "legacy EffectiveLookAtTransform reflects followOffset when usePerModeTransforms is false");

		// Validation checks
		CameraSettings valSettings;
		Check(valSettings.IsValid(), "default settings are valid");
		valSettings.transformTrack.rotation.x = 361.0f;
		Check(!valSettings.IsValid(), "rotation exceeding 360 deg is invalid");
		valSettings = CameraSettings{};
		valSettings.transformFollowMove.position.y = std::numeric_limits<float>::quiet_NaN();
		Check(!valSettings.IsValid(), "non-finite position is invalid");
		valSettings = CameraSettings{};
		valSettings.transformLookAt.rotation.z = -365.0f;
		Check(!valSettings.IsValid(), "negative rotation exceeding 360 deg is invalid");

		// 2. Per-mode Transform isolation
		CameraSettings iso;
		iso.usePerModeTransforms = true;
		iso.transformTrack = {{1.0f, 2.0f, 3.0f}, {10.0f, 20.0f, 30.0f}};
		iso.transformFollowMove = {{4.0f, 5.0f, 6.0f}, {40.0f, 50.0f, 60.0f}};
		iso.transformLookAt = {{7.0f, 8.0f, 9.0f}, {70.0f, 80.0f, 90.0f}};

		// Switching mode preserves all three independent transforms
		iso.mode = CameraMode::Track;
		Check(iso.EffectiveTrackTransform() == CameraModeTransform({{1.0f, 2.0f, 3.0f}, {10.0f, 20.0f, 30.0f}}),
		      "Track effective transform matches transformTrack");
		Check(iso.transformFollowMove.position.x == 4.0f && iso.transformLookAt.position.x == 7.0f,
		      "Other mode transforms untouched in Track mode");

		iso.mode = CameraMode::Follow;
		iso.followBehavior = FollowBehavior::FollowMove;
		Check(iso.EffectiveFollowMoveTransform() == CameraModeTransform({{4.0f, 5.0f, 6.0f}, {40.0f, 50.0f, 60.0f}}),
		      "FollowMove effective transform matches transformFollowMove");
		Check(iso.transformTrack.position.x == 1.0f && iso.transformLookAt.position.x == 7.0f,
		      "Track and LookAt transforms untouched in FollowMove mode");

		iso.followBehavior = FollowBehavior::LookAt;
		Check(iso.EffectiveLookAtTransform() == CameraModeTransform({{7.0f, 8.0f, 9.0f}, {70.0f, 80.0f, 90.0f}}),
		      "LookAt effective transform matches transformLookAt");
		Check(iso.transformTrack.position.x == 1.0f && iso.transformFollowMove.position.x == 4.0f,
		      "Track and FollowMove transforms untouched in LookAt mode");

		// 3. Controller execution: FollowMove pure translation + local rotation
		CameraController cam;
		static CameraFrame mockActual;
		static Vector3 mockHips{0, 1, 0};
		mockActual.position = {0, 1, 0};
		mockActual.rotation = Quaternion::Identity();
		mockActual.fov = 45;
		cam.fnGetCamera = []() -> void* { return reinterpret_cast<void*>(0x50); };
		cam.fnGetTransform = [](void*) -> void* { return reinterpret_cast<void*>(0x60); };
		cam.fnAlive = [](void*) { return true; };
		cam.fnRead = [](void*, void*, CameraFrame& out) { out = mockActual; return true; };
		cam.fnWrite = [](void*, void*, const CameraFrame& f) { mockActual = f; };
		cam.fnActorPose = [](void*, Vector3& origin, Quaternion& rot, Vector3& h)
		{
			origin = {0, 0, 0};
			rot = Quaternion::Identity();
			h = mockHips;
			return true;
		};

		cam.settings.mode = CameraMode::Follow;
		cam.settings.followBehavior = FollowBehavior::FollowMove;
		cam.settings.followRate = {1, 1, 1}; // test mode transforms independently of temporal response
		cam.settings.usePerModeTransforms = true;
		cam.settings.followFactor = {0.0f, 0.0f, 0.0f}; // isolate camera local translation
		cam.settings.transformFollowMove = {{0.0f, 0.6f, 3.0f}, {0.0f, 0.0f, 0.0f}};
		cam.Tick(true, 0.0f, 0.016f, actor);

		const Vector3 baseFollowPos = cam.desired.position;
		const Quaternion baseFollowRot = cam.desired.rotation;
		Check(std::fabs(baseFollowPos.x - 0.0f) < 1e-5f &&
		      std::fabs(baseFollowPos.y - 1.6f) < 1e-5f &&
		      std::fabs(baseFollowPos.z - 3.0f) < 1e-5f,
		      "FollowMove default position is at followOrigin + default offset");

		// Changing position.x and position.y translates without changing rotation!
		cam.settings.transformFollowMove.position = {2.5f, 1.8f, 3.0f};
		cam.Tick(true, 0.016f, 0.016f, actor);
		Check(std::fabs(cam.desired.position.x - 2.5f) < 1e-5f &&
		      std::fabs(cam.desired.position.y - 2.8f) < 1e-5f &&
		      std::fabs(cam.desired.position.z - 3.0f) < 1e-5f,
		      "FollowMove translates camera position cleanly");
		Check(QuatNear(cam.desired.rotation, baseFollowRot),
		      "FollowMove position change does NOT cause auto-orbiting or rotation shift");

		// Changing rotation applies local Pitch/Yaw/Roll offset
		cam.settings.transformFollowMove.rotation = {15.0f, -20.0f, 10.0f};
		cam.Tick(true, 0.032f, 0.016f, actor);
		const Quaternion expectedLocalRot = (baseFollowRot * Quaternion::FromEulerDegreesXYZ(15.0f, -20.0f, 10.0f)).Normalized();
		Check(QuatNear(cam.desired.rotation, expectedLocalRot),
		      "FollowMove rotation change applies local Euler offset directly to camera");
		Check(std::fabs(cam.desired.position.x - 2.5f) < 1e-5f,
		      "FollowMove rotation change does not alter camera translation");

		// 4. Controller execution: LookAt fixed tripod + gaze + rotation offset
		cam.settings.followBehavior = FollowBehavior::LookAt;
		cam.settings.transformLookAt = {{0.0f, 0.6f, 3.0f}, {0.0f, 0.0f, 0.0f}};
		cam.settings.followFactor = {1.0f, 1.0f, 1.0f};
		cam.Tick(true, 0.048f, 0.016f, actor);

		const Vector3 lookAtTripodPos = cam.desired.position;
		Check(std::fabs(lookAtTripodPos.x - 0.0f) < 1e-5f &&
		      std::fabs(lookAtTripodPos.y - 1.6f) < 1e-5f &&
		      std::fabs(lookAtTripodPos.z - 3.0f) < 1e-5f,
		      "LookAt position stays at basePos tripod");

		// Moving hips changes look target, but camera position remains fixed!
		mockHips = {3.0f, 2.0f, -1.0f};
		cam.Tick(true, 0.064f, 0.016f, actor);
		Check(std::fabs(cam.desired.position.x - lookAtTripodPos.x) < 1e-5f &&
		      std::fabs(cam.desired.position.y - lookAtTripodPos.y) < 1e-5f &&
		      std::fabs(cam.desired.position.z - lookAtTripodPos.z) < 1e-5f,
		      "LookAt tripod position remains fixed when hips move");

		// LookAt rotation offset applies on top of dynamic gaze
		// Un-rotated gaze direction
		cam.settings.transformLookAt.rotation = {0.0f, 0.0f, 0.0f};
		cam.Tick(true, 0.096f, 0.016f, actor);
		const Quaternion pureGazeRot = cam.desired.rotation;
		cam.settings.transformLookAt.rotation = {12.0f, 24.0f, 0.0f};
		cam.Tick(true, 0.112f, 0.016f, actor);
		const Quaternion expectedGazeRot = (pureGazeRot * Quaternion::FromEulerDegreesXYZ(12.0f, 24.0f, 0.0f)).Normalized();
		Check(QuatNear(cam.desired.rotation, expectedGazeRot),
		      "LookAt post-multiplies local rotation offset onto gaze rotation");
		Check(std::fabs(cam.desired.position.x - lookAtTripodPos.x) < 1e-5f,
		      "LookAt position remains fixed with rotation offset");

		// 5. Track (VMD) Transform: position and rotation offset sampled frame
		cam.settings.mode = CameraMode::Track;
		cam.settings.transformTrack = {{0.5f, -0.3f, 1.2f}, {10.0f, -15.0f, 5.0f}};
		CameraTrack testTrack;
		CameraFrame testFrame;
		testFrame.position = {2.0f, 3.0f, 4.0f};
		testFrame.rotation = Quaternion::FromEulerDegreesXYZ(5.0f, 10.0f, 0.0f);
		testFrame.fov = 40.0f;
		testTrack.frames = {testFrame};
		cam.track = testTrack;
		cam.Tick(true, 0.128f, 0.016f, actor);

		Check(std::fabs(cam.desired.position.x - (2.0f + 0.5f)) < 1e-5f &&
		      std::fabs(cam.desired.position.y - (3.0f - 0.3f)) < 1e-5f &&
		      std::fabs(cam.desired.position.z - (4.0f + 1.2f)) < 1e-5f,
		      "Track mode position applies transformTrack.position offset to sampled frame");
		const Quaternion expectedTrackRot = (testFrame.rotation * Quaternion::FromEulerDegreesXYZ(10.0f, -15.0f, 5.0f)).Normalized();
		Check(QuatNear(cam.desired.rotation, expectedTrackRot),
		      "Track mode rotation applies transformTrack.rotation offset to sampled frame");
		cam.Release();
	}
}
