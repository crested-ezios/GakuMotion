#pragma once

#include "GkShoeMeshFrames.hpp"

namespace ShoeMeshProbe
{
	inline Json Renderer(void* renderer, const Foot* const* feet, int& processed, int& candidates, Stage stage)
	{
		Json j = Object();
		Set(j, "name", Text(Name(renderer)));
		Set(j, "renderer_pointer", Text(PointerText(renderer)));
		std::string error;
		bool enabled = false, active = false;
		const bool enabledKnown = Primitive(getEnabled, renderer, 0x02, &enabled, error);
		Set(j, "enabled_known", Flag(enabledKnown));
		Set(j, "enabled", enabledKnown ? Flag(enabled) : Json{});
		Set(j, "enabled_error", Text(error));
		error.clear();
		Pin gameObject(Invoke(getGameObject, renderer, nullptr, error));
		const bool activeKnown = gameObject.Get() && Primitive(getActive, gameObject.Get(), 0x02, &active, error);
		Set(j, "active_in_hierarchy_known", Flag(activeKnown));
		Set(j, "active_in_hierarchy", activeKnown ? Flag(active) : Json{});
		Set(j, "active_error", Text(error));
		error.clear();
		Pin shared(Invoke(getSharedMesh, renderer, nullptr, error));
		Set(j, "shared_mesh_error", Text(error));
		if (!shared.Get())
		{
			Set(j, "status", Text("shared_mesh_unavailable"));
			return j;
		}
		Set(j, "shared_mesh_name", Text(Name(shared.Get())));
		Set(j, "shared_mesh_pointer", Text(PointerText(shared.Get())));
		int32_t count = 0;
		bool readable = false;
		if (!Primitive(getVertexCount, shared.Get(), 0x08, &count, error))
		{
			Set(j, "status", Text("vertex_count_failed"));
			Set(j, "error", Text(error));
			return j;
		}
		Set(j, "vertex_count", Number(count));
		error.clear();
		const bool readableKnown = Primitive(getReadable, shared.Get(), 0x02, &readable, error);
		Set(j, "shared_mesh_readable_known", Flag(readableKnown));
		Set(j, "shared_mesh_is_readable", readableKnown ? Flag(readable) : Json{});
		Set(j, "readable_error", Text(error));
		error.clear();
		Pin bones(Invoke(getBones, renderer, nullptr, error));
		Json names = Array();
		bool hasCurrentFoot = false;
		if (bones.Get())
		{
			Set(j, "bones_array", ArrayInfo(bones.Get()));
			const uintptr_t boneCount = ArrayLength(bones.Get());
			if (boneCount > 512)
			{
				Set(j, "status", Text("bone_count_limit"));
				return j;
			}
			for (int b = 0; b < static_cast<int>(boneCount); ++b)
			{
				Pin bone(Element(bones.Get(), b, error));
				if (!error.empty())break;
				Json item = Object();
				Set(item, "index", Number(b));
				Set(item, "name", Text(Name(bone.Get())));
				Json matches = Array();
				for (int f = 0; f < 4; ++f)if (bone.Get() && bone.Get() == feet[f]->bone.Get())
				{
					Push(matches, Number(f));
					hasCurrentFoot = true;
				}
				Set(item, "current_humanoid_matches", std::move(matches));
				Push(names, std::move(item));
			}
		}
		Set(j, "bones", std::move(names));
		Set(j, "bones_error", Text(error));
		Set(j, "has_current_foot_or_toe_bone", Flag(hasCurrentFoot));
		Set(j, "bone_weights",
		    Text(
			    "not attempted in this minimal probe; no inferred weights, bind-pose vertices, or semantic shoe labels"));
		if (!enabledKnown || !activeKnown || !enabled || !active)
		{
			Set(j, "status", Text("renderer_not_confirmed_current_active"));
			return j;
		}
		if (!hasCurrentFoot)
		{
			Set(j, "status", Text("no_current_foot_toe_bone_match"));
			return j;
		}
		if (stage != Stage::Vertices)
		{
			Set(j, "status", Text("active_renderer_current_foot_toe_observed"));
			Set(j, "vertex_collection_attempted", Flag(false));
			return j;
		}
		if (count <= 0 || count > kMeshVertexLimit || processed + count > kTotalVertexLimit)
		{
			Set(j, "status", Text("vertex_budget_limit"));
			return j;
		}
		error.clear();
		Pin transform(Invoke(getTransform, renderer, nullptr, error));
		Matrix toWorld, toLocal;
		if (!transform.Get() || !ReadMatrix(getLocalToWorld, transform.Get(), toWorld, error) || !ReadMatrix(
			getWorldToLocal, transform.Get(), toLocal, error) || !InversePair(toWorld, toLocal))
		{
			Set(j, "status", Text("renderer_matrix_failed"));
			Set(j, "error", Text(error.empty() ? "renderer matrix inverse check failed" : error));
			return j;
		}
		Set(j, "renderer_local_to_world", MatrixJson(toWorld));
		Set(j, "renderer_world_to_local", MatrixJson(toLocal));
		Pin scaleBox(Invoke(getLossyScale, transform.Get(), nullptr, error));
		UnityVector3 scale{};
		const bool scaleKnown = error.empty() && ReadVector(scaleBox.Get(), scale, error);
		Set(j, "renderer_lossy_scale", scaleKnown ? Vector(scale) : Json{});
		Set(j, "renderer_scale_error", Text(error));
		const bool unitScale = scaleKnown && std::fabs(scale.x - 1) < 0.001f && std::fabs(scale.y - 1) < 0.001f &&
			std::fabs(scale.z - 1) < 0.001f;
		Set(j, "bake_scale_conversion_verified", Flag(unitScale));
		Set(j, "scale_interpretation", Text(unitScale
			                                    ? "unit renderer scale; complete renderer matrix used"
			                                    : "non-unit/unknown renderer scale: useScale=false compensation not established; preserve raw baked vertices and matrices as evidence, world/Foot-local candidates are provisional"));
		Json points = Array();
		bool bakeSucceeded = false;
		int32_t bakedCount = 0;
		{
			const std::string rendererPhase = tracePhase;
			if (!Progress(rendererPhase + ".scratch_mesh_allocate"))
			{
				Set(j, "status", Text("trace_write_failed"));
				return j;
			}
			Scratch scratch(TraceFirst("void* il2cpp_object_new(UnityEngine.Mesh class)")
				                ? api.objectNew(meshClass)
				                : nullptr, j);
			if (!scratch.mesh.Get())
			{
				Set(j, "status", Text("scratch_allocation_failed"));
				return j;
			}
			error.clear();
			Invoke(meshCtor, scratch.mesh.Get(), nullptr, error);
			if (!error.empty())
			{
				Set(j, "status", Text("scratch_ctor_failed"));
				Set(j, "error", Text(error));
				scratch.Close();
				return j;
			}
			bool useScale = false;
			void* args[] = {scratch.mesh.Get(), &useScale};
			if (!Progress(rendererPhase + ".bake_mesh"))
			{
				Set(j, "status", Text("trace_write_failed"));
				scratch.Close();
				return j;
			}
			Invoke(bakeMesh, renderer, args, error);
			bakeSucceeded = error.empty();
			Set(j, "bake_use_scale", Flag(useScale));
			Set(j, "bake_error", Text(error));
			Set(j, "bake_succeeded", Flag(bakeSucceeded));
			if (!bakeSucceeded)
			{
				Set(j, "status", Text("bake_failed"));
				scratch.Close();
				return j;
			}
			if (!Primitive(getVertexCount, scratch.mesh.Get(), 0x08, &bakedCount, error) || bakedCount != count)
			{
				Set(j, "status", Text("baked_vertex_count_mismatch"));
				Set(j, "baked_vertex_count", Number(bakedCount));
				Set(j, "error", Text(error));
				scratch.Close();
				return j;
			}
			if (!Progress(rendererPhase + ".vertices_read"))
			{
				Set(j, "status", Text("trace_write_failed"));
				scratch.Close();
				return j;
			}
			Pin vertices(Invoke(getVertices, scratch.mesh.Get(), nullptr, error));
			if (!vertices.Get())
			{
				Set(j, "status", Text("baked_vertices_unavailable"));
				Set(j, "error", Text(error));
				scratch.Close();
				return j;
			}
			Set(j, "vertices_array", ArrayInfo(vertices.Get()));
			const uintptr_t length = ArrayLength(vertices.Get());
			if (length != static_cast<uintptr_t>(bakedCount))
			{
				Set(j, "status", Text("baked_array_count_mismatch"));
				scratch.Close();
				return j;
			}
			if (api.arrayElementSize && TraceFirst("uint32_t il2cpp_array_element_size(Il2CppClass*)") && api.
				arrayElementSize(ObjectClass(vertices.Get())) != sizeof(UnityVector3))
			{
				Set(j, "status", Text("vector_array_element_size_mismatch"));
				scratch.Close();
				return j;
			}
			processed += bakedCount;
			int inspected = 0;
			bool conversionFinite = true;
			UnityVector3 lower{}, upper{};
			Json rawSamples = Array(), allVertices = Array();
			std::vector<int> sampleIndices;
			for (int i = 0; i < (std::min)(16, bakedCount); ++i)sampleIndices.push_back(i);
			for (int k = 0; k < 16; ++k)
			{
				const int index = (bakedCount - 1) * k / 15;
				if (std::find(sampleIndices.begin(), sampleIndices.end(), index) == sampleIndices.end())sampleIndices.
					push_back(index);
			}
			for (int i = 0; i < bakedCount; ++i)
			{
				Pin boxed(Element(vertices.Get(), i, error));
				UnityVector3 local{};
				if (!error.empty() || !ReadVector(boxed.Get(), local, error))break;
				++inspected;
				// Complete index-aligned evidence, independent of changing radius
				// membership. Foot/Toe coordinates are reconstructed offline.
				Json vertex = Object();
				Set(vertex, "vertex_index", Number(i));
				Set(vertex, "renderer_local", Vector(local));
				Push(allVertices, std::move(vertex));
				if (inspected == 1)lower = upper = local;
				else
				{
					lower.x = (std::min)(lower.x, local.x);
					lower.y = (std::min)(lower.y, local.y);
					lower.z = (std::min)(lower.z, local.z);
					upper.x = (std::max)(upper.x, local.x);
					upper.y = (std::max)(upper.y, local.y);
					upper.z = (std::max)(upper.z, local.z);
				}
				if (std::find(sampleIndices.begin(), sampleIndices.end(), i) != sampleIndices.end())
				{
					Json raw = Object();
					Set(raw, "vertex_index", Number(i));
					Set(raw, "renderer_local", Vector(local));
					Push(rawSamples, std::move(raw));
				}
				const UnityVector3 world = Point(toWorld, local);
				if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z))
				{
					conversionFinite = false;
					continue;
				}
				Json neighborhoods = Array();
				for (int side = 0; side < 2; ++side)
				{
					const Foot& foot = *feet[side];
					const Foot& toe = *feet[side + 2];
					const float footDistance = foot.valid ? Distance(world, foot.origin) : 1e9f;
					const float toeDistance = toe.valid ? Distance(world, toe.origin) : 1e9f;
					if ((std::min)(footDistance, toeDistance) > kNeighborhoodRadius)continue;
					Json n = Object();
					Set(n, "side", Text(side == 0 ? "left" : "right"));
					if (foot.valid)
					{
						Set(n, "foot_local", Vector(Point(foot.toLocal, world)));
						Set(n, "foot_distance_world_m", Number(footDistance));
					}
					if (toe.valid)
					{
						Set(n, "toe_local", Vector(Point(toe.toLocal, world)));
						Set(n, "toe_distance_world_m", Number(toeDistance));
					}
					Push(neighborhoods, std::move(n));
				}
				if (neighborhoods.arrayItems.empty())continue;
				if (candidates >= kCandidateLimit)
				{
					Set(j, "candidate_limit_reached", Flag(true));
					continue;
				}
				Json p = Object();
				Set(p, "vertex_index", Number(i));
				Set(p, "renderer_local", Vector(local));
				Set(p, "world", Vector(world));
				Set(p, "neighborhoods", std::move(neighborhoods));
				Push(points, std::move(p));
				++candidates;
			}
			Set(j, "inspected_vertices", Number(inspected));
			Set(j, "vertex_read_error", Text(error));
			Set(j, "finite_renderer_local_vertices", Number(inspected));
			Set(j, "baked_vertex_read_complete", Flag(inspected == bakedCount));
			Set(j, "raw_baked_samples", std::move(rawSamples));
			Set(j, "baked_vertices", std::move(allVertices));
			Set(j, "vertex_index_basis",
			    Text(
				    "unchanged sharedMesh object and vertex count; index in each complete BakeMesh vertices array; no topology writes by this collector"));
			Json envelope = Object();
			if (inspected)
			{
				Set(envelope, "min", Vector(lower));
				Set(envelope, "max", Vector(upper));
			}
			Set(j, "renderer_local_aabb", std::move(envelope));
			Set(j, "world_conversion_finite", Flag(conversionFinite));
			Set(j, "foot_world_conversion_reliable",
			    Flag(unitScale && conversionFinite && feet[0]->valid && feet[1]->valid && feet[2]->valid && feet[3]->
				    valid));
			Set(j, "status", Text(inspected == bakedCount ? "baked_vertices_read" : "partial_vertex_read_failure"));
			Progress(rendererPhase + ".scratch_mesh_destroy");
			scratch.Close();
		}
		if (!Progress(tracePhase + ".renderer_readback"))
		{
			Set(j, "renderer_frames_unchanged", Flag(false));
			Set(j, "renderer_readback_error", Text("trace_write_failed"));
		}
		else
		{
			Matrix afterWorld, afterLocal;
			error.clear();
			const bool read = ReadMatrix(getLocalToWorld, transform.Get(), afterWorld, error) && ReadMatrix(
				getWorldToLocal, transform.Get(), afterLocal, error) && InversePair(afterWorld, afterLocal);
			double delta = 0;
			if (read)for (int k = 0; k < 16; ++k)
			{
				delta = (std::max)(delta, std::fabs(static_cast<double>(toWorld.m[k]) - afterWorld.m[k]));
				delta = (std::max)(delta, std::fabs(static_cast<double>(toLocal.m[k]) - afterLocal.m[k]));
			}
			Set(j, "renderer_frames_unchanged", Flag(read && delta <= 1e-5));
			Set(j, "renderer_max_abs_matrix_delta", read ? Number(delta) : Json{});
			Set(j, "renderer_readback_error", Text(error));
			if (read)
			{
				Set(j, "renderer_local_to_world_after", MatrixJson(afterWorld));
				Set(j, "renderer_world_to_local_after", MatrixJson(afterLocal));
			}
			error.clear();
			Pin afterMesh(Invoke(getSharedMesh, renderer, nullptr, error));
			Set(j, "shared_mesh_identity_unchanged", Flag(error.empty() && afterMesh.Get() == shared.Get()));
			Set(j, "shared_mesh_readback_error", Text(error));
		}
		Set(j, "candidates", std::move(points));
		return j;
	}
} // namespace ShoeMeshProbe
