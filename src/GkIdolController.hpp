#pragma once

#include "GkMotionData.hpp"
#include "GkLog.hpp"
#include "GkBlendshapePlayer.hpp"
#include "GkEyePose.hpp"
#include "GkGroundAnchor.hpp"
#include "GkRootHeightCalibration.hpp"
#include "GkBoneOverride.hpp"   // 全身骨骼旋转「自定义编辑」覆盖层
#include "GkAuxChannelModel.hpp"  // 辅助骨驱动模型：全部系数由实机录制标定（Plan A）
#include "GkWaistSupport.hpp"
#include "idol/GkAuxGroundTruth.hpp"
#include "idol/GkIdolTypes.hpp"
#include "idol/GkOfficialQuartzMath.hpp"
#include <unordered_map>
#include <vector>
#include <array>
#include <string>
#include <cstring>
#include <cmath>
#include <functional>
#include <atomic>

namespace GakuMotion
{
	class IdolController
	{
	public:
		std::unordered_map<void*, IdolBoneBinding> bindings;
		IdolBoneBinding currentBinding;

		// Records only completed original-pose Foot writes. A consumer clears the
		// actor/mask before Tick and must not infer freshness from clip time alone.
		struct FootPoseWriteStamp
		{
			uint64_t serial{0};
			void* actor{nullptr};
			unsigned mask{0};
			std::array<void*, 2> transforms{};
		} footPoseWriteStamp;

		struct EyePoseWriteStamp
		{
			uint64_t serial{0};
			void* actor{nullptr};
			unsigned mask{0};
			std::array<void*, 2> transforms{};
			std::array<Quaternion, 2> localTargets{};
		} eyePoseWriteStamp;

		// Signature callback types to interface with UnityResolve or Il2Cpp
		using GetBoneTransformFn = void* (*)(void* actor, int humanoidBoneId);
		using GetComponentTransformFn = void* (*)(void* component);
		using SetPositionFn = void (*)(void* transform, float x, float y, float z);
		using SetRotationFn = void (*)(void* transform, float x, float y, float z, float w);
		using GetPositionFn = void (*)(void* transform, float* outPos);
		using GetRotationFn = void (*)(void* transform, float* outRot);
		using SetComponentEnabledFn = void (*)(void* component, bool enabled);
		using GetComponentEnabledFn = bool (*)(void* component);
		using AnimatorRebindFn = void (*)(void* animator);
		using GetAnimatorControllerFn = void* (*)(void* animator);
		using SetAnimatorControllerFn = void (*)(void* animator, void* controller);
		using PlayablePauseFn = void (*)(void* director);
		using GetChildCountFn = int (*)(void* transform);
		using GetChildFn = void* (*)(void* transform, int index);
		using GetTransformNameFn = std::string (*)(void* transform);
		using LogFn = void (*)(const char* fmt, ...);

		// PROBE A3/A6 fire once per clip load, on the first frame that writes bones.
		// Set after each ApplyMotionToBinding() call so one run of one clip produces
		// one block of output instead of 52 lines per frame.
		//
		// It is reset at the top of every rebind (see Rebind()) rather than only at
		// load: PROBE A2 runs on each rebind while A3/A6 used to run only once per
		// PROCESS, so with three clips played in one session the A2 block and the
		// A3 block could describe two different binds. Reading restLocal from one and
		// the delta from the other produced a convincing but completely fictional
		// "the composition disagrees by 86 deg" result. A3 now also restates
		// restLocal and rVmd on the write frame (see below), so its block is
		// self-contained and no cross-block pairing is required.
		bool probeFirstFrameDone{false};

