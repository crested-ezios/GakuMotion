namespace GakuMotion
{
	inline void IdolController::CaptureAnimatorState(IdolBoneBinding& b)
	{
		if (!b.cachedAnimator || b.animatorStateCaptured) return;
		b.savedAnimatorController = fnGetAnimatorController ? fnGetAnimatorController(b.cachedAnimator) : nullptr;
		b.savedAnimatorEnabled = fnGetAnimatorEnabled ? fnGetAnimatorEnabled(b.cachedAnimator) : true;
		b.animatorStateCaptured = true;
	}

	inline void IdolController::DetachAnimatorController(IdolBoneBinding& b)
	{
		if (!b.cachedAnimator || !fnGetAnimatorController || !fnSetAnimatorController) return;
		if (void* controller = fnGetAnimatorController(b.cachedAnimator))
		{
			// An official timeline may replace the controller while we own it.
			// Preserve that replacement for Stop instead of resurrecting an old one.
			b.savedAnimatorController = controller;
			fnSetAnimatorController(b.cachedAnimator, nullptr);
			b.animatorControllerDetached = true;
		}
	}

	inline void IdolController::RestoreAnimatorState(IdolBoneBinding& b, bool rebind)
	{
		void* anim = b.cachedAnimator;
		if (!anim || !b.animatorStateCaptured) return;
		if (b.animatorControllerDetached && fnSetAnimatorController)
		{
			fnSetAnimatorController(anim, b.savedAnimatorController);
		}
		// Rebinding an untouched, live graph resets its native dynamic state.
		// Also avoid evaluating it twice through currentBinding + cachedAnimator.
		if (rebind && (b.animatorControllerDetached || b.animatorWasDisabled || b.animatorRebound) &&
			fnAnimatorRebind)
		{
			fnAnimatorRebind(anim);
		}
		if (b.animatorWasDisabled && fnSetAnimatorEnabled)
		{
			fnSetAnimatorEnabled(anim, b.savedAnimatorEnabled);
		}
		b.animatorStateCaptured = false;
		b.savedAnimatorController = nullptr;
		b.animatorControllerDetached = false;
		b.animatorWasDisabled = false;
		b.animatorRebound = false;
	}

	inline void IdolController::DefensiveMute(void* animatorComponent, void* playableDirector)
	{
		DefensiveMute(currentBinding.actor, animatorComponent, playableDirector);
	}

	inline void IdolController::DefensiveMute(void* actor, void* animatorComponent, void* playableDirector)
	{
		void* act = actor ? actor : currentBinding.actor;
		if (!act) return;

		auto it = bindings.find(act);
		// Never mutate the current actor when a non-target callback arrives.
		if (!IsTargetActor(act) || it == bindings.end() || !it->second.bindPoseCaptured ||
			it->second.boundBoneCount == 0)
			return;
		IdolBoneBinding* bPtr = &it->second;

		if (animatorComponent)
		{
			bPtr->cachedAnimator = animatorComponent;
			cachedAnimator = animatorComponent;
		}
		if (playableDirector)
		{
			bPtr->cachedPlayableDirector = playableDirector;
		}

		// The official timeline can replace a controller or enable its Animator.
		// Legacy continually suppresses it; the experiment keeps its graph live.
		if (preserveAnimatorForSecondaryPhysics)
		{
			// Undo only our own legacy takeover on a hot A/B switch. Do not
			// freeze Animator.speed: its deltaTime also feeds the swing jobs.
			if (bPtr->animatorControllerDetached || bPtr->animatorWasDisabled || bPtr->animatorRebound)
			{
				RestoreAnimatorState(*bPtr, false);
			}
		}
		else if (bPtr->cachedAnimator)
		{
			CaptureAnimatorState(*bPtr);
			DetachAnimatorController(*bPtr);
			if (fnSetAnimatorEnabled)
			{
				fnSetAnimatorEnabled(bPtr->cachedAnimator, false);
				bPtr->animatorWasDisabled = true;
			}
		}

		if (!preserveAnimatorForSecondaryPhysics && bPtr->cachedPlayableDirector && fnPausePlayableDirector)
		{
			fnPausePlayableDirector(bPtr->cachedPlayableDirector);
		}

		bPtr->defensivelyMuted = true;
	}

	inline void IdolController::ResetToRestPose(IdolBoneBinding& b)
	{
		if (!b.actor || !b.bindPoseCaptured) return;
		if (b.placementYawApplied && b.placementRestRootCaptured && b.rootTransform && fnSetLocalRotation)
		{
			const auto& q = b.placementRestRootLocal;
			fnSetLocalRotation(b.rootTransform, q.x, q.y, q.z, q.w);
			b.placementYawApplied = false;
		}

		// 1. 物理回写根骨骼站姿标高，防止停顿残留导致下蹲过深
		if (b.rootTransform)
		{
			Vector3 worldHips = b.restActorPos + b.restActorRot.Rotate(b.restRootPos);
			if (fnSetWorldPosition)
			{
				fnSetWorldPosition(b.rootTransform, worldHips.x, worldHips.y, worldHips.z);
			}
			else if (fnSetLocalPosition)
			{
				fnSetLocalPosition(b.rootTransform, b.restRootPos.x, b.restRootPos.y, b.restRootPos.z);
			}
		}

		// 2. 物理回写所有已绑定骨骼的纯净静息旋转
		for (size_t i = 0; i < b.boneTransforms.size() && i < b.restRotations.size(); ++i)
		{
			void* trans = b.boneTransforms[i];
			if (!trans) continue;
			Quaternion qWorld = b.restActorRot * b.restRotations[i];
			if (fnSetWorldRotation)
			{
				fnSetWorldRotation(trans, qWorld.x, qWorld.y, qWorld.z, qWorld.w);
			}
			else if (fnSetLocalRotation)
			{
				fnSetLocalRotation(trans, b.restRotations[i].x, b.restRotations[i].y, b.restRotations[i].z,
				                   b.restRotations[i].w);
			}
		}

		// 3. 物理回写全部手臂辅助骨（_Roll_H 扭转骨 与 _H 关节半角骨）的初始静息局部/世界姿态
		auto restoreAuxRest = [this, &b](void* trans, const Quaternion& qRestLocal, const Quaternion& qRestRel)
		{
			if (!trans) return;
			if (fnSetWorldRotation)
			{
				Quaternion qWorld = b.restActorRot * qRestRel;
				fnSetWorldRotation(trans, qWorld.x, qWorld.y, qWorld.z, qWorld.w);
			}
			else if (fnSetLocalRotation)
			{
				fnSetLocalRotation(trans, qRestLocal.x, qRestLocal.y, qRestLocal.z, qRestLocal.w);
			}
		};
		restoreAuxRest(b.leftArmAux.upperArmRollH, b.leftArmAux.upperArmRollHRestLocal,
		               b.leftArmAux.upperArmRollHRestRel);
		restoreAuxRest(b.leftArmAux.upperArmH, b.leftArmAux.upperArmHRestLocal, b.leftArmAux.upperArmHRestRel);
		restoreAuxRest(b.leftArmAux.foreArmRollH, b.leftArmAux.foreArmRollHRestLocal,
		               b.leftArmAux.foreArmRollHRestRel);
		restoreAuxRest(b.leftArmAux.foreArmH, b.leftArmAux.foreArmHRestLocal, b.leftArmAux.foreArmHRestRel);
		restoreAuxRest(b.leftArmAux.handH, b.leftArmAux.handHRestLocal, b.leftArmAux.handHRestRel);

		restoreAuxRest(b.rightArmAux.upperArmRollH, b.rightArmAux.upperArmRollHRestLocal,
		               b.rightArmAux.upperArmRollHRestRel);
		restoreAuxRest(b.rightArmAux.upperArmH, b.rightArmAux.upperArmHRestLocal, b.rightArmAux.upperArmHRestRel);
		restoreAuxRest(b.rightArmAux.foreArmRollH, b.rightArmAux.foreArmRollHRestLocal,
		               b.rightArmAux.foreArmRollHRestRel);
		restoreAuxRest(b.rightArmAux.foreArmH, b.rightArmAux.foreArmHRestLocal, b.rightArmAux.foreArmHRestRel);
		restoreAuxRest(b.rightArmAux.handH, b.rightArmAux.handHRestLocal, b.rightArmAux.handHRestRel);
		if (!b.upperChestInClip)
			restoreAuxRest(b.upperChestTransform, Quaternion::Identity(), b.upperChestRestRel);
	}

	inline BodyFaceRestoreResult IdolController::RestoreForCapture()
	{
		BodyFaceRestoreResult result;
		if (captureRestoreUnconfirmed)
		{
			result.bodyModified = true;
			result.bodyConfirmed = false;
		}
		auto closePosition = [](const float* actual, const Vector3& expected)
		{
			return std::isfinite(actual[0]) && std::isfinite(actual[1]) && std::isfinite(actual[2]) &&
				std::fabs(actual[0] - expected.x) <= 1e-4f && std::fabs(actual[1] - expected.y) <= 1e-4f &&
				std::fabs(actual[2] - expected.z) <= 1e-4f;
		};
		auto checkRotation = [this](void* transform, const Quaternion& expected)
		{
			if (!transform || !fnGetWorldRotation) return false;
			float q[4]{NAN, NAN, NAN, NAN}; fnGetWorldRotation(transform, q);
			const Quaternion actual{q[0], q[1], q[2], q[3]};
			const float norm = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
			return std::isfinite(norm) && std::fabs(norm - 1.0f) <= 1e-3f &&
				std::fabs(Quaternion::Dot(actual, expected)) >= 1.0f - 1e-4f;
		};
		auto restoreOne = [&](IdolBoneBinding& b)
		{
			const bool bodyOwned = b.animatorStateCaptured || b.defensivelyMuted || b.eyePoseModified;
			result.bodyModified |= bodyOwned;
			result.gazeModified |= b.eyePoseModified;
			bool bodyOk = true;
			if (bodyOwned)
			{
				const auto before = b;
				if (!b.actor || !b.bindPoseCaptured || !fnSetWorldRotation || !fnGetWorldRotation ||
					!fnSetWorldPosition || !fnGetWorldPosition) bodyOk = false;
				else
				{
					ResetToRestPose(b);
					if (b.rootTransform)
					{
						float p[3]{NAN,NAN,NAN}; fnGetWorldPosition(b.rootTransform, p);
						if (!closePosition(p, b.restActorPos + b.restActorRot.Rotate(b.restRootPos))) bodyOk = false;
					}
					if (b.boneTransforms.size() != b.restRotations.size()) bodyOk = false;
					for (size_t i = 0; i < b.boneTransforms.size() && i < b.restRotations.size(); ++i)
						if (b.boneTransforms[i] && !checkRotation(b.boneTransforms[i], b.restActorRot * b.restRotations[i])) bodyOk = false;
					auto aux = [&](void* t, const Quaternion& q) { if (t && !checkRotation(t, b.restActorRot * q)) bodyOk = false; };
					for (const auto* chain : {&b.leftArmAux, &b.rightArmAux})
					{
						aux(chain->upperArmRollH, chain->upperArmRollHRestRel); aux(chain->upperArmH, chain->upperArmHRestRel);
						aux(chain->foreArmRollH, chain->foreArmRollHRestRel); aux(chain->foreArmH, chain->foreArmHRestRel);
						aux(chain->handH, chain->handHRestRel);
					}
					if (!b.upperChestInClip) aux(b.upperChestTransform, b.upperChestRestRel);
				}
				RestoreAnimatorState(b, true);
				if (before.animatorStateCaptured)
				{
					if (!before.cachedAnimator || !fnGetAnimatorController || !fnGetAnimatorEnabled ||
						fnGetAnimatorController(before.cachedAnimator) != before.savedAnimatorController ||
						fnGetAnimatorEnabled(before.cachedAnimator) != before.savedAnimatorEnabled) bodyOk = false;
				}
				// No saved PlayableDirector play-state or resume API exists here.
				// A graph actually paused by GM cannot be certified as restored.
				if (!preserveAnimatorForSecondaryPhysics && before.cachedPlayableDirector && fnPausePlayableDirector) bodyOk = false;
				for (int side=0; side<2; ++side)
				{
					const auto saved = before.waistSupport[side];
					if (!saved.captured) continue;
					WaistSupportChain live;
					if (!ResolveWaistSupportTarget(b, side, live) || !saved.SameTargetIdentity(live) ||
						!fnGetLocalPosition || !fnGetLocalRotation) { bodyOk = false; continue; }
					RestoreWaistSupport(b);
					float p[3]{NAN,NAN,NAN}, q[4]{NAN,NAN,NAN,NAN};
					fnGetLocalPosition(live.helper, p); fnGetLocalRotation(live.helper, q);
					if (!closePosition(p,saved.savedLocalPosition) ||
						!std::isfinite(q[0]+q[1]+q[2]+q[3]) ||
						std::fabs(Quaternion::Dot({q[0],q[1],q[2],q[3]},saved.savedLocalRotation)) < 1.0f-1e-4f) bodyOk=false;
				}
				b.defensivelyMuted = !bodyOk && before.defensivelyMuted;
				if (!bodyOk)
				{
					b.animatorStateCaptured = before.animatorStateCaptured;
					b.savedAnimatorController = before.savedAnimatorController;
					b.savedAnimatorEnabled = before.savedAnimatorEnabled;
					b.animatorControllerDetached = before.animatorControllerDetached;
					b.animatorWasDisabled = before.animatorWasDisabled;
					b.animatorRebound = before.animatorRebound;
					b.waistSupport = before.waistSupport;
				}
				result.bodyConfirmed &= bodyOk;
				if (b.eyePoseModified) result.gazeConfirmed &= bodyOk;
				if (bodyOk) b.eyePoseModified = false;
			}
			const auto face = b.face.RestoreForCapture(b.actor, faceApi);
			result.faceModified |= face.modified; result.faceConfirmed &= face.confirmed;
			for (int slot=0; slot<2; ++slot)
			{
				const auto talk = b.talks[slot].RestoreForCapture(b.actor, slot, faceApi);
				result.faceModified |= talk.modified; result.faceConfirmed &= talk.confirmed;
			}
		};
		try
		{
			for (auto& entry : bindings) restoreOne(entry.second);
			auto current = bindings.find(currentBinding.actor);
			if (current != bindings.end()) currentBinding = current->second;
			else restoreOne(currentBinding);
			cachedAnimator = nullptr;
		}
		catch (...) { captureRestoreUnconfirmed = true; result.bodyConfirmed = result.faceConfirmed = result.gazeConfirmed = false; }
		return result;
	}

	inline void IdolController::RestoreMute(void* animatorComponent, void* playableDirector)
	{
		if (captureCheckEnforced) { RestoreForCapture(); return; }
		(void)animatorComponent;
		(void)playableDirector;
		auto restoreOne = [this](IdolBoneBinding& b)
		{
			if (!b.actor || !b.bindPoseCaptured) return;
			if (b.animatorStateCaptured || b.defensivelyMuted)
			{
				// 停止播放瞬间，立即回写纯净静息姿态，杜绝停在动作中间帧
				ResetToRestPose(b);
				RestoreAnimatorState(b, true);
				b.defensivelyMuted = false;
			}
			// Controller activation/Rebind may reset facial data too. Restore
			// the pre-takeover face only after those Animator mutations finish.
			b.face.Restore(b.actor, faceApi);
			for (int slot = 0; slot < 2; ++slot) b.talks[slot].Restore(b.actor, slot, faceApi);
			RestoreWaistSupport(b);
			b.eyePoseModified = false;
		};

		// Scene transitions discard this map before it can be restored. Within
		// the same scene restore each owned actor once, including ALL-target mode.
		for (auto& entry : bindings) restoreOne(entry.second);
		auto current = bindings.find(currentBinding.actor);
		if (current != bindings.end())
		{
			currentBinding = current->second;
		}
		else
		{
			restoreOne(currentBinding);
		}
		cachedAnimator = nullptr;
	}

	inline Quaternion IdolController::FromEulerXYZ(float pitchDeg, float yawDeg, float rollDeg)
	{
		return Quaternion::FromEulerDegreesXYZ(pitchDeg, yawDeg, rollDeg);
	}
} // namespace GakuMotion
