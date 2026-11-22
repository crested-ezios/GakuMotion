#pragma once

#include "../GkMotionData.hpp"
#include <algorithm>
#include <cmath>

namespace GakuMotion
{
	// =========================================================================
	// 官方 QuartzDriver 原生骨骼解算与保角共形数学变换引擎 (Phase 3 源码级移植)
	// 逆向自: ActorAnimation.ActorAnimationMathUtility & ActorAnimationQuartzDriver
	// 特性: 100% 纯静态内联 C++ 数学实现，零虚表调用、零游戏内部指针依赖
	// =========================================================================
	struct OfficialQuartzMath
	{
		static constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
		static constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;

		// 1. FormatRangeDegree (RVA 0x9b02d0): 官方角度折叠收敛至 [-180, +180] 区间
		static inline float FormatRangeDegree(float deg) noexcept
		{
			if (deg > 180.0f) return deg - 360.0f;
			if (deg < -180.0f) return deg + 360.0f;
			return deg;
		}

		// 2. SingleCoefficientConversion (RVA 0x9b0760): 官方单轴限位与系数缩放映射
		static inline float SingleCoefficientConversion(float angle, float coef, float minVal = -180.0f,
		                                                float maxVal = 180.0f) noexcept
		{
			float clamped = angle;
			if (clamped < minVal) clamped = minVal;
			if (clamped > maxVal) clamped = maxVal;
			return clamped * coef;
		}

		// 3. FromToRotation (RVA 0x9b0300): 官方向量正交基旋转四元数求取（零扭转测地大圆 Swing）
		static inline Quaternion FromToRotation(const Vector3& from, const Vector3& to) noexcept
		{
			float lenFrom = std::sqrt(from.x * from.x + from.y * from.y + from.z * from.z);
			float lenTo = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
			if (lenFrom <= 1e-6f || lenTo <= 1e-6f) return Quaternion::Identity();

			Vector3 v0(from.x / lenFrom, from.y / lenFrom, from.z / lenFrom);
			Vector3 v1(to.x / lenTo, to.y / lenTo, to.z / lenTo);

			float d = v0.x * v1.x + v0.y * v1.y + v0.z * v1.z;
			if (d >= 0.999999f)
			{
				return Quaternion::Identity();
			}
			if (d <= -0.999999f)
			{
				Vector3 ortho = std::abs(v0.x) > 0.5f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
				Vector3 axis(
					v0.y * ortho.z - v0.z * ortho.y,
					v0.z * ortho.x - v0.x * ortho.z,
					v0.x * ortho.y - v0.y * ortho.x
				);
				float axisLen = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
				if (axisLen > 1e-6f)
				{
					axis.x /= axisLen;
					axis.y /= axisLen;
					axis.z /= axisLen;
				}
				return Quaternion(axis.x, axis.y, axis.z, 0.0f);
			}

			Vector3 c(
				v0.y * v1.z - v0.z * v1.y,
				v0.z * v1.x - v0.x * v1.z,
				v0.x * v1.y - v0.y * v1.x
			);
			Quaternion q(c.x, c.y, c.z, 1.0f + d);
			return q.Normalized();
		}

		// 4. BendRoll 解耦结果
		struct BendRollResult
		{
			float rollAngleDeg{0.0f}; // 沿主骨轴的纯 Roll 角（度）
			float bendYDeg{0.0f}; // 侧摆角 Y（度）
			float bendZDeg{0.0f}; // 侧摆角 Z（度）
			Quaternion qBend; // 纯保角侧摆（Swing）四元数
			Quaternion qRoll; // 纯轴向扭转（Roll）四元数
		};

