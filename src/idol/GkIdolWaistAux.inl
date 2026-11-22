namespace GakuMotion
{
	inline bool IdolController::WaistSupportApisAvailable() const noexcept
	{
		return fnGetBoneTransform && fnGetChildCount && fnGetChild && fnGetTransformName &&
			fnGetWorldPosition && fnGetWorldRotation && fnGetLocalPosition &&
			fnGetLocalRotation && fnSetLocalPosition && fnSetLocalRotation && fnGetLossyScale;
	}

	inline void* IdolController::FindExactWaistChild(void* parent, const char* name) const
	{
		if (!parent) return nullptr;
		const int count = fnGetChildCount(parent);
		if (count < 0 || count > 4096) return nullptr;
		void* found = nullptr;
		for (int index = 0; index < count; ++index)
		{
			void* child = fnGetChild(parent, index);
			if (child && fnGetTransformName(child) == name)
			{
				if (found) return nullptr; // Ambiguous names are not a binding.
				found = child;
			}
		}
		return found;
	}

	inline bool IdolController::ResolveWaistSupportTarget(const IdolBoneBinding& b, int side, WaistSupportChain& chain) const
	{
		chain = WaistSupportChain{};
		// Restoring saved LOCAL values needs identity and setters, but no
		// scale/world-pose getters. Keep this independent of the solver gate.
		if (!fnGetBoneTransform || !fnGetChildCount || !fnGetChild || !fnGetTransformName ||
			!fnSetLocalPosition || !fnSetLocalRotation || !b.actor || !b.bindPoseCaptured ||
			!CanDriveActor(b.actor) || side < 0 || side > 1)
			return false;
		// Only dereference freshly acquired transforms. b.rootTransform and the
		// saved chain below are comparison tokens, never liveness evidence.
		chain.hips = fnGetBoneTransform(b.actor, 0);
		if (!chain.hips || chain.hips != b.rootTransform || fnGetTransformName(chain.hips) != "Hips") return false;
		chain.pelvis = FindExactWaistChild(chain.hips, "Pelvis");
		if (!chain.pelvis) return false;
		chain.waistOffset = FindExactWaistChild(chain.pelvis, side == 0 ? "LeftWaist_O" : "RightWaist_O");
		chain.helper = FindExactWaistChild(chain.waistOffset, side == 0 ? "LeftWaist_H" : "RightWaist_H");
		return chain.waistOffset && chain.helper;
	}

	inline bool IdolController::ResolveWaistSupportReferences(const IdolBoneBinding& b, int side, WaistSupportChain& chain) const
	{
		const char* upperName = side == 0 ? "LeftUpLeg" : "RightUpLeg";
		chain.upperLeg = FindExactWaistChild(chain.pelvis, upperName);
		if (!chain.upperLeg || chain.upperLeg != fnGetBoneTransform(b.actor, side + 1)) return false;
		chain.thighOffset = FindExactWaistChild(chain.upperLeg, side == 0 ? "LeftThigh_O" : "RightThigh_O");
		return chain.thighOffset != nullptr;
	}

	inline bool IdolController::WaistSupportScalesValid(const WaistSupportChain& chain) const
	{
		for (void* transform : {
			     chain.hips, chain.pelvis, chain.upperLeg, chain.waistOffset,
			     chain.thighOffset, chain.helper
		     })
		{
			float value[3]{0, 0, 0};
			fnGetLossyScale(transform, value);
			if (!waist_support::IsUnitScale({value[0], value[1], value[2]})) return false;
		}
		return true;
	}

	inline bool IdolController::WaistSupportRotationValid(const Quaternion& q) noexcept
	{
		if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) return false;
		const double norm = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w;
		return std::fabs(norm - 1.) <= 1e-3;
	}

	inline void IdolController::RestoreWaistSupport(IdolBoneBinding& b)
	{
		for (int side = 0; side < 2; ++side)
		{
			auto& saved = b.waistSupport[side];
			if (!saved.captured) continue;
			WaistSupportChain live;
			if (ResolveWaistSupportTarget(b, side, live) && saved.SameTargetIdentity(live))
			{
				const auto p = saved.savedLocalPosition;
				const auto q = saved.savedLocalRotation;
				fnSetLocalPosition(live.helper, p.x, p.y, p.z);
				fnSetLocalRotation(live.helper, q.x, q.y, q.z, q.w);
			}
			saved = WaistSupportChain{};
		}
	}

	inline void IdolController::RestoreWaistSupportSide(WaistSupportChain& saved, const WaistSupportChain& live)
	{
		if (saved.captured && saved.SameTargetIdentity(live))
		{
			const auto p = saved.savedLocalPosition;
			const auto q = saved.savedLocalRotation;
			fnSetLocalPosition(live.helper, p.x, p.y, p.z);
			fnSetLocalRotation(live.helper, q.x, q.y, q.z, q.w);
		}
		saved = live;
	}

	inline void IdolController::ApplyWaistSupport(IdolBoneBinding& b)
	{
		if (!experimentalWaistSupportEnabled) return;
		unsigned applied = 0;
		for (int side = 0; side < 2; ++side)
		{
			auto& saved = b.waistSupport[side];
			WaistSupportChain live;
			if (!ResolveWaistSupportTarget(b, side, live))
			{
				saved = WaistSupportChain{}; // Never touch the former rig.
				continue;
			}
			if (!ResolveWaistSupportReferences(b, side, live))
			{
				// Losing a solver reference does not invalidate a freshly
				// reacquired helper. Roll that same target back before skipping.
				RestoreWaistSupportSide(saved, live);
				continue;
			}
			if (!saved.SameIdentity(live))
			{
				// A reference-only replacement does not create a new local
				// target. Keep the pre-first-write snapshot for that same live
				// helper/parent, rather than capturing our own previous output.
				if (saved.captured && saved.SameTargetIdentity(live))
				{
					live.savedLocalPosition = saved.savedLocalPosition;
					live.savedLocalRotation = saved.savedLocalRotation;
					live.captured = true;
				}
				saved = live;
			}
			if (!WaistSupportApisAvailable() || !WaistSupportScalesValid(live))
			{
				RestoreWaistSupportSide(saved, live);
				continue;
			}
			float pW[3]{}, pT[3]{}, qW[4]{};
			fnGetWorldPosition(live.waistOffset, pW);
			fnGetWorldPosition(live.thighOffset, pT);
			fnGetWorldRotation(live.waistOffset, qW);
			waist_support::Result result;
			if (!waist_support::Solve({pW[0], pW[1], pW[2]}, {qW[0], qW[1], qW[2], qW[3]},
			                          {pT[0], pT[1], pT[2]}, result))
			{
				RestoreWaistSupportSide(saved, live);
				continue;
			}
			if (!saved.captured)
			{
				float originalP[3]{}, originalQ[4]{};
				fnGetLocalPosition(live.helper, originalP);
				fnGetLocalRotation(live.helper, originalQ);
				saved.savedLocalPosition = {originalP[0], originalP[1], originalP[2]};
				saved.savedLocalRotation = {originalQ[0], originalQ[1], originalQ[2], originalQ[3]};
				if (!waist_support::Finite(saved.savedLocalPosition) ||
					!WaistSupportRotationValid(saved.savedLocalRotation))
					continue;
				saved.captured = true;
			}
			const auto& p = result.localPosition;
			const auto& q = result.localRotation;
			fnSetLocalPosition(live.helper, p.x, p.y, p.z);
			fnSetLocalRotation(live.helper, q.x, q.y, q.z, q.w);
			applied |= 1u << side;
		}
		if (!b.waistSupportDiagnosticLogged)
		{
			GM_DEBUG(
				"[GakuMotion][WaistSupport][Experimental] actor=%p appliedMask=%u (Waist_H only; scoped acceptance passed, broader scope unverified)",
				b.actor, applied);
			b.waistSupportDiagnosticLogged = true;
		}
	}

	inline bool IdolController::SetExperimentalWaistSupportEnabled(bool enabled)
	{
		if (enabled == experimentalWaistSupportEnabled) return true;
		if (!enabled)
		{
			for (auto& entry : bindings) RestoreWaistSupport(entry.second);
			auto current = bindings.find(currentBinding.actor);
			if (current != bindings.end()) currentBinding = current->second;
			else RestoreWaistSupport(currentBinding);
		}
		experimentalWaistSupportEnabled = enabled;
		for (auto& entry : bindings) entry.second.waistSupportDiagnosticLogged = false;
		currentBinding.waistSupportDiagnosticLogged = false;
		return true;
	}

	inline void* IdolController::ResolveModelSourceTransform(const IdolBoneBinding& b, int hid) noexcept
	{
		switch (hid)
		{
		case 11: return b.leftArmAux.shoulder;
		case 13: return b.leftArmAux.upperArm;
		case 15: return b.leftArmAux.foreArm;
		case 17: return b.leftArmAux.hand;
		case 12: return b.rightArmAux.shoulder;
		case 14: return b.rightArmAux.upperArm;
		case 16: return b.rightArmAux.foreArm;
		case 18: return b.rightArmAux.hand;
		default: return nullptr;
		}
	}

	inline void* IdolController::ResolveModelAuxTransform(const IdolBoneBinding& b, const char* auxBone) noexcept
	{
		if (!auxBone) return nullptr;
		if (std::strcmp(auxBone, "LeftArm_Roll_H") == 0) return b.leftArmAux.upperArmRollH;
		if (std::strcmp(auxBone, "LeftArm_H") == 0) return b.leftArmAux.upperArmH;
		if (std::strcmp(auxBone, "LeftForeArm_Roll_H") == 0) return b.leftArmAux.foreArmRollH;
		if (std::strcmp(auxBone, "LeftForeArm_H") == 0) return b.leftArmAux.foreArmH;
		if (std::strcmp(auxBone, "LeftHand_H") == 0) return b.leftArmAux.handH;
		if (std::strcmp(auxBone, "RightArm_Roll_H") == 0) return b.rightArmAux.upperArmRollH;
		if (std::strcmp(auxBone, "RightArm_H") == 0) return b.rightArmAux.upperArmH;
		if (std::strcmp(auxBone, "RightForeArm_Roll_H") == 0) return b.rightArmAux.foreArmRollH;
		if (std::strcmp(auxBone, "RightForeArm_H") == 0) return b.rightArmAux.foreArmH;
		if (std::strcmp(auxBone, "RightHand_H") == 0) return b.rightArmAux.handH;
		return nullptr;
	}

	inline void IdolController::ApplyAuxModelToBinding(const IdolBoneBinding& b) const
	{
		if (!fnSetLocalRotation || !fnGetLocalRotation) return;

		const aux_model::AuxChannelModel* channels = aux_model::GetChannels();
		const int count = aux_model::GetChannelCount();

		Quaternion srcLocal[19];
		bool srcValid[19] = {false};

		for (int i = 0; i < count; ++i)
		{
			const aux_model::AuxChannelModel& m = channels[i];
			void* auxTrans = ResolveModelAuxTransform(b, m.auxBone);
			if (!auxTrans) continue;

			const int hid = m.sourceHid;
			if (hid < 0 || hid >= 19) continue;

			if (!srcValid[hid])
			{
				void* src = ResolveModelSourceTransform(b, hid);
				if (!src) return;
				float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
				fnGetLocalRotation(src, r);
				// q and -q are the same orientation. The fitted model's atan2
				// and rotation-vector paths otherwise produce different auxiliary
				// poses for them. Use the shortest-rotation hemisphere only for
				// model input; never change the source bone's actual rotation.
				// At exactly 180 degrees, choose the first nonzero vector component.
				const bool flip = r[3] < 0.0f || (r[3] == 0.0f &&
					(r[0] < 0.0f || (r[0] == 0.0f &&
					(r[1] < 0.0f || (r[1] == 0.0f && r[2] < 0.0f)))));
				const float sign = flip ? -1.0f : 1.0f;
				srcLocal[hid] = Quaternion(sign * r[0], sign * r[1], sign * r[2], sign * r[3]);
				srcValid[hid] = true;
			}

			float angleDeg = aux_model::EvaluateAuxAngleDeg(m, srcLocal[hid]);
			// 模型是在一支完整舞曲的旋转流形上标定的，超出范围时钳到训练域附近，
			// 避免多项式在未见姿态上的自由外插。
			if (angleDeg > 180.0f) angleDeg = 180.0f;
			else if (angleDeg < -180.0f) angleDeg = -180.0f;

			const float half = angleDeg * 0.5f * 0.01745329252f;
			const float s = std::sin(half);
			const float c = std::cos(half);
			Quaternion q(0.0f, 0.0f, 0.0f, 1.0f);
			if (m.auxAxis == 0) q.x = s;
			else if (m.auxAxis == 1) q.y = s;
			else q.z = s;
			q.w = c;

			fnSetLocalRotation(auxTrans, q.x, q.y, q.z, q.w);
		}
	}

	inline void IdolController::SetAuxDriveMode(int mode)
	{
		if (mode < 0 || mode >= kAuxModeCount) mode = kAuxNormal;
		if (mode == auxDriveMode) return;
		auxDriveMode = mode;
		if (fnLogInfo)
		{
			fnLogInfo("[GakuMotion][AuxMode] switched to %d = %s", mode, AuxDriveModeName(mode));
		}
	}

	inline bool IdolController::LoadAuxGroundTruth(const std::string& path)
	{
		std::string err;
		const bool ok = auxGroundTruth.Load(path, err);
		if (fnLogInfo)
		{
			fnLogInfo("[GakuMotion][AuxReplay] load %s -> %s (%zu tracks, fps=%.2f)%s%s",
			          path.c_str(), ok ? "OK" : "FAILED", auxGroundTruth.tracks.size(),
			          auxGroundTruth.fps,
			          err.empty() ? "" : " err=", err.c_str());
		}
		return ok;
	}

	inline const Quaternion* IdolController::LookupAuxReplay(int humanoidId, bool isRoll, float timeSeconds) const noexcept
	{
		if (auxGroundTruth.Empty()) return nullptr;
		return auxGroundTruth.Sample(humanoidId, isRoll ? 0 : 1, timeSeconds);
	}
} // namespace GakuMotion
