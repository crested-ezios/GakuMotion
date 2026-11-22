#pragma once

#include "GkShoeMeshExtract.hpp"

namespace ShoeMeshProbe
{
	// Cache only the small request JSON, not Actor/Transform/Renderer pointers. All
	// actors in a multi-actor frame must still get their own exact identity check.
	inline bool OfficialRequestReady(const std::string& directory, void* actor, SnapshotContext& context)
	{
		if (probeFaulted || directory.empty() || context.source != Source::OfficialLive)return false;
		const std::string path = directory + "/shoe_mesh_probe.request.json";
		const auto now = std::chrono::steady_clock::now();
		if (path != officialCachedPath || now >= officialNextPoll)
		{
			officialCachedPath = path;
			officialNextPoll = now + std::chrono::seconds(1);
			officialCachedRequest = Json{};
			std::error_code ec;
			const auto size = std::filesystem::file_size(GakuMotion::MotionPathFromUtf8(path), ec);
			std::string error;
			if (!ec && size > 0 && size <= 16384 && !
				GakuMotion::jsonlite::ParseFile(path, officialCachedRequest, error))officialCachedRequest = Json{};
		}
		std::string error;
		uint32_t seq = 0;
		Stage stage;
		if (!officialCachedRequest.IsObject() || !MatchRequest(officialCachedRequest, actor, &context, error) || !
			Sequence(officialCachedRequest, seq) || seq <= lastSeq || !ParseStage(officialCachedRequest, stage, error))
			return false;
		// A cached positive is only a cheap candidate. Re-read before the caller
		// may Resolve, so a removed/replaced/malformed request cannot enable it.
		Json current;
		std::error_code ec;
		const auto size = std::filesystem::file_size(GakuMotion::MotionPathFromUtf8(path), ec);
		if (ec || size == 0 || size > 16384 || !GakuMotion::jsonlite::ParseFile(path, current, error) || !current.
			IsObject() ||
			!MatchRequest(current, actor, &context, error) || !Sequence(current, seq) || seq <= lastSeq || !ParseStage(
				current, stage, error))
			return false;
		const Json* costume = current.Find("costume_id");
		context.costumeId = costume ? costume->stringValue : "";
		if (std::filesystem::exists(GakuMotion::MotionPathFromUtf8(directory + "/shoe_mesh_probe_" + std::to_string(seq) + ".json"), ec) || ec)return
			false;
		if (std::filesystem::exists(GakuMotion::MotionPathFromUtf8(directory + "/shoe_mesh_probe_" + std::to_string(seq) + ".trace.json"), ec) || ec)
			return false;
		return true;
	}

