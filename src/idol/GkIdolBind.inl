namespace GakuMotion
{
	inline void IdolController::BindActor(void* actor, const MotionClip& clip, void* animator)
	{
		if (!CanDriveActor(actor) || !fnGetBoneTransform) return;

		// 2026-10-07: target gate lives HERE, at bind time -- not in
		// ApplyMotion. A non-target actor must never create a binding at all:
		// binding is what captures the rest pose and fills boneTransforms, and
		// OnActorChanged() clears the whole map, so a binding created for one
		// actor gets wiped by the next actor's rebind anyway. Gating here keeps
		// the map holding exactly one actor's data.
		if (!IsTargetActor(actor))
		{
			return;
		}

		// Preserve the actor's original snapshot across song changes. Bone-table
		// identity is checked by humanoid id, not merely by table size.
		auto it = bindings.find(actor);
		if (it != bindings.end() && it->second.bindPoseCaptured &&
			it->second.boundBoneCount > 0 &&
			it->second.rootTransform == fnGetBoneTransform(actor, 0) &&
			(!animator || it->second.cachedAnimator == animator) &&
			it->second.boneHids.size() == it->second.boneTransforms.size())
		{
			auto& existing = it->second;
			bool sameBones = existing.boneHids.size() == clip.bones.size();
			for (size_t i = 0; sameBones && i < clip.bones.size(); ++i)
				sameBones = existing.boneHids[i] == clip.bones[i].humanoid_id;
			if (existing.clipBindingDirty || !sameBones)
			{
				RestoreWaistSupport(existing);
				existing.waistSupportDiagnosticLogged = false;
				const auto old = existing;
				existing.boneTransforms.assign(clip.bones.size(), nullptr);
				existing.restRotations.assign(clip.bones.size(), Quaternion::Identity());
				existing.restLocals.assign(clip.bones.size(), Quaternion::Identity());
				existing.boneHids.resize(clip.bones.size());
				existing.boundBoneCount = 0;
				existing.upperChestInClip = false;
				for (size_t i = 0; i < clip.bones.size(); ++i)
				{
					const int hid = clip.bones[i].humanoid_id;
					existing.boneHids[i] = static_cast<uint16_t>(hid);
					auto previous = std::find(old.boneHids.begin(), old.boneHids.end(), hid);
					if (previous != old.boneHids.end())
					{
						size_t k = size_t(previous - old.boneHids.begin());
						existing.boneTransforms[i] = old.boneTransforms[k];
						existing.restRotations[i] = old.restRotations[k];
						existing.restLocals[i] = old.restLocals[k];
					}
					else
					{
						// Newly requested bones were never driven by the old clip.
						void* t = fnGetBoneTransform(actor, hid);
						existing.boneTransforms[i] = t;
						float q[4]{0, 0, 0, 1};
						if (t && fnGetLocalRotation)
						{
							fnGetLocalRotation(t, q);
							existing.restLocals[i] = Quaternion(q[0], q[1], q[2], q[3]);
						}
						if (t && fnGetWorldRotation)
						{
							fnGetWorldRotation(t, q);
							existing.restRotations[i] = existing.restActorRot.Inverse() * Quaternion(
								q[0], q[1], q[2], q[3]);
						}
					}
					if (existing.boneTransforms[i]) ++existing.boundBoneCount;
					if (hid == 54) existing.upperChestInClip = true;
				}
				UpdateClipGroundAnchor(existing, clip);
				existing.clipBindingDirty = false;
				existing.lastGeometryProbeTime = -99.0f;
				probeFirstFrameDone = false;
			}
			existing.face.enableBaseline = facialEnableBaseline;
			existing.face.baselineMultiplier = facialBaselineMultiplier;
			existing.face.characterOverride = facialCharacterOverride;
			existing.face.enableMikuMouthFit = facialMikuMouthFit;
			existing.face.enableMikuEyeFit = facialMikuEyeFit;
			existing.face.facialFitAllowUnverified = facialMikuFitAllowUnverified;
			existing.face.Prepare(actor, clip, faceApi);
			currentBinding = it->second;
			return;
		}

		// Actors appear before their humanoid skeleton is ready. Never detach
		// their controller or cache an empty "successful" binding: doing so
		// prevents initialization and leaves Preview/first Play at bones=0.
		if (!fnGetBoneTransform(actor, 0)) return;

		IdolBoneBinding& b = bindings[actor];
		RestoreWaistSupport(b);
		b.face.Restore(actor, faceApi);
		for (int slot = 0; slot < 2; ++slot) b.talks[slot].Restore(actor, slot, faceApi);
		b.Reset();
		b.actor = actor;
		b.face.enableBaseline = facialEnableBaseline;
		b.face.baselineMultiplier = facialBaselineMultiplier;
		b.face.facialWeights = facialWeights;
		b.face.characterOverride = facialCharacterOverride;
		b.face.enableMikuMouthFit = facialMikuMouthFit;
		b.face.enableMikuEyeFit = facialMikuEyeFit;
		b.face.facialFitAllowUnverified = facialMikuFitAllowUnverified;
		b.face.Prepare(actor, clip, faceApi);
		if (animator) b.cachedAnimator = animator;
		cachedAnimator = b.cachedAnimator;

		// ---- PROBE A1: is the actor already in a dance pose at bind time? ----
		// Opt-in only (GAKUMOTION_ENABLE_GEOMETRY_PROBE). The question this
		// answered -- "is BindActor capturing a dance pose instead of rest" --
		// was settled on 2026-10-05; keeping it on costs 9 lines per bind.
#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
		ProbeBindPreRebindPose(actor);
#endif

		// Snap actor to clean Prefab Bind Pose before measuring restRotations
		// Detach runtimeAnimatorController to eliminate Idle animation/controller pollution
		//
		// 2026-10-07 A/B SWITCH. Animator.Rebind() was added in 659dd79
		// ("Integrate Animator.Rebind for pristine prefab bind pose capture",
		// 2026-09-30) with the intent of capturing a clean rest pose. On a
		// Unity humanoid it does the opposite: it snaps every bone to the
		// prefab T-POSE, so whatever the engine reports afterwards is that
		// T-Pose, not the pose the rig was actually in.
		//
		// Measured on device, A1 (before Rebind) vs A2 (after):
		//     hid=13 LeftUpperArm  local devId 48.65 deg  ->  restLocal devId 0.00 deg
		//     hid=0  Hips          hips world y  ~0.88      ->  restRootPos.y = -0.0122
		// Every restLocal comes back exactly Identity, and the hips land at floor
		// level. That is the "character sunk into the floor" report, and it dates
		// from this Rebind, not from anything newer.
		//
		// Keep the controller detach (that part is doing real work against Idle
		// pollution) but make the Rebind itself switchable, so the causal claim
		// can be tested on device instead of argued.
		const bool preserveAnimator = PreserveAnimatorForSecondaryPhysics();
		void* anim = animator ? animator : b.cachedAnimator;
		if (!preserveAnimator && anim)
		{
			CaptureAnimatorState(b);
			DetachAnimatorController(b);
			if (useAnimatorRebind && fnAnimatorRebind)
			{
				fnAnimatorRebind(anim);
				b.animatorRebound = true;
			}
		}
		if (anim)
		{
			GM_DEBUG("[GakuMotion][Animator] actor=%p policy=%s controllerDetached=%d rebind=%d",
			          actor, preserveAnimator ? "preserve-graph(experimental)" : "legacy-mute",
			          int(b.animatorControllerDetached), int(b.animatorRebound));
		}

		// Bone 0 is Hips / Root
		b.rootTransform = fnGetBoneTransform(actor, 0);
		b.actorTransform = fnGetComponentTransform ? fnGetComponentTransform(actor) : nullptr;

		// Capture actor rest world transform
		if (b.actorTransform)
		{
			float aPos[3]{0.0f, 0.0f, 0.0f};
			float aRot[4]{0.0f, 0.0f, 0.0f, 1.0f};
			if (fnGetWorldPosition) fnGetWorldPosition(b.actorTransform, aPos);
			if (fnGetWorldRotation) fnGetWorldRotation(b.actorTransform, aRot);
			b.restActorPos = Vector3(aPos[0], aPos[1], aPos[2]);
			b.restActorRot = Quaternion(aRot[0], aRot[1], aRot[2], aRot[3]);
		}

		// Capture Hips rest position relative to actor
		if (b.rootTransform)
		{
			if (fnGetLocalRotation)
			{
				float q[4]{0, 0, 0, 1};
				fnGetLocalRotation(b.rootTransform, q);
				b.placementRestRootLocal = Quaternion(q[0], q[1], q[2], q[3]);
				b.placementRestRootCaptured = true;
			}
			float hPos[3]{0.0f, 0.0f, 0.0f};
			if (fnGetWorldPosition)
			{
				fnGetWorldPosition(b.rootTransform, hPos);
			}
			else if (fnGetLocalPosition)
			{
				fnGetLocalPosition(b.rootTransform, hPos);
			}
			Vector3 hipsWorld(hPos[0], hPos[1], hPos[2]);
			if (b.actorTransform)
			{
				b.restRootPos = b.restActorRot.Inverse().Rotate(hipsWorld - b.restActorPos);
			}
			else
			{
				b.restRootPos = hipsWorld;
			}
		}

		// Capture Hips rest position relative to actor
		//
		// 2026-10-07 PROBE A10. The character is reported as "sunk into the
		// floor" and restRootPos.y measures -0.0122, i.e. the hips sit at floor
		// level. Two very different causes look identical from the outside:
		//
		//   (a) the hips really are that low because the rig's own Hips bone sits
		//       at the actor origin -- then the number is CORRECT and the fix is
		//       elsewhere (the pose, or the camera reading it from above);
		//   (b) the rest position was captured from the wrong space or after the
		//       rig had already been posed -- then the number is WRONG.
		//
		// The two are distinguishable only by measuring. This prints, all on one
		// line at capture time: the hips in WORLD space, the actor root in WORLD
		// space, the difference, and -- the decisive one -- the length of the
		// head-to-foot vector, which for a standing character must be roughly
		// the character's height. A rig whose hips sit at the actor origin gives
		// a head-to-foot vector equal to the standing height, which is (a).
		// ---- The ground offset, measured per character, not hardcoded ----
		// A Unity humanoid rests with the hips at the actor origin and the feet
		// one stature BELOW that. This scene's floor is at y = 0, so leaving the
		// hips at the origin leaves the feet underground -- which is exactly the
		// "sunk into the floor" report, and exactly what the screenshots at
		// 1.0s / 2.0s / 5.0s show (standing correctly, torso above the boards,
		// legs below them).
		//
		// The fix is the character's OWN stature, which the rig hands us at bind
		// time -- not a tuned constant. Placing the hips at origin + stature puts
		// the feet back where they were measured. Statures differ per character,
		// so this must be captured per binding; a hardcoded number would be
		// correct for exactly one character.
		//
		// OUTSIDE the fnLogInfo guard on purpose: this is production behaviour,
		// not a diagnostic, and must not depend on logging being enabled.
		{
			float hp[3]{0, 0, 0};
			float fp[3]{0, 0, 0};
			bool haveFoot = false;
			if (b.rootTransform && fnGetWorldPosition) fnGetWorldPosition(b.rootTransform, hp);
			if (fnGetBoneTransform)
			{
				void* lf = fnGetBoneTransform(actor, 5); // LeftFoot
				if (lf && fnGetWorldPosition)
				{
					fnGetWorldPosition(lf, fp);
					haveFoot = true;
				}
			}
			if (haveFoot)
			{
				const float stature = hp[1] - fp[1];
				// Guard against a rig that reports a degenerate drop (both points
				// at the origin, or the foot transform missing): a zero lift is
				// the safe fallback because it reproduces the old behaviour.
				b.groundLift = (stature > 0.05f && stature < 5.0f) ? stature : 0.0f;
			}

#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
			ProbeBindGroundAndSegments(actor, b, hp, fp, haveFoot);
#endif
		}

		// Capture Bind Pose for all mapped bones.
		//   restRotations : actor-relative WORLD  -> gk_world_dump / legacy path
		//   restLocals    : LOCAL                  -> gkclip path (option A)
		// World/local rests are retained for restoration and auxiliary bones.
		// They are not composed into v2+ gkclip main-bone rotations.
		b.boneTransforms.resize(clip.bones.size(), nullptr);
		b.restRotations.resize(clip.bones.size(), Quaternion::Identity());
		b.restLocals.resize(clip.bones.size(), Quaternion::Identity());
		b.boneHids.resize(clip.bones.size());

		size_t boundCount = 0;
		for (size_t i = 0; i < clip.bones.size(); ++i)
		{
			int hid = clip.bones[i].humanoid_id;
			b.boneHids[i] = static_cast<uint16_t>(hid);
			void* trans = fnGetBoneTransform(actor, hid);
			b.boneTransforms[i] = trans;

			if (trans)
			{
				boundCount++;
				float rot[4]{0.0f, 0.0f, 0.0f, 1.0f};
				if (fnGetWorldRotation)
				{
					fnGetWorldRotation(trans, rot);
					Quaternion qWorld(rot[0], rot[1], rot[2], rot[3]);
					b.restRotations[i] = b.restActorRot.Inverse() * qWorld;
				}
				else if (fnGetLocalRotation)
				{
					fnGetLocalRotation(trans, rot);
					b.restRotations[i] = Quaternion(rot[0], rot[1], rot[2], rot[3]);
				}
				// Local rest is captured independently of the world path above:
				// fnGetWorldRotation is usually present, so relying on the
				// fallback branch for this would leave every restLocal at
				// Identity and silently flatten the character's rest pose.
				if (fnGetLocalRotation)
				{
					float lrot[4]{0.0f, 0.0f, 0.0f, 1.0f};
					fnGetLocalRotation(trans, lrot);
					b.restLocals[i] = Quaternion(lrot[0], lrot[1], lrot[2], lrot[3]);
				}
			}
		}

		b.boundBoneCount = boundCount;

		b.bindGroundLift = b.groundLift;
		UpdateClipGroundAnchor(b, clip);

		// ---- PROBE A2: dump the captured restLocal, and compare against the
		// pre-rebind reading above. A non-zero delta means the capture basis is
		// not the rest pose. This is the value every gkclip composition uses, so
		// it must be seen, not assumed.
		//
		// A3/A6 are rearmed here, in the same function that captures restLocal,
		// so the A2 block and the A3 block always describe ONE bind. Leaving the
		// flag set across rebinds made A3 a once-per-process event, which is how
		// a previous analysis ended up comparing one clip's rest against another
		// clip's delta.
		probeFirstFrameDone = false;
#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
		ProbeBindPostRebindRestLocals(b, clip);
#endif

		BindArmAuxiliaryChains(actor, b, clip);

		b.bindPoseCaptured = true;
		currentBinding = b;

		if (fnLogInfo)
		{
			fnLogInfo("[GakuMotion] Actor %p bound: %zu / %zu bones (useRestRotations=%s)",
			          actor, boundCount, clip.bones.size(), useRestRotations ? "true" : "false");
		}
	}
} // namespace GakuMotion
