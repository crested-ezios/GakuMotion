#pragma once

#include "GkShoeMeshApi.hpp"

namespace ShoeMeshProbe
{
	struct Matrix
	{
		float m[16]{};
	};

	inline Json Vector(const UnityVector3& v)
	{
		Json j = Array();
		Push(j, Number(v.x));
		Push(j, Number(v.y));
		Push(j, Number(v.z));
		return j;
	}

	inline Json MatrixJson(const Matrix& m)
	{
		Json j = Array();
		for (float v : m.m)Push(j, Number(v));
		return j;
	}

	inline bool ReadMatrix(Method method, void* transform, Matrix& out, std::string& error)
	{
		Pin boxed(Invoke(method, transform, nullptr, error));
		if (!boxed.Get())return false;
		if (ObjectClass(boxed.Get()) != matrixClass)
		{
			error = "Matrix4x4 boxed class mismatch";
			return false;
		}
		if (!TraceFirst("void il2cpp_field_get_value(boxed Matrix4x4, field, float*)"))
		{
			error = "trace persistence failed";
			return false;
		}
		for (int i = 0; i < 16; ++i)
		{
			g_faceRuntime.readField(boxed.Get(), matrixFields[i], &out.m[i]);
			if (!std::isfinite(out.m[i]))
			{
				error = "nonfinite transform matrix";
				return false;
			}
		}
		if (std::fabs(out.m[12]) > 1e-4f || std::fabs(out.m[13]) > 1e-4f || std::fabs(out.m[14]) > 1e-4f ||
			std::fabs(out.m[15] - 1) > 1e-4f)
		{
			error = "nonaffine transform matrix";
			return false;
		}
		return true;
	}