		GetBoneTransformFn fnGetBoneTransform{nullptr};
		GetComponentTransformFn fnGetComponentTransform{nullptr};
		GetComponentTransformFn fnGetTransformParent{nullptr};
		SetPositionFn fnSetWorldPosition{nullptr};
		SetRotationFn fnSetWorldRotation{nullptr};
		GetPositionFn fnGetWorldPosition{nullptr};
		GetRotationFn fnGetWorldRotation{nullptr};
		SetPositionFn fnSetLocalPosition{nullptr};
		SetRotationFn fnSetLocalRotation{nullptr};
		GetPositionFn fnGetLocalPosition{nullptr};
		GetRotationFn fnGetLocalRotation{nullptr};
		std::function<void(void*, float*)> fnGetLossyScale;
		SetComponentEnabledFn fnSetAnimatorEnabled{nullptr};
		GetComponentEnabledFn fnGetAnimatorEnabled{nullptr};
		AnimatorRebindFn fnAnimatorRebind{nullptr};
		GetAnimatorControllerFn fnGetAnimatorController{nullptr};
		SetAnimatorControllerFn fnSetAnimatorController{nullptr};
		PlayablePauseFn fnPausePlayableDirector{nullptr};
		GetChildCountFn fnGetChildCount{nullptr};
		GetChildFn fnGetChild{nullptr};
		GetTransformNameFn fnGetTransformName{nullptr};
		FaceApi faceApi;
		// Runtime supplies the photography-scene boundary. Offline users may omit
		// it; the native plugin fails closed when its scene API is unavailable.
		bool (*fnCanDriveActor)(void*){nullptr};

		bool CanDriveActor(void* actor) const
		{
			return actor && (!fnCanDriveActor || fnCanDriveActor(actor));
		}

		LogFn fnLogInfo{nullptr};
		LogFn fnLogWarn{nullptr};

		// Dynamic arm A-Pose offsets (degrees)
		float armOffsetPitch{0.0f};
		float armOffsetYaw{0.0f};
		float armOffsetRoll{0.0f};

		// If true (default), bone rotations are calibrated relative to actor-relative rest pose
		bool useRestRotations{true};
		void* cachedAnimator{nullptr};
		// Opt-in until hair/cloth and same-frame body input are verified on device.
		bool preserveAnimatorForSecondaryPhysics{false};
		bool captureCheckEnforced{false};
		bool captureRestoreUnconfirmed{false};
		bool facialEnableBaseline{true};
		bool facialMikuMouthFit{true};
		// Published-profile components default to their accepted grade only: the
		// eye map applies, the inverse-distance mouth map stays opt-in.
		bool facialMikuEyeFit{true};
		bool facialMikuFitAllowUnverified{false};
		struct FacialUiSnapshot
		{
			std::atomic<unsigned> value{0};
			FacialUiSnapshot() = default;
			FacialUiSnapshot(const FacialUiSnapshot& other) : value(other.load()) {}
			FacialUiSnapshot& operator=(const FacialUiSnapshot& other) { store(other.load()); return *this; }
			unsigned load(std::memory_order order = std::memory_order_relaxed) const { return value.load(order); }
			void store(unsigned next, std::memory_order order = std::memory_order_relaxed) { value.store(next, order); }
		};
		FacialUiSnapshot guiMikuFitSnapshot; // Coherent GUI read; preserves controller fixture copy semantics.
		float facialBaselineMultiplier{1.0f};
		FacialWeights facialWeights;
		std::string facialCharacterOverride{"auto"};

	private:
		bool experimentalWaistSupportEnabled{true};


		bool WaistSupportApisAvailable() const noexcept;
		void* FindExactWaistChild(void* parent, const char* name) const;
		bool ResolveWaistSupportTarget(const IdolBoneBinding& b, int side, WaistSupportChain& chain) const;
		bool ResolveWaistSupportReferences(const IdolBoneBinding& b, int side, WaistSupportChain& chain) const;
		bool WaistSupportScalesValid(const WaistSupportChain& chain) const;
		static bool WaistSupportRotationValid(const Quaternion& q) noexcept;
		void RestoreWaistSupport(IdolBoneBinding& b);
		void RestoreWaistSupportSide(WaistSupportChain& saved, const WaistSupportChain& live);
		void ApplyWaistSupport(IdolBoneBinding& b);

