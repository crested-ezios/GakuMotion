namespace GakuMotion
{
#if GAKUMOTION_ENABLE_GEOMETRY_PROBE
	inline void IdolController::ProbeMotionPostApplyGeometry(IdolBoneBinding& b, const MotionClip& clip, const Vector3& vmdRootPos,
	                                                        const Quaternion& curActorRot, float currentTimeSeconds, bool probeA3Pending) const
	{
		// ---- PROBE A9: WHO is this actor, and what are its real bone lengths? ----
		// WHY THIS EXISTS. A8 measured thighLen 0.3496 m against 0.4558 m in
		// ground_truth_real.json -- a 23.3% mismatch -- and GakuMotion NEVER
		// WRITES BONE LENGTHS. It writes rotations and the root transform only.
		// Bone length is owned by the rig's skeleton, so a mismatch cannot be a
		// scale bug in our code; it means the two numbers describe DIFFERENT
		// CHARACTERS. Cross-checking confirmed it: the device's restLocal differs
		// from gakumas_bind_pose.json (source: official_fmw_saki.json) on every
		// bone by 7-52 deg, and Hips by 147 deg. Meanwhile ground_truth_real.json
		// declares model = "YYB Hatsune Miku_default_1.0ver". So the device actor
		// is NEITHER Saki NOR that Miku, and every absolute number I have been
		// comparing against is from the wrong skeleton.
		//
		// That makes this a correctness prerequisite, not a nicety: the leg-spread
		// verdict (A8: 166 deg vs truth 8.96 deg) is only meaningful once the
		// reference is the same character. Note the A8 signal that does NOT depend
		// on any reference: verticalFraction 0.044/0.211 means the thighs are
		// near-horizontal, and 1.0 means standing. That is geometry, not a
		// comparison, and it already condemns the pose.
		//
		// What this prints, one self-contained block:
		//   - the actor transform's own name and each measured bone's name,
		//     so the character can be identified from the log alone
		//   - the BONE LENGTHS as the rig actually has them at bind time, from
		//     each bone's own local position -- these are the numbers any
		//     comparison must use
		//   - the actor's local position, to catch a scaled actor root
		//
		// The bone index lookup lives OUTSIDE both probes' guards on purpose: A8
		// needs it too, and A8's guard (fnGetWorldPosition) is independent of
		// A9's (fnGetTransformName). Declaring it inside A9's if-block would
		// make A8 fail to compile-scope the moment the name getter is absent.
		int iHips = -1, iLThigh = -1, iRThigh = -1, iLKnee = -1, iRKnee = -1;
		int iLFoot = -1, iRFoot = -1, iRoot = -1, iLToes = -1, iRToes = -1;
		for (size_t k = 0; k < clip.bones.size(); ++k)
		{
			switch (clip.bones[k].humanoid_id)
			{
			case 0: iHips = (int)k;
				break;
			case 1: iLThigh = (int)k;
				break;
			case 2: iRThigh = (int)k;
				break;
			case 3: iLKnee = (int)k;
				break;
			case 4: iRKnee = (int)k;
				break;
			case 5: iLFoot = (int)k;
				break;
			case 6: iRFoot = (int)k;
				break;
			case 19: iLToes = (int)k;
				break;
			case 20: iRToes = (int)k;
				break;
			default: break;
			}
		}
		if (probeA3Pending && fnGetTransformName)
		{
			if (iHips >= 0 && (size_t)iHips < b.boneTransforms.size())
			{
				fnLogInfo("[PROBE A9] actor=%p name=\"%s\" | hips transform name = \"%s\"",
				          b.actor,
				          b.actorTransform ? fnGetTransformName(b.actorTransform).c_str() : "(none)",
				          fnGetTransformName(b.boneTransforms[iHips]).c_str());
			}
			else if (b.actorTransform)
			{
				fnLogInfo("[PROBE A9] actor=%p name=\"%s\" (no hips transform)",
				          b.actor, fnGetTransformName(b.actorTransform).c_str());
			}
			// Bone-local positions give the rig's real segment lengths.
			if (fnGetLocalPosition)
			{
				auto blen = [&](int boneIdx, const char* label) -> void
				{
					if (boneIdx < 0 || (size_t)boneIdx >= b.boneTransforms.size()) return;
					float lp[3]{0, 0, 0};
					fnGetLocalPosition(b.boneTransforms[boneIdx], lp);
					const float l = std::sqrt(lp[0] * lp[0] + lp[1] * lp[1] + lp[2] * lp[2]);
					fnLogInfo("[PROBE A9]   localPos %-12s (%2d) = (%+.4f,%+.4f,%+.4f) |len|=%.4f",
					          label, boneIdx, lp[0], lp[1], lp[2], l);
				};
				blen(iLThigh, "LUpperLeg");
				blen(iLKnee, "LLowerLeg");
				blen(iLFoot, "LFoot");
				blen(iRThigh, "RUpperLeg");
				blen(iRKnee, "RLowerLeg");
				blen(iRFoot, "RFoot");
				blen(iLToes, "LToes");
				blen(iRToes, "RToes");
				blen(iHips, "Hips");
			}
			if (b.actorTransform && fnGetLocalPosition)
			{
				float ap[3]{0, 0, 0};
				fnGetLocalPosition(b.actorTransform, ap);
				fnLogInfo("[PROBE A9] actor localPos = (%+.4f,%+.4f,%+.4f)  (a non-1 scale here"
				          " would mean the whole body is scaled)", ap[0], ap[1], ap[2]);
			}
		}

		// ---- PROBE A8: POSITION, and the leg spread computed from it ----------
		// WHY. A7 settled the rotations: engWorld = Ry(90.000 deg) * predWorld, the
		// same rigid rotation on all eight leg bones and on the root (spread across
		// bones 0.005 deg). A rigid left-multiplication cannot change the angle
		// between two bones, and indeed the engine's two thigh world rotations sit
		// 14.49 deg apart against our predicted 14.53 deg and a ground truth of
		// 8.96 deg. So bone ORIENTATIONS are essentially right and the ~180 deg
		// render has to be a POSITION defect. A7 could not test that: world
		// rotations carry no child offsets, so no spread can be derived from them.
		//
		// This probe therefore reads positions, and it computes the leg spread the
		// same way the official extractor does (tools/extract_ground_truth_real.py
		// :673-676) -- hip->knee vectors, NOT the pelvis-width shortcut that
		// produced a fake 83.141 deg constant earlier.
		//
		// Reference numbers from the OFFICIAL run of this very motion:
		//   Hips.y 0.857 - 1.197 (standing, ~1.05 typical)
		//   LFoot.y 0.106 - 0.613, RFoot.y 0.106 - 1.404 (feet on the floor)
		//   leg_spread: 0.43 - 136.26 deg, and at f0 it is 8.96 deg
		// So: feet near y=0 and spread well under 90 => correct. Feet high, or
		// spread near 180 => position defect, and the y numbers say how much.
		//
		// ONE self-contained line, after every bone has been written, so the
		// positions are the final ones and cannot be paired across frames.
		if (probeA3Pending && fnGetWorldPosition)
		{
			auto wp = [&](void* t, Vector3& out) -> bool
			{
				if (!t) return false;
				float p[3]{0, 0, 0};
				fnGetWorldPosition(t, p);
				out = Vector3(p[0], p[1], p[2]);
				return true;
			};
			// iHips / iLThigh / iRThigh / iLKnee / iRKnee / iLFoot / iRFoot are
			// resolved once in PROBE A9 above so both probes share one lookup.
			Vector3 pHips, pLTh, pRTh, pLK, pRK, pLF, pRF, pRoot;
			const bool ok = (iHips >= 0)
				&& wp((size_t)iHips < b.boneTransforms.size() ? b.boneTransforms[iHips] : nullptr, pHips)
				&& wp((size_t)iLThigh < b.boneTransforms.size() ? b.boneTransforms[iLThigh] : nullptr, pLTh)
				&& wp((size_t)iRThigh < b.boneTransforms.size() ? b.boneTransforms[iRThigh] : nullptr, pRTh)
				&& wp((size_t)iLKnee < b.boneTransforms.size() ? b.boneTransforms[iLKnee] : nullptr, pLK)
				&& wp((size_t)iRKnee < b.boneTransforms.size() ? b.boneTransforms[iRKnee] : nullptr, pRK)
				&& wp((size_t)iLFoot < b.boneTransforms.size() ? b.boneTransforms[iLFoot] : nullptr, pLF)
				&& wp((size_t)iRFoot < b.boneTransforms.size() ? b.boneTransforms[iRFoot] : nullptr, pRF);
			const bool okRoot = b.rootTransform && wp(b.rootTransform, pRoot);

			if (ok)
			{
				// Official formula: thigh axis = hip -> knee.
				auto vsub = [](const Vector3& a, const Vector3& c)
				{
					return Vector3(c.x - a.x, c.y - a.y, c.z - a.z);
				};
				auto vlen = [](const Vector3& v)
				{
					return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
				};
				auto vdot = [](const Vector3& u, const Vector3& v)
				{
					return u.x * v.x + u.y * v.y + u.z * v.z;
				};
				auto vang = [&vlen, &vdot](const Vector3& u, const Vector3& v)
				{
					const float lu = vlen(u), lv = vlen(v);
					if (lu < 1e-9f || lv < 1e-9f) return 0.0f;
					float d = vdot(u, v) / (lu * lv);
					if (d > 1.0f) d = 1.0f;
					if (d < -1.0f) d = -1.0f;
					return std::acos(d) * 57.29578f;
				};
				const float spread = vang(vsub(pHips, pLK), vsub(pHips, pRK));
				const float thighLenL = vlen(vsub(pLTh, pLK));
				const float thighLenR = vlen(vsub(pRTh, pRK));
				// A splayed leg is short in Y and long in X/Z; a straight standing
				// leg is long in Y. This ratio says which one we actually have,
				// and it needs no angle convention at all.
				const float yFracL = std::fabs(pLK.y - pLTh.y) / (thighLenL > 1e-9f ? thighLenL : 1.0f);
				const float yFracR = std::fabs(pRK.y - pRTh.y) / (thighLenR > 1e-9f ? thighLenR : 1.0f);

				if (fnLogInfo)
				{
					fnLogInfo("[PROBE A8] POSITION actor=%p t=%.3f | Hips=(%.4f,%.4f,%.4f)"
					          " LThigh=(%.4f,%.4f,%.4f) LKnee=(%.4f,%.4f,%.4f)"
					          " RThigh=(%.4f,%.4f,%.4f) RKnee=(%.4f,%.4f,%.4f)"
					          " LFoot=(%.4f,%.4f,%.4f) RFoot=(%.4f,%.4f,%.4f)",
					          b.actor, currentTimeSeconds,
					          pHips.x, pHips.y, pHips.z,
					          pLTh.x, pLTh.y, pLTh.z, pLK.x, pLK.y, pLK.z,
					          pRTh.x, pRTh.y, pRTh.z, pRK.x, pRK.y, pRK.z,
					          pLF.x, pLF.y, pLF.z, pRF.x, pRF.y, pRF.z);
					fnLogInfo("[PROBE A8]   OFFICIAL leg_spread=%.2f deg (hip->knee) |"
					          " thighLen L=%.4f R=%.4f | verticalFraction L=%.3f R=%.3f"
					          " | Hips.y=%.4f LFoot.y=%.4f RFoot.y=%.4f",
					          spread, thighLenL, thighLenR, yFracL, yFracR,
					          pHips.y, pLF.y, pRF.y);
					fnLogInfo("[PROBE A8]   reference: official run gives Hips.y~1.05"
					          " feet~0.10-0.25 spread<=136.26 (f0=8.96). Written root"
					          " local=(%.4f,%.4f,%.4f) restRootPos=(%.4f,%.4f,%.4f)"
					          " restActorPos=(%.4f,%.4f,%.4f) rootWorld=%s(%.4f,%.4f,%.4f)",
					          b.restRootPos.x + vmdRootPos.x,
					          b.restRootPos.y + vmdRootPos.y,
					          b.restRootPos.z + vmdRootPos.z,
					          b.restRootPos.x, b.restRootPos.y, b.restRootPos.z,
					          b.restActorPos.x, b.restActorPos.y, b.restActorPos.z,
					          okRoot ? "yes" : "unread",
					          pRoot.x, pRoot.y, pRoot.z);
					fnLogInfo("[PROBE A8]   curActorRot=(%.4f,%.4f,%.4f,%.4f)"
					          " restActorRot=(%.4f,%.4f,%.4f,%.4f) vmdRootPos=(%.4f,%.4f,%.4f)",
					          curActorRot.x, curActorRot.y, curActorRot.z, curActorRot.w,
					          b.restActorRot.x, b.restActorRot.y, b.restActorRot.z, b.restActorRot.w,
					          vmdRootPos.x, vmdRootPos.y, vmdRootPos.z);
				}
			}
			else if (fnLogInfo)
			{
				fnLogInfo("[PROBE A8] POSITION UNAVAILABLE (missing transform:"
				          " hips=%d lthigh=%d rthigh=%d lknee=%d rknee=%d lfoot=%d rfoot=%d)",
				          iHips, iLThigh, iRThigh, iLKnee, iRKnee, iLFoot, iRFoot);
			}
		}

		// Opt-in geometry diagnostics; normal playback must not stream probes.
		// ---- PROBE A11: torso uprightness, EVERY frame --------------------
		// The screenshots at1.5s and 4.5s show the character leaning far back
		// with the legs thrown forward and the body nearly horizontal. The
		// official ground truth for this very motion says otherwise:
		//
		//     torso verticality (hips->head vs world up): min 0.852, median 0.993
		//     Hips.y: 0.97 - 1.08   LFoot.y: 0.11 - 0.20  (feet on the floor)
		//
		// i.e. the character is UPRIGHT for the whole 82 seconds and never drops
		// below hip height 0.97 m. So the render is not "a bit off", it is
		// qualitatively wrong, and the measurement that decides it is the torso
		// verticality -- not any bone angle, because the bone angles can all be
		// self-consistent while the whole body is tipped over.
		//
		// Unlike A8 this runs EVERY frame while the clip plays, so a trend is
		// visible rather than a single sample, and it is throttled to ~2 Hz so
		// the log stays readable over an 82-second track.
		//
		// Read it as: 1.0 upright, 0.0 horizontal, negative head-below-hips.
		if (fnLogInfo && fnGetWorldPosition && fnGetBoneTransform
			&& b.boneTransforms.size() > 6)
		{
			if (currentTimeSeconds < b.lastGeometryProbeTime ||
				currentTimeSeconds - b.lastGeometryProbeTime > 0.5f)
			{
				b.lastGeometryProbeTime = currentTimeSeconds;
				auto wpos = [&](int boneIdx, float* out) -> bool
				{
					if (boneIdx < 0 || (size_t)boneIdx >= b.boneTransforms.size()) return false;
					if (!b.boneTransforms[boneIdx]) return false;
					fnGetWorldPosition(b.boneTransforms[boneIdx], out);
					return true;
				};
				// Head is humanoid id 10, Hips is 0, feet are 5 and 6.
				int idxHips = -1, idxHead = -1, idxLFoot = -1, idxRFoot = -1;
				for (size_t k = 0; k < clip.bones.size(); ++k)
				{
					switch (clip.bones[k].humanoid_id)
					{
					case 0: idxHips = (int)k;
						break;
					case 10: idxHead = (int)k;
						break;
					case 5: idxLFoot = (int)k;
						break;
					case 6: idxRFoot = (int)k;
						break;
					default: break;
					}
				}
				float hp[3], hd[3], lf[3], rf[3];
				if (wpos(idxHips, hp) && wpos(idxHead, hd)
					&& wpos(idxLFoot, lf) && wpos(idxRFoot, rf))
				{
					const float dx = hd[0] - hp[0], dy = hd[1] - hp[1], dz = hd[2] - hp[2];
					const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
					const float vert = (len > 1e-6f) ? (dy / len) : 0.0f;
					// The same segment verticality A13 measured at bind time, now
					// during playback. At bind the chain summed to -0.776 (the full
					// stature: thigh -0.984, shin -0.935), so if playback shows a
					// much smaller drop the leg is folding somewhere, and the
					// per-segment numbers say WHERE -- which no bone-angle check
					// can tell us, because every local rotation can be perfectly
					// self-consistent while the chain folds.
					int idxKnee = -1;
					for (size_t k = 0; k < clip.bones.size(); ++k)
					{
						if (clip.bones[k].humanoid_id == 3)
						{
							idxKnee = (int)k;
							break;
						}
					}
					float kp[3]{};
					const bool haveKnee = wpos(idxKnee, kp);
					float vThigh = 0.0f, vShin = 0.0f;
					if (haveKnee)
					{
						const float tdx = kp[0] - hp[0], tdy = kp[1] - hp[1], tdz = kp[2] - hp[2];
						const float tl = std::sqrt(tdx * tdx + tdy * tdy + tdz * tdz);
						if (tl > 1e-6f) vThigh = tdy / tl;
						const float sdx = lf[0] - kp[0], sdy = lf[1] - kp[1], sdz = lf[2] - kp[2];
						const float sl = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz);
						if (sl > 1e-6f) vShin = sdy / sl;
					}
					const float drop = lf[1] - hp[1];
					auto kneeAngle = [&](int thigh, int knee, int foot) -> float
					{
						float t[3]{}, k[3]{}, f[3]{};
						if (!wpos(thigh, t) || !wpos(knee, k) || !wpos(foot, f)) return -1.0f;
						float dot = 0, aa = 0, bb = 0;
						for (int j = 0; j < 3; ++j)
						{
							float a = k[j] - t[j], c = f[j] - k[j];
							dot += a * c;
							aa += a * a;
							bb += c * c;
						}
						if (aa * bb < 1e-12f) return -1.0f;
						return std::acos(std::clamp(dot / std::sqrt(aa * bb), -1.0f, 1.0f)) * 57.2957795f;
					};
					fnLogInfo("[PROBE A11] actor=%p t=%6.2f | torsoVert=%+.4f"
					          " | Hips.y=%+.4f LFoot.y=%+.4f RFoot.y=%+.4f"
					          " | footY-meanHipY=%+.4f"
					          " | thighVert=%+.4f shinVert=%+.4f"
					          " | thigh+shin drop=%+.4f | kneeDeg L/R=%.2f/%.2f",
					          b.actor, currentTimeSeconds, vert,
					          hp[1], lf[1], rf[1],
					          (lf[1] + rf[1]) * 0.5f - hp[1],
					          vThigh, vShin, drop,
					          kneeAngle(iLThigh, iLKnee, iLFoot), kneeAngle(iRThigh, iRKnee, iRFoot));
				}
			}
		}

	}
#endif
} // namespace GakuMotion
