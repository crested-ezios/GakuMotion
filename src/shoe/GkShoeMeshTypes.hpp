#pragma once

// Included inside ShoeMeshProbe / GakuMotionPlugin.cpp's anonymous namespace.
namespace ShoeMeshProbe
{
	using Json = GakuMotion::jsonlite::Value;
	using Method = const void*;
	// Current 6000.0.77f1 GameAssembly uses an opaque pointer-width GCHandle.
	// A uint32_t signature truncates it on this build; confirmed by crash dump,
	// exact relink MAP and current VRify's live IL2CPP binding. This is not the old
	// uint32 IL2CPP ABI. Keep every return/argument/storage site the same width.
	using Handle = void*;
	static_assert(sizeof(Handle) == sizeof(void*));
	constexpr int kRendererLimit = 128, kMeshVertexLimit = 100000, kTotalVertexLimit = 250000;
	constexpr int kCandidateLimit = 50000;
	constexpr float kNeighborhoodRadius = 0.4f;

	enum class Stage { Handle, Renderers, Vertices };

	enum class Source { Photo, OfficialLive };

	struct SnapshotContext
	{
		Source source = Source::Photo;
		DWORD processId = 0;
		int sceneHandle = 0;
		std::string sceneName, costumeId;
	};

	inline const char* StageName(Stage stage)
	{
		switch (stage)
		{
		case Stage::Renderers: return "renderers";
		case Stage::Vertices: return "vertices";
		default: return "handle";
		}
	}

	inline bool ParseStage(const Json& request, Stage& stage, std::string& error)
	{
		stage = Stage::Handle;
		const Json* value = request.Find("stage");
		if (!value)return true;
		if (value->IsString())
		{
			if (value->stringValue == "handle")return true;
			if (value->stringValue == "renderers")
			{
				stage = Stage::Renderers;
				return true;
			}
			if (value->stringValue == "vertices")
			{
				stage = Stage::Vertices;
				return true;
			}
		}
		error = "stage must be handle, renderers, or vertices; omitted stage defaults to handle";
		return false;
	}

	inline Json Object()
	{
		Json v;
		v.type = GakuMotion::jsonlite::Type::Object;
		return v;
	}

	inline Json Array()
	{
		Json v;
		v.type = GakuMotion::jsonlite::Type::Array;
		return v;
	}

	inline Json Text(const std::string& s)
	{
		Json v;
		v.type = GakuMotion::jsonlite::Type::String;
		v.stringValue = s;
		return v;
	}

	inline Json Number(double n)
	{
		Json v;
		if (std::isfinite(n))
		{
			v.type = GakuMotion::jsonlite::Type::Number;
			v.numberValue = n;
		}
		return v;
	}

	inline Json Flag(bool b)
	{
		Json v;
		v.type = GakuMotion::jsonlite::Type::Bool;
		v.boolValue = b;
		return v;
	}

	inline void Set(Json& j, const std::string& key, Json value)
	{
		j.objectMembers.emplace_back(key, std::move(value));
	}

	inline void Push(Json& j, Json value) { j.arrayItems.emplace_back(std::move(value)); }

	inline bool IsCurrentActiveFootRenderer(const Json& renderer)
	{
		for (const char* key : {
			     "enabled_known", "enabled", "active_in_hierarchy_known", "active_in_hierarchy",
			     "has_current_foot_or_toe_bone"
		     })
		{
			const Json* value = renderer.Find(key);
			if (!value || !value->IsBool() || !value->boolValue)return false;
		}
		return true;
	}

	inline std::string Quote(const std::string& s)
	{
		std::string out = "\"";
		for (unsigned char c : s)
		{
			if (c == '"' || c == '\\')
			{
				out += '\\';
				out += static_cast<char>(c);
			}
			else if (c < 32)
			{
				char escaped[7];
				std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
				out += escaped;
			}
			else out += static_cast<char>(c);
		}
		return out + '"';
	}

