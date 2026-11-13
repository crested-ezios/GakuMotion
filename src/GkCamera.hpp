#pragma once

#include "GkMotionData.hpp"
#include "GkRestoreChecks.hpp"
#include <array>
#include <mutex>

namespace GakuMotion
{
	inline bool CameraFinite(const Vector3& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}

	// Unity forward is +Z. Preserve the supplied up direction (including roll).
	inline Quaternion CameraLookRotation(Vector3 forward, Vector3 up = {0, 1, 0})
	{
		auto normalize = [](Vector3 v)
		{
			float n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
			return n > 1e-6f ? Vector3{v.x / n, v.y / n, v.z / n} : Vector3{};
		};
		auto cross = [](Vector3 a, Vector3 b)
		{
			return Vector3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
		};
		forward = normalize(forward);
		if (forward.x == 0 && forward.y == 0 && forward.z == 0) return Quaternion::Identity();
		Vector3 right = normalize(cross(up, forward));
		if (right.x == 0 && right.y == 0 && right.z == 0)
			right = normalize(cross({0, 0, 1}, forward));
		up = cross(forward, right);
		float m00 = right.x, m01 = up.x, m02 = forward.x;
		float m10 = right.y, m11 = up.y, m12 = forward.y;
		float m20 = right.z, m21 = up.z, m22 = forward.z;
		Quaternion q;
		float trace = m00 + m11 + m22;
		if (trace > 0)
		{
			float s = std::sqrt(trace + 1) * 2;
			q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s / 4};
		}
		else if (m00 > m11 && m00 > m22)
		{
			float s = std::sqrt(1 + m00 - m11 - m22) * 2;
			q = {s / 4, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
		}
		else if (m11 > m22)
		{
			float s = std::sqrt(1 + m11 - m00 - m22) * 2;
			q = {(m01 + m10) / s, s / 4, (m12 + m21) / s, (m02 - m20) / s};
		}
		else
		{
			float s = std::sqrt(1 + m22 - m00 - m11) * 2;
			q = {(m02 + m20) / s, (m12 + m21) / s, s / 4, (m10 - m01) / s};
		}
		return q.Normalized();
	}

	// Rate is the fraction of remaining error removed in 1/60 second, not a
	// fraction of displacement from the capture pose. Compensate for tick rate.
	inline float CameraFollowAlpha(float rate, float dt)
	{
		if (!std::isfinite(dt) || dt <= 0.0f || rate <= 0.0f) return 0.0f;
		if (rate >= 1.0f) return 1.0f;
		return -std::expm1(std::log1p(-rate) * (60.0f * dt));
	}

	struct CameraFrame
	{
		Vector3 position;
		Quaternion rotation;
		float fov{45};
		float orthographicSize{2};
		uint32_t flags{0}; // bit 0: orthographic; bit 1: cut from preceding sample
	};

	static_assert(sizeof(CameraFrame) == 40);

	class CameraTrack
	{
	public:
		float fps{60};
		std::vector<CameraFrame> frames;
		std::string error;

		bool Load(const std::string& path)
		{
			frames.clear();
			error.clear();
			std::ifstream in(MotionPathFromUtf8(path), std::ios::binary | std::ios::ate);
			auto fail = [&](const char* message)
			{
				error = message;
				return false;
			};
			if (!in) return fail("camera file unavailable");
			const auto length = in.tellg();
			in.seekg(0);
			char magic[8]{};
			uint32_t version = 0, count = 0;
			in.read(magic, 8);
			in.read(reinterpret_cast<char*>(&version), 4);
			in.read(reinterpret_cast<char*>(&fps), 4);
			in.read(reinterpret_cast<char*>(&count), 4);
			if (!in || std::memcmp(magic, "GKCAM\0\0\0", 8) != 0 || version != 1)
				return fail("unsupported camera header");
			if (!std::isfinite(fps) || fps <= 0 || fps > 240 || count == 0 || count > 1000000 ||
				length != static_cast<std::streamoff>(20ull + 40ull * count))
				return fail("invalid camera length or sample rate");
			std::vector<CameraFrame> next(count);
			in.read(reinterpret_cast<char*>(next.data()), 40ull * count);
			for (auto& f : next)
			{
				float norm = Quaternion::Dot(f.rotation, f.rotation);
				if (!CameraFinite(f.position) || !std::isfinite(norm) || norm < 1e-8f ||
					!std::isfinite(f.fov) || f.fov < 1 || f.fov > 179 ||
					!std::isfinite(f.orthographicSize) || f.orthographicSize <= 0 || (f.flags & ~3u))
					return fail("invalid camera sample");
				f.rotation = f.rotation.Normalized();
			}
			frames = std::move(next);
			return true;
		}