		// 5. EulerToBendroll (RVA 0x9affd0): 官方保角共形投影与轴向分解核心函数
		// 沿骨轴方向向量 uLocal，将旋转解算为零扭转侧摆 qBend 与纯轴向扭转 qRoll
		static inline BendRollResult EulerToBendroll(
			const Quaternion& q,
			const Vector3& boneAxis) noexcept
		{
			BendRollResult res;
			float axisLen = std::sqrt(boneAxis.x * boneAxis.x + boneAxis.y * boneAxis.y + boneAxis.z * boneAxis.z);
			Vector3 u = (axisLen > 1e-6f)
				            ? Vector3(boneAxis.x / axisLen, boneAxis.y / axisLen, boneAxis.z / axisLen)
				            : Vector3(1.0f, 0.0f, 0.0f);

			// 1. 将骨轴经 q 旋转得到目标朝向矢量 V
			Vector3 V = q.Rotate(u);

			// 2. 官方 FromToRotation: 计算零扭转测地大圆侧摆四元数 qBend
			res.qBend = FromToRotation(u, V);

			// 3. 从总旋转剥离出纯轴向扭转: q = qBend * qRoll => qRoll = qBend^-1 * q
			res.qRoll = (res.qBend.Inverse() * q).Normalized();

			// 消除 4D 球面最短路径符号翻转
			if (res.qRoll.w < 0.0f)
			{
				res.qRoll = Quaternion(-res.qRoll.x, -res.qRoll.y, -res.qRoll.z, -res.qRoll.w);
			}

			// 4. 计算纯 Roll 角（沿骨轴投影）并经 FormatRangeDegree 约束
			float rollProj = res.qRoll.x * u.x + res.qRoll.y * u.y + res.qRoll.z * u.z;
			float rawRollAngle = 2.0f * std::atan2(rollProj, res.qRoll.w) * kRad2Deg;
			res.rollAngleDeg = FormatRangeDegree(rawRollAngle);

			// 5. 官方保角共形投影侧摆角: tan(theta/2) = V_lat / (V_ax + 1)
			float uDotV = u.x * V.x + u.y * V.y + u.z * V.z;
			float denom = std::max(0.0001f, uDotV + 1.0f);
			Vector3 lat(V.x - u.x * uDotV, V.y - u.y * uDotV, V.z - u.z * uDotV);
			res.bendYDeg = FormatRangeDegree(2.0f * std::atan2(lat.y, denom) * kRad2Deg);
			res.bendZDeg = FormatRangeDegree(2.0f * std::atan2(lat.z, denom) * kRad2Deg);

			return res;
		}

		// 6. AxisrotationToQuaternionWithBendRoll (RVA 0x9ae2f0): 官方正交重构公式
		// 将纯侧摆四元数 qBend 与经过系数换算后的 Roll 角度合成最终四元数
		static inline Quaternion AxisrotationToQuaternionWithBendRoll(
			const Quaternion& qBend,
			float rollAngleDeg,
			const Vector3& boneAxis) noexcept
		{
			float axisLen = std::sqrt(boneAxis.x * boneAxis.x + boneAxis.y * boneAxis.y + boneAxis.z * boneAxis.z);
			Vector3 u = (axisLen > 1e-6f)
				            ? Vector3(boneAxis.x / axisLen, boneAxis.y / axisLen, boneAxis.z / axisLen)
				            : Vector3(1.0f, 0.0f, 0.0f);

			float halfRad = (rollAngleDeg * 0.5f) * kDeg2Rad;
			float sinH = std::sin(halfRad);
			float cosH = std::cos(halfRad);
			Quaternion qRoll(u.x * sinH, u.y * sinH, u.z * sinH, cosH);
			return (qBend * qRoll).Normalized();
		}