	inline void WriteJson(std::ostream& out, const Json& j)
	{
		switch (j.type)
		{
		case GakuMotion::jsonlite::Type::String: out << Quote(j.stringValue);
			break;
		case GakuMotion::jsonlite::Type::Number:
			{
				char n[40];
				std::snprintf(n, sizeof(n), "%.17g", j.numberValue);
				out << n;
				break;
			}
		case GakuMotion::jsonlite::Type::Bool: out << (j.boolValue ? "true" : "false");
			break;
		case GakuMotion::jsonlite::Type::Array:
			out << '[';
			for (size_t i = 0; i < j.arrayItems.size(); ++i)
			{
				if (i)out << ',';
				WriteJson(out, j.arrayItems[i]);
			}
			out << ']';
			break;
		case GakuMotion::jsonlite::Type::Object:
			out << '{';
			for (size_t i = 0; i < j.objectMembers.size(); ++i)
			{
				if (i)out << ',';
				out << Quote(j.objectMembers[i].first) << ':';
				WriteJson(out, j.objectMembers[i].second);
			}
			out << '}';
			break;
		default: out << "null";
			break;
		}
	}

	// Fixed file for a request, atomically replaced and flushed before native calls.
	// Only first use of an API in a phase and phase boundaries touch disk. In
	// particular Array.GetValue/readField/GCHandle use inside vertex loops do not.
	struct MethodSignature
	{
		Method method;
		std::string signature;
	};

	inline std::vector<MethodSignature> methodSignatures;
	inline std::string tracePath, tracePhase, traceOperation;
	inline auto traceStage = Stage::Handle;
	inline uint32_t traceSeq = 0;
	inline int traceFrame = 0, traceWrites = 0;
	inline float traceClipTime = 0;
	inline bool traceActive = false, traceHealthy = true;
	inline bool probeFaulted = false, traceNativeFault = false;
	inline DWORD traceFaultCode = 0;
	inline Json traceProgress = Array();
	inline std::vector<std::string> traceSeen;