		bool Sample(float seconds, CameraFrame& out) const
		{
			if (frames.empty() || !std::isfinite(seconds)) return false;
			float frame = std::clamp(seconds * fps, 0.0f, static_cast<float>(frames.size() - 1));
			size_t a = static_cast<size_t>(frame), b = std::min(a + 1, frames.size() - 1);
			float t = frame - static_cast<float>(a);
			out = frames[a];
			if (frames[b].flags & 2u) return true;
			out.position = Vector3::Lerp(out.position, frames[b].position, t);
			out.rotation = Quaternion::Slerp(out.rotation, frames[b].rotation, t);
			out.fov += (frames[b].fov - out.fov) * t;
			out.orthographicSize += (frames[b].orthographicSize - out.orthographicSize) * t;
			return true;
		}
	};

	enum class CameraMode : int { Track = 0, Hold = 1, Follow = 2 };
	enum class FollowBehavior : int { LookAt = 0, FollowMove = 1 };
	enum class CameraFollowTarget : int { Hips = 0, Chest = 1, Head = 2 };

	inline const char* CameraFollowTargetName(CameraFollowTarget target)
	{
		switch (target)
		{
		case CameraFollowTarget::Hips: return "hips";
		case CameraFollowTarget::Chest: return "chest";
		case CameraFollowTarget::Head: return "head";
		}
		return "unknown";
	}

	inline const char* CameraFollowTargetLabel(CameraFollowTarget target)
	{
		switch (target)
		{
		case CameraFollowTarget::Hips: return "髋部";
		case CameraFollowTarget::Chest: return "胸部";
		case CameraFollowTarget::Head: return "头部";
		}
		return "未知部位";
	}

	inline int CameraFollowTargetBoneId(CameraFollowTarget target)
	{
		switch (target)
		{
		case CameraFollowTarget::Hips: return 0;
		case CameraFollowTarget::Chest: return 8;
		case CameraFollowTarget::Head: return 10;
		}
		return -1;
	}

	struct CameraModeTransform
	{
		Vector3 position{0.0f, 0.0f, 0.0f};
		Vector3 rotation{0.0f, 0.0f, 0.0f}; // Local Euler degrees (XYZ: Pitch, Yaw, Roll)

		bool IsValid() const
		{
			return CameraFinite(position) && CameraFinite(rotation) &&
				   std::fabs(rotation.x) <= 360.0f &&
				   std::fabs(rotation.y) <= 360.0f &&
				   std::fabs(rotation.z) <= 360.0f;
		}

		bool operator==(const CameraModeTransform& b) const
		{
			return position.x == b.position.x && position.y == b.position.y && position.z == b.position.z &&
				   rotation.x == b.rotation.x && rotation.y == b.rotation.y && rotation.z == b.rotation.z;
		}
	};

	struct CameraSettings
	{
		bool enabled{true};
		CameraMode mode{CameraMode::Track};
		FollowBehavior followBehavior{FollowBehavior::FollowMove};
		CameraFollowTarget followTarget{CameraFollowTarget::Hips};
		bool followActor{false}; // also translate the VMD path with the selected actor
		bool followPlacementYaw{false}; // rotate the camera with the primary actor placement yaw
		bool limitToScene{false};
		Vector3 boundsMin{-5, 0.05f, -5}, boundsMax{5, 5, 5}; // world-space AABB, opt-in
		Vector3 followOffset{0, 0.6f, 3}, targetOffset{0, 0.4f, 0}; // captured hips camera offset / selected part aim offset
		Vector3 vmdOffset{0, 0, 0}; // initial offset in actor coordinate frame (e.g. height adjustment for shorter idols)
		float vmdRangeScale{1.0f}; // scale the VMD camera path around an actor-local chest-height anchor
		Vector3 followFactor{1.0f, 1.0f, 1.0f}; // captured camera-local X/Y/Z displacement fractions
		Vector3 followRate{0.1f, 0.1f, 0.1f}; // per-axis response at 60 Hz: 0 freezes, 1 tracks immediately
		float followFov{45};
		bool srpFinalOverwriteFallback{false};
		int actorShadowExperimentMode{0}; // 0: Default, 1: Anchor near actor, 2: Bypass