		void BindArmAuxiliaryChains(void* actor, IdolBoneBinding& b, const MotionClip& clip) const;
		bool ComputeMainBoneTarget(IdolBoneBinding& b, const MotionClip& clip, size_t i, int hid, void* trans,
		                           const Quaternion& rVmd, const Quaternion& rRestRel, const Quaternion& curActorRot,
		                           bool overridesActive, bool hasDynamicArm,
		                           const Quaternion& qArmLeft, const Quaternion& qArmRight,
		                           const std::array<Quaternion, 64>& writtenWorldRots,
		                           const std::array<bool, 64>& writtenWorldRotValid,
		                           Quaternion& outTarget, Quaternion& outGkLocal,
		                           bool& outIsGkClip, int& appliedOverrides);
		void ApplyArmAuxiliaryForBone(const IdolBoneBinding& b, int hid,
		                              const Quaternion& rRestRel, const Quaternion& curActorRot,
		                              const Quaternion& rTarget,
		                              const std::array<Quaternion, 64>& writtenWorldRots,
		                              const std::array<bool, 64>& writtenWorldRotValid,
		                              float currentTimeSeconds) const;
		void ApplyPlacementYawToBinding(IdolBoneBinding& b, const Vector3& curActorPos,
		                                const Vector3& worldOffset, bool clipWritesRoot) const;
#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
		void ProbeBindPreRebindPose(void* actor) const;
		void ProbeBindGroundAndSegments(void* actor, const IdolBoneBinding& b, const float hp[3], const float fp[3], bool haveFoot) const;
		void ProbeBindPostRebindRestLocals(const IdolBoneBinding& b, const MotionClip& clip) const;
		void ProbeMotionBoneReadback(const IdolBoneBinding& b, const MotionClip& clip, size_t i, int hid, void* trans,
		                             const Quaternion& rVmd, const Quaternion& gkLocalOut,
		                             const std::vector<Quaternion>& vmdRotations,
		                             float currentTimeSeconds, bool isGkClip, bool probeA3Pending,
		                             bool overridesActive, bool hasDynamicArm) const;
		void ProbeMotionPostApplyGeometry(IdolBoneBinding& b, const MotionClip& clip, const Vector3& vmdRootPos,
		                                  const Quaternion& curActorRot, float currentTimeSeconds, bool probeA3Pending) const;
#endif

	public:
		bool IsExperimentalWaistSupportEnabled() const noexcept { return experimentalWaistSupportEnabled; }
		bool SetExperimentalWaistSupportEnabled(bool enabled);

		const BoneOverrideSet* boneOverrides{nullptr};
		int lastAppliedOverrideCount{0}; // 上一帧实际生效的覆盖条目数
		int auxDriveMode{kAuxNormal};

		static void* ResolveModelSourceTransform(const IdolBoneBinding& b, int hid) noexcept;
		static void* ResolveModelAuxTransform(const IdolBoneBinding& b, const char* auxBone) noexcept;
		void ApplyAuxModelToBinding(const IdolBoneBinding& b) const;
		void SetAuxDriveMode(int mode);
		void CycleAuxDriveMode() { SetAuxDriveMode((auxDriveMode + 1) % kAuxModeCount); }
		bool IsAuxInjectionEnabled() const noexcept { return auxDriveMode == kAuxNormal; }

		AuxGroundTruth auxGroundTruth;
		bool LoadAuxGroundTruth(const std::string& path);
		// humanoidId: 13/14=UpperArm, 15/16=LowerArm, 17/18=Hand；isRoll: true=_Roll_H, false=_H
		const Quaternion* LookupAuxReplay(int humanoidId, bool isRoll, float timeSeconds) const noexcept;
		void SetBoneOverrides(const BoneOverrideSet* overrides) noexcept { boneOverrides = overrides; }
		void InvalidateClipBindings();