	inline Json Collect(void* actor, int frame, float clipTime, uint32_t seq, Stage stage = Stage::Handle,
	                    const SnapshotContext* context = nullptr)
	{
		Json report = Object();
		Set(report, "schema", Text("gakumotion-shoe-mesh-evidence-v2"));
		Set(report, "seq", Number(seq));
		Set(report, "stage", Text(StageName(stage)));
		EvidenceContext(report, clipTime, context);
		Set(report, "actor_pointer", Text(PointerText(actor)));
		Set(report, "unity_frame", Number(frame));
		Set(report, "api", apiEvidence);
		Json handleApi = Object();
		Set(handleApi, "new", Text("void* il2cpp_gchandle_new(void*, bool pinned=true)"));
		Set(handleApi, "get_target", Text("void* il2cpp_gchandle_get_target(void*)"));
		Set(handleApi, "free", Text("void il2cpp_gchandle_free(void*)"));
		Set(report, "handle_api", std::move(handleApi));
		Set(report, "collector_pose_setter_calls", Number(0));
		Set(report, "matrix_layout",
		    Text("row-major JSON, named boxed fields; complete affine transforms include scale"));
		Set(report, "vertex_source", Text(stage == Stage::Vertices
			                                  ? "temporary Mesh populated by SkinnedMeshRenderer.BakeMesh(mesh,false); never sharedMesh.vertices"
			                                  : "not attempted by this stage"));
		Set(report, "selection", Text(stage == Stage::Vertices
			                              ? "unlabelled actual baked vertices within 0.4 world metres of a current Foot or Toe origin; may include body/boot/sole geometry, not an authenticated shoe or sole segmentation"
			                              : "no vertex or shoe geometry evidence requested"));
		Set(report, "renderer_limit", Number(kRendererLimit));
		Set(report, "mesh_vertex_limit", Number(kMeshVertexLimit));
		Set(report, "total_vertex_limit", Number(kTotalVertexLimit));
		Set(report, "candidate_limit", Number(kCandidateLimit));
		if (!Progress("actor_handle_new"))
		{
			Set(report, "status", Text("trace_write_failed"));
			return report;
		}
		if (!actor || !api.handleNew || !api.handleTarget || !api.handleFree)
		{
			Set(report, "status", Text("actor_handle_api_unavailable"));
			return report;
		}
		Pin actorPin(actor);
		Json handle = Object();
		Set(handle, "storage_bytes", Number(sizeof(Handle)));
		Set(handle, "actor_pointer", Text(PointerText(actor)));
		Set(handle, "handle_pointer", Text(PointerText(actorPin.id)));
		Set(handle, "new_returned_nonnull", Flag(actorPin.id != nullptr));
		Progress("actor_handle_first_target");
		void* first = actorPin.Get();
		Set(handle, "first_target_matches_actor", Flag(first == actor));
		Progress("actor_handle_repeated_target");
		void* second = actorPin.Get();
		Set(handle, "repeated_target_matches_actor", Flag(second == actor));
		const bool identity = actorPin.id && first == actor && second == actor;
		if (stage == Stage::Handle || !identity)
		{
			Progress("actor_handle_free");
			const bool hadHandle = actorPin.id != nullptr;
			actorPin.Close();
			Set(handle, "free_completed", Flag(hadHandle));
			Set(report, "actor_handle_validation", std::move(handle));
			Set(report, "status",
			    Text(identity ? "actor_handle_identity_verified" : "actor_gc_handle_identity_failed"));
			Set(report, "renderer_collection_attempted", Flag(false));
			Set(report, "vertex_collection_attempted", Flag(false));
			return report;
		}
		Set(report, "actor_handle_validation", std::move(handle));
		if (!(stage == Stage::Vertices ? available : rendererAvailable))
		{
			Set(report, "status", Text("required_stage_runtime_path_unavailable"));
			return report;
		}
		if (!Progress("foot_toe_matrices"))
		{
			Set(report, "status", Text("trace_write_failed"));
			return report;
		}
		Set(report, "actor_name", Text(Name(actorPin.Get())));
		Foot left(actorPin.Get(), 5), right(actorPin.Get(), 6), leftToe(actorPin.Get(), 19), rightToe(
			     actorPin.Get(), 20);
		const Foot* feet[] = {&left, &right, &leftToe, &rightToe};
		Json bones = Object();
		Set(bones, "left_foot", left.Evidence());
		Set(bones, "right_foot", right.Evidence());
		Set(bones, "left_toe", leftToe.Evidence());
		Set(bones, "right_toe", rightToe.Evidence());
		Set(report, "humanoid_frames", std::move(bones));
		if (!left.valid && !right.valid)
		{
			Set(report, "status", Text("current_foot_frames_unavailable"));
			PoseReadback(report, feet);
			return report;
		}
		if (!Progress("renderer_type_handle"))
		{
			Set(report, "status", Text("trace_write_failed"));
			return report;
		}
		if (!rendererTypeHandle)
		{
			void* klassType = TraceFirst("Il2CppType* il2cpp_class_get_type(SkinnedMeshRenderer class)")
				                  ? api.classType(rendererClass)
				                  : nullptr;
			void* managedType = klassType && TraceFirst("System.Type* il2cpp_type_get_object(Il2CppType*)")
				                    ? api.typeObject(klassType)
				                    : nullptr;
			if (managedType && TraceFirst("void* il2cpp_gchandle_new(void*, bool pinned=true)"))rendererTypeHandle = api
				.handleNew(managedType, true);
		}
		if (!rendererTypeHandle)
		{
			Set(report, "status", Text("renderer_type_handle_unavailable"));
			PoseReadback(report, feet);
			return report;
		}
		if (!Progress("renderer_enumeration"))
		{
			Set(report, "status", Text("trace_write_failed"));
			return report;
		}
		// The Type handle uses the same pointer-width ABI as the actor handle.
		if (!TraceFirst("void* il2cpp_gchandle_get_target(void*)"))
		{
			Set(report, "status", Text("trace_write_failed"));
			return report;
		}
		bool includeInactive = true;
		void* type = api.handleTarget(rendererTypeHandle);
		void* args[] = {type, &includeInactive};
		std::string error;
		if (!type)
		{
			Set(report, "status", Text("renderer_type_handle_target_unavailable"));
			return report;
		}
		Pin renderers(Invoke(getComponents, actorPin.Get(), args, error));
		if (!renderers.Get())
		{
			Set(report, "status", Text("renderer_enumeration_failed"));
			Set(report, "error", Text(error));
			PoseReadback(report, feet);
			return report;
		}
		Set(report, "renderers_array", ArrayInfo(renderers.Get()));
		const uintptr_t count = ArrayLength(renderers.Get());
		Set(report, "renderer_list_truncated", Flag(count > kRendererLimit));
		Json records = Array();
		int processed = 0, candidates = 0, failures = 0, matched = 0;
		const int bounded = static_cast<int>((std::min)(count, static_cast<uintptr_t>(kRendererLimit)));
		for (int i = 0; i < bounded; ++i)
		{
			if (!Progress("renderer_" + std::to_string(i)))
			{
				Set(report, "status", Text("trace_write_failed"));
				return report;
			}
			Pin renderer(Element(renderers.Get(), i, error));
			if (!error.empty() || !renderer.Get() || ObjectClass(renderer.Get()) != rendererClass)
			{
				Json failed = Object();
				Set(failed, "index", Number(i));
				Set(failed, "status", Text("renderer_array_element_failed"));
				Set(failed, "error", Text(error));
				Push(records, std::move(failed));
				++failures;
				error.clear();
				continue;
			}
			Json j = Renderer(renderer.Get(), feet, processed, candidates, stage);
			// Membership remains true after the vertices phase changes status.
			if (IsCurrentActiveFootRenderer(j))++matched;
			Set(j, "index", Number(i));
			Push(records, std::move(j));
		}
		Set(report, "renderers", std::move(records));
		Set(report, "processed_vertices", Number(processed));
		Set(report, "candidate_vertices", Number(candidates));
		Set(report, "renderer_element_failures", Number(failures));
		Set(report, "active_foot_toe_renderers", Number(matched));
		Set(report, "renderer_collection_attempted", Flag(true));
		Set(report, "vertex_collection_attempted", Flag(stage == Stage::Vertices));
		Progress("foot_toe_pose_readback");
		PoseReadback(report, feet);
		Set(report, "status", Text(stage == Stage::Renderers
			                           ? (matched > 0
				                              ? "current_foot_toe_renderers_observed"
				                              : "no_current_foot_toe_renderer_evidence")
			                           : (candidates > 0
				                              ? "neighborhood_vertices_observed"
				                              : "no_neighborhood_vertex_evidence")));
		return report;
	}