		CameraModeTransform transformTrack{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
		CameraModeTransform transformFollowMove{{0.0f, 0.6f, 3.0f}, {0.0f, 0.0f, 0.0f}};
		CameraModeTransform transformLookAt{{0.0f, 0.6f, 3.0f}, {0.0f, 0.0f, 0.0f}};
		bool usePerModeTransforms{false};

		static CameraModeTransform DefaultTrackTransform() { return {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}; }
		static CameraModeTransform DefaultFollowMoveTransform() { return {{0.0f, 0.6f, 3.0f}, {0.0f, 0.0f, 0.0f}}; }
		static CameraModeTransform DefaultLookAtTransform() { return {{0.0f, 0.6f, 3.0f}, {0.0f, 0.0f, 0.0f}}; }

		CameraModeTransform EffectiveTrackTransform() const
		{
			if (usePerModeTransforms) return transformTrack;
			return {vmdOffset, transformTrack.rotation};
		}

		CameraModeTransform EffectiveFollowMoveTransform() const
		{
			if (usePerModeTransforms) return transformFollowMove;
			return {followOffset, transformFollowMove.rotation};
		}

		CameraModeTransform EffectiveLookAtTransform() const
		{
			if (usePerModeTransforms) return transformLookAt;
			return {followOffset, transformLookAt.rotation};
		}

		bool IsValid() const
		{
			const int modeValue = static_cast<int>(mode);
			const int followBehaviorValue = static_cast<int>(followBehavior);
			return modeValue >= 0 && modeValue <= 2 && followBehaviorValue >= 0 && followBehaviorValue <= 1 &&
				CameraFollowTargetBoneId(followTarget) >= 0 &&
				CameraFinite(boundsMin) && CameraFinite(boundsMax) &&
				boundsMin.x <= boundsMax.x && boundsMin.y <= boundsMax.y && boundsMin.z <= boundsMax.z &&
				CameraFinite(followOffset) && CameraFinite(targetOffset) &&
				CameraFinite(vmdOffset) && std::isfinite(vmdRangeScale) && vmdRangeScale >= 0.3f && vmdRangeScale <= 1.0f &&
				CameraFinite(followFactor) &&
				followFactor.x >= 0.0f && followFactor.x <= 1.0f &&
				followFactor.y >= 0.0f && followFactor.y <= 1.0f &&
				followFactor.z >= 0.0f && followFactor.z <= 1.0f &&
				CameraFinite(followRate) &&
				followRate.x >= 0.0f && followRate.x <= 1.0f &&
				followRate.y >= 0.0f && followRate.y <= 1.0f &&
				followRate.z >= 0.0f && followRate.z <= 1.0f &&
				std::isfinite(followFov) && followFov >= 1 && followFov <= 179 &&
				actorShadowExperimentMode >= 0 && actorShadowExperimentMode <= 2 &&
				transformTrack.IsValid() &&
				transformFollowMove.IsValid() &&
				transformLookAt.IsValid();
		}

		bool operator==(const CameraSettings& b) const
		{
			auto same = [](Vector3 a, Vector3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
			return enabled == b.enabled && mode == b.mode && followBehavior == b.followBehavior && followTarget == b.followTarget &&
				followActor == b.followActor && followPlacementYaw == b.followPlacementYaw &&
				limitToScene == b.limitToScene && same(boundsMin, b.boundsMin) && same(boundsMax, b.boundsMax) &&
				same(followOffset, b.followOffset) && same(targetOffset, b.targetOffset) &&
				same(vmdOffset, b.vmdOffset) && vmdRangeScale == b.vmdRangeScale &&
				same(followFactor, b.followFactor) && same(followRate, b.followRate) && followFov == b.followFov &&
				srpFinalOverwriteFallback == b.srpFinalOverwriteFallback &&
				actorShadowExperimentMode == b.actorShadowExperimentMode &&
				transformTrack == b.transformTrack &&
				transformFollowMove == b.transformFollowMove &&
				transformLookAt == b.transformLookAt &&
				usePerModeTransforms == b.usePerModeTransforms;
		}
	};

	// Pure controller: Unity access is supplied by the plugin, only on the main thread.
	// An active session includes Pause, so a paused MMD frame cannot be moved manually.
	class CameraController
	{
	public:
		CameraSettings settings;
		CameraTrack track;
		void* camera{nullptr};
		void* transform{nullptr};
		void* actor{nullptr};
		CameraFrame saved, desired;
		Vector3 origin, followOrigin;
		Quaternion originRotation;
		bool owned{false};
		bool captureCheckEnforced{false}, captureRestorePending{false};
		bool followCaptured{false};
		std::string loadedDirectory;

		void* (*fnGetCamera)(){nullptr};
		void* (*fnGetTransform)(void*){nullptr};
		bool (*fnAlive)(void*){nullptr};
		bool (*fnRead)(void*, void*, CameraFrame&){nullptr};
		void (*fnWrite)(void*, void*, const CameraFrame&){nullptr};
		bool (*fnActorPose)(void*, Vector3&, Quaternion&, Vector3&){nullptr};
		// Read only on the Unity-thread tick; Hips already comes from fnActorPose.
		bool (*fnFollowTarget)(void*, CameraFollowTarget, Vector3&){nullptr};
		void (*fnReleaseVcam)(bool){nullptr};
		// Includes virtual-camera/brain/focus mutations beyond CameraFrame.
		CaptureRestoreStatus (*fnReleaseVcamForCapture)(){nullptr};
		void (*fnWriteFovFallback)(void*, float){nullptr};

		struct Snapshot
		{
			CameraSettings settings;
			bool owned{false}, hasTrack{false};
			std::string error;
			bool available{false};
			Vector3 position;
			float fov{45};
			std::string followTargetError;
		};

		Snapshot ReadSnapshot() const
		{
			std::lock_guard lock(snapshotMutex_);
			return snapshot_;
		}

		void Publish()
		{
			std::lock_guard lock(snapshotMutex_);
			snapshot_ = {
				settings, owned, !track.frames.empty(), track.error,
				fnGetCamera && fnGetTransform && fnAlive && fnRead && fnWrite && fnActorPose,
				desired.position, desired.fov, followTargetError_
			};
		}

		void Release(bool restore = true)
		{
			if (restore && captureCheckEnforced) { ReleaseForCapture(); return; }
			// Destroyed cameras are never written during a scene transition.
			if (owned && restore && fnAlive && fnAlive(camera) && fnAlive(transform) && fnWrite)
				fnWrite(camera, transform, saved);
			if (fnReleaseVcam) fnReleaseVcam(restore);
			owned = false;
			captureRestorePending = false;
			camera = transform = actor = nullptr;
			followCaptured = false;
			followLocal_ = {};
			lastFollowFrame_ = 0;
			followTargetError_.clear();
			Publish();
		}

		CaptureRestoreStatus ReleaseForCapture()
		{
			CaptureRestoreStatus result{owned || captureRestorePending, true};
			try
			{
				if (owned || captureRestorePending)
				{
					if (!fnAlive || !fnAlive(camera) || !fnAlive(transform) || !fnRead || !fnWrite)
						result.confirmed = false;
					else
					{
						fnWrite(camera, transform, saved);
						CameraFrame after;
						const auto closeValue = [](float a, float b) { return std::isfinite(a) && std::isfinite(b) && std::fabs(a-b) <= 1e-4f; };
						const auto unit = [](const Quaternion& q) {
							const float n=Quaternion::Dot(q,q); return std::isfinite(n) && std::fabs(n-1.0f)<=1e-3f;
						};
						if (!fnRead(camera, transform, after) || !closeValue(after.position.x, saved.position.x) ||
							!closeValue(after.position.y, saved.position.y) || !closeValue(after.position.z, saved.position.z) ||
							!closeValue(after.fov,saved.fov) || !closeValue(after.orthographicSize,saved.orthographicSize) ||
							(after.flags & 1u) != (saved.flags & 1u) ||
							!unit(after.rotation) || !unit(saved.rotation) ||
							std::fabs(Quaternion::Dot(after.rotation, saved.rotation)) < 1.0f-1e-4f) result.confirmed=false;
					}
				}
				if (fnReleaseVcamForCapture)
				{
					const auto extra = fnReleaseVcamForCapture();
					result.modified |= extra.modified;
					result.confirmed &= extra.confirmed;
				}
				else if ((owned || captureRestorePending) && fnReleaseVcam) { fnReleaseVcam(true); result.confirmed = false; }
			}
			catch (...) { result.confirmed = false; }
			owned = false;
			captureRestorePending = !result.confirmed;
			if (result.confirmed) camera = transform = actor = nullptr;
			followCaptured = false;
			followLocal_ = {};
			lastFollowFrame_ = 0;
			followTargetError_.clear();
			Publish();
			return result;
		}

		// Imports replace the selected song's sidecar in place. Marking it dirty
		// keeps the original restore pose and actor anchor throughout that session.
		void InvalidateTrack() { trackDirty_ = true; }
		static bool ReadTrack(const std::string& directory, const MotionClip& clip, CameraTrack& next)
		{
			const auto path = directory + "/camera.gkcamera";
			std::error_code ec;
			const bool exists = std::filesystem::exists(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
			if (ec) { next.error = "cannot inspect camera file"; return false; }
			if (exists) return next.Load(path);
			next = {};
			next.fps = clip.fps;
			for (const auto& sample : clip.camera_frames)
			{
				CameraFrame frame;
				frame.position = sample.position;
				frame.rotation = CameraLookRotation(sample.target - sample.position);
				frame.fov = sample.fov;
				next.frames.push_back(frame);
			}
			return true;
		}
		void InstallTrack(const std::string& directory, CameraTrack next)
		{
			if (loadedDirectory != directory) Release();
			loadedDirectory = directory;
			trackDirty_ = false;
			track = std::move(next);
			Publish();
		}

		void LoadTrack(const std::string& directory, const MotionClip& clip, bool forceReload = false)
		{
			if (loadedDirectory == directory && !trackDirty_ && !forceReload) return;
			if (loadedDirectory != directory) Release();
			loadedDirectory = directory;
			trackDirty_ = false;
			track.frames.clear();
			track.error.clear();
			const auto path = directory + "/camera.gkcamera";
			if (std::ifstream(MotionPathFromUtf8(path), std::ios::binary)) track.Load(path);
			else
			{
				track.fps = clip.fps;
				for (const auto& c : clip.camera_frames)
				{
					CameraFrame f;
					f.position = c.position;
					f.rotation = CameraLookRotation(c.target - c.position);
					f.fov = c.fov;
					track.frames.push_back(f);
				}
			}
			Publish();
		}

		void Tick(bool active, float time, float dt, void* targetActor, uint64_t physicalFrame = 0,
		          float placementYawDegrees = 0.0f, Vector3 trackAnchorLocal = {})
		{
			if (!active || !settings.enabled || !settings.IsValid() || !targetActor)
			{
				Release();
				return;
			}
			if (!fnGetCamera || !fnGetTransform || !fnAlive || !fnRead || !fnWrite || !fnActorPose) return;
			void* next = fnGetCamera();
			if (!next || !fnAlive(next))
			{
				Release(false);
				return;
			}
			if (owned && (camera != next || actor != targetActor || !fnAlive(transform))) Release();
			Vector3 actorOrigin, hips;
			Quaternion actorRotation;
			if (!fnActorPose(targetActor, actorOrigin, actorRotation, hips))
			{
				Release();
				return;
			}
			Vector3 followTarget = hips;
			if (settings.mode == CameraMode::Follow && settings.followTarget != CameraFollowTarget::Hips &&
				(!fnFollowTarget || !fnFollowTarget(targetActor, settings.followTarget, followTarget) || !CameraFinite(followTarget)))
			{
				followTargetError_ = std::string("跟随部位（") + CameraFollowTargetLabel(settings.followTarget) +
					"）当前不可用；" + (owned ? "保持上次镜头，等待目标恢复。" : "等待目标恢复后接管镜头。");
				if (owned) fnWrite(camera, transform, desired);
				Publish();
				return;
			}
			followTargetError_.clear();
			if (!owned)
			{
				void* tr = fnGetTransform(next);
				CameraFrame initial;
				if (!tr || !fnAlive(tr) || !fnRead(next, tr, initial)) return;
				camera = next;
				transform = tr;
				actor = targetActor;
				saved = desired = initial;
				origin = actorOrigin;
				originRotation = actorRotation;
				followOrigin = hips;
				followCaptured = true;
				owned = true;
			}
			// Placement yaw turns the pose without changing the actor transform.
			// Apply the same world-up rotation to the camera basis, around its actor anchor.
			const Quaternion placementYaw = settings.followPlacementYaw && std::isfinite(placementYawDegrees)
				? Quaternion::FromEulerDegreesXYZ(0.0f, placementYawDegrees, 0.0f) : Quaternion::Identity();
			const Quaternion viewBasis = placementYaw * originRotation;
			CameraFrame frame = saved;
			bool hasTrack = settings.mode == CameraMode::Track && track.Sample(time, frame);
			if (hasTrack)
			{
				// The actor anchor includes the user XYZ placement. Its later
				// translation must move the VMD path as well. Full hips following
				// already includes that translation, so use the captured anchor
				// in that mode to avoid applying placement twice.
				const CameraModeTransform tf = settings.EffectiveTrackTransform();
				const Quaternion qRot = Quaternion::FromEulerDegreesXYZ(tf.rotation.x, tf.rotation.y, tf.rotation.z);
				if (settings.vmdRangeScale != 1.0f)
				{
					// A fixed actor-local anchor avoids scaling the track towards the floor.
					// Preserve orientation and scale the implied focus distance with the pose.
					const Vector3 pivot{0.0f, 1.0f, 0.0f};
					const Vector3 delta = frame.position - pivot;
					frame.position = pivot + Vector3{delta.x * settings.vmdRangeScale,
						delta.y * settings.vmdRangeScale, delta.z * settings.vmdRangeScale};
					frame.orthographicSize *= settings.vmdRangeScale;
				}
				// The Live baseline follows the actor transform, while the authored
				// track follows the optional placement yaw around that baseline.
				const Vector3 anchor = CameraFinite(trackAnchorLocal) ? actorRotation.Rotate(trackAnchorLocal) : Vector3{};
				frame.position = (settings.followActor ? origin : actorOrigin) + anchor + viewBasis.Rotate(frame.position + tf.position);
				frame.rotation = (viewBasis * frame.rotation * qRot).Normalized();
			}
			else if (settings.mode == CameraMode::Track)
			{
				const CameraModeTransform tf = settings.EffectiveTrackTransform();
				const Quaternion qRot = Quaternion::FromEulerDegreesXYZ(tf.rotation.x, tf.rotation.y, tf.rotation.z);
				frame.position = actorOrigin + placementYaw.Rotate(saved.position - actorOrigin) + viewBasis.Rotate(tf.position);
				frame.rotation = (placementYaw * saved.rotation * qRot).Normalized();
			}
			const bool followMode = settings.mode == CameraMode::Follow;
			Vector3 followDelta{};
			if (followMode || settings.followActor)
			{
				const Quaternion basis = saved.rotation.Normalized();
				// Integrate once per physical frame. Render callbacks only reapply
				// desired; paused playback still chases using real elapsed time.
				if (physicalFrame == 0 || physicalFrame != lastFollowFrame_)
				{
					const Vector3 local = basis.Inverse().Rotate(followTarget - followOrigin);
					const Vector3 goal{local.x * settings.followFactor.x,
						local.y * settings.followFactor.y, local.z * settings.followFactor.z};
					followLocal_.x += (goal.x - followLocal_.x) * CameraFollowAlpha(settings.followRate.x, dt);
					followLocal_.y += (goal.y - followLocal_.y) * CameraFollowAlpha(settings.followRate.y, dt);
					followLocal_.z += (goal.z - followLocal_.z) * CameraFollowAlpha(settings.followRate.z, dt);
					lastFollowFrame_ = physicalFrame;
				}
				followDelta = basis.Rotate(followLocal_);
			}
			else followLocal_ = {};
			Vector3 followLookTarget{};
			if (followMode)
			{
				if (settings.followBehavior == FollowBehavior::FollowMove)
				{
					const CameraModeTransform tf = settings.EffectiveFollowMoveTransform();
					const Quaternion qRot = Quaternion::FromEulerDegreesXYZ(tf.rotation.x, tf.rotation.y, tf.rotation.z);
					const Vector3 basePos = followOrigin + viewBasis.Rotate(tf.position);
					frame.position = basePos + followDelta;
					Vector3 baseForward = viewBasis.Rotate(settings.targetOffset - Vector3{0.0f, 0.6f, 3.0f});
					if (baseForward.x == 0.0f && baseForward.y == 0.0f && baseForward.z == 0.0f)
						baseForward = viewBasis.Rotate(Vector3{0.0f, 0.0f, -1.0f});
					frame.rotation = (CameraLookRotation(baseForward) * qRot).Normalized();
					frame.fov = settings.followFov;
					frame.flags = 0;
				}
				else
				{
					const CameraModeTransform tf = settings.EffectiveLookAtTransform();
					const Quaternion qRot = Quaternion::FromEulerDegreesXYZ(tf.rotation.x, tf.rotation.y, tf.rotation.z);
					const Vector3 basePos = followOrigin + viewBasis.Rotate(tf.position);
					const Vector3 baseLook = followOrigin + viewBasis.Rotate(settings.targetOffset);
					followLookTarget = baseLook + followDelta;
					frame.position = basePos;
					frame.rotation = (CameraLookRotation(followLookTarget - frame.position) * qRot).Normalized();
					frame.fov = settings.followFov;
					frame.flags = 0;
				}
			}
			else if (settings.followActor && followCaptured)
			{
				frame.position = frame.position + followDelta;
			}
			if (settings.limitToScene)
			{
				frame.position.x = std::clamp(frame.position.x, settings.boundsMin.x, settings.boundsMax.x);
				frame.position.y = std::clamp(frame.position.y, settings.boundsMin.y, settings.boundsMax.y);
				frame.position.z = std::clamp(frame.position.z, settings.boundsMin.z, settings.boundsMax.z);
				if (followMode && settings.followBehavior == FollowBehavior::LookAt)
				{
					const CameraModeTransform tf = settings.EffectiveLookAtTransform();
					const Quaternion qRot = Quaternion::FromEulerDegreesXYZ(tf.rotation.x, tf.rotation.y, tf.rotation.z);
					frame.rotation = (CameraLookRotation(followLookTarget - frame.position) * qRot).Normalized();
				}
			}
			desired = frame;
			fnWrite(camera, transform, desired);
			Publish();
		}

		// Compatibility for callers that pass a Live track anchor without yaw.
		void Tick(bool active, float time, float dt, void* targetActor, uint64_t physicalFrame, Vector3 trackAnchorLocal)
		{
			Tick(active, time, dt, targetActor, physicalFrame, 0.0f, trackAnchorLocal);
		}

		// Called at the render boundary after all actor/Cinemachine updates.
		void BeforeRender(void* renderingCamera)
		{
			if (owned && fnAlive && (!fnAlive(camera) || !fnAlive(transform) || !fnAlive(actor)))
			{
				Release(false);
				return;
			}
			if (owned && renderingCamera == camera && fnAlive && fnAlive(camera) && fnAlive(transform))
			{
				if (fnWrite) fnWrite(camera, transform, desired);
				if (settings.srpFinalOverwriteFallback && fnWriteFovFallback)
				{
					fnWriteFovFallback(camera, desired.fov);
				}
			}
		}

		bool BlocksTransform(void* tr) const { return owned && tr == transform; }
		bool BlocksCamera(void* c) const { return owned && c == camera; }

	private:
		Vector3 followLocal_{}; // filtered displacement in the captured camera basis
		uint64_t lastFollowFrame_{0};
		std::string followTargetError_;
		bool trackDirty_{false};
		mutable std::mutex snapshotMutex_;
		Snapshot snapshot_;
	};
} // namespace GakuMotion