		// 7. AxisrotationToQuaternionWithBendAngles (RVA 0x9ae2f0 纯角度闭环版):
		// 官方反向立体保角共形投影: 从 (bendYDeg, bendZDeg) 与 rollAngleDeg 纯角度直接重构四元数 (100% 零Slerp)
		static inline Quaternion AxisrotationToQuaternionWithBendAngles(
			float bendYDeg,
			float bendZDeg,
			float rollAngleDeg,
			const Vector3& boneAxis) noexcept
		{
			float axisLen = std::sqrt(boneAxis.x * boneAxis.x + boneAxis.y * boneAxis.y + boneAxis.z * boneAxis.z);
			Vector3 u = (axisLen > 1e-6f)
				            ? Vector3(boneAxis.x / axisLen, boneAxis.y / axisLen, boneAxis.z / axisLen)
				            : Vector3(1.0f, 0.0f, 0.0f);

			float halfRadY = (bendYDeg * 0.5f) * kDeg2Rad;
			float halfRadZ = (bendZDeg * 0.5f) * kDeg2Rad;
			float tY = std::tan(halfRadY);
			float tZ = std::tan(halfRadZ);
			float sq = tY * tY + tZ * tZ;
			float denom = 1.0f + sq;

			Vector3 V(
				(u.x < 0.0f) ? -(1.0f - sq) / denom : (1.0f - sq) / denom,
				(2.0f * tY) / denom,
				(2.0f * tZ) / denom
			);

			Quaternion qBend = FromToRotation(u, V);

			float halfRollRad = (rollAngleDeg * 0.5f) * kDeg2Rad;
			float sinH = std::sin(halfRollRad);
			float cosH = std::cos(halfRollRad);
			Quaternion qRoll(u.x * sinH, u.y * sinH, u.z * sinH, cosH);

			return (qBend * qRoll).Normalized();
		}
	};

	// -------------------------------------------------------------------------
	// 官方原生 QuartzDriver 局部保角解算器 (Pure Conformal Local Solver)
	// 1. 严格在局部关节增量空间 (dQ_local) 中求取纯净保角分解
	// 2. 官方 EulerToBendroll 分离纯轴向扭转 (Roll) 与保角侧摆角 (BendY, BendZ)
	// 3. 严格禁止 4D 四元数 Slerp，全链路采用 SingleCoefficientConversion 线性保角缩放
	// 4. AxisrotationToQuaternionWithBendAngles 正交合成最终纯净局部增量
	// -------------------------------------------------------------------------
	struct QuartzDriverAuxResult
	{
		Quaternion qMainLocal; // 主骨骼局部增量（100% 原始动力学，保持肘关节与前臂铰链朝向原态）
		Quaternion qRollHLocal; // _Roll_H 局部相对姿态（100% Bend + 50% Roll，用于消除糖果纸扭转）
		Quaternion qJointHLocal; // _H 关节半角支撑骨局部增量（50% Bend + 0% Roll）
	};

	inline QuartzDriverAuxResult SolveQuartzAuxiliaryBones(
		const Quaternion& dQ_local,
		const Vector3& boneAxis,
		bool hasRollH) noexcept
	{
		QuartzDriverAuxResult res;
		OfficialQuartzMath::BendRollResult bendRoll = OfficialQuartzMath::EulerToBendroll(dQ_local, boneAxis);

		// 1. _H 纯弯曲半角支撑骨：0% Roll, 50% Bend Y, 50% Bend Z
		float halfBendY = OfficialQuartzMath::SingleCoefficientConversion(bendRoll.bendYDeg, 0.5f);
		float halfBendZ = OfficialQuartzMath::SingleCoefficientConversion(bendRoll.bendZDeg, 0.5f);
		res.qJointHLocal = OfficialQuartzMath::AxisrotationToQuaternionWithBendAngles(
			halfBendY, halfBendZ, 0.0f, boneAxis);

		// 2. _Roll_H 轴向扭转分流骨：100% Bend Y, 100% Bend Z, 50% Roll
		if (hasRollH)
		{
			float halfRollDeg = OfficialQuartzMath::SingleCoefficientConversion(bendRoll.rollAngleDeg, 0.5f);
			res.qRollHLocal = OfficialQuartzMath::AxisrotationToQuaternionWithBendAngles(
				bendRoll.bendYDeg, bendRoll.bendZDeg, halfRollDeg, boneAxis);
		}
		else
		{
			res.qRollHLocal = Quaternion::Identity();
		}

		// 3. 主骨骼承担 100% 全量位姿（100% Bend + 100% Roll）
		res.qMainLocal = dQ_local;

		return res;
	}
} // namespace GakuMotion