	// Keep SEH in a separate POD-only frame. /EHsc cannot catch native AV; putting
	// __try in Collect/Tick (which own Json, Pin and strings) would cause C2712.
	// A native fault can bypass C++ destructors. Do not touch Unity to recover or
	// claim that the actor, GCHandles, or temporary Mesh remain consistent.
	inline void CollectInto(Json* report, void* actor, int frame, float clipTime, uint32_t seq, Stage stage,
	                        const SnapshotContext* context)
	{
		*report = Collect(actor, frame, clipTime, seq, stage, context);
	}

	inline bool GuardedCollect(Json* report, void* actor, int frame, float clipTime, uint32_t seq, Stage stage,
	                           DWORD* faultCode, const SnapshotContext* context = nullptr)
	{
#if defined(_MSC_VER)
		__try
		{
			CollectInto(report, actor, frame, clipTime, seq, stage, context);
			return true;
		}
		__except (GetExceptionCode() == 0xE06D7363UL ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER)
		{
			*faultCode = GetExceptionCode();
			probeFaulted = true;
			return false;
		}
#else
		// Legacy offline MinGW builds have no MSVC SEH guard. Runtime probe builds
		// use the project's MSVC toolchain; never describe this as native recovery.
		CollectInto(report, actor, frame, clipTime, seq, stage, context);return true;
#endif
	}

