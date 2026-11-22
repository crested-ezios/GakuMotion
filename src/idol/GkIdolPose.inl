namespace GakuMotion
{
	inline void IdolController::ApplyMotionToBinding(IdolBoneBinding& b, const MotionClip& clip, float currentTimeSeconds,
	                                                 float facialTimeSeconds)
	{
		if (!b.actor || !b.bindPoseCaptured || b.boneTransforms.empty()) return;

		++footPoseWriteStamp.serial;
		footPoseWriteStamp.actor = b.actor;
		footPoseWriteStamp.mask = 0;
		footPoseWriteStamp.transforms = {};
		++eyePoseWriteStamp.serial;
		eyePoseWriteStamp.actor = b.actor;
		eyePoseWriteStamp.mask = 0;
		eyePoseWriteStamp.transforms = {};

		Vector3 vmdRootPos;
		std::vector<Quaternion> vmdRotations;
		clip.Sample(currentTimeSeconds, vmdRootPos, vmdRotations);
		// A partial clip may omit Hips. Start that unkeyed root from its original
		// local pose, never from the preceding frame's placed rotation.
		const bool clipWritesRoot = [&]
		{
			for (size_t i = 0; i < clip.bones.size() && i < b.boneTransforms.size(); ++i)
				if (clip.bones[i].humanoid_id == 0 && b.boneTransforms[i] == b.rootTransform) return true;
			return false;
		}();
		if ((placementYawDegrees != 0.0f || b.placementYawApplied) && !clipWritesRoot &&
			b.rootTransform && fnGetLocalRotation && fnSetLocalRotation)
		{
			if (!b.placementRestRootCaptured)
			{
				float q[4]{0, 0, 0, 1}; fnGetLocalRotation(b.rootTransform, q);
				b.placementRestRootLocal = Quaternion(q[0], q[1], q[2], q[3]);
				b.placementRestRootCaptured = true;
			}
			const auto& q = b.placementRestRootLocal;
			fnSetLocalRotation(b.rootTransform, q.x, q.y, q.z, q.w);
			b.placementYawApplied = false;
		}
		if (facialTimeSeconds < 0.0f) facialTimeSeconds = currentTimeSeconds;
		if (facialTimeSeconds != currentTimeSeconds && std::any_of(clip.bones.begin(), clip.bones.end(),
			[](const auto& bone) { return bone.head_space_eye_delta; }))
		{
			// Eye rotations imported from the facial VMD share the pose stream,
			// but must follow that file's timing together with its morphs.
			Vector3 unusedRoot;
			std::vector<Quaternion> facialRotations;
			clip.Sample(facialTimeSeconds, unusedRoot, facialRotations);
			for (size_t i = 0; i < clip.bones.size(); ++i)
				if (clip.bones[i].head_space_eye_delta) vmdRotations[i] = facialRotations[i];
		}
		b.face.enableBaseline = facialEnableBaseline;
		b.face.baselineMultiplier = facialBaselineMultiplier;
		b.face.facialWeights = facialWeights;
		b.face.characterOverride = facialCharacterOverride;
		b.face.enableMikuMouthFit = facialMikuMouthFit;
		b.face.enableMikuEyeFit = facialMikuEyeFit;
		b.face.facialFitAllowUnverified = facialMikuFitAllowUnverified;
		b.face.Apply(b.actor, clip, facialTimeSeconds, faceApi);
		// Roles come from the binding, never from pointer arithmetic on the
		// compiled table: an external profile has no home there.
		const unsigned fittedRole = b.face.fitRole;
		const unsigned eyeDecision = static_cast<unsigned>(b.face.mikuEyeDecision);
		guiMikuFitSnapshot.store(static_cast<unsigned>(b.face.mikuFitDecision) | (fittedRole << 8) |
			(eyeDecision << 16), std::memory_order_relaxed);

		// 1. Current Actor World Transform (and optional multi-actor reference anchor)
		Vector3 ownActorPos = b.restActorPos;
		Quaternion ownActorRot = b.restActorRot;
		if (b.actorTransform && fnGetWorldPosition && fnGetWorldRotation)
		{
			float aPos[3]{0.0f, 0.0f, 0.0f};
			float aRot[4]{0.0f, 0.0f, 0.0f, 1.0f};
			fnGetWorldPosition(b.actorTransform, aPos);
			fnGetWorldRotation(b.actorTransform, aRot);
			ownActorPos = Vector3(aPos[0], aPos[1], aPos[2]);
			ownActorRot = Quaternion(aRot[0], aRot[1], aRot[2], aRot[3]);
		}
		const Vector3 curActorPos = referenceAnchorActive ? referenceAnchorPos : ownActorPos;
		const Quaternion curActorRot = referenceAnchorActive ? referenceAnchorRot : ownActorRot;
		const Vector3 worldOffset = referenceAnchorActive ? curActorRot.Rotate(initialPositionOffset) : initialPositionOffset;

		// Anchor supported v2+ clips to the actor pivot using one clip-wide
		// FK floor offset. Bind-time hips height never enters that path.
		// Other clips retain the legacy baseline. In solo mode the initial
		// placement is WORLD-space; when referenceAnchorActive is true it is
		// expressed in slot-1 local coordinates and rotated by curActorRot.
		if (b.rootTransform)
		{
			Vector3 localTarget(
				b.restRootPos.x + vmdRootPos.x,
				(b.clipGroundAnchored ? 0.0f : b.restRootPos.y) + vmdRootPos.y + b.groundLift +
				(rootHeightCalibrationEnabled ? b.calibratedRootHeight : 0.0f),
				b.restRootPos.z + vmdRootPos.z
			);
			Vector3 worldTarget = curActorPos + curActorRot.Rotate(localTarget) + worldOffset;
			if (fnSetWorldPosition)
			{
				fnSetWorldPosition(b.rootTransform, worldTarget.x, worldTarget.y, worldTarget.z);
			}
			else if (fnSetLocalPosition)
			{
				localTarget = localTarget + curActorRot.Inverse().Rotate(worldOffset);
				fnSetLocalPosition(b.rootTransform, localTarget.x, localTarget.y, localTarget.z);
			}
		}

		// 3. Bone World Rotations
		bool hasDynamicArm = (armOffsetPitch != 0.0f || armOffsetYaw != 0.0f || armOffsetRoll != 0.0f);
		Quaternion qArmLeft = hasDynamicArm
			                      ? FromEulerXYZ(armOffsetPitch, armOffsetYaw, armOffsetRoll)
			                      : Quaternion::Identity();
		Quaternion qArmRight = hasDynamicArm
			                       ? FromEulerXYZ(armOffsetPitch, -armOffsetYaw, -armOffsetRoll)
			                       : Quaternion::Identity();

		// 4. 全身骨骼旋转自定义编辑（外部 JSON 热加载覆盖表）
		const bool overridesActive =
			(boneOverrides != nullptr) && boneOverrides->enabled && !boneOverrides->Empty();
		int appliedOverrides = 0;

		// 缓存本帧已写入骨骼的世界旋转，供下游子关节与半角支撑骨实时引用
		std::array<Quaternion, 64> writtenWorldRots;
		std::array<bool, 64> writtenWorldRotValid{};
		writtenWorldRotValid.fill(false);

		// PROBE A3/A6 fire once, on the first frame that actually writes a bone.
		bool probeA3Pending = probeFirstFrameDone ? false : true;

		for (size_t i = 0; i < clip.bones.size() && i < b.boneTransforms.size(); ++i)
		{
			void* trans = b.boneTransforms[i];
			if (!trans || i >= vmdRotations.size()) continue;

			const Quaternion& rVmd = vmdRotations[i];
			const Quaternion& rRestRel = (i < b.restRotations.size()) ? b.restRotations[i] : Quaternion::Identity();
			const int hid = clip.bones[i].humanoid_id;
			Quaternion rTarget = Quaternion::Identity();
			Quaternion gkLocalOut = Quaternion::Identity();
			bool isGkClip = false;
			if (!ComputeMainBoneTarget(b, clip, i, hid, trans, rVmd, rRestRel, curActorRot,
			                           overridesActive, hasDynamicArm, qArmLeft, qArmRight,
			                           writtenWorldRots, writtenWorldRotValid,
			                           rTarget, gkLocalOut, isGkClip, appliedOverrides))
			{
				continue;
			}

			// 写入主骨骼旋转
			//
			// gkclip MUST go through setLocalRotation. Handing a canonical delta
			// to setWorldRotation makes the engine solve for a local rotation
			// that back-subtracts the parent chain, which is a documented
			// failure mode in this project (it is what the "thin waist trough"
			// bug turned out to be). gk_world_dump keeps the absolute world
			// write, which is exactly what that format records.
			bool mainRotationWritten = false;
			if (isGkClip)
			{
				if (fnSetLocalRotation)
				{
					Quaternion writeLocal = gkLocalOut;
					if (referenceAnchorActive && trans == b.rootTransform)
					{
						Quaternion ownParentRot = ownActorRot;
						if (fnGetTransformParent && fnGetWorldRotation)
						{
							if (void* parent = fnGetTransformParent(trans))
							{
								float pq[4]{0.0f, 0.0f, 0.0f, 1.0f};
								fnGetWorldRotation(parent, pq);
								ownParentRot = Quaternion(pq[0], pq[1], pq[2], pq[3]).Normalized();
							}
							else
							{
								ownParentRot = Quaternion::Identity();
							}
						}
						writeLocal = (ownParentRot.Inverse() * curActorRot * gkLocalOut).Normalized();
					}
					fnSetLocalRotation(trans, writeLocal.x, writeLocal.y, writeLocal.z, writeLocal.w);
					mainRotationWritten = true;
				}
				else if (fnSetWorldRotation)
				{
					// Degraded path: no local setter available, so fall back to
					// the world write using the already-composed rTarget.
					const Quaternion writeWorld = (referenceAnchorActive && trans == b.rootTransform)
						                              ? (curActorRot * rTarget).Normalized()
						                              : rTarget;
					fnSetWorldRotation(trans, writeWorld.x, writeWorld.y, writeWorld.z, writeWorld.w);
					mainRotationWritten = true;
				}
			}
			else if (fnSetWorldRotation)
			{
				fnSetWorldRotation(trans, rTarget.x, rTarget.y, rTarget.z, rTarget.w);
				mainRotationWritten = true;
			}
			else if (fnSetLocalRotation)
			{
				fnSetLocalRotation(trans, rTarget.x, rTarget.y, rTarget.z, rTarget.w);
				mainRotationWritten = true;
			}
			if (mainRotationWritten && (hid == 5 || hid == 6))
			{
				const unsigned side = static_cast<unsigned>(hid - 5);
				footPoseWriteStamp.mask |= 1u << side;
				footPoseWriteStamp.transforms[side] = trans;
			}
			if (mainRotationWritten && (hid == 21 || hid == 22)) b.eyePoseModified = true;
			if (mainRotationWritten && clip.bones[i].head_space_eye_delta && (hid == 21 || hid == 22))
			{
				const unsigned side = static_cast<unsigned>(hid - 21);
				eyePoseWriteStamp.mask |= 1u << side;
				eyePoseWriteStamp.transforms[side] = trans;
				eyePoseWriteStamp.localTargets[side] = gkLocalOut;
			}

			if (hid >= 0 && hid < 64)
			{
				writtenWorldRots[hid] = rTarget;
				writtenWorldRotValid[hid] = true;
			}

#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
			ProbeMotionBoneReadback(b, clip, i, hid, trans, rVmd, gkLocalOut, vmdRotations, currentTimeSeconds, isGkClip,
			                        probeA3Pending, overridesActive, hasDynamicArm);
#endif

			// 51 骨骼动作缺失 UpperChest 保护：自动同步 Chest (hid=8) 旋转，彻底根治肩膀-胸腔接缝破坑
			if (hid == 8 && !b.upperChestInClip && b.upperChestTransform)
			{
				if (fnSetWorldRotation)
				{
					fnSetWorldRotation(b.upperChestTransform, rTarget.x, rTarget.y, rTarget.z, rTarget.w);
				}
				else if (fnSetLocalRotation)
				{
					fnSetLocalRotation(b.upperChestTransform, 0.0f, 0.0f, 0.0f, 1.0f);
				}
				writtenWorldRots[54] = rTarget;
				writtenWorldRotValid[54] = true;
			}
				ApplyArmAuxiliaryForBone(b, hid, rRestRel, curActorRot, rTarget, writtenWorldRots, writtenWorldRotValid, currentTimeSeconds);
			}

		if (auxDriveMode == kAuxNormal)
		{
			ApplyAuxModelToBinding(b);
		}
		// This opt-in geometric support sees the completed main pose and arm
		// auxiliary postpass. It writes only exact Waist_H local transforms.
		ApplyWaistSupport(b);
		// Placement is one rigid world-Y rotation of the completed Hips subtree.
		// Child locals, auxiliary output and face pose remain mutually coherent.
		// Rotate around the original actor pivot, then retain the world-axis XYZ
		// translation. Never rotate the official actor/layout transform itself.
			ApplyPlacementYawToBinding(b, curActorPos, worldOffset, clipWritesRoot);

#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
			ProbeMotionPostApplyGeometry(b, clip, vmdRootPos, curActorRot, currentTimeSeconds, probeA3Pending);
#endif
		lastAppliedOverrideCount = appliedOverrides;
		probeFirstFrameDone = true; // PROBE A3/A6 are strictly one-shot
	}

	inline void IdolController::ApplyMotion(const MotionClip& clip, float currentTimeSeconds, void* actor,
	                                        float facialTimeSeconds)
	{
		if (actor)
		{
			auto it = bindings.find(actor);
			if (it != bindings.end())
			{
				ApplyMotionToBinding(it->second, clip, currentTimeSeconds, facialTimeSeconds);
				currentBinding = it->second;
				return;
			}
		}
		// 2026-10-07 self-heal: the target actor was set, but no binding exists
		// for it this frame. Either the scene reloaded (the pointer is stale and
		// every actor would be rejected) or the target never bound. Binding
		// nothing means the character freezes in its rest pose -- audio plays,
		// the status line reads bones=0/0, and nothing moves. So if the target
		// produced no binding, fall back to driving whatever actor we DO have a
		// binding for rather than silently doing nothing.
		if (currentBinding.actor && currentBinding.bindPoseCaptured)
		{
			ApplyMotionToBinding(currentBinding, clip, currentTimeSeconds, facialTimeSeconds);
		}
	}
} // namespace GakuMotion
