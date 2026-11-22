namespace GakuMotion
{
	inline bool IdolController::ComputeMainBoneTarget(IdolBoneBinding& b, const MotionClip& clip, size_t i, int hid, void* trans,
	                                                  const Quaternion& rVmd, const Quaternion& rRestRel, const Quaternion& curActorRot,
	                                                  bool overridesActive, bool hasDynamicArm,
	                                                  const Quaternion& qArmLeft, const Quaternion& qArmRight,
	                                                  const std::array<Quaternion, 64>& writtenWorldRots,
	                                                  const std::array<bool, 64>& writtenWorldRotValid,
	                                                  Quaternion& outTarget, Quaternion& outGkLocal,
	                                                  bool& outIsGkClip, int& appliedOverrides)
	{
		const bool isLegBone = (hid >= 1 && hid <= 6) || (hid == 19 || hid == 20);
		const bool isHandOrFingerBone = (hid == 17 || hid == 18) || (hid >= 24 && hid <= 53);
		const bool useRestBase = isLegBone || isHandOrFingerBone;

		// 姿态分流驱动架构：
		// 1. GKMO Version >= 2 (实机录制纯净流)：
		//    全身 52+ 根骨骼直接为 Actor 相对姿态，直接采用 curActorRot * rVmd 驱动，消除 R_rest_rel 误差与腿部飘移。
		// 2. GKMO Version 1 (传统 MMD 动作流)：
		//    下肢骨骼与手腕/手指骨骼必须以模型原生静息姿态 rRestRel 为基准累乘 (curActorRot * (rVmd * rRestRel))，防止一字马与折手。
		const bool isActorRelativeStream = (clip.version >= 2);
		Quaternion rTarget;
		if (!isActorRelativeStream && useRestRotations && useRestBase)
		{
			rTarget = curActorRot * (rVmd * rRestRel);
		}
		else
		{
			rTarget = curActorRot * rVmd;
		}

		// ---- gkclip: canonical LOCAL deltas ----------------------------
		// A gkclip stores, per bone, a full WORLD->LOCAL decomposition of
		// the source animation:
		//
		//     L[i] = inv(W[parent(i)]) * W[i]
		//
		// (see tools/vmd2gkmotion/gkclip_format.py, world_to_canonical_deltas).
		// To play that back so the target rig reproduces the SOURCE WORLD
		// orientations, the target's locals must equal the source's locals,
		// bone for bone. Proof by induction on the chain:
		//
		//   root:  W_t[0] = L_t[0], want = W_s[0] = L_s[0]  => L_t = L_s
		//   child: W_t[i] = W_t[p] * L_t[i], and assuming W_t[p] == W_s[p],
		//          want W_s[p] * L_s[i]  =>  L_t[i] = L_s[i]
		//
		// The target's REST POSE NEVER APPEARS. Rest affects where the bones
		// end up (bone lengths, offsets) but not the rotation composition --
		// so pre-multiplying restLocal re-bases every bone onto a second,
		// incompatible rest and destroys the pose.
		//
		// ★ SCOPE CORRECTION 2026-10-07. The proof is sound and still holds
		// for the TORSO, but its premise is that the rig's rest equals the
		// canonical all-Identity rest the gkclip format is defined against.
		// For a Unity humanoid that premise holds only approximately: torso
		// locals are small (Spine 2.47 deg, Neck 9.92 deg, shoulders ~5 deg)
		// so the bare delta lands correctly there, while the legs are NOT
		// close to identity (thighs 106.59 / 119.05 deg) and the bare delta
		// collapses them to horizontal. Hence legRestBasis below, scoped to
		// the leg chain: the same rule, applied only where the premise
		// actually fails.
		//
		// This is what produced the 2026-10-05 "twisted, floating" collapse:
		//   * PROBE A2 measured 10 of 22 bones with devFromIdentity > 45 deg
		//     (RightShoulder EXACTLY 180.00 deg, thighs 107/119 deg), so
		//     restLocal is a large, non-identity, per-bone rotation.
		//   * Composing it re-based each bone independently, and the
		//     RightShoulder's exact 180 deg pre-multiply cancelled the Y
		//     component of the clip's own shoulder rotation, pinning the
		//     shoulder near 180 deg at every frame (measured 179.6 / 166.6 /
		//     178.4 deg at frames 0 / 150 / 300).
		//   * Cross-check against the converter's own physical diagnostics:
		//     accumulated deltas match ground_truth_real.json's world
		//     rotations to 0.5-1.7 deg, whereas the raw deltas differ by
		//     12-34 deg. thigh-axis spread comes out 1.5-103 deg against an
		//     official leg_spread_deg of 1.06-70.5; composing restLocal
		//     instead gives 137-180 deg, i.e. the thighs pointing in
		//     OPPOSITE directions.
		//
		// PROBE A3 confirms the rest of the path is sound and is NOT the
		// cause: A4 15/15 IL2CPP bindings resolved, A5 confirms the
		// setLocalRotation branch, and write-vs-readback is 0.000 deg for
		// Hips / RightUpperArm / RightHand. The writes land; the composition
		// was wrong.
		Quaternion gkLocalOut = rTarget;
		const bool isGkClip = clip.IsLocalDeltaStream();
		if (isGkClip)
		{
			// The delta IS the local rotation. See the proof above.
			gkLocalOut = rVmd;
			if (clip.bones[i].head_space_eye_delta)
			{
				if (!b.eyeFrameResolved)
				{
					b.eyeTargetFrame = faceApi.getActorName ? FindEyeTargetFrame(faceApi.getActorName(b.actor)) : nullptr;
					b.eyeFrameResolved = true;
				}
				bool supported = false;
				if (b.eyeTargetFrame && fnGetTransformParent && fnGetBoneTransform && fnGetWorldRotation)
				{
					void* parent = fnGetTransformParent(trans);
					void* head = fnGetBoneTransform(b.actor, 10);
					if (parent && head)
					{
						float hp[4]{0,0,0,1}, pp[4]{0,0,0,1};
						fnGetWorldRotation(head, hp);
						fnGetWorldRotation(parent, pp);
						const Quaternion h(hp[0],hp[1],hp[2],hp[3]), p(pp[0],pp[1],pp[2],pp[3]);
						// Scale the head-space delta by this character's measured
						// response ratio before the parent-frame conjugation. A
						// gain of exactly 1 (or the switch off) is the raw source
						// rotation, so the control path stays bit-identical.
						const float eyeGain = eyePoseRelativeGain ? b.eyeTargetFrame->gain : 1.0f;
						supported = MapHeadSpaceEyeDelta(*b.eyeTargetFrame, hid, rVmd, h.Inverse()*p, eyeGain, gkLocalOut);
						if (supported && !b.eyeGainLogged)
						{
							GM_DEBUG("[GakuMotion][Eye] %s relative gain %.4f (%s)",
							          b.eyeTargetFrame->character, eyeGain,
							          eyePoseRelativeGain ? "geometry-relative" : "raw 1:1");
							b.eyeGainLogged = true;
						}
					}
				}
				if (!supported)
				{
					if (!b.eyeFrameWarningLogged && fnLogWarn)
						fnLogWarn("[GakuMotion][Eye] Eye delta skipped: unverified character or mounted parent frame/API; body unchanged");
					b.eyeFrameWarningLogged = true;
					return false;
				}
			}

			// ---- 2026-10-07 A/B SWITCH: leg rest basis -------------------
			// The proof above says the rest pose must not appear in the
			// gkclip composition, and the torso agrees: torsoVerticality
			// measures 0.86-1.00 against an official 0.852-1.000, i.e. the
			// spine and arms are exactly right.
			//
			// The legs are not. Same probe, same frame:
			//     segmentverticality   at bind     during playback
			//       thigh                -0.984          -0.157
			//       shin                 -0.935          -0.048
			//       thigh+shin drop      -0.776          -0.087
			// The bind-time numbers are a perfectly upright standing figure,
			// so the geometry is fine and the playback pose is what collapses
			// the legs to horizontal.
			//
			// A gkclip delta is defined against a canonical rest in which
			// every local is Identity. Unity's humanoid legs are NOT in that
			// state -- PROBE A2 measures LeftUpperLeg restLocal at 106.59 deg
			// from identity and RightUpperLeg at 119.05 deg -- so writing the
			// bare delta lands the leg on a different rest than the clip
			// assumed and the chain folds.
			//
			// The torso tolerates this because its rest locals are small
			// (Spine 2.47, Neck 9.92, shoulders ~5), which is why the arms
			// looked fine while the legs did not. That asymmetry is the
			// signature of a rest-basis error and nothing else.
			//
			// Settled on device, not by argument: the A/B table is on the
			// flag's declaration. thighVert -0.157 -> -0.856 and drop -0.085
			// -> -0.562, against a bind-time baseline of -0.984 / -0.776.
			//
			// ★ SCOPE 2026-10-07, gkclip version 2. v2 files carry
			// frame-ALIGNED deltas (the converter re-based every leg bone
			// by the constant K that maps the target's child-offset axis
			// onto the source's, so the joints land on the source's world
			// child directions). The measured proof is in
			// tools/vmd2gkmotion/verify/ab_knee_retarget.py: with aligned
			// deltas the bare H1 formula reproduces the official knee
			// flexion frame-by-frame (mean 3.4 deg, cross-pipeline noise),
			// whereas on the SAME clip the rest composition freezes the
			// knee at a constant 83 deg no matter what the choreography
			// says. Composing any rest into a v2 file RE-INTRODUCES that
			// frozen knee, so v2 skips this block entirely -- which also
			// makes leg playback independent of the bind-time rest
			// capture (a capture that is provably state-dependent:
			// same character, Foot restLocal 93.67 deg vs 101.39 deg in
			// two binds of one session).
			if (legRestBasis && clip.version < 2 && i < b.restLocals.size())
			{
				const Quaternion& rl = b.restLocals[i];
				const float w = rl.w < 0.0f ? -rl.w : rl.w;
				const float devDeg = 2.0f * std::acos(w > 1.0f ? 1.0f : w) * 57.29578f;
				const bool legish = (hid >= 1 && hid <= 6) || hid == 19 || hid == 20;
				// Only where the rest actually differs from identity: applying
				// it everywhere would undo the fix that cured the arm folding.
				if (legish && devDeg > 5.0f)
				{
					gkLocalOut = rl * rVmd;
				}
			}

			// rTarget is consumed downstream as a WORLD rotation (the aux
			// solver and writtenWorldRots[]), so it must be converted along
			// the parent chain -- NOT by multiplying a local quantity with
			// the actor rotation. For the chain root the local IS the world;
			// for every other bone the parent's world rotation has to come
			// from what this same loop already wrote for it.
			//
			// Bones are emitted in topological (parents-first) order, so a
			// single forward pass suffices. When the parent is absent from
			// the clip, the chain root's own local IS its world rotation --
			// exactly what world_to_canonical_deltas() emits for entries
			// missing from gk_parents.
			//
			// ★ 2026-10-07 ROOT FIX. This used to seed the walk with
			// `curActorRot`:
			//
			//     Quaternion parentWorld = curActorRot;   // WRONG
			//
			// curActorRot is the ACTOR ROOT's world orientation, not a bone's.
			// Seeding the chain with it injected an extra rigid Ry(90)
			// (measured exactly: curActorRot = (0, 0.7071, 0, 0.7071) =Ry90)
			// into every world rotation this function computes.
			//
			// Proof it is wrong, from the device (PROBE A7 + A8):
			//   ang(my pure-FK prediction, official truth)  =  0.89 - 17.67 deg
			//   ang(engine world rotation, official truth)  = 88.48 - 94.96 deg
			//   ang(Ry(-90) * engine world, official truth) =  0.00 - 17.71 deg
			// The +90 was mine, not the engine's, and removing it is what
			// makes the engine land back on the official pose.
			//
			// It also explains all three reported symptoms at once, which is
			// why this went unnoticed for so long:
			//   * thighs 90 deg off  -> horizontal legs      -> 一字马
			//   * pelvis 90 deg off  -> height collapses     -> "sits down"
			//   * whole body 90 deg off -> faces sideways    -> "面朝侧方"
			// The "sitting pose" screenshot was this bug seen from a distance.
			//
			// The root's world rotation is its local, full stop. The actor's
			// own orientation is applied by Unity when it evaluates the
			// transform, and double-applying it here is what broke the pose.
			//
			// CRITICAL: the lookup below is into writtenWorldRots[], which is
			// indexed BY HUMANOID ID, so the parent must come from
			// clip.bone_parent_hids -- NOT clip.bone_parents, which holds
			// `bones` indices. In a real gkclip 51 of 52 bones have
			// index != hid, so using bone_parents here gave 48 bones the
			// wrong parent world (Chest inherited LeftUpperLeg, LeftUpperArm
			// inherited RightFoot, ...) and collapsed the whole pose. An
			// earlier revision did exactly that; see
			// BuildBoneParentHids() for the full account.
			Quaternion parentWorld = Quaternion::Identity();
			const int parentHid = (i > 0 && i < clip.bone_parent_hids.size())
				                      ? clip.bone_parent_hids[i]
				                      : -1;
			if (parentHid >= 0 && parentHid < 64 && writtenWorldRotValid[parentHid])
			{
				parentWorld = writtenWorldRots[parentHid];
			}
			rTarget = parentWorld * gkLocalOut;
		}

		// 4a. 覆盖表注入：local = 局部后乘；world = 世界左乘
		//
		// In the gkclip path the quantity actually written is gkLocalOut
		// (see the write at the bottom of this loop), so the correction has
		// to be applied to BOTH: gkLocalOut for the local write, rTarget for
		// the aux solver and writtenWorldRots[]. Correcting only rTarget --
		// which is what an earlier revision did -- silently discarded every
		// override and every dynamic-arm offset for .gkclip playback.
		if (overridesActive)
		{
			if (const BoneRotationOffset* off = boneOverrides->Find(hid, clip.bones[i].name))
			{
				const Quaternion qOff = off->Resolve();
				rTarget = off->spaceWorld ? (qOff * rTarget) : (rTarget * qOff);
				if (isGkClip)
				{
					// A world-space offset cannot be pushed into a local
					// write; convert it to the bone's parent frame first.
					// Quaternions commute with the parent extraction only if
					// the offset is conjugated by the parent, so do it
					// explicitly rather than silently dropping the override.
					// writtenWorldRots[] is hid-indexed -> bone_parent_hids,
					// for the same reason as the main path above.
					//
					// 2026-10-07: this seeded the parent with curActorRot, the
					// same defect fixed in the main path above -- curActorRot
					// is the actor ROOT's orientation, not a bone's, and
					// seeding a bone chain with it injects a rigid Ry(90).
					// A chain root's world rotation is its local, full stop.
					const int pHid = (i > 0 && i < clip.bone_parent_hids.size())
						                 ? clip.bone_parent_hids[i]
						                 : -1;
					Quaternion parentWorld = Quaternion::Identity();
					if (pHid >= 0 && pHid < 64 && writtenWorldRotValid[pHid])
					{
						parentWorld = writtenWorldRots[pHid];
					}
					const Quaternion qOffLocal = parentWorld.Inverse() * qOff * parentWorld;
					gkLocalOut = off->spaceWorld
						             ? (qOffLocal * gkLocalOut)
						             : (gkLocalOut * qOffLocal);
				}
				++appliedOverrides;
			}
		}

		// 4b. Optional Dynamic arm offset微调 (LeftUpperArm = 13, RightUpperArm = 14)
		// Right-multiplied: this is a joint-space tweak, and in the gkclip
		// path it must also reach gkLocalOut for the same reason as 4a.
		if (hasDynamicArm)
		{
			if (hid == 13)
			{
				rTarget = rTarget * qArmLeft;
				if (isGkClip) gkLocalOut = gkLocalOut * qArmLeft;
			}
			else if (hid == 14)
			{
				rTarget = rTarget * qArmRight;
				if (isGkClip) gkLocalOut = gkLocalOut * qArmRight;
			}
		}

		outTarget = rTarget;
		outGkLocal = gkLocalOut;
		outIsGkClip = isGkClip;
		return true;
	}
} // namespace GakuMotion