		RootHeightCalibration rootHeightCalibration;
		bool rootHeightCalibrationEnabled{true};
		void LoadRootHeightCalibration(const std::string& path, const MotionClip& clip);
		void UpdateClipGroundAnchor(IdolBoneBinding& b, const MotionClip& clip);
		void BindActor(void* actor, const MotionClip& clip, void* animator = nullptr);
		void DiscardBindings();
		void OnActorChanged(void* newActor);

		void CaptureAnimatorState(IdolBoneBinding& b);
		void DetachAnimatorController(IdolBoneBinding& b);
		void RestoreAnimatorState(IdolBoneBinding& b, bool rebind);
		void DefensiveMute(void* animatorComponent, void* playableDirector);
		void DefensiveMute(void* actor, void* animatorComponent, void* playableDirector);
		void ResetToRestPose(IdolBoneBinding& b);
		BodyFaceRestoreResult RestoreForCapture();
		void RestoreMute(void* animatorComponent = nullptr, void* playableDirector = nullptr);

		static Quaternion FromEulerXYZ(float pitchDeg, float yawDeg, float rollDeg);

		using OfficialQuartzMath = GakuMotion::OfficialQuartzMath;
		using QuartzDriverAuxResult = GakuMotion::QuartzDriverAuxResult;
		static inline QuartzDriverAuxResult SolveQuartzAuxiliaryBones(
			const Quaternion& dQ_local,
			const Vector3& boneAxis,
			bool hasRollH) noexcept
		{
			return GakuMotion::SolveQuartzAuxiliaryBones(dQ_local, boneAxis, hasRollH);
		}

		void ApplyMotionToBinding(IdolBoneBinding& b, const MotionClip& clip, float currentTimeSeconds,
		                          float facialTimeSeconds = -1.0f);

		// ---- Target actor gating ------------------------------------------------
		// The host calls OnLateUpdate once per actor per frame, in scene order
		// (hski -> jsna -> hume -> ttmr), so without a gate EVERY character in the
		// scene is driven by the clip. That produced two separate failures:
		//
		//  1. Visual: all four characters dance, not just the one being watched.
		//  2. Diagnostic: PROBE output from different actors is interleaved and
		//     IDENTICAL in shape, so a log read top-to-bottom mixes characters.
		//     On 2026-10-07 that showed up as two PROBE A8 blocks in the SAME
		//     frame reporting leg_spread 166.02 and 29.65 -- one character standing
		//     (correct, after the Ry(90) fix) and one still splayed.
		//
		// nullptr means "drive every actor", which preserves the old behaviour for
		// single-character scenes. The plugin sets it when the user picks a target.
		void* targetActor{nullptr};

		void SetTargetActor(void* a) { targetActor = a; }
		bool IsTargetActor(void* a) const { return targetActor == nullptr || targetActor == a; }

		// A/B switch for Animator.Rebind() at bind time. See the comment at the call
		// site: on a Unity humanoid it snaps every bone to the prefab T-Pose, so the
		// "rest pose" captured after it is the T-Pose and the hips land at floor
		// level. Default OFF because the device measurement says that is what sinks
		// the character. RestoreMute()'s Rebind is a different thing and unaffected.
		bool useAnimatorRebind{false};

		// Fold the rig's local rest basis into the gkclip write, for the LEG chain
		// only. PROBE A2 measures the humanoid's leg rest far from Identity
		// (LeftUpperLeg 106.59 deg, RightUpperLeg 119.05 deg), and a gkclip delta is
		// defined against a canonical all-Identity rest, so writing the bare delta
		// lands the leg on a rest the clip never assumed.
		//
		// Settled on device by an A/B over the whole clip (tools/ab_leg_rest.py),
		// measuring per-segment verticality against the bind-time baseline:
		//
		//   condition      torso      thigh      shin       drop
		//   bare delta    +0.9895    -0.1565   -0.0464    -0.0852
		//   with basis    +0.9891    -0.8559   -0.5564    -0.5624
		//   BIND baseline ~ +0.990   -0.984    -0.935     -0.7764
		//
		// The torso is unmoved either way (+0.99), which is why the arm-folding fix
		// was unaffected and why this could hide for so long: an error of 106-119 deg
		// on the legs is invisible to every bone-angle check that only compares
		// relatives.
		//
		// Scoped to the legs deliberately. Applying it to the whole body would undo
		// the H1 fix that cured the arm folding, since the torso's rest locals are
		// small (Spine 2.47 deg, Neck 9.92 deg) and the bare delta is correct there.
		bool legRestBasis{true};