	inline std::string PointerText(const void* pointer)
	{
		char value[2 + sizeof(uintptr_t) * 2 + 1]{};
		std::snprintf(value, sizeof(value), "0x%llx",
		              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pointer)));
		return value;
	}

	inline bool Sequence(const Json& request, uint32_t& seq)
	{
		const Json* value = request.Find("seq");
		if (!value || !value->IsNumber())return false;
		const double n = value->numberValue;
		if (!std::isfinite(n) || n < 1 || n > 2147483647.0 || std::floor(n) != n)return false;
		seq = static_cast<uint32_t>(n);
		return true;
	}

	// Pure request checks: no metadata resolution or Unity/managed getter calls.
	// A request for a different callback is ignored, never redirected to its actor.
	inline bool MatchRequest(const Json& request, void* actor, const SnapshotContext* context, std::string& error)
	{
		const Json* source = request.Find("source");
		const Json* profile = request.Find("profile");
		const bool official = context && context->source == Source::OfficialLive;
		if (!profile || !profile->IsString())
		{
			error = "profile missing or invalid";
			return false;
		}
		if (!official)
		{
			const bool photoSource = !source || (source->IsString() && source->stringValue == "photo");
			if (!photoSource || profile->stringValue != "saki-blue-training-boots")
			{
				error = "not a compatible photo request";
				return false;
			}
			return true;
		}
		if (!actor || context->sceneName != "Live" || !context->processId || context->sceneHandle == 0)
		{
			error = "current callback is not a verified Live actor";
			return false;
		}
		if (!source || !source->IsString() || source->stringValue != "official-live" || profile->stringValue !=
			"saki-current-outfit")
		{
			error = "official source/profile mismatch";
			return false;
		}
		const Json* pid = request.Find("process_id");
		const Json* scene = request.Find("scene_handle");
		const Json* pointer = request.Find("actor_pointer");
		if (!pid || !pid->IsNumber() || !std::isfinite(pid->numberValue) || pid->numberValue != static_cast<double>(
			context->processId))
		{
			error = "process_id mismatch";
			return false;
		}
		if (!scene || !scene->IsNumber() || !std::isfinite(scene->numberValue) || scene->numberValue != static_cast<
			double>(context->sceneHandle))
		{
			error = "scene_handle mismatch";
			return false;
		}
		if (!pointer || !pointer->IsString() || pointer->stringValue != PointerText(actor))
		{
			error = "actor_pointer mismatch";
			return false;
		}
		const Json* costume = request.Find("costume_id");
		if (costume && (!costume->IsString() || costume->stringValue.size() > 128))
		{
			error = "costume_id must be an optional requester string of at most 128 bytes";
			return false;
		}
		return true;
	}

	inline void EvidenceContext(Json& report, float clipTime, const SnapshotContext* context)
	{
		const bool official = context && context->source == Source::OfficialLive;
		Set(report, "source", Text(official ? "official-live" : "photo"));
		Set(report, "requested_profile", Text(official ? "saki-current-outfit" : "saki-blue-training-boots"));
		Set(report, "profile_identity",
		    Text("requester assertion; actor/outfit identity is not automatically authenticated"));
		Set(report, "clip_time", official ? Json{} : Number(clipTime));
		Set(report, "clock_source",
		    Text(official ? "official animation time not read; clip_time unavailable" : "gakumotion-player"));
		if (official)
		{
			Set(report, "process_id", Number(context->processId));
			Set(report, "scene_handle", Number(context->sceneHandle));
			Set(report, "scene_name", Text(context->sceneName));
			Set(report, "costume_id", context->costumeId.empty() ? Json{} : Text(context->costumeId));
			Set(report, "costume_identity",
			    Text("requester label only; current costume/ownership is not authenticated by the collector"));
		}
	}

	inline SnapshotContext traceContext;
	inline bool traceHasContext = false;

	inline bool TraceWrite(const std::string& operation, const std::string& state)
	{
		if (!traceActive)return true;
		if (!traceHealthy)return false;
		traceOperation = operation;
		Json trace = Object();
		Set(trace, "schema", Text("gakumotion-shoe-mesh-trace-v1"));
		Set(trace, "seq", Number(traceSeq));
		Set(trace, "stage", Text(StageName(traceStage)));
		Set(trace, "unity_frame", Number(traceFrame));
		EvidenceContext(trace, traceClipTime, traceHasContext ? &traceContext : nullptr);
		Set(trace, "phase", Text(tracePhase));
		Set(trace, "operation", Text(operation));
		Set(trace, "state", Text(state));
		Set(trace, "write_index", Number(++traceWrites));
		if (traceNativeFault)
		{
			Set(trace, "native_fault_code", Number(traceFaultCode));
			Set(trace, "probe_requests_blocked", Flag(true));
		}
		std::ostringstream text;
		WriteJson(text, trace);
		text << '\n';
		const auto bytes = text.str();
		const auto target = GakuMotion::MotionPathFromUtf8(tracePath), temporary = GakuMotion::MotionPathFromUtf8(tracePath + ".tmp");
		HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
		                          nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			traceHealthy = false;
			return false;
		}
		DWORD written = 0;
		const bool good = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written
			== bytes.size() && FlushFileBuffers(file);
		CloseHandle(file);
		traceHealthy = good && MoveFileExW(temporary.c_str(), target.c_str(),
		                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
		return traceHealthy;
	}

	inline bool TraceFirst(const std::string& signature)
	{
		if (!traceActive)return true;
		if (!traceHealthy)return false;
		if (std::find(traceSeen.begin(), traceSeen.end(), signature) != traceSeen.end())return true;
		traceSeen.push_back(signature);
		return TraceWrite(signature, "before_native_call");
	}

	inline bool Progress(const std::string& phase)
	{
		tracePhase = phase;
		traceSeen.clear();
		Push(traceProgress, Text(phase));
		return TraceWrite("phase_boundary", "entered");
	}

	inline void BeginTrace(const std::string& path, Stage stage, uint32_t seq, int frame, float clipTime,
	                       const SnapshotContext* context = nullptr)
	{
		tracePath = path;
		traceStage = stage;
		traceSeq = seq;
		traceFrame = frame;
		traceClipTime = clipTime;
		traceHasContext = context != nullptr;
		traceContext = context ? *context : SnapshotContext{};
		traceWrites = 0;
		traceHealthy = true;
		traceActive = !path.empty();
		traceProgress = Array();
		traceSeen.clear();
		tracePhase.clear();
		traceOperation.clear();
		traceNativeFault = false;
		traceFaultCode = 0;
	}

	inline std::string Signature(Method method)
	{
		for (const auto& item : methodSignatures)if (item.method == method)return item.signature;
		return "il2cpp_runtime_invoke (unregistered metadata method)";
	}
} // namespace ShoeMeshProbe