	inline bool InversePair(const Matrix& a, const Matrix& b)
	{
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c)
			{
				double v = 0;
				for (int k = 0; k < 4; ++k)v += a.m[r * 4 + k] * b.m[k * 4 + c];
				if (std::fabs(v - (r == c ? 1 : 0)) > 3e-3)return false;
			}
		return true;
	}

	inline UnityVector3 Point(const Matrix& m, const UnityVector3& p)
	{
		return {
			m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3], m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7],
			m.m[8] * p.x + m.m[9] * p.y + m.m[10] * p.z + m.m[11]
		};
	}

	inline float Distance(const UnityVector3& a, const UnityVector3& b)
	{
		const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
		return std::sqrt(x * x + y * y + z * z);
	}

	inline bool ReadVector(void* boxed, UnityVector3& value, std::string& error)
	{
		if (!boxed || ObjectClass(boxed) != vectorClass)
		{
			error = "Vector3 boxed class mismatch";
			return false;
		}
		if (!TraceFirst("void il2cpp_field_get_value(boxed Vector3, field, float*)"))
		{
			error = "trace persistence failed";
			return false;
		}
		g_faceRuntime.readField(boxed, vectorFields[0], &value.x);
		g_faceRuntime.readField(boxed, vectorFields[1], &value.y);
		g_faceRuntime.readField(boxed, vectorFields[2], &value.z);
		if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
		{
			error = "nonfinite baked vertex";
			return false;
		}
		return true;
	}

	struct Foot
	{
		Pin bone;
		Matrix toWorld, toLocal;
		UnityVector3 origin{}, worldPosition{};
		bool valid = false;
		std::string error;

		static void* CurrentBone(void* actor, int id)
		{
			if (!Progress("foot_toe_matrices.humanoid_" + std::to_string(id)))return nullptr;
			return TraceFirst("HelperGetBoneTransform(actor, humanoid_id) [existing validated helper]")
				       ? HelperGetBoneTransform(actor, id)
				       : nullptr;
		}

		Foot(void* actor, int id) : bone(CurrentBone(actor, id))
		{
			valid = bone.Get() && ReadMatrix(getLocalToWorld, bone.Get(), toWorld, error) && ReadMatrix(
				getWorldToLocal, bone.Get(), toLocal, error);
			if (valid && !InversePair(toWorld, toLocal))
			{
				valid = false;
				error = "bone matrix inverse check failed";
			}
			if (valid)
			{
				origin = Point(toWorld, {0, 0, 0});
				Pin position(Invoke(getWorldPosition, bone.Get(), nullptr, error));
				valid = error.empty() && ReadVector(position.Get(), worldPosition, error) && Distance(
					origin, worldPosition) < 0.001f;
				if (!valid && error.empty())error = "world position disagrees with matrix origin";
			}
			else if (error.empty())error = "current humanoid bone unavailable";
		}

		Json Evidence() const
		{
			Json j = Object();
			Set(j, "valid", Flag(valid));
			Set(j, "name", Text(Name(bone.Get())));
			Set(j, "bone_pointer", Text(PointerText(bone.Get())));
			Set(j, "error", Text(error));
			if (valid)
			{
				Set(j, "local_to_world", MatrixJson(toWorld));
				Set(j, "world_to_local", MatrixJson(toLocal));
				Set(j, "world_origin", Vector(origin));
				Set(j, "world_position_getter", Vector(worldPosition));
			}
			return j;
		}
	};

	inline void PoseReadback(Json& report, const Foot* const* feet)
	{
		Json frames = Array();
		bool allUnchanged = true;
		double maximum = 0;
		constexpr double tolerance = 1e-5;
		for (int i = 0; i < 4; ++i)
		{
			const Foot& before = *feet[i];
			Matrix afterWorld, afterLocal;
			std::string error;
			const bool read = before.valid && ReadMatrix(getLocalToWorld, before.bone.Get(), afterWorld, error) &&
				ReadMatrix(getWorldToLocal, before.bone.Get(), afterLocal, error) &&
				InversePair(afterWorld, afterLocal);
			Json frame = Object();
			Set(frame, "humanoid_frame_index", Number(i));
			Set(frame, "readback_valid", Flag(read));
			bool same = false;
			double delta = 0;
			if (read)
			{
				for (int k = 0; k < 16; ++k)
				{
					delta = (std::max)(delta, std::fabs(static_cast<double>(before.toWorld.m[k]) - afterWorld.m[k]));
					delta = (std::max)(delta, std::fabs(static_cast<double>(before.toLocal.m[k]) - afterLocal.m[k]));
				}
				same = delta <= tolerance;
				maximum = (std::max)(maximum, delta);
				Set(frame, "local_to_world_after", MatrixJson(afterWorld));
				Set(frame, "world_to_local_after", MatrixJson(afterLocal));
				Set(frame, "max_abs_matrix_delta", Number(delta));
			}
			if (!read && error.empty())error = before.valid
				                                   ? "matrix inverse readback failed"
				                                   : "before frame unavailable";
			Set(frame, "unchanged_within_tolerance", Flag(same));
			Set(frame, "error", Text(error));
			Push(frames, std::move(frame));
			allUnchanged = allUnchanged && same;
		}
		Set(report, "pose_readback", std::move(frames));
		Set(report, "pose_matrix_tolerance", Number(tolerance));
		Set(report, "max_abs_matrix_delta", Number(maximum));
		Set(report, "pose_frames_unchanged", Flag(allUnchanged));
		Set(report, "data_consistency", Text(allUnchanged
			                                     ? "four Foot/Toe matrices unchanged within tolerance across collection"
			                                     : "unverified: a Foot/Toe frame was unavailable, failed readback, or changed during collection"));
	}

	struct Scratch
	{
		Pin mesh;
		Json& evidence;
		bool closed = false;

		Scratch(void* object, Json& j) : mesh(object), evidence(j)
		{
		}

		void Close()
		{
			if (!closed && mesh.Get())
			{
				closed = true;
				std::string error;
				void* args[] = {mesh.Get()};
				Invoke(destroyObject, nullptr, args, error);
				Set(evidence, "scratch_destroy_error", Text(error));
				Set(evidence, "scratch_destroy_requested", Flag(error.empty()));
			}
		}

		~Scratch() { Close(); }
	};
} // namespace ShoeMeshProbe