	inline void Tick(void* actor, int unityFrame, float clipTime, const SnapshotContext* context = nullptr)
	{
		// The caller owns scene gating. Fail closed if called from any other thread.
		if (probeFaulted || configDirectory.empty() || !actor || GetCurrentThreadId() != resolveThread)return;
		const auto now = std::chrono::steady_clock::now();
		if (now < nextPoll)return;
		nextPoll = now + std::chrono::seconds(1);
		const std::string requestPath = configDirectory + "/shoe_mesh_probe.request.json";
		std::error_code ec;
		const auto size = std::filesystem::file_size(GakuMotion::MotionPathFromUtf8(requestPath), ec);
		if (ec || size == 0 || size > 16384)return;
		Json request;
		std::string error;
		if (!GakuMotion::jsonlite::ParseFile(requestPath, request, error) || !request.IsObject())return;
		if (!MatchRequest(request, actor, context, error))return;
		uint32_t seq = 0;
		if (!Sequence(request, seq) || seq <= lastSeq)return;
		lastSeq = seq;
		const std::string output = configDirectory + "/shoe_mesh_probe_" + std::to_string(seq) + ".json";
		if (std::filesystem::exists(GakuMotion::MotionPathFromUtf8(output), ec))return; // Re-loading the DLL must not repeat the same request.
		const std::string trace = configDirectory + "/shoe_mesh_probe_" + std::to_string(seq) + ".trace.json";
		if (std::filesystem::exists(GakuMotion::MotionPathFromUtf8(trace), ec))return; // Native crash is not permission to replay the same seq.
		auto stage = Stage::Handle;
		const bool validStage = ParseStage(request, stage, error);
		SnapshotContext currentContext = context ? *context : SnapshotContext{};
		if (currentContext.source == Source::OfficialLive)
		{
			const Json* costume = request.Find("costume_id");
			currentContext.costumeId = costume ? costume->stringValue : "";
		}
		const SnapshotContext* evidenceContext = context ? &currentContext : nullptr;
		BeginTrace(trace, stage, seq, unityFrame, clipTime, evidenceContext);
		Json report;
		try
		{
			if (validStage)
			{
				DWORD fault = 0;
				if (!GuardedCollect(&report, actor, unityFrame, clipTime, seq, stage, &fault, evidenceContext))
				{
					traceNativeFault = true;
					traceFaultCode = fault;
					report = Object();
					Set(report, "schema", Text("gakumotion-shoe-mesh-evidence-v2"));
					Set(report, "seq", Number(seq));
					Set(report, "api", apiEvidence);
					Set(report, "status", Text("forensic_failure"));
					Set(report, "native_fault_code", Number(fault));
					Set(report, "fault_phase", Text(tracePhase));
					Set(report, "fault_operation", Text(traceOperation));
					Set(report, "probe_requests_blocked", Flag(true));
					Set(report, "collector_pose_setter_calls", Number(0));
					Set(report, "data_consistency",
					    Text(
						    "unverified after native fault; no subsequent Unity calls or recovery attempted; RAII cleanup may have been bypassed"));
					TraceWrite(traceOperation, "native_fault_stopped");
				}
			}
			else
			{
				Progress("request_rejected");
				report = Object();
				Set(report, "schema", Text("gakumotion-shoe-mesh-evidence-v2"));
				Set(report, "seq", Number(seq));
				Set(report, "status", Text("invalid_stage"));
				Set(report, "error", Text(error));
				Set(report, "collector_pose_setter_calls", Number(0));
			}
		}
		catch (const std::exception& exception)
		{
			report = Object();
			Set(report, "seq", Number(seq));
			Set(report, "status", Text("native_collection_exception"));
			Set(report, "error", Text(exception.what()));
			Set(report, "collector_pose_setter_calls", Number(0));
			Set(report, "data_consistency", Text("unverified: collection terminated before readback"));
		}
		catch (...)
		{
			report = Object();
			Set(report, "seq", Number(seq));
			Set(report, "status", Text("native_collection_exception"));
			Set(report, "collector_pose_setter_calls", Number(0));
			Set(report, "data_consistency", Text("unverified: collection terminated before readback"));
		}
		if (!report.Find("clock_source"))EvidenceContext(report, clipTime, evidenceContext);
		if (!report.Find("actor_pointer"))Set(report, "actor_pointer", Text(PointerText(actor)));
		if (!traceNativeFault)Progress("collection_returned");
		Set(report, "stage", Text(validStage ? StageName(stage) : "invalid"));
		Set(report, "progress", traceProgress);
		Set(report, "trace_path", Text(trace));
		Set(report, "trace_persisted", Flag(traceHealthy));
		Set(report, "trace_write_count", Number(traceWrites));
		traceActive = false;
		const std::string temporary = output + ".tmp";
		{
			std::ofstream file(GakuMotion::MotionPathFromUtf8(temporary), std::ios::binary | std::ios::trunc);
			if (!file)
			{
				if (g_hostContext.logWarn)g_hostContext.
					logWarn("[GakuMotion][ShoeMeshProbe] cannot open evidence file");
				return;
			}
			WriteJson(file, report);
			file << '\n';
			file.flush();
			if (!file)
			{
				if (g_hostContext.logWarn)g_hostContext.logWarn("[GakuMotion][ShoeMeshProbe] evidence write failed");
				return;
			}
		}
		std::filesystem::rename(GakuMotion::MotionPathFromUtf8(temporary), GakuMotion::MotionPathFromUtf8(output), ec);
		if (ec)
		{
			if (g_hostContext.logWarn)g_hostContext.logWarn("[GakuMotion][ShoeMeshProbe] evidence rename failed: %s",
			                                                ec.message().c_str());
			return;
		}
		if (g_hostContext.logInfo)g_hostContext.logInfo("[GakuMotion][ShoeMeshProbe] one-shot evidence saved: %s",
		                                                output.c_str());
	}
} // namespace ShoeMeshProbe
