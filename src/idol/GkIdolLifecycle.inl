namespace GakuMotion
{
	inline void IdolController::InvalidateClipBindings()
	{
		for (auto& entry : bindings)
		{
			entry.second.clipBindingDirty = true;
			entry.second.face.boundClip = nullptr;
		}
		currentBinding.clipBindingDirty = true;
		currentBinding.face.boundClip = nullptr;
	}

	inline void IdolController::LoadRootHeightCalibration(const std::string& path, const MotionClip& clip)
	{
		std::string error;
		if (!rootHeightCalibration.Load(path, clip, error))
		{
			if (fnLogInfo) fnLogInfo("[GakuMotion][RootHeight] ignored %s: %s", path.c_str(), error.c_str());
		}
		else if (rootHeightCalibration.enabled && fnLogInfo)
		{
			fnLogInfo("[GakuMotion][RootHeight] loaded %s: %.5f m (target rig match required)",
			          path.c_str(), rootHeightCalibration.offset);
		}
	}

	inline void IdolController::UpdateClipGroundAnchor(IdolBoneBinding& b, const MotionClip& clip)
	{
		b.groundLift = b.bindGroundLift;
		b.calibratedRootHeight = 0.0f;
		b.clipGroundAnchored = false;
		if (clip.version >= 2 &&
			b.actorTransform && fnGetLocalPosition)
		{
			std::array<Vector3, 21> offsets{};
			std::array<bool, 21> present{};
			for (size_t i = 0; i < clip.bones.size(); ++i)
			{
				int hid = clip.bones[i].humanoid_id;
				if (hid >= 21 || !b.boneTransforms[i]) continue;
				float p[3]{};
				fnGetLocalPosition(b.boneTransforms[i], p);
				offsets[hid] = Vector3(p[0], p[1], p[2]);
				present[hid] = true;
			}
			float lift = 0.0f;
			b.clipGroundAnchored = ComputeClipGroundLift(clip, offsets, present, lift);
			if (b.clipGroundAnchored)
			{
				b.groundLift = lift;
				b.calibratedRootHeight = rootHeightCalibration.Match(offsets, present);
			}
			GM_DEBUG(
				"[GakuMotion][GroundAnchor] actor=%p version=%u valid=%d lift=%.4f (foot/toe markers; actor pivot floor)",
				b.actor, clip.version, int(b.clipGroundAnchored), b.groundLift);
		}
	}

	inline void IdolController::BindArmAuxiliaryChains(void* actor, IdolBoneBinding& b, const MotionClip& clip) const
	{
		// ---------------------------------------------------------------------
		// ---------------------------------------------------------------------
		// 辅助骨精确探查与绑定：
		// 探查 UpperArm, ForeArm, Hand 下的原生 _Roll_H 与 _H 辅助骨
		// 注意：Gakumas 中 Hand_H 位于 ForeArm 的子节点下，必须按精确骨骼名解析杜绝覆盖
		// ---------------------------------------------------------------------
		auto findAuxChildByName = [this](void* parent, const char* targetSubstr) -> void*
		{
			if (!parent || !fnGetChildCount || !fnGetChild || !fnGetTransformName) return nullptr;
			int count = fnGetChildCount(parent);
			for (int i = 0; i < count; ++i)
			{
				void* child = fnGetChild(parent, i);
				if (!child) continue;
				std::string cname = fnGetTransformName(child);
				if (cname.find(targetSubstr) != std::string::npos)
				{
					return child;
				}
			}
			return nullptr;
		};

		auto captureAuxRest = [this, &b](void* trans, Quaternion& outLocal, Quaternion& outActorRel)
		{
			outLocal = Quaternion::Identity();
			outActorRel = Quaternion::Identity();
			if (trans)
			{
				if (fnGetLocalRotation)
				{
					float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
					fnGetLocalRotation(trans, r);
					outLocal = Quaternion(r[0], r[1], r[2], r[3]);
				}
				if (fnGetWorldRotation)
				{
					float rw[4]{0.0f, 0.0f, 0.0f, 1.0f};
					fnGetWorldRotation(trans, rw);
					Quaternion qWorld(rw[0], rw[1], rw[2], rw[3]);
					outActorRel = b.restActorRot.Inverse() * qWorld;
				}
			}
		};

		auto captureBoneRestRel = [this, &b](void* trans, Quaternion& outActorRel)
		{
			outActorRel = Quaternion::Identity();
			if (trans)
			{
				if (fnGetWorldRotation)
				{
					float rw[4]{0.0f, 0.0f, 0.0f, 1.0f};
					fnGetWorldRotation(trans, rw);
					Quaternion qWorld(rw[0], rw[1], rw[2], rw[3]);
					outActorRel = b.restActorRot.Inverse() * qWorld;
				}
				else if (fnGetLocalRotation)
				{
					float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
					fnGetLocalRotation(trans, r);
					outActorRel = Quaternion(r[0], r[1], r[2], r[3]);
				}
			}
		};

		b.leftArmAux.shoulder = fnGetBoneTransform(actor, 11);
		b.leftArmAux.upperArm = fnGetBoneTransform(actor, 13);
		b.leftArmAux.foreArm = fnGetBoneTransform(actor, 15);
		b.leftArmAux.hand = fnGetBoneTransform(actor, 17);

		// 捕获 UpperChest (Spine2, hid=54) 防破坑保护
		b.upperChestTransform = fnGetBoneTransform(actor, 54);
		b.upperChestInClip = false;
		for (const auto& bone : clip.bones)
		{
			if (bone.humanoid_id == 54)
			{
				b.upperChestInClip = true;
				break;
			}
		}
		captureBoneRestRel(b.upperChestTransform, b.upperChestRestRel);

		captureBoneRestRel(b.leftArmAux.shoulder, b.leftArmAux.shoulderRestRel);
		captureBoneRestRel(b.leftArmAux.upperArm, b.leftArmAux.upperArmRestRel);
		captureBoneRestRel(b.leftArmAux.foreArm, b.leftArmAux.foreArmRestRel);
		captureBoneRestRel(b.leftArmAux.hand, b.leftArmAux.handRestRel);

		b.leftArmAux.upperArmRollH = findAuxChildByName(b.leftArmAux.upperArm, "LeftArm_Roll_H");
		b.leftArmAux.upperArmH = findAuxChildByName(b.leftArmAux.upperArm, "LeftArm_H");
		b.leftArmAux.foreArmRollH = findAuxChildByName(b.leftArmAux.foreArm, "LeftForeArm_Roll_H");
		b.leftArmAux.foreArmH = findAuxChildByName(b.leftArmAux.foreArm, "LeftForeArm_H");
		b.leftArmAux.handH = findAuxChildByName(b.leftArmAux.foreArm, "LeftHand_H");
		if (!b.leftArmAux.handH && b.leftArmAux.hand)
		{
			b.leftArmAux.handH = findAuxChildByName(b.leftArmAux.hand, "LeftHand_H");
		}

		captureAuxRest(b.leftArmAux.upperArmH, b.leftArmAux.upperArmHRestLocal, b.leftArmAux.upperArmHRestRel);
		captureAuxRest(b.leftArmAux.upperArmRollH, b.leftArmAux.upperArmRollHRestLocal,
		               b.leftArmAux.upperArmRollHRestRel);
		captureAuxRest(b.leftArmAux.foreArmH, b.leftArmAux.foreArmHRestLocal, b.leftArmAux.foreArmHRestRel);
		captureAuxRest(b.leftArmAux.foreArmRollH, b.leftArmAux.foreArmRollHRestLocal,
		               b.leftArmAux.foreArmRollHRestRel);
		captureAuxRest(b.leftArmAux.handH, b.leftArmAux.handHRestLocal, b.leftArmAux.handHRestRel);

		b.rightArmAux.shoulder = fnGetBoneTransform(actor, 12);
		b.rightArmAux.upperArm = fnGetBoneTransform(actor, 14);
		b.rightArmAux.foreArm = fnGetBoneTransform(actor, 16);
		b.rightArmAux.hand = fnGetBoneTransform(actor, 18);

		captureBoneRestRel(b.rightArmAux.shoulder, b.rightArmAux.shoulderRestRel);
		captureBoneRestRel(b.rightArmAux.upperArm, b.rightArmAux.upperArmRestRel);
		captureBoneRestRel(b.rightArmAux.foreArm, b.rightArmAux.foreArmRestRel);
		captureBoneRestRel(b.rightArmAux.hand, b.rightArmAux.handRestRel);

		b.rightArmAux.upperArmRollH = findAuxChildByName(b.rightArmAux.upperArm, "RightArm_Roll_H");
		b.rightArmAux.upperArmH = findAuxChildByName(b.rightArmAux.upperArm, "RightArm_H");
		b.rightArmAux.foreArmRollH = findAuxChildByName(b.rightArmAux.foreArm, "RightForeArm_Roll_H");
		b.rightArmAux.foreArmH = findAuxChildByName(b.rightArmAux.foreArm, "RightForeArm_H");
		b.rightArmAux.handH = findAuxChildByName(b.rightArmAux.foreArm, "RightHand_H");
		if (!b.rightArmAux.handH && b.rightArmAux.hand)
		{
			b.rightArmAux.handH = findAuxChildByName(b.rightArmAux.hand, "RightHand_H");
		}

		captureAuxRest(b.rightArmAux.upperArmH, b.rightArmAux.upperArmHRestLocal, b.rightArmAux.upperArmHRestRel);
		captureAuxRest(b.rightArmAux.upperArmRollH, b.rightArmAux.upperArmRollHRestLocal,
		               b.rightArmAux.upperArmRollHRestRel);
		captureAuxRest(b.rightArmAux.foreArmH, b.rightArmAux.foreArmHRestLocal, b.rightArmAux.foreArmHRestRel);
		captureAuxRest(b.rightArmAux.foreArmRollH, b.rightArmAux.foreArmRollHRestLocal,
		               b.rightArmAux.foreArmRollHRestRel);
		captureAuxRest(b.rightArmAux.handH, b.rightArmAux.handHRestLocal, b.rightArmAux.handHRestRel);

		// Pointer dumps are suppressed: they were two long lines per bind and
		// buried the lines that actually matter. The warnings below stay --
		// a missing _H bone is a real finding, the addresses are not.
		if (fnLogInfo)
		{
			if (!b.leftArmAux.upperArmH || !b.leftArmAux.foreArmH || !b.leftArmAux.handH)
			{
				fnLogInfo("[GakuMotion][AuxBind][WARNING] One or more Left Arm _H auxiliary bones were not found!");
			}
			if (!b.rightArmAux.upperArmH || !b.rightArmAux.foreArmH || !b.rightArmAux.handH)
			{
				fnLogInfo(
					"[GakuMotion][AuxBind][WARNING] One or more Right Arm _H auxiliary bones were not found!");
			}
		}
	}

	inline void IdolController::DiscardBindings()
	{
		guiMikuFitSnapshot.store(0, std::memory_order_relaxed);
		captureRestoreUnconfirmed = false;
		bindings.clear();
		currentBinding.Reset();
		cachedAnimator = nullptr;
	}

	inline void IdolController::OnActorChanged(void* newActor)
	{
		// 2026-10-07 REGRESSION FIX. This unconditionally did
		//
		//     bindings.clear();  currentBinding.Reset();  cachedAnimator = nullptr;
		//
		// but the host calls OnLateUpdate once per actor per frame, so with four
		// characters on stage it ran four times a frame, each time throwing away
		// the binding the previous actor had just built. The device symptom was
		// brutal: PROBE A2 reported the RAW T-POSE instead of the captured rest
		// pose -- LeftUpperArm and RightUpperArm came back bit-identical
		// (-0.0000, 0.2442, 0.3317, 0.9112), devId 48.65 deg where the correct
		// value is 82.92 deg -- because the capture never got past the first bone
		// before the next actor wiped it. Every format stopped animating while
		// audio kept playing, and the status line sat at "Preview(f0) bones=0/0".
		//
		// Switching characters must not happen by accident. If a DIFFERENT actor
		// arrives while the current one is still bound and animating, keep the
		// current binding and ignore the newcomer. A genuine scene change is
		// still handled, because a gone actor leaves no live binding to keep.
		if (newActor && currentBinding.actor && currentBinding.bindPoseCaptured
			&& currentBinding.actor != newActor
			&& currentBinding.boundBoneCount > 0)
		{
			if (fnLogInfo)
			{
				fnLogInfo("[GakuMotion] OnActorChanged: ignoring %p, still driving %p"
				          " (multi-actor scene; use the GUI target selector)",
				          newActor, currentBinding.actor);
			}
			return;
		}
		if (fnLogInfo)
		{
			fnLogInfo(
				"[GakuMotion] IdolController::OnActorChanged: %p -> %p (clearing all bindings and controllers)",
				currentBinding.actor, newActor);
		}
		DiscardBindings();
	}

	inline void IdolController::CopyRuntimeConfigurationFrom(const IdolController& source)
	{
		fnGetBoneTransform = source.fnGetBoneTransform;
		fnGetComponentTransform = source.fnGetComponentTransform;
		fnGetTransformParent = source.fnGetTransformParent;
		fnSetWorldPosition = source.fnSetWorldPosition; fnSetWorldRotation = source.fnSetWorldRotation;
		fnGetWorldPosition = source.fnGetWorldPosition; fnGetWorldRotation = source.fnGetWorldRotation;
		fnSetLocalPosition = source.fnSetLocalPosition; fnSetLocalRotation = source.fnSetLocalRotation;
		fnGetLocalPosition = source.fnGetLocalPosition; fnGetLocalRotation = source.fnGetLocalRotation;
		fnGetLossyScale = source.fnGetLossyScale;
		fnSetAnimatorEnabled = source.fnSetAnimatorEnabled; fnGetAnimatorEnabled = source.fnGetAnimatorEnabled;
		fnAnimatorRebind = source.fnAnimatorRebind;
		fnGetAnimatorController = source.fnGetAnimatorController; fnSetAnimatorController = source.fnSetAnimatorController;
		fnPausePlayableDirector = source.fnPausePlayableDirector;
		fnGetChildCount = source.fnGetChildCount; fnGetChild = source.fnGetChild;
		fnGetTransformName = source.fnGetTransformName;
		faceApi = source.faceApi; fnCanDriveActor = source.fnCanDriveActor;
		fnLogInfo = source.fnLogInfo; fnLogWarn = source.fnLogWarn;
		armOffsetPitch = source.armOffsetPitch; armOffsetYaw = source.armOffsetYaw; armOffsetRoll = source.armOffsetRoll;
		useRestRotations = source.useRestRotations; legRestBasis = source.legRestBasis;
		preserveAnimatorForSecondaryPhysics = source.preserveAnimatorForSecondaryPhysics;
		captureCheckEnforced = source.captureCheckEnforced; useAnimatorRebind = source.useAnimatorRebind;
		facialEnableBaseline = source.facialEnableBaseline; facialMikuMouthFit = source.facialMikuMouthFit;
		facialMikuEyeFit = source.facialMikuEyeFit;
		facialMikuFitAllowUnverified = source.facialMikuFitAllowUnverified;
		facialWeights = source.facialWeights;
		facialBaselineMultiplier = source.facialBaselineMultiplier; facialCharacterOverride = source.facialCharacterOverride;
		SetExperimentalWaistSupportEnabled(source.experimentalWaistSupportEnabled);
		auxDriveMode = source.auxDriveMode; rootHeightCalibrationEnabled = source.rootHeightCalibrationEnabled;
		eyePoseRelativeGain = source.eyePoseRelativeGain;
	}
} // namespace GakuMotion