		// HID21/22 gkclip deltas are scaled by the character's measured eye-region
		// response ratio (see GkEyePose.hpp and GkEyeTargetFrames.hpp). This is not
		// LookAt and not a late write: it is the same single eye write, only with a
		// geometry-derived angle. false = raw 1:1 source rotation (the rejected
		// baseline and the A/B control).
		bool eyePoseRelativeGain{true};

		// User's initial placement relative to the original scene position (metres,
		// WORLD axes). Extends the old Y-only rootHeightOffset to X/Y/Z. Applied to
		// driven poses, not actor/rest snapshots, so replays and song switches cannot
		// accumulate translation. RestoreMute still returns to the original scene.
		// Zero this extra placement before measuring the unmodified motion stream.
		Vector3 initialPositionOffset{0.0f, 0.0f, 0.0f};

		bool SetInitialPositionOffset(const Vector3& offset)
		{
			if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) return false;
			initialPositionOffset = offset;
			return true;
		}

		bool referenceAnchorActive{false};
		Vector3 referenceAnchorPos{0.0f, 0.0f, 0.0f};
		Quaternion referenceAnchorRot{0.0f, 0.0f, 0.0f, 1.0f};

		// Set an explicit slot-1 reference anchor so dependent slots interpret
		// initialPositionOffset and placementYawDegrees in slot-1 local space.
		bool SetReferenceAnchor(const Vector3& pos, const Quaternion& rot) noexcept
		{
			if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return false;
			const float n2 = rot.x * rot.x + rot.y * rot.y + rot.z * rot.z + rot.w * rot.w;
			if (!std::isfinite(n2) || n2 < 1e-8f) return false;
			referenceAnchorPos = pos;
			referenceAnchorRot = rot.Normalized();
			referenceAnchorActive = true;
			return true;
		}

		void ClearReferenceAnchor() noexcept
		{
			referenceAnchorActive = false;
			referenceAnchorPos = {0.0f, 0.0f, 0.0f};
			referenceAnchorRot = Quaternion::Identity();
		}

		bool SetAnchorOverride(const Vector3& pos, const Quaternion& rot) noexcept
		{
			return SetReferenceAnchor(pos, rot);
		}

		void ClearAnchorOverride() noexcept { ClearReferenceAnchor(); }

		float placementYawDegrees{0.0f};

		bool SetPlacementYawDegrees(float degrees) noexcept
		{
			if (!std::isfinite(degrees)) return false;
			placementYawDegrees = std::remainder(degrees, 360.0f);
			return true;
		}

		// Share runtime adapters and user policy, never another actor's bindings,
		// placement, per-clip calibration/source evidence or override/replay data.

		void CopyRuntimeConfigurationFrom(const IdolController& source);
		void ApplyMotion(const MotionClip& clip, float currentTimeSeconds, void* actor = nullptr,
		                 float facialTimeSeconds = -1.0f);
	};
} // namespace GakuMotion

#include "idol/GkIdolWaistAux.inl"
#include "idol/GkIdolBindProbes.inl"
#include "idol/GkIdolPostProbes.inl"
#include "idol/GkIdolLifecycle.inl"
#include "idol/GkIdolBind.inl"
#include "idol/GkIdolAnimatorRestore.inl"
#include "idol/GkIdolBoneTarget.inl"
#include "idol/GkIdolArmAuxSolver.inl"
#include "idol/GkIdolPose.inl"
