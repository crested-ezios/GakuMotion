namespace GakuMotion
{
	inline void IdolController::ApplyArmAuxiliaryForBone(const IdolBoneBinding& b, int hid,
	                                                     const Quaternion& rRestRel, const Quaternion& curActorRot,
	                                                     const Quaternion& rTarget,
	                                                     const std::array<Quaternion, 64>& writtenWorldRots,
	                                                     const std::array<bool, 64>& writtenWorldRotValid,
	                                                     float currentTimeSeconds) const
	{
		if (auxDriveMode == kAuxNormal) return;
		// 4c. 原生辅助骨分流：
		// (1) 官方 QuartzDriver 保角共形重构 + _Roll_H 局部 50% 继承（主管骨骼轴向肌肉扭转）
		// (2) 关节 50% 弯曲半角支撑骨 _H（主管肩袖腋下、手肘、手腕弯曲支撑，根除抬手夹角凹陷破面）
		void* auxRollH = nullptr;
		Quaternion qAuxRollHWorld;
		const Quaternion* auxRollHRestLocal = nullptr;

		void* auxJointH = nullptr;
		Quaternion qJointHWorld;
		const Quaternion* auxJointHRestLocal = nullptr;
		Quaternion qParentForLocal = rTarget;

		if (hid == 13 || hid == 14)
		{
			// 大臂骨（LeftUpperArm=13, RightUpperArm=14）
			const bool isLeft = (hid == 13);
			const Vector3 uLocal = isLeft ? Vector3(-1.0f, 0.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
			const int shoulderHid = isLeft ? 11 : 12;

			Quaternion qShoulderWorld = curActorRot;
			if (shoulderHid < 64 && writtenWorldRotValid[shoulderHid])
			{
				qShoulderWorld = writtenWorldRots[shoulderHid];
			}
			else
			{
				void* shoulderTrans = isLeft ? b.leftArmAux.shoulder : b.rightArmAux.shoulder;
				if (shoulderTrans && fnGetWorldRotation)
				{
					float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
					fnGetWorldRotation(shoulderTrans, r);
					qShoulderWorld = Quaternion(r[0], r[1], r[2], r[3]);
				}
				else
				{
					qShoulderWorld = curActorRot * (isLeft
						                                ? b.leftArmAux.shoulderRestRel
						                                : b.rightArmAux.shoulderRestRel);
				}
			}
			const Quaternion qShoulderRestWorld = curActorRot * (isLeft
				                                                     ? b.leftArmAux.shoulderRestRel
				                                                     : b.rightArmAux.shoulderRestRel);
			const Quaternion qUpperArmRestWorld = curActorRot * (isLeft
				                                                     ? b.leftArmAux.upperArmRestRel
				                                                     : b.rightArmAux.upperArmRestRel);

			// 纯局部空间坐标变换（无任何世界空间增量或非线性 Slerp）：
			const Quaternion qLocalRest = qShoulderRestWorld.Inverse() * qUpperArmRestWorld;
			const Quaternion qLocalCur = qShoulderWorld.Inverse() * rTarget;
			const Quaternion dQ_local = qLocalRest.Inverse() * qLocalCur;

			const bool hasRollH = isLeft
				                      ? (b.leftArmAux.upperArmRollH != nullptr)
				                      : (b.rightArmAux.upperArmRollH != nullptr);
			QuartzDriverAuxResult aux = SolveQuartzAuxiliaryBones(dQ_local, uLocal, hasRollH);

			// 1. 主骨骼保持 100% 原始动力学位姿（rTarget 不做人为截断，确保肘关节与前臂铰链朝向原态）

			// 2. UpperArm_Roll_H 世界空间承担 100% Bend + 50% Roll
			auxRollH = isLeft ? b.leftArmAux.upperArmRollH : b.rightArmAux.upperArmRollH;
			auxRollHRestLocal = isLeft
				                    ? &b.leftArmAux.upperArmRollHRestLocal
				                    : &b.rightArmAux.upperArmRollHRestLocal;
			if (auxRollH)
			{
				qAuxRollHWorld = qShoulderWorld * (qLocalRest * aux.qRollHLocal);
			}

			// 3. 肩袖/腋下半角支撑骨 (LeftArm_H / RightArm_H): 承载 50% Bend, 0% Roll
			auxJointH = isLeft ? b.leftArmAux.upperArmH : b.rightArmAux.upperArmH;
			auxJointHRestLocal = isLeft ? &b.leftArmAux.upperArmHRestLocal : &b.rightArmAux.upperArmHRestLocal;
			if (auxJointH)
			{
				qJointHWorld = qShoulderWorld * (qLocalRest * aux.qJointHLocal);
				qParentForLocal = rTarget;
			}
		}
		else if (hid == 15 || hid == 16)
		{
			// 前臂骨（LeftLowerArm=15, RightLowerArm=16）
			const bool isLeft = (hid == 15);
			const Vector3 uLocal = isLeft ? Vector3(-1.0f, 0.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
			const int upperArmHid = isLeft ? 13 : 14;

			Quaternion qUpperArmWorld = curActorRot;
			if (upperArmHid < 64 && writtenWorldRotValid[upperArmHid])
			{
				qUpperArmWorld = writtenWorldRots[upperArmHid];
			}
			else
			{
				void* upperArmTrans = isLeft ? b.leftArmAux.upperArm : b.rightArmAux.upperArm;
				if (upperArmTrans && fnGetWorldRotation)
				{
					float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
					fnGetWorldRotation(upperArmTrans, r);
					qUpperArmWorld = Quaternion(r[0], r[1], r[2], r[3]);
				}
				else
				{
					qUpperArmWorld = curActorRot * (isLeft
						                                ? b.leftArmAux.upperArmRestRel
						                                : b.rightArmAux.upperArmRestRel);
				}
			}
			const Quaternion qUpperArmRestWorld = curActorRot * (isLeft
				                                                     ? b.leftArmAux.upperArmRestRel
				                                                     : b.rightArmAux.upperArmRestRel);
			const Quaternion qLowerArmRestWorld = curActorRot * rRestRel;

			// 纯局部空间坐标变换：
			const Quaternion qLocalRest = qUpperArmRestWorld.Inverse() * qLowerArmRestWorld;
			const Quaternion qLocalCur = qUpperArmWorld.Inverse() * rTarget;
			const Quaternion dQ_local = qLocalRest.Inverse() * qLocalCur;

			// 官方管线实证：前臂为严格 1-DOF 局部铰链，ForeArm_Roll_H 保持静息局部姿态 (hasRollH = false)
			QuartzDriverAuxResult aux = SolveQuartzAuxiliaryBones(dQ_local, uLocal, false);

			auxRollH = isLeft ? b.leftArmAux.foreArmRollH : b.rightArmAux.foreArmRollH;
			auxRollHRestLocal = isLeft
				                    ? &b.leftArmAux.foreArmRollHRestLocal
				                    : &b.rightArmAux.foreArmRollHRestLocal;
			if (auxRollH)
			{
				qAuxRollHWorld = rTarget * (isLeft
					                            ? b.leftArmAux.foreArmRollHRestLocal
					                            : b.rightArmAux.foreArmRollHRestLocal);
			}

			// 肘关节半角支撑骨 (LeftForeArm_H / RightForeArm_H): 承载纯弯曲半角 (50% Bend, 0% Roll)
			auxJointH = isLeft ? b.leftArmAux.foreArmH : b.rightArmAux.foreArmH;
			auxJointHRestLocal = isLeft ? &b.leftArmAux.foreArmHRestLocal : &b.rightArmAux.foreArmHRestLocal;
			if (auxJointH)
			{
				qJointHWorld = qUpperArmWorld * (qLocalRest * aux.qJointHLocal);
				qParentForLocal = rTarget;
			}
		}
		else if (hid == 17 || hid == 18)
		{
			// 手掌骨（LeftHand=17, RightHand=18）
			const bool isLeft = (hid == 17);
			const Vector3 uLocal = isLeft ? Vector3(-1.0f, 0.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
			const int foreArmHid = isLeft ? 15 : 16;

			Quaternion qForeArmWorld = curActorRot;
			if (foreArmHid < 64 && writtenWorldRotValid[foreArmHid])
			{
				qForeArmWorld = writtenWorldRots[foreArmHid];
			}
			else
			{
				void* foreArmTrans = isLeft ? b.leftArmAux.foreArm : b.rightArmAux.foreArm;
				if (foreArmTrans && fnGetWorldRotation)
				{
					float r[4]{0.0f, 0.0f, 0.0f, 1.0f};
					fnGetWorldRotation(foreArmTrans, r);
					qForeArmWorld = Quaternion(r[0], r[1], r[2], r[3]);
				}
				else
				{
					qForeArmWorld = curActorRot * (isLeft
						                               ? b.leftArmAux.foreArmRestRel
						                               : b.rightArmAux.foreArmRestRel);
				}
			}
			const Quaternion qForeArmRestWorld = curActorRot * (isLeft
				                                                    ? b.leftArmAux.foreArmRestRel
				                                                    : b.rightArmAux.foreArmRestRel);
			const Quaternion qHandRestWorld = curActorRot * rRestRel;

			// 纯局部空间坐标变换：
			const Quaternion qLocalRest = qForeArmRestWorld.Inverse() * qHandRestWorld;
			const Quaternion qLocalCur = qForeArmWorld.Inverse() * rTarget;
			const Quaternion dQ_local = qLocalRest.Inverse() * qLocalCur;

			QuartzDriverAuxResult aux = SolveQuartzAuxiliaryBones(dQ_local, uLocal, false);

			// 腕关节半角支撑骨 (LeftHand_H / RightHand_H): 承载 50% Bend, 0% Roll
			auxJointH = isLeft ? b.leftArmAux.handH : b.rightArmAux.handH;
			auxJointHRestLocal = isLeft ? &b.leftArmAux.handHRestLocal : &b.rightArmAux.handHRestLocal;
			if (auxJointH)
			{
				qJointHWorld = qForeArmWorld * (qLocalRest * aux.qJointHLocal);
				qParentForLocal = qForeArmWorld; // LeftHand_H 是 LeftForeArm 的子节点
			}
		}


		// ------------------------------------------------------------------
		// 辅助骨写入闸门（实机靶向对照实验核心）
		// ------------------------------------------------------------------
		// kAuxNormal   : 原路径，逐帧注入 Quartz 解算出的半角 / 半扭转
		// kAuxSilence  : 硬锁初始绑定局部姿态 -> 辅助骨变成父骨的刚性延伸，零贡献
		// kAuxIdentity : 局部旋转强制 Identity
		// kAuxReplay   : 真值直灌（由 AuxGroundTruth 提供，缺失数据时自动降级为 Silence）
		// 判定含义：若切到 Silence/Identity 后「细腰波谷」完全消失 =>
		//          塌陷 100% 由现行注入公式造成（公式错）；
		//          若塌陷纹丝不变 => 塌陷与辅助骨无关，须排查 Sleeve 骨 / Position 位移 / 材质形变。
		// ------------------------------------------------------------------
		const Quaternion kIdentityForAux = Quaternion::Identity();
		if (auxRollH && !auxRollHRestLocal) auxRollHRestLocal = &kIdentityForAux;
		if (auxJointH && !auxJointHRestLocal) auxJointHRestLocal = &kIdentityForAux;

		if (auxDriveMode == kAuxNormal) return; // 模式 0 由帧末统一模型后处理接管

		for (int pass = 0; pass < 2; ++pass)
		{
			void* auxTrans = (pass == 0) ? auxRollH : auxJointH;
			if (!auxTrans) continue;

			Quaternion qRun;
			if (auxDriveMode == kAuxLegacy)
			{
				qRun = (pass == 0) ? qAuxRollHWorld : qJointHWorld;
			}
			else if (auxDriveMode == kAuxReplay)
			{
				const Quaternion* qRecorded = LookupAuxReplay(hid, pass == 0, currentTimeSeconds);
				if (qRecorded)
				{
					qRun = *qRecorded; // 官方逐帧真值（局部空间）
				}
				else
				{
					qRun = (pass == 0) ? *auxRollHRestLocal : *auxJointHRestLocal; // 无数据 -> 降级为静默
				}
			}
			else if (auxDriveMode == kAuxIdentity)
			{
				qRun = Quaternion::Identity();
			}
			else
			{
				qRun = (pass == 0) ? *auxRollHRestLocal : *auxJointHRestLocal;
			}

			if (auxDriveMode == kAuxLegacy)
			{
				// 旧契约：世界空间直写（仅保留作对照 —— 已确认这正是「细腰波谷」的直接成因）
				if (fnSetWorldRotation)
				{
					fnSetWorldRotation(auxTrans, qRun.x, qRun.y, qRun.z, qRun.w);
				}
				else if (fnSetLocalRotation)
				{
					const Quaternion qLocal = qParentForLocal.Inverse() * qRun;
					fnSetLocalRotation(auxTrans, qLocal.x, qLocal.y, qLocal.z, qLocal.w);
				}
			}
			else if (fnSetLocalRotation)
			{
				// 静默 / 归零 / 真值：一律局部空间硬写，语义最干净
				fnSetLocalRotation(auxTrans, qRun.x, qRun.y, qRun.z, qRun.w);
			}
		}
	}

	inline void IdolController::ApplyPlacementYawToBinding(IdolBoneBinding& b, const Vector3& curActorPos,
	                                                        const Vector3& worldOffset, bool clipWritesRoot) const
	{
		// Placement is one rigid world-Y rotation of the completed Hips subtree.
		// Child locals, auxiliary output and face pose remain mutually coherent.
		// Rotate around the original actor pivot, then retain the world-axis XYZ
		// translation. Never rotate the official actor/layout transform itself.
		if (placementYawDegrees != 0.0f && b.rootTransform && fnGetWorldRotation &&
			fnGetWorldPosition && fnSetWorldPosition &&
			(fnSetWorldRotation || (fnSetLocalRotation && fnGetTransformParent)) &&
			(clipWritesRoot || b.placementRestRootCaptured))
		{
			float p[3]{}, r[4]{0, 0, 0, 1};
			fnGetWorldPosition(b.rootTransform, p); fnGetWorldRotation(b.rootTransform, r);
			const auto yaw = Quaternion::FromEulerDegreesXYZ(0, placementYawDegrees, 0);
			const auto pivot = curActorPos + worldOffset;
			const auto target = pivot + yaw.Rotate(Vector3(p[0], p[1], p[2]) - pivot);
			const auto rotation = (yaw * Quaternion(r[0], r[1], r[2], r[3])).Normalized();
			if (fnSetLocalRotation && fnGetTransformParent)
			{
				Quaternion parentRotation = Quaternion::Identity();
				if (void* parent = fnGetTransformParent(b.rootTransform))
				{
					float q[4]{0, 0, 0, 1}; fnGetWorldRotation(parent, q);
					parentRotation = Quaternion(q[0], q[1], q[2], q[3]);
				}
				const auto local = (parentRotation.Inverse() * rotation).Normalized();
				fnSetLocalRotation(b.rootTransform, local.x, local.y, local.z, local.w);
			}
			else fnSetWorldRotation(b.rootTransform, rotation.x, rotation.y, rotation.z, rotation.w);
			fnSetWorldPosition(b.rootTransform, target.x, target.y, target.z);
			b.placementYawApplied = true;
		}
	}
} // namespace GakuMotion
