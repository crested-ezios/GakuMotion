#pragma once

#include "../GkMotionData.hpp"
#include "../GkBlendshapePlayer.hpp"
#include "../GkEyePose.hpp"
#include "../GkWaistSupport.hpp"
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace GakuMotion
{
	// 手臂辅助骨链缓存：包含肩、臂、腕各关节的原生 _Roll_H 与 _H 辅助骨
	struct ArmAuxiliaryChain
	{
		void* shoulder{nullptr};
		Quaternion shoulderRestRel{0.0f, 0.0f, 0.0f, 1.0f};

		void* upperArm{nullptr};
		Quaternion upperArmRestRel{0.0f, 0.0f, 0.0f, 1.0f};
		void* upperArmH{nullptr}; // e.g. LeftArm_H (关节半角支撑)
		Quaternion upperArmHRestLocal{0.0f, 0.0f, 0.0f, 1.0f}; // 静息局部旋转
		Quaternion upperArmHRestRel{0.0f, 0.0f, 0.0f, 1.0f}; // 相对 Actor 的静息世界旋转
		void* upperArmRollH{nullptr}; // e.g. LeftArm_Roll_H (轴向扭转分担)
		Quaternion upperArmRollHRestLocal{0.0f, 0.0f, 0.0f, 1.0f}; // 静息局部旋转
		Quaternion upperArmRollHRestRel{0.0f, 0.0f, 0.0f, 1.0f}; // 相对 Actor 的静息世界旋转

		void* foreArm{nullptr};
		Quaternion foreArmRestRel{0.0f, 0.0f, 0.0f, 1.0f};
		void* foreArmH{nullptr}; // e.g. LeftForeArm_H (肘关节半角支撑)
		Quaternion foreArmHRestLocal{0.0f, 0.0f, 0.0f, 1.0f}; // 静息局部旋转
		Quaternion foreArmHRestRel{0.0f, 0.0f, 0.0f, 1.0f}; // 相对 Actor 的静息世界旋转
		void* foreArmRollH{nullptr}; // e.g. LeftForeArm_Roll_H (前臂扭转分担)
		Quaternion foreArmRollHRestLocal{0.0f, 0.0f, 0.0f, 1.0f}; // 静息局部旋转
		Quaternion foreArmRollHRestRel{0.0f, 0.0f, 0.0f, 1.0f}; // 相对 Actor 的静息世界旋转

		void* hand{nullptr};
		Quaternion handRestRel{0.0f, 0.0f, 0.0f, 1.0f};
		void* handH{nullptr}; // e.g. LeftHand_H (腕关节半角支撑)
		Quaternion handHRestLocal{0.0f, 0.0f, 0.0f, 1.0f}; // 静息局部旋转
		Quaternion handHRestRel{0.0f, 0.0f, 0.0f, 1.0f}; // 相对 Actor 的静息世界旋转

		void Reset()
		{
			shoulder = upperArm = upperArmH = upperArmRollH = nullptr;
			shoulderRestRel = upperArmRestRel = Quaternion::Identity();
			upperArmHRestLocal = upperArmHRestRel = Quaternion::Identity();
			upperArmRollHRestLocal = upperArmRollHRestRel = Quaternion::Identity();

			foreArm = foreArmH = foreArmRollH = nullptr;
			foreArmRestRel = Quaternion::Identity();
			foreArmHRestLocal = foreArmHRestRel = Quaternion::Identity();
			foreArmRollHRestLocal = foreArmRollHRestRel = Quaternion::Identity();

			hand = handH = nullptr;
			handRestRel = Quaternion::Identity();
			handHRestLocal = handHRestRel = Quaternion::Identity();
		}
	};

	// Identity tokens only: no saved pointer may be read or written until the
	// complete live chain has been reacquired from the actor's current humanoid.
	struct WaistSupportChain
	{
		void* hips{nullptr};
		void* pelvis{nullptr};
		void* upperLeg{nullptr};
		void* waistOffset{nullptr};
		void* thighOffset{nullptr};
		void* helper{nullptr};
		Vector3 savedLocalPosition;
		Quaternion savedLocalRotation;
		bool captured{false};

		bool SameTargetIdentity(const WaistSupportChain& other) const noexcept
		{
			return hips == other.hips && pelvis == other.pelvis &&
				waistOffset == other.waistOffset && helper == other.helper;
		}

		bool SameIdentity(const WaistSupportChain& other) const noexcept
		{
			return hips == other.hips && pelvis == other.pelvis && upperLeg == other.upperLeg &&
				waistOffset == other.waistOffset && thighOffset == other.thighOffset && helper == other.helper;
		}
	};

	// Structure caching bone transforms and bind poses for a specific actor
	struct IdolBoneBinding
	{
		void* actor{nullptr};
		BlendshapeBinding face;
		void* actorTransform{nullptr};
		void* rootTransform{nullptr};
		Vector3 restRootPos{0.0f, 0.0f, 0.0f}; // relative to actor
		Vector3 restActorPos{0.0f, 0.0f, 0.0f};
		Quaternion restActorRot{0.0f, 0.0f, 0.0f, 1.0f};
		Quaternion placementRestRootLocal;
		bool placementRestRootCaptured{false}, placementYawApplied{false};

		// Clip-wide floor offset when clipGroundAnchored; legacy bind-pose lift
		// otherwise. bindGroundLift preserves the original fallback across songs.
		float groundLift{0.0f};
		float bindGroundLift{0.0f};
		float calibratedRootHeight{0.0f};
		bool clipGroundAnchored{false};
		bool clipBindingDirty{false};
		float lastGeometryProbeTime{-99.0f};

		// Index corresponds to clip.bones[b]
		std::vector<void*> boneTransforms;
		std::vector<uint16_t> boneHids;
		std::vector<Quaternion> restRotations; // R_rest world (actor-relative)

		// R_rest LOCAL, captured at the same instant. This is the only character-
		// dependent quantity gkclip needs, and it lives here in RAM for the
		// lifetime of the binding: nothing is written to disk, no cache is keyed on
		// character or outfit, and the clip file stays role-agnostic.
		// (DevAgents/architecture/ACTION_FORMATS.md 2.2, option A frozen in #021.)
		std::vector<Quaternion> restLocals;
		const EyeTargetFrame* eyeTargetFrame{nullptr};
		bool eyeFrameResolved{false}, eyeFrameWarningLogged{false}, eyeGainLogged{false};
		bool bindPoseCaptured{false};
		bool defensivelyMuted{false};
		bool eyePoseModified{false};
		std::array<NativeTalkBinding, 2> talks{};
		size_t boundBoneCount{0}; // 成功绑定的骨骼数（日志/面板状态展示）

		void* cachedAnimator{nullptr};
		void* savedAnimatorController{nullptr};
		bool animatorStateCaptured{false};
		bool savedAnimatorEnabled{true};
		bool animatorControllerDetached{false};
		bool animatorWasDisabled{false};
		bool animatorRebound{false};
		void* cachedPlayableDirector{nullptr};

		// 原生手臂辅助骨缓存（以原生骨骼为主、阻尼保底为辅）
		ArmAuxiliaryChain leftArmAux;
		ArmAuxiliaryChain rightArmAux;
		std::array<WaistSupportChain, 2> waistSupport;
		bool waistSupportDiagnosticLogged{false};

		// UpperChest (Spine2, hid=54) 防破坑保护
		void* upperChestTransform{nullptr};
		Quaternion upperChestRestRel{0.0f, 0.0f, 0.0f, 1.0f};
		bool upperChestInClip{false};

		void Reset()
		{
			actor = nullptr;
			face = BlendshapeBinding{};
			actorTransform = nullptr;
			rootTransform = nullptr;
			restRootPos = {0.0f, 0.0f, 0.0f};
			restActorPos = {0.0f, 0.0f, 0.0f};
			restActorRot = {0.0f, 0.0f, 0.0f, 1.0f};
			placementRestRootLocal = Quaternion::Identity();
			placementRestRootCaptured = placementYawApplied = false;
			groundLift = 0.0f;
			bindGroundLift = 0.0f;
			calibratedRootHeight = 0.0f;
			clipGroundAnchored = false;
			clipBindingDirty = false;
			lastGeometryProbeTime = -99.0f;
			boneTransforms.clear();
			boneHids.clear();
			restRotations.clear();
			restLocals.clear();
			eyeTargetFrame = nullptr;
			eyeFrameResolved = eyeFrameWarningLogged = eyeGainLogged = false;
			bindPoseCaptured = false;
			defensivelyMuted = false;
			eyePoseModified = false;
			talks = {};
			boundBoneCount = 0;
			cachedAnimator = nullptr;
			savedAnimatorController = nullptr;
			animatorStateCaptured = false;
			savedAnimatorEnabled = true;
			animatorControllerDetached = false;
			animatorWasDisabled = false;
			animatorRebound = false;
			cachedPlayableDirector = nullptr;
			leftArmAux.Reset();
			rightArmAux.Reset();
			waistSupport = {};
			waistSupportDiagnosticLogged = false;
			upperChestTransform = nullptr;
			upperChestRestRel = Quaternion::Identity();
			upperChestInClip = false;
		}
	};
} // namespace GakuMotion
