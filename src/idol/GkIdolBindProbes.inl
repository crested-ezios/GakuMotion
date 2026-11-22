namespace GakuMotion
{
#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
	inline void IdolController::ProbeBindPreRebindPose(void* actor) const
	{
	// ---- PROBE A1: is the actor already in a dance pose at bind time? ----
	// Opt-in only (GAKUMOTION_ENABLE_GEOMETRY_PROBE). The question this
	// answered -- "is BindActor capturing a dance pose instead of rest" --
	// was settled on 2026-10-05; keeping it on costs 9 lines per bind.
	if (fnLogInfo && fnGetLocalRotation)
	{
		static const int kProbeBones[] = {0, 7, 8, 13, 14, 15, 16, 17, 18};
		const int kProbeCount = static_cast<int>(sizeof(kProbeBones) / sizeof(kProbeBones[0]));
		for (int bi = 0; bi < kProbeCount; ++bi)
		{
			void* pt = fnGetBoneTransform(actor, kProbeBones[bi]);
			if (!pt) continue;
			float pre[4]{0.0f, 0.0f, 0.0f, 1.0f};
			fnGetLocalRotation(pt, pre);
			Quaternion qPre(pre[0], pre[1], pre[2], pre[3]);
			float pw[4]{0.0f, 0.0f, 0.0f, 1.0f};
			if (fnGetWorldRotation) fnGetWorldRotation(pt, pw);
			fnLogInfo(
				"[PROBE A1] pre-rebind hid=%2d local=(%.4f,%.4f,%.4f,%.4f) |w|=%.6f world=(%.4f,%.4f,%.4f,%.4f)",
				kProbeBones[bi], pre[0], pre[1], pre[2], pre[3],
				std::sqrt(qPre.x * qPre.x + qPre.y * qPre.y + qPre.z * qPre.z + qPre.w * qPre.w),
				pw[0], pw[1], pw[2], pw[3]);
		}
	}
	}

	inline void IdolController::ProbeBindGroundAndSegments(void* actor, const IdolBoneBinding& b, const float hp[3], const float fp[3], bool haveFoot) const
	{
	if (fnLogInfo)
	{
		float ap[3]{0, 0, 0};
		if (b.actorTransform && fnGetWorldPosition) fnGetWorldPosition(b.actorTransform, ap);
		fnLogInfo("[PROBE A10] actor=%p | actorRootWorld=(%.4f,%.4f,%.4f)"
		          " | hipsWorld=(%.4f,%.4f,%.4f)"
		          " | restRootPos=(%.4f,%.4f,%.4f)",
		          actor, ap[0], ap[1], ap[2], hp[0], hp[1], hp[2],
		          b.restRootPos.x, b.restRootPos.y, b.restRootPos.z);
		if (haveFoot)
		{
			fnLogInfo("[PROBE A10]   LeftFoot world y=%.4f | stature=%.4f m"
			          " | groundLift applied=%.4f",
			          fp[1], hp[1] - fp[1], b.groundLift);
		}

		// ---- PROBE A12: segment lengths, measured HERE ----------------
		// At bind time the rig is in its rest pose, so the distance
		// between a joint and its child IS the segment length. That is
		// the only moment the measurement is meaningful -- during playback
		// the rotations move the joints and the numbers become pose
		// dependent, which is what made an earlier reading useless.
		//
		// Why this matters now: A11 shows torsoVerticality is correct
		// (0.86-1.00, matching the official 0.852-1.000) while the feet
		// sit only 0.15 m below the hips where the official run puts them
		// 0.93 m below. Direction is right, LENGTH is wrong. The only
		// way that happens with correct orientations is a compressed
		// bone offset, and this prints each segment so the compressed
		// one is identified rather than guessed.
		//
		// get_localPosition returned all zeros for these transforms
		// earlier (a silent no-op in the host helper), so the distance is
		// taken from world positions instead.
		auto seg = [&](int aHid, int bHid, const char* label)
		{
			if (!fnGetBoneTransform || !fnGetWorldPosition) return;
			void* ta = fnGetBoneTransform(actor, aHid);
			void* tb = fnGetBoneTransform(actor, bHid);
			if (!ta || !tb) return;
			float pa[3], pb[3];
			fnGetWorldPosition(ta, pa);
			fnGetWorldPosition(tb, pb);
			const float dx = pb[0] - pa[0], dy = pb[1] - pa[1], dz = pb[2] - pa[2];
			fnLogInfo("[PROBE A12] %-22s hid %2d->%2d  len=%.4f m",
			          label, aHid, bHid, std::sqrt(dx * dx + dy * dy + dz * dz));
		};
		seg(0, 1, "Hips->UpperLeg(thigh)");
		seg(1, 3, "UpperLeg->LowerLeg");
		seg(3, 5, "LowerLeg->Foot(shin)");
		seg(5, 19, "Foot->Toes");
		seg(0, 7, "Hips->Spine");
		seg(7, 54, "Spine->UpperChest");
		seg(0, 10, "Hips->Head(whole body)");

		// ---- PROBE A13: each segment's DIRECTION at bind time ----------
		// A12 shows the segment LENGTHS are plausible (thigh 0.3496 +
		// shin 0.3981 = 0.7477 m) yet the arithmetic contradicts A10:
		// two downward segments of that total should put the foot
		// 0.7477 m below the hips, and A10 measured 0.1046 m. So the
		// discrepancy is not length -- it is that the segments do not
		// point where the gkclip says they should.
		//
		// gkclip stores hid=3 LeftLowerLeg delta as EXACT identity, which
		// means "the shin's world rotation equals the thigh's", i.e. the
		// shin must continue straight down. If it does not, something
		// between the delta and the rig is not honouring the local write.
		//
		// verticality: +1 = pointing straight up, -1 = straight down.
		auto segDir = [&](int aHid, int bHid, const char* label)
		{
			if (!fnGetBoneTransform || !fnGetWorldPosition) return;
			void* ta = fnGetBoneTransform(actor, aHid);
			void* tb = fnGetBoneTransform(actor, bHid);
			if (!ta || !tb) return;
			float pa[3], pb[3];
			fnGetWorldPosition(ta, pa);
			fnGetWorldPosition(tb, pb);
			const float dx = pb[0] - pa[0], dy = pb[1] - pa[1], dz = pb[2] - pa[2];
			const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (len < 1e-6f) return;
			fnLogInfo("[PROBE A13] %-22s dy=%+.4f of len=%.4f =>"
			          " verticality=%+.4f  (down = -1)",
			          label, dy, len, dy / len);
		};
		segDir(0, 1, "Hips->UpperLeg");
		segDir(1, 3, "UpperLeg->LowerLeg");
		segDir(3, 5, "LowerLeg->Foot");
		segDir(0, 7, "Hips->Spine");
		segDir(7, 54, "Spine->UpperChest");
	}
	}

	inline void IdolController::ProbeBindPostRebindRestLocals(const IdolBoneBinding& b, const MotionClip& clip) const
	{
	// ---- PROBE A2: dump the captured restLocal, and compare against the
	// pre-rebind reading above. A non-zero delta means the capture basis is
	// not the rest pose. This is the value every gkclip composition uses, so
	// it must be seen, not assumed.
	if (fnLogInfo && fnGetLocalRotation)
	{
		static const int kProbeBones[] = {
			0, 7, 8, 54, 9, 10, 13, 14, 15, 16, 17, 18, 19, 20, 1, 2, 3, 4, 5, 6, 11, 12
		};
		const int kProbeCount = static_cast<int>(sizeof(kProbeBones) / sizeof(kProbeBones[0]));
		fnLogInfo(
			"[PROBE A2] captured restLocal (post-rebind) -- reference ONLY, no longer composed into the gkclip write:");
		for (size_t i = 0; i < clip.bones.size(); ++i)
		{
			const int hid = clip.bones[i].humanoid_id;
			bool wanted = false;
			for (int k = 0; k < kProbeCount; ++k)
			{
				if (kProbeBones[k] == hid)
				{
					wanted = true;
					break;
				}
			}
			if (!wanted || i >= b.restLocals.size()) continue;
			const Quaternion& rl = b.restLocals[i];
			// Angle from identity tells us how far the captured rest is from a
			// canonical T-pose. A gkclip delta is only meaningful relative to
			// whichever convention the capture actually used.
			const float w = rl.w < 0.0f ? -rl.w : rl.w;
			const float devDeg = 2.0f * std::acos(w > 1.0f ? 1.0f : w) * 57.29578f;
			fnLogInfo("[PROBE A2]   hid=%2d %-16s restLocal=(%.4f,%.4f,%.4f,%.4f) devFromIdentity=%7.2fdeg",
			          hid, clip.bones[i].name.c_str(), rl.x, rl.y, rl.z, rl.w, devDeg);
		}
	}
	}

	inline void IdolController::ProbeMotionBoneReadback(const IdolBoneBinding& b, const MotionClip& clip, size_t i, int hid, void* trans,
	                                                   const Quaternion& rVmd, const Quaternion& gkLocalOut,
	                                                   const std::vector<Quaternion>& vmdRotations,
	                                                   float currentTimeSeconds, bool isGkClip, bool probeA3Pending,
	                                                   bool overridesActive, bool hasDynamicArm) const
	{
		// ---- PROBE A7: the leg chain, self-contained ------------------------
		// WHY THIS EXISTS. The offline reconstruction says candidate B (write
		// the gkclip delta directly) reproduces the official leg spread to
		// 4.5 deg mean error, and that no sampled frame comes near 180 deg.
		// The on-device render shows ~180 deg. Both cannot be true, so the
		// disagreement must live in something the offline model lacks: the
		// engine's own post-write state, or a second code path.
		//
		// Everything is printed in ONE line so cross-frame pairing is
		// impossible (probe self-containment rule):
		//   wrote     - what we handed to setLocalRotation
		//   readback  - get_localRotation right after
		//   engWorld  - get_worldRotation right after. THIS is what the eye
		//               sees, and the only number comparable to a screenshot.
		//   predWorld - what pure FK over the raw deltas we just wrote predicts
		//   dWR / dPE - write-vs-readback and prediction-vs-engine
		// If engWorld tracks predWorld the engine obeyed us and the defect is
		// upstream (data, or a later pass). If it diverges, the engine rewrites
		// us and every offline model is moot.
		//
		// Bones 1..6 and 19,20: the whole leg chain including toes, since a
		// "horizontal split" could equally be a foot-pitch problem.
		if (probeA3Pending && isGkClip && fnGetLocalRotation && fnGetWorldRotation
			&& ((hid >= 1 && hid <= 6) || hid == 19 || hid == 20))
		{
			// angle between two rotations, via |dot|. NEVER via the squared
			// norm: for a unit quaternion x^2+y^2+z^2+w^2 is identically 1,
			// so acos of it is always 0 (the old PROBE A6 bug).
			auto ang = [](const Quaternion& a, const Quaternion& c) -> float
			{
				float d = std::fabs(Quaternion::Dot(a, c));
				if (d > 1.0f) d = 1.0f;
				return 2.0f * std::acos(d) * 57.29578f;
			};
			auto devId = [&](const Quaternion& a) -> float
			{
				float w = a.w < 0.0f ? -a.w : a.w;
				if (w > 1.0f) w = 1.0f;
				return 2.0f * std::acos(w) * 57.29578f;
			};

			float rb[4]{0, 0, 0, 1};
			fnGetLocalRotation(trans, rb);
			const Quaternion qBack(rb[0], rb[1], rb[2], rb[3]);
			float gw[4]{0, 0, 0, 1};
			fnGetWorldRotation(trans, gw);
			const Quaternion qEngWorld(gw[0], gw[1], gw[2], gw[3]);

			// Parent lookup in BOTH id spaces, and print both so the reader
			// can check they agree (mixing them was the#025 hardware bug).
			// bone_parents[i] is an index into bones[]; bone_parent_hids[i] is
			// a humanoid id. Parent of bone i is bones[bone_parents[i]].
			int pIdx = -1;
			if (i < clip.bone_parents.size())
			{
				const int raw = clip.bone_parents[i];
				if (raw >= 0 && raw < (int)clip.bones.size()) pIdx = raw;
			}
			int pHidDirect = (i < clip.bone_parent_hids.size()) ? clip.bone_parent_hids[i] : -1;
			int pHid = (pIdx >= 0) ? clip.bones[pIdx].humanoid_id : -1;

			// Pure-FK prediction: walk the ancestor chain multiplying the raw
			// deltas we just wrote. Uses NO rest term and nothing the engine
			// did, so it is an honest prediction of the engine's FK.
			//
			// GUARD BUG, FIXED 2026-10-06. This loop read `cur > 0`, but the
			// PELVIS ROOT has humanoid_id 0. The condition therefore stopped
			// the walk before it did anything, and predWorld collapsed to
			// gkLocalOut -- the bone's own delta with the pelvis missing --
			// so every dPE in the first device run (a uniform 116.26 deg) was
			// meaningless. The loop must run until the parent is ABSENT, not
			// until the id is positive. Termination is guaranteed by the
			// guard counter and by the bone_parents lookup returning -1 at
			// the root, never by comparing an id to zero.
			Quaternion qPred = gkLocalOut;
			{
				Quaternion acc = gkLocalOut;
				int cur = pHid;
				for (int guard = 0; guard < 64 && cur >= 0; ++guard)
				{
					// locate the clip entry with this hid
					size_t k = clip.bones.size();
					for (size_t m = 0; m < clip.bones.size(); ++m)
					{
						if (clip.bones[m].humanoid_id == cur)
						{
							k = m;
							break;
						}
					}
					if (k >= clip.bones.size()) break;
					if (k >= vmdRotations.size()) break;
					acc = vmdRotations[k] * acc; // parent * child
					const int pi = clip.bone_parents[k];
					if (pi < 0 || pi >= (int)clip.bones.size()) break;
					cur = clip.bones[pi].humanoid_id;
				}
				qPred = acc;
			}

			if (fnLogInfo)
			{
				fnLogInfo("[PROBE A7] actor=%p hid=%2d %-15s t=%.3f pHid=%d(pIdx=%d,pHidTbl=%d)"
				          " | wrote=(%.4f,%.4f,%.4f,%.4f) readback=(%.4f,%.4f,%.4f,%.4f)"
				          " dWR=%6.2f | engWorld=(%.4f,%.4f,%.4f,%.4f)"
				          " predWorld=(%.4f,%.4f,%.4f,%.4f) dPE=%7.2f | engDevId=%7.2f",
				          b.actor, hid, clip.bones[i].name.c_str(), currentTimeSeconds,
				          pHid, pIdx, pHidDirect,
				          gkLocalOut.x, gkLocalOut.y, gkLocalOut.z, gkLocalOut.w,
				          rb[0], rb[1], rb[2], rb[3], ang(gkLocalOut, qBack),
				          gw[0], gw[1], gw[2], gw[3],
				          qPred.x, qPred.y, qPred.z, qPred.w, ang(qPred, qEngWorld),
				          devId(qEngWorld));
			}
		}

		// ---- PROBE A3: write-then-readback on the first playback frame ----
		// Rules out the last class of silent failure: the value being handed
		// to set_localRotation is correct in our memory but never reaching
		// the Transform (wrong method resolved, an Animator overriding us on
		// the following line, or a host that recomputes locals internally).
		// Reading back what the engine actually stored cannot lie.
		// One frame only, three representative bones.
		if (probeA3Pending && fnGetLocalRotation && (hid == 0 || hid == 14 || hid == 18))
		{
			float back[4]{0.0f, 0.0f, 0.0f, 1.0f};
			fnGetLocalRotation(trans, back);
			const Quaternion qBack(back[0], back[1], back[2], back[3]);
			const float dotWant = std::fabs(gkLocalOut.x * back[0] + gkLocalOut.y * back[1]
				+ gkLocalOut.z * back[2] + gkLocalOut.w * back[3]);
			const float angWant = 2.0f * std::acos(dotWant > 1.0f ? 1.0f : dotWant) * 57.29578f;
			if (fnLogInfo)
			{
				fnLogInfo(
					"[PROBE A3] hid=%2d %-16s wrote=(%.4f,%.4f,%.4f,%.4f) readback=(%.4f,%.4f,%.4f,%.4f) diff=%7.3fdeg %s",
					hid, clip.bones[i].name.c_str(),
					gkLocalOut.x, gkLocalOut.y, gkLocalOut.z, gkLocalOut.w,
					back[0], back[1], back[2], back[3], angWant,
					angWant < 0.5f ? "OK" : "*** WRITE DID NOT LAND ***");
				// Everything needed to re-derive this line offline, on ONE
				// line. An earlier revision dumped restLocal at rebind time
				// (PROBE A2) and the delta at playback time (A6) as separate
				// blocks, which is unsound: A2 fires on every Rebind while
				// A3/A6 fire once per process, so with several clips played
				// in one session the two blocks can describe different binds.
				// That mistake produced a bogus "rest*delta disagrees with
				// wrote by 86 deg" conclusion. restLocal and rVmd are
				// therefore restated HERE, at the exact frame of the write,
				// together with which write branch actually ran.
				const Quaternion& rlNow = (i < b.restLocals.size())
					                          ? b.restLocals[i]
					                          : Quaternion::Identity();
				fnLogInfo(
					"[PROBE A3]   same-frame inputs: restLocal=(%.4f,%.4f,%.4f,%.4f) rVmd/delta=(%.4f,%.4f,%.4f,%.4f) writePath=%s overrides=%s dynamicArm=%s",
					rlNow.x, rlNow.y, rlNow.z, rlNow.w,
					rVmd.x, rVmd.y, rVmd.z, rVmd.w,
					fnSetLocalRotation
						? "setLocalRotation"
						: (fnSetWorldRotation ? "setWorldRotation(DEGRADED)" : "NONE"),
					overridesActive ? "ON" : "off",
					hasDynamicArm ? "ON" : "off");
				// PROBE A6: the root delta is what orients the whole pelvis.
				// Print it explicitly -- a ~154 deg Y rotation here rotates
				// every world-space direction that is derived downstream,
				// including the root translation direction.
				if (hid == 0)
				{
					// The angle from identity must come from |w|, NOT from the
					// squared norm. For a unit quaternion x^2+y^2+z^2+w^2 is
					// identically 1, so acos() of it is always 0 -- an earlier
					// revision of this probe printed angleFromIdentity=0.00deg
					// for a delta that was in fact 153.80deg off identity, which
					// is exactly the kind of plausible-looking nonsense that
					// sends the next person down the wrong path.
					const float dw = rVmd.w < 0.0f ? -rVmd.w : rVmd.w;
					const float dAng = 2.0f * std::acos(dw > 1.0f ? 1.0f : dw) * 57.29578f;
					const float qn = std::sqrt(rVmd.x * rVmd.x + rVmd.y * rVmd.y
						+ rVmd.z * rVmd.z + rVmd.w * rVmd.w);
					if (fnLogInfo)
					{
						fnLogInfo(
							"[PROBE A6] Hips root delta at t=%.3fs = (%.4f,%.4f,%.4f,%.4f) |q|=%.6f angleFromIdentity=%.2fdeg",
							currentTimeSeconds, rVmd.x, rVmd.y, rVmd.z, rVmd.w, qn, dAng);
						fnLogInfo(
							"[PROBE A6]   restLocal=(%.4f,%.4f,%.4f,%.4f)  gkLocalOut(written)=(%.4f,%.4f,%.4f,%.4f)",
							b.restLocals[i].x, b.restLocals[i].y, b.restLocals[i].z, b.restLocals[i].w,
							gkLocalOut.x, gkLocalOut.y, gkLocalOut.z, gkLocalOut.w);
						if (fnGetWorldRotation)
						{
							float bw[4]{0, 0, 0, 1};
							fnGetWorldRotation(trans, bw);
							fnLogInfo("[PROBE A6]   Hips world rotation after write = (%.4f,%.4f,%.4f,%.4f)",
							          bw[0], bw[1], bw[2], bw[3]);
						}
						if (fnGetWorldPosition)
						{
							float bp[3]{0, 0, 0};
							fnGetWorldPosition(trans, bp);
							fnLogInfo(
								"[PROBE A6]   Hips world position after write = (%.4f,%.4f,%.4f)  [restRootPos=(%.4f,%.4f,%.4f)]",
								bp[0], bp[1], bp[2], b.restRootPos.x, b.restRootPos.y, b.restRootPos.z);
						}
					}
				}
			}
		}
	}
#endif
} // namespace GakuMotion
