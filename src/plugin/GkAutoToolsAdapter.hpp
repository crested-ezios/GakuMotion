#pragma once

// =============================================================================
// GkAutoToolsAdapter.hpp — Optional gakuautotools.registry/v1 adapter
// =============================================================================
// Lazily connects to GakuAutoTools' process-wide tool registry on runtime ready
// and retries lazily on main-thread ticks so plugin load order never matters
// and there is zero impact when GakuAutoTools is absent.
// =============================================================================

#include "GakuAutoToolsRegistry.h"
#include "GakuBurstBridge.h"
#include "GakuFaceDiagnostics.h"
#include "GakuMotionActor.h"
#include "GakuMotionClock.h"
#include "GakuMotionControl.h"
#include "GakuMotionSceneNodes.h"
#include "GakuPhysicsDiagnostics.h"
#include "../GakuMotion.hpp"
#include "../GkJsonLite.hpp"
#include "../web/GkBridgeJson.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace GakuMotion::autotools
{
	inline constexpr const char* kOwnerPluginId = "gakumotion";

	struct PhysicsSlotView
	{
		GakuPhysicsDiagnosticSnapshot snapshot{};
		std::string reason;
	};

	struct BurstSlotView
	{
		uint64_t token{};
		void* actor{};
		bool faulted{};
		bool outstanding{};
		uint64_t sequence{};
		uint64_t generation{};
		int minimumFrame{-1};
		GbbStatus status{};
		GbbPerf perf{};
		std::string reason{"not_requested"};
	};

	struct BurstSummaryView
	{
		bool multiHasCaptureInput{false};
		std::string multiReason{"not_requested"};
		std::vector<BurstSlotView> multiSlots;
		bool singleHasCaptureInput{false};
		std::string singleReason{"not_requested"};
		GbbStatus singleStatus{};
		GbbPerf singlePerf{};
		int requestedBackend{0};
		int actualBackend{0};
		bool beforeQuartz{false};
		bool sleeveDiagnostics{false};
		std::function<int(const wchar_t* path)> dumpTraces;
	};

	struct AdapterHooks
	{
		std::function<GakuStatus(const char* name, uint32_t version, uint32_t size, void* out)> queryService;
		std::function<bool()> isShuttingDown;
		std::function<void(const char*)> logInfo;
		std::function<void(const char*)> logWarn;
		std::function<PhysicsSlotView()> readSinglePhysics;
		std::function<std::vector<PhysicsSlotView>()> readEnsemblePhysics;
		std::function<BurstSummaryView()> readBurstSummary;
	};

	class Adapter
	{
	public:
		Adapter(Core& core, AdapterHooks hooks)
			: core_(core), hooks_(std::move(hooks))
		{
		}

		~Adapter()
		{
			Shutdown();
		}

		Adapter(const Adapter&) = delete;
		Adapter& operator=(const Adapter&) = delete;

		bool IsConnected() const noexcept
		{
			return connected_.load(std::memory_order_acquire);
		}

		void OnRuntimeReady()
		{
			if (IsStopping()) return;
			std::lock_guard<std::mutex> lock(connectMutex_);
			if (connected_.load(std::memory_order_acquire) || IsStopping()) return;
			tickCounter_.store(0, std::memory_order_relaxed);
			nextRetryTick_.store(0, std::memory_order_relaxed);
			TryConnectLocked();
		}

		void OnTick()
		{
			if (connected_.load(std::memory_order_acquire) || IsStopping()) return;
			const uint64_t tick = tickCounter_.fetch_add(1, std::memory_order_relaxed);
			if (tick < nextRetryTick_.load(std::memory_order_relaxed)) return;

			std::lock_guard<std::mutex> lock(connectMutex_);
			if (connected_.load(std::memory_order_acquire) || IsStopping()) return;
			if (!TryConnectLocked())
			{
				nextRetryTick_.store(tick + 60, std::memory_order_relaxed);
			}
		}

		void Shutdown()
		{
			shuttingDown_.store(true, std::memory_order_release);
			{
				std::lock_guard<std::mutex> lock(leaseMutex_);
				sceneNodesLease_ = 0;
				sceneNodesRevision_ = 0;
			}
			std::lock_guard<std::mutex> lock(connectMutex_);
			if (connected_.load(std::memory_order_acquire))
			{
				if (registry_.unregisterOwner)
				{
					registry_.unregisterOwner(registry_.context, kOwnerPluginId);
				}
				registry_ = {};
				connected_.store(false, std::memory_order_release);
			}
		}

	private:
		static std::string JStr(const std::string& v) { return webpanel::JString(v); }
		static std::string JNum(double v) { return webpanel::JNum(v); }
		static std::string JBool(bool v) { return webpanel::JBool(v); }
		template <class T>
		static std::string JUInt(T v) { return std::to_string(static_cast<uint64_t>(v)); }

		static std::string MakeErrorJson(const std::string& error,
		                                 const std::string& status = "",
		                                 const std::string& action = "")
		{
			std::string out = "{\"ok\":false";
			if (!action.empty()) out += ",\"action\":" + JStr(action);
			if (!status.empty()) out += ",\"status\":" + JStr(status);
			out += ",\"error\":" + JStr(error) + "}";
			return out;
		}

		static const char* ControlErrorName(control::v0::Error err) noexcept
		{
			using E = control::v0::Error;
			switch (err)
			{
			case E::Ok: return "Ok";
			case E::Pending: return "Pending";
			case E::Unavailable: return "Unavailable";
			case E::InvalidArgument: return "InvalidArgument";
			case E::VersionMismatch: return "VersionMismatch";
			case E::SizeMismatch: return "SizeMismatch";
			case E::Unsupported: return "Unsupported";
			case E::NotFound: return "NotFound";
			case E::NotReady: return "NotReady";
			case E::Protected: return "Protected";
			case E::StaleSession: return "StaleSession";
			case E::StaleMotion: return "StaleMotion";
			case E::Busy: return "Busy";
			case E::Failed: return "Failed";
			}
			return "Unknown";
		}

		static const char* SceneStatusName(scene_nodes::v1::Status st) noexcept
		{
			using S = scene_nodes::v1::Status;
			switch (st)
			{
			case S::Ok: return "Ok";
			case S::Unavailable: return "Unavailable";
			case S::InvalidArgument: return "InvalidArgument";
			case S::VersionMismatch: return "VersionMismatch";
			case S::SizeMismatch: return "SizeMismatch";
			case S::NotReady: return "NotReady";
			case S::StaleCatalog: return "StaleCatalog";
			case S::Busy: return "Busy";
			case S::Protected: return "Protected";
			case S::Failed: return "Failed";
			case S::WrongThread: return "WrongThread";
			}
			return "Unknown";
		}

		static const char* BurstStateName(uint32_t state) noexcept
		{
			switch (state)
			{
			case GBB_STATE_UNINITIALIZED: return "uninitialized";
			case GBB_STATE_DISCOVERING: return "discovering";
			case GBB_STATE_READY: return "ready";
			case GBB_STATE_OBSERVING: return "observing";
			case GBB_STATE_INJECTING: return "injecting";
			case GBB_STATE_UNAVAILABLE: return "unavailable";
			case GBB_STATE_FAULTED: return "faulted";
			case GBB_STATE_SHUTDOWN: return "shutdown";
			default: return "unknown";
			}
		}

		static const char* PhysicsBackendName(int backend) noexcept
		{
			switch (backend)
			{
			case 0: return "off";
			case 1: return "fallback";
			case 2: return "native";
			default: return "unknown";
			}
		}

		static bool IsFiniteInt(const jsonlite::Value* v, int64_t& out) noexcept
		{
			if (!v || !v->IsNumber() || !std::isfinite(v->numberValue) ||
				v->numberValue != std::floor(v->numberValue))
				return false;
			if (v->numberValue < static_cast<double>(INT64_MIN) ||
				v->numberValue > static_cast<double>(INT64_MAX))
				return false;
			out = static_cast<int64_t>(v->numberValue);
			return true;
		}

		static bool IsFiniteNumber(const jsonlite::Value* v, double& out) noexcept
		{
			if (!v || !v->IsNumber() || !std::isfinite(v->numberValue)) return false;
			out = v->numberValue;
			return true;
		}

		static const jsonlite::Value* FindAlias(const jsonlite::Value& obj, const char* primary, const char* alias = nullptr) noexcept
		{
			if (const auto* p = obj.Find(primary)) return p;
			if (alias) return obj.Find(alias);
			return nullptr;
		}

		static bool ParseArgsObject(const char* argsJson, jsonlite::Value& out, std::string& error)
		{
			if (!argsJson || *argsJson == '\0')
			{
				out = {};
				out.type = jsonlite::Type::Object;
				return true;
			}
			if (!jsonlite::ParseText(argsJson, out, error)) return false;
			if (!out.IsObject())
			{
				error = "Tool arguments must be a JSON object";
				return false;
			}
			return true;
		}

		static GakuStatus WriteJsonResponse(const std::string& json,
		                                    uint32_t responseCapacity,
		                                    char* responseJson,
		                                    uint32_t* requiredBytes)
		{
			if (!requiredBytes) return GAKU_E_INVALID_ARGUMENT;
			const uint32_t needed = static_cast<uint32_t>(json.size() + 1);
			*requiredBytes = needed;
			if (!responseJson || responseCapacity < needed) return GAKU_E_SIZE;
			std::memcpy(responseJson, json.data(), needed);
			return GAKU_OK;
		}

		bool IsStopping() const noexcept
		{
			return shuttingDown_.load(std::memory_order_acquire) ||
				(hooks_.isShuttingDown && hooks_.isShuttingDown());
		}

		template <class Handler>
		static GakuStatus InvokeTrampoline(void* userData,
		                                   const char* argsJson,
		                                   uint32_t responseCapacity,
		                                   char* responseJson,
		                                   uint32_t* requiredBytes,
		                                   Handler&& handler)
		{
			if (!requiredBytes) return GAKU_E_INVALID_ARGUMENT;
			auto* self = static_cast<Adapter*>(userData);
			if (!self || self->IsStopping())
			{
				return WriteJsonResponse(MakeErrorJson("gakumotion shutting down", "Unavailable"),
				                         responseCapacity, responseJson, requiredBytes);
			}
			try
			{
				jsonlite::Value args;
				std::string parseError;
				if (!ParseArgsObject(argsJson, args, parseError))
				{
					return WriteJsonResponse(MakeErrorJson(parseError, "InvalidArgument"),
					                         responseCapacity, responseJson, requiredBytes);
				}
				const std::string result = handler(*self, args);
				return WriteJsonResponse(result, responseCapacity, responseJson, requiredBytes);
			}
			catch (...)
			{
				try
				{
					return WriteJsonResponse(MakeErrorJson("Internal exception", "Failed"),
					                         responseCapacity, responseJson, requiredBytes);
				}
				catch (...)
				{
					return GAKU_E_INTERNAL;
				}
			}
		}

		static GakuStatus InvokeMotionStatus(void* ud, const char* a, uint32_t c, char* r, uint32_t* req)
		{
			return InvokeTrampoline(ud, a, c, r, req, [](Adapter& s, const jsonlite::Value& args) { return s.HandleMotionStatus(args); });
		}
		static GakuStatus InvokeMotionList(void* ud, const char* a, uint32_t c, char* r, uint32_t* req)
		{
			return InvokeTrampoline(ud, a, c, r, req, [](Adapter& s, const jsonlite::Value& args) { return s.HandleMotionList(args); });
		}
		static GakuStatus InvokeMotionLoad(void* ud, const char* a, uint32_t c, char* r, uint32_t* req)
		{
			return InvokeTrampoline(ud, a, c, r, req, [](Adapter& s, const jsonlite::Value& args) { return s.HandleMotionLoad(args); });
		}
		static GakuStatus InvokeMotionTransport(void* ud, const char* a, uint32_t c, char* r, uint32_t* req)
		{
			return InvokeTrampoline(ud, a, c, r, req, [](Adapter& s, const jsonlite::Value& args) { return s.HandleMotionTransport(args); });
		}
		static GakuStatus InvokeMotionSceneNodes(void* ud, const char* a, uint32_t c, char* r, uint32_t* req)
		{
			return InvokeTrampoline(ud, a, c, r, req, [](Adapter& s, const jsonlite::Value& args) { return s.HandleMotionSceneNodes(args); });
		}
		static GakuStatus InvokeMotionFormation(void* ud, const char* a, uint32_t c, char* r, uint32_t* req)
		{
			return InvokeTrampoline(ud, a, c, r, req, [](Adapter& s, const jsonlite::Value& args) { return s.HandleMotionFormation(args); });
		}
		static GakuStatus InvokeMotionExperiment(void* ud, const char* a, uint32_t c, char* r, uint32_t* req)
		{
			return InvokeTrampoline(ud, a, c, r, req, [](Adapter& s, const jsonlite::Value& args) { return s.HandleMotionExperiment(args); });
		}
		static GakuStatus InvokeDiagPhysics(void* ud, const char* a, uint32_t c, char* r, uint32_t* req)
		{
			return InvokeTrampoline(ud, a, c, r, req, [](Adapter& s, const jsonlite::Value& args) { return s.HandleDiagPhysics(args); });
		}
		static GakuStatus InvokeDiagFace(void* ud, const char* a, uint32_t c, char* r, uint32_t* req)
		{
			return InvokeTrampoline(ud, a, c, r, req, [](Adapter& s, const jsonlite::Value& args) { return s.HandleDiagFace(args); });
		}
		static GakuStatus InvokeDiagBurst(void* ud, const char* a, uint32_t c, char* r, uint32_t* req)
		{
			return InvokeTrampoline(ud, a, c, r, req, [](Adapter& s, const jsonlite::Value& args) { return s.HandleDiagBurst(args); });
		}

		bool TryConnectLocked()
		{
			if (!hooks_.queryService) return false;
			GakuAutoToolsRegistryV1 reg{};
			const GakuStatus st = hooks_.queryService(
				GAKU_SERVICE_AUTOTOOLS_REGISTRY,
				GAKU_AUTOTOOLS_REGISTRY_VERSION,
				sizeof(reg),
				&reg);
			if (st != GAKU_OK) return false;
			if (reg.abiVersion != GAKU_AUTOTOOLS_REGISTRY_VERSION ||
				reg.structSize < sizeof(GakuAutoToolsRegistryV1) ||
				!reg.registerTool || !reg.unregisterOwner)
			{
				return false;
			}

			const GakuAutoToolsToolDescV1 tools[] = {
				{
					GAKU_AUTOTOOLS_REGISTRY_VERSION,
					sizeof(GakuAutoToolsToolDescV1),
					kOwnerPluginId,
					"motion.status",
					"Read current GakuMotion playback, timeline, actor, formation, and clock status.",
					R"({"type":"object","properties":{}})",
					GAKU_AUTOTOOLS_THREAD_MAIN,
					0u,
					&InvokeMotionStatus,
					this
				},
				{
					GAKU_AUTOTOOLS_REGISTRY_VERSION,
					sizeof(GakuAutoToolsToolDescV1),
					kOwnerPluginId,
					"motion.list",
					"List available GakuMotion tracks, loaded key, library revision, and track details.",
					R"({"type":"object","properties":{}})",
					GAKU_AUTOTOOLS_THREAD_MAIN,
					0u,
					&InvokeMotionList,
					this
				},
				{
					GAKU_AUTOTOOLS_REGISTRY_VERSION,
					sizeof(GakuAutoToolsToolDescV1),
					kOwnerPluginId,
					"motion.load",
					"Load a GakuMotion track by exact library key (supports solo and multi-idol formation).",
					R"({"type":"object","properties":{"motion_key":{"type":"string"},"autoplay":{"type":"boolean","default":false}},"required":["motion_key"]})",
					GAKU_AUTOTOOLS_THREAD_MAIN,
					0u,
					&InvokeMotionLoad,
					this
				},
				{
					GAKU_AUTOTOOLS_REGISTRY_VERSION,
					sizeof(GakuAutoToolsToolDescV1),
					kOwnerPluginId,
					"motion.transport",
					"Control GakuMotion playback: play, pause, stop, seek, speed, offset, camera, or scene.",
					R"({"type":"object","properties":{"action":{"type":"string","enum":["play","pause","stop","seek","speed","offset","camera","scene"]},"value":{"type":"number"},"offset":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"scene":{"type":"integer"},"camera":{"type":"object"}},"required":["action"]})",
					GAKU_AUTOTOOLS_THREAD_MAIN,
					0u,
					&InvokeMotionTransport,
					this
				},
				{
					GAKU_AUTOTOOLS_REGISTRY_VERSION,
					sizeof(GakuAutoToolsToolDescV1),
					kOwnerPluginId,
					"motion.scene_nodes",
					"Inspect, apply, or release GakuMotion stage background scene node visibility.",
					R"({"type":"object","properties":{"action":{"type":"string","enum":["list","apply","release"],"default":"list"},"updates":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"handle":{"type":"integer"},"active":{"type":"boolean"}}}}}})",
					GAKU_AUTOTOOLS_THREAD_MAIN,
					0u,
					&InvokeMotionSceneNodes,
					this
				},
				{
					GAKU_AUTOTOOLS_REGISTRY_VERSION,
					sizeof(GakuAutoToolsToolDescV1),
					kOwnerPluginId,
					"motion.formation",
					"Query or update multi-idol StageFormation slot status, selection, and swapping.",
					R"({"type":"object","properties":{"action":{"type":"string","enum":["status","select_slot","swap_slots"],"default":"status"},"slot_index":{"type":"integer"},"from_slot":{"type":"integer"},"to_slot":{"type":"integer"}}})",
					GAKU_AUTOTOOLS_THREAD_MAIN,
					0u,
					&InvokeMotionFormation,
					this
				},
				{
					GAKU_AUTOTOOLS_REGISTRY_VERSION,
					sizeof(GakuAutoToolsToolDescV1),
					kOwnerPluginId,
					"motion.experiment",
					"Read or update live experiment settings and the selected actor's mouth weight in memory.",
					R"({"type":"object","properties":{"before_quartz_experiment":{"type":"boolean"},"secondary_physics_backend":{"type":["integer","string"]},"preserve_animator":{"type":"boolean"},"miku_mouth_fit":{"type":["boolean","integer","null"]},"mouth_weight":{"type":"number","minimum":0,"maximum":2},"waist_support":{"type":"boolean"}}})",
					GAKU_AUTOTOOLS_THREAD_MAIN,
					0u,
					&InvokeMotionExperiment,
					this
				},
				{
					GAKU_AUTOTOOLS_REGISTRY_VERSION,
					sizeof(GakuAutoToolsToolDescV1),
					kOwnerPluginId,
					"diag.physics",
					"Read live GakuMotion physics diagnostics across solo or all formation slots.",
					R"({"type":"object","properties":{"slot_index":{"type":"integer"}}})",
					GAKU_AUTOTOOLS_THREAD_MAIN,
					0u,
					&InvokeDiagPhysics,
					this
				},
				{
					GAKU_AUTOTOOLS_REGISTRY_VERSION,
					sizeof(GakuAutoToolsToolDescV1),
					kOwnerPluginId,
					"diag.face",
					"Read filtered GakuMotion facial channel diagnostics on the active or selected actor.",
					R"({"type":"object","properties":{"only_active":{"type":"boolean","default":true},"only_diff":{"type":"boolean","default":false},"eps":{"type":"number","default":0.0001},"filter":{"type":"string","default":""},"limit":{"type":"integer","default":64},"slot_index":{"type":"integer"}}})",
					GAKU_AUTOTOOLS_THREAD_MAIN,
					0u,
					&InvokeDiagFace,
					this
				},
				{
					GAKU_AUTOTOOLS_REGISTRY_VERSION,
					sizeof(GakuAutoToolsToolDescV1),
					kOwnerPluginId,
					"diag.burst",
					"Read live GakuBurstBridge / multi-burst physics bridge summary and optional trace dump.",
					R"({"type":"object","properties":{"dump_trace":{"type":"boolean","default":false}}})",
					GAKU_AUTOTOOLS_THREAD_MAIN,
					0u,
					&InvokeDiagBurst,
					this
				}
			};

			for (const auto& desc : tools)
			{
				const GakuStatus regSt = reg.registerTool(reg.context, &desc);
				if (regSt != GAKU_OK)
				{
					reg.unregisterOwner(reg.context, kOwnerPluginId);
					if (hooks_.logWarn)
					{
						char msg[192]{};
						std::snprintf(msg, sizeof(msg),
							"[GakuMotion][AutoTools] Failed to register tool '%s' (status=%d)", desc.name, regSt);
						hooks_.logWarn(msg);
					}
					return false;
				}
			}

			registry_ = reg;
			connected_.store(true, std::memory_order_release);
			if (hooks_.logInfo)
			{
				hooks_.logInfo("[GakuMotion][AutoTools] Registered 10 tools via gakuautotools.registry/v1");
			}
			return true;
		}

		// ---- Tool Handlers -------------------------------------------------

		std::string HandleMotionStatus(const jsonlite::Value&)
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			control::v0::State st{};
			const auto stErr = core_.ReadControlState(sizeof(st), &st);
			GakuMotionActorSnapshot actorSnap{};
			actorSnap.abiVersion = GAKU_MOTION_ACTOR_VERSION;
			actorSnap.structSize = sizeof(actorSnap);
			const bool hasActor = core_.ReadSelectedActor(sizeof(actorSnap), &actorSnap) == GAKU_OK;
			GakuMotionClockSnapshotV1 clockSnap{};
			clockSnap.abiVersion = GAKU_MOTION_CLOCK_VERSION;
			clockSnap.structSize = sizeof(clockSnap);
			const bool hasClock = core_.ReadClockSnapshot(&clockSnap) == GAKU_OK;

			const bool ready = (stErr == control::v0::Error::Ok) && (st.available != 0);
			return std::string("{\"ok\":true,\"available\":") + JBool(core_.initialized) +
				",\"ready\":" + JBool(ready) +
				",\"protectedContext\":" + JBool(st.protectedContext != 0) +
				",\"captureSuspended\":" + JBool(st.captureSuspended != 0) +
				",\"formationActive\":" + JBool(core_.formation.active) +
				",\"playback\":" + JUInt(st.playback) +
				",\"time\":" + JNum(st.time) +
				",\"duration\":" + JNum(st.duration) +
				",\"speed\":" + JNum(st.speed) +
				",\"motionCount\":" + JUInt(st.motionCount) +
				",\"loadedMotionKey\":" + JStr(st.loadedMotionKey) +
				",\"actor\":{\"available\":" + JBool(hasActor) +
				",\"actorToken\":" + JUInt(actorSnap.actorToken) +
				",\"character\":" + JStr(actorSnap.character) +
				",\"displayName\":" + JStr(actorSnap.displayName) +
				",\"faceName\":" + JStr(actorSnap.faceName) + "}" +
				",\"clock\":{\"available\":" + JBool(hasClock) +
				",\"playbackState\":" + JUInt(clockSnap.playbackState) +
				",\"currentTimeSeconds\":" + JNum(clockSnap.currentTimeSeconds) +
				",\"timelineDurationSeconds\":" + JNum(clockSnap.timelineDurationSeconds) +
				",\"playbackSpeed\":" + JNum(clockSnap.playbackSpeed) +
				",\"trackTitle\":" + JStr(clockSnap.trackTitle) + "}}";
		}

		std::string HandleMotionList(const jsonlite::Value&)
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			std::string motionsArr = "[";
			std::string detailsArr = "[";
			for (size_t i = 0; i < core_.motions.size(); ++i)
			{
				const auto& entry = core_.motions[i];
				if (i)
				{
					motionsArr += ",";
					detailsArr += ",";
				}
				motionsArr += JStr(entry.name);
				const int slotCount = (entry.isGroup || !entry.positions.empty())
					? (std::max)(entry.positionCount, static_cast<int>(entry.positions.size()))
					: (std::max)(1, entry.positionCount);
				detailsArr += "{\"key\":" + JStr(entry.name) +
					",\"title\":" + JStr(entry.title) +
					",\"subtitle\":" + JStr(entry.subtitle) +
					",\"duration\":" + JNum(entry.durationSeconds) +
					",\"durationSeconds\":" + JNum(entry.durationSeconds) +
					",\"totalFrames\":" + JUInt(entry.totalFrames) +
					",\"fps\":" + JNum(entry.fps) +
					",\"isGroup\":" + JBool(entry.isGroup) +
					",\"slotCount\":" + JNum(slotCount) +
					",\"hasAudio\":" + JBool(entry.hasAudio) +
					",\"hasCamera\":" + JBool(entry.hasCamera) +
					",\"hasFacial\":" + JBool(entry.hasFacial) + "}";
			}
			motionsArr += "]";
			detailsArr += "]";

			return "{\"ok\":true,\"available\":" + JBool(core_.ControlReady()) +
				",\"libraryRevision\":" + JUInt(core_.libraryRevision) +
				",\"motionCount\":" + JUInt(core_.motions.size()) +
				",\"loadedMotionKey\":" + JStr(core_.loadedMotionKey) +
				",\"motions\":" + motionsArr +
				",\"details\":" + detailsArr + "}";
		}

		std::string HandleMotionLoad(const jsonlite::Value& args)
		{
			const auto* keyNode = FindAlias(args, "motion_key", "motionKey");
			if (!keyNode || !keyNode->IsString() || keyNode->stringValue.empty())
			{
				return MakeErrorJson("motion_key is required", "InvalidArgument", "load");
			}
			const std::string& motionKey = keyNode->stringValue;
			if (motionKey.size() >= control::v0::KeyCapacity)
			{
				return MakeErrorJson("motion_key exceeds KeyCapacity", "InvalidArgument", "load");
			}

			bool autoplay = false;
			if (const auto* ap = args.Find("autoplay"))
			{
				if (!ap->IsBool())
					return MakeErrorJson("autoplay must be a boolean", "InvalidArgument", "load");
				autoplay = ap->boolValue;
			}

			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (!core_.initialized)
				return MakeErrorJson("Core is not initialized", "NotReady", "load");
			if (core_.captureSuspended || !core_.InjectionAllowed())
				return MakeErrorJson("Injection is blocked in the current context", "Protected", "load");

			const int index = FindPlaylistIndex(core_.motions, motionKey);
			if (index < 0)
				return MakeErrorJson("Motion key not found: " + motionKey, "NotFound", "load");
			if (!core_.PrepareInjection())
				return MakeErrorJson("Protected scene or capture gate blocked injection", "Protected", "load");

			core_.stageStoppedByTransport = false;
			if (!core_.SelectMotionByIndex(index, autoplay, !autoplay))
			{
				++core_.loadedMotionGeneration;
				return MakeErrorJson("Failed to load motion: " + motionKey, "Failed", "load");
			}

			return "{\"ok\":true,\"action\":\"load\",\"status\":\"Ok\""
				",\"motionKey\":" + JStr(motionKey) +
				",\"loadedMotionKey\":" + JStr(core_.loadedMotionKey) +
				",\"autoplay\":" + JBool(autoplay) +
				",\"formationActive\":" + JBool(core_.formation.active) +
				",\"playback\":" + JUInt(static_cast<uint32_t>(core_.player.state)) +
				",\"duration\":" + JNum(core_.player.TimelineDuration()) +
				",\"libraryRevision\":" + JUInt(core_.libraryRevision) +
				",\"motionGeneration\":" + JUInt(core_.loadedMotionGeneration) + "}";
		}

		std::string HandleMotionTransport(const jsonlite::Value& args)
		{
			const auto* actNode = args.Find("action");
			if (!actNode || !actNode->IsString() || actNode->stringValue.empty())
				return MakeErrorJson("action is required", "InvalidArgument");
			const std::string& action = actNode->stringValue;

			using namespace control::v0;
			CommandKind kind{};
			if (action == "play") kind = CommandKind::Play;
			else if (action == "pause") kind = CommandKind::Pause;
			else if (action == "stop") kind = CommandKind::Stop;
			else if (action == "seek") kind = CommandKind::Seek;
			else if (action == "speed") kind = CommandKind::Speed;
			else if (action == "offset") kind = CommandKind::Offset;
			else if (action == "camera") kind = CommandKind::Camera;
			else if (action == "scene") kind = CommandKind::Scene;
			else return MakeErrorJson("Unknown motion transport action: " + action, "InvalidArgument", action);

			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			State st{};
			const Error stErr = core_.ReadControlState(sizeof(st), &st);
			if (stErr != Error::Ok)
				return MakeErrorJson("ReadControlState failed", ControlErrorName(stErr), action);

			Command cmd{};
			cmd.kind = kind;
			cmd.session = st.session;
			cmd.motionGeneration = st.generation;
			cmd.libraryRevision = st.libraryRevision;
			cmd.offset = st.offset;
			cmd.camera = st.camera;
			cmd.scene = st.scene;

			if (kind == CommandKind::Seek || kind == CommandKind::Speed)
			{
				double val = 0.0;
				if (!IsFiniteNumber(args.Find("value"), val))
					return MakeErrorJson("value must be a finite number", "InvalidArgument", action);
				cmd.value = static_cast<float>(val);
			}
			else if (kind == CommandKind::Offset)
			{
				const auto* off = args.Find("offset");
				if (!off)
					return MakeErrorJson("offset is required", "InvalidArgument", action);
				double x = 0.0, y = 0.0, z = 0.0;
				if (off->IsArray())
				{
					if (off->Size() != 3 ||
						!IsFiniteNumber(off->At(0), x) ||
						!IsFiniteNumber(off->At(1), y) ||
						!IsFiniteNumber(off->At(2), z))
						return MakeErrorJson("offset array must have 3 finite numbers", "InvalidArgument", action);
				}
				else if (off->IsObject())
				{
					if (!IsFiniteNumber(off->Find("x"), x) ||
						!IsFiniteNumber(off->Find("y"), y) ||
						!IsFiniteNumber(off->Find("z"), z))
						return MakeErrorJson("offset object must have finite x, y, z", "InvalidArgument", action);
				}
				else
				{
					return MakeErrorJson("offset must be [x,y,z] or {x,y,z}", "InvalidArgument", action);
				}
				cmd.offset = Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
			}
			else if (kind == CommandKind::Scene)
			{
				int64_t sceneIdx = -1;
				if (!IsFiniteInt(args.Find("scene"), sceneIdx) || sceneIdx < 0)
					return MakeErrorJson("scene must be a non-negative integer", "InvalidArgument", action);
				cmd.scene = static_cast<uint32_t>(sceneIdx);
			}
			else if (kind == CommandKind::Camera)
			{
				const auto* cam = args.Find("camera");
				if (!cam || !cam->IsObject())
					return MakeErrorJson("camera object is required", "InvalidArgument", action);
				if (const auto* en = cam->Find("enabled"))
				{
					int64_t iv = 0;
					if (en->IsBool()) cmd.camera.enabled = en->boolValue ? 1u : 0u;
					else if (IsFiniteInt(en, iv)) cmd.camera.enabled = iv != 0 ? 1u : 0u;
					else return MakeErrorJson("camera.enabled must be boolean", "InvalidArgument", action);
				}
				if (const auto* m = cam->Find("mode"))
				{
					int64_t iv = 0;
					if (!IsFiniteInt(m, iv) || iv < 0)
						return MakeErrorJson("camera.mode must be a non-negative integer", "InvalidArgument", action);
					cmd.camera.mode = static_cast<uint32_t>(iv);
				}
				if (const auto* fb = FindAlias(*cam, "followBehavior", "follow_behavior"))
				{
					int64_t iv = 0;
					if (!IsFiniteInt(fb, iv) || iv < 0)
						return MakeErrorJson("camera.followBehavior must be a non-negative integer", "InvalidArgument", action);
					cmd.camera.followBehavior = static_cast<uint32_t>(iv);
				}
				if (const auto* ft = FindAlias(*cam, "followTarget", "follow_target"))
				{
					int64_t iv = 0;
					if (!IsFiniteInt(ft, iv) || iv < 0)
						return MakeErrorJson("camera.followTarget must be a non-negative integer", "InvalidArgument", action);
					cmd.camera.followTarget = static_cast<uint32_t>(iv);
				}
				const auto* rate = FindAlias(*cam, "rateXYZ", "rate_xyz");
				if (!rate) rate = FindAlias(*cam, "followRate", "follow_rate");
				if (rate)
				{
					double rx = 0.0, ry = 0.0, rz = 0.0;
					if (rate->IsArray())
					{
						if (rate->Size() != 3 ||
							!IsFiniteNumber(rate->At(0), rx) ||
							!IsFiniteNumber(rate->At(1), ry) ||
							!IsFiniteNumber(rate->At(2), rz))
							return MakeErrorJson("camera rate must have 3 finite numbers", "InvalidArgument", action);
					}
					else if (rate->IsObject())
					{
						if (!IsFiniteNumber(rate->Find("x"), rx) ||
							!IsFiniteNumber(rate->Find("y"), ry) ||
							!IsFiniteNumber(rate->Find("z"), rz))
							return MakeErrorJson("camera rate object must have x, y, z", "InvalidArgument", action);
					}
					else
					{
						return MakeErrorJson("camera rate must be [x,y,z] or {x,y,z}", "InvalidArgument", action);
					}
					cmd.camera.rateXYZ[0] = static_cast<float>(rx);
					cmd.camera.rateXYZ[1] = static_cast<float>(ry);
					cmd.camera.rateXYZ[2] = static_cast<float>(rz);
				}
			}

			const Error valid = ValidateCommand(&cmd);
			if (valid != Error::Ok)
				return MakeErrorJson(Message(valid), ControlErrorName(valid), action);

			const Error execErr = core_.ExecuteControl(cmd);
			if (execErr == Error::Ok &&
				(kind == CommandKind::Play || kind == CommandKind::Pause || kind == CommandKind::Seek))
			{
				for (auto& kv : core_.formationTracks)
				{
					if (!kv.second) continue;
					kv.second->SyncClockState(core_.player.currentTime);
					kv.second->state = core_.player.state;
				}
			}

			return std::string("{\"ok\":") + JBool(execErr == Error::Ok) +
				",\"action\":" + JStr(action) +
				",\"status\":" + JStr(ControlErrorName(execErr)) +
				",\"message\":" + JStr(Message(execErr)) +
				",\"playback\":" + JUInt(static_cast<uint32_t>(core_.player.state)) +
				",\"time\":" + JNum(core_.player.currentTime) +
				",\"duration\":" + JNum(core_.player.TimelineDuration()) +
				",\"speed\":" + JNum(core_.player.playbackSpeedMultiplier) + "}";
		}

		std::string HandleMotionSceneNodes(const jsonlite::Value& args)
		{
			using namespace scene_nodes::v1;
			std::string action = "list";
			if (const auto* actNode = args.Find("action"))
			{
				if (!actNode->IsString() || actNode->stringValue.empty())
					return MakeErrorJson("action must be a non-empty string", "InvalidArgument");
				action = actNode->stringValue;
			}

			if (action == "list")
			{
				State st{};
				const Status s = core_.stageManager.ReadSceneNodesState(sizeof(st), &st);
				std::string nodesJson = "[";
				for (uint32_t i = 0; i < st.count; ++i)
				{
					Node n{};
					if (core_.stageManager.ReadSceneNode(st.catalogRevision, i, sizeof(n), &n) == Status::Ok)
					{
						if (nodesJson.size() > 1) nodesJson += ",";
						nodesJson += "{\"handle\":" + JUInt(n.handle) +
							",\"ordinal\":" + JUInt(n.ordinal) +
							",\"active\":" + JBool(n.active != 0) +
							",\"name\":" + JStr(n.name) + "}";
					}
				}
				nodesJson += "]";
				return std::string("{\"ok\":") + JBool(s == Status::Ok && st.available != 0) +
					",\"available\":" + JBool(st.available != 0) +
					",\"ready\":" + JBool(st.ready != 0) +
					",\"count\":" + JUInt(st.count) +
					",\"owned\":" + JBool(st.owned != 0) +
					",\"restorePending\":" + JBool(st.restorePending != 0) +
					",\"catalogRevision\":" + JUInt(st.catalogRevision) +
					",\"status\":" + JStr(SceneStatusName(st.status)) +
					",\"message\":" + JStr(st.message) +
					",\"nodes\":" + nodesJson + "}";
			}

			if (action == "release")
			{
				std::lock_guard<std::mutex> leaseLock(leaseMutex_);
				if (sceneNodesLease_ != 0)
				{
					const Status rel = core_.stageManager.ReleaseSceneNodes(sceneNodesLease_);
					if (rel == Status::Ok || rel == Status::StaleCatalog)
					{
						sceneNodesLease_ = 0;
						sceneNodesRevision_ = 0;
					}
					return std::string("{\"ok\":") + JBool(rel == Status::Ok) +
						",\"action\":\"release\"" +
						",\"status\":" + JStr(SceneStatusName(rel)) + "}";
				}
				return "{\"ok\":true,\"action\":\"release\",\"status\":\"Ok\"}";
			}

			if (action != "apply")
			{
				return MakeErrorJson("Unknown scene_nodes action: " + action, "InvalidArgument", action);
			}

			struct NodeUpdate
			{
				std::string name;
				bool hasHandle{false};
				uint64_t handle{0};
				bool active{true};
			};
			std::vector<NodeUpdate> updates;
			if (const auto* arr = args.Find("updates"))
			{
				if (!arr->IsArray())
					return MakeErrorJson("updates must be an array", "InvalidArgument", "apply");
				for (const auto& item : arr->arrayItems)
				{
					if (!item.IsObject())
						return MakeErrorJson("each update must be an object", "InvalidArgument", "apply");
					NodeUpdate u;
					if (const auto* nm = item.Find("name"))
					{
						if (!nm->IsString())
							return MakeErrorJson("update.name must be a string", "InvalidArgument", "apply");
						u.name = nm->stringValue;
					}
					if (const auto* h = item.Find("handle"))
					{
						int64_t hv = 0;
						if (!IsFiniteInt(h, hv) || hv <= 0)
							return MakeErrorJson("update.handle must be a positive integer", "InvalidArgument", "apply");
						u.hasHandle = true;
						u.handle = static_cast<uint64_t>(hv);
					}
					if (const auto* act = item.Find("active"))
					{
						if (!act->IsBool())
							return MakeErrorJson("update.active must be a boolean", "InvalidArgument", "apply");
						u.active = act->boolValue;
					}
					if (!u.name.empty() || u.hasHandle) updates.push_back(std::move(u));
				}
			}

			std::lock_guard<std::mutex> leaseLock(leaseMutex_);
			State st{};
			if (core_.stageManager.ReadSceneNodesState(sizeof(st), &st) != Status::Ok || !st.available || !st.ready)
			{
				return MakeErrorJson("Scene nodes not ready", SceneStatusName(st.status), "apply");
			}

			if (sceneNodesLease_ != 0 && sceneNodesRevision_ != st.catalogRevision)
			{
				core_.stageManager.ReleaseSceneNodes(sceneNodesLease_);
				sceneNodesLease_ = 0;
				sceneNodesRevision_ = 0;
			}

			std::vector<Node> catalog;
			catalog.reserve(st.count);
			for (uint32_t i = 0; i < st.count; ++i)
			{
				Node n{};
				if (core_.stageManager.ReadSceneNode(st.catalogRevision, i, sizeof(n), &n) == Status::Ok)
					catalog.push_back(n);
			}

			if (sceneNodesLease_ == 0)
			{
				uint64_t lease = 0;
				const Status acq = core_.AcquireSceneNodes(st.catalogRevision, &lease);
				if (acq != Status::Ok || lease == 0)
				{
					return MakeErrorJson(std::string("scene-nodes acquire failed: ") + SceneStatusName(acq),
					                     SceneStatusName(acq), "apply");
				}
				sceneNodesLease_ = lease;
				sceneNodesRevision_ = st.catalogRevision;
			}

			std::vector<Visibility> vis;
			vis.reserve(catalog.size());
			for (const auto& n : catalog)
			{
				uint32_t activeVal = n.active;
				for (const auto& u : updates)
				{
					if ((!u.name.empty() && u.name == n.name) ||
						(u.hasHandle && u.handle == n.handle) ||
						(!u.name.empty() && u.name == std::to_string(n.handle)))
					{
						activeVal = u.active ? 1u : 0u;
					}
				}
				vis.push_back(Visibility{n.handle, activeVal, 0u});
			}

			const Status app = core_.ApplySceneNodes(sceneNodesLease_, st.catalogRevision,
			                                         static_cast<uint32_t>(vis.size()), vis.data());
			if (app == Status::StaleCatalog)
			{
				core_.stageManager.ReleaseSceneNodes(sceneNodesLease_);
				sceneNodesLease_ = 0;
				sceneNodesRevision_ = 0;
			}

			return std::string("{\"ok\":") + JBool(app == Status::Ok) +
				",\"action\":\"apply\"" +
				",\"status\":" + JStr(SceneStatusName(app)) +
				",\"appliedCount\":" + JUInt(vis.size()) +
				",\"catalogRevision\":" + JUInt(st.catalogRevision) +
				",\"lease\":" + JUInt(sceneNodesLease_) + "}";
		}

		std::string HandleMotionFormation(const jsonlite::Value& args)
		{
			std::string action = "status";
			if (const auto* actNode = args.Find("action"))
			{
				if (!actNode->IsString() || actNode->stringValue.empty())
					return MakeErrorJson("action must be a non-empty string", "InvalidArgument");
				action = actNode->stringValue;
			}

			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (!core_.fnEnsembleActorEligible && !core_.player.controller.fnCanDriveActor &&
				core_.formation.actorEpoch != core_.sceneActorRegistry.Epoch())
			{
				core_.SyncFormationWithRoster();
			}

			if (action == "select_slot")
			{
				int64_t slotIdx = -1;
				if (!IsFiniteInt(FindAlias(args, "slot_index", "slotIndex"), slotIdx) ||
					slotIdx < 0 || slotIdx >= core_.formation.slotCount)
				{
					return MakeErrorJson("slot_index is out of range", "InvalidArgument", action);
				}
				if (core_.captureSuspended || !core_.InjectionAllowed())
					return MakeErrorJson("Injection is blocked in the current context", "Protected", action);
				if (!core_.SelectSlot(static_cast<int>(slotIdx)))
					return MakeErrorJson("Failed to select slot", "Failed", action);
			}
			else if (action == "swap_slots")
			{
				const auto* fromNode = FindAlias(args, "from_slot", "fromSlot");
				if (!fromNode) fromNode = args.Find("fromSlotIndex");
				const auto* toNode = FindAlias(args, "to_slot", "toSlot");
				if (!toNode) toNode = args.Find("toSlotIndex");
				int64_t fromIdx = -1, toIdx = -1;
				if (!IsFiniteInt(fromNode, fromIdx) || !IsFiniteInt(toNode, toIdx) ||
					fromIdx < 0 || fromIdx >= core_.formation.slotCount ||
					toIdx < 0 || toIdx >= core_.formation.slotCount ||
					fromIdx == toIdx)
				{
					return MakeErrorJson("from_slot and to_slot must be distinct valid slot indices", "InvalidArgument", action);
				}
				if (core_.captureSuspended || !core_.InjectionAllowed())
					return MakeErrorJson("Injection is blocked in the current context", "Protected", action);
				if (!core_.SwapSlots(static_cast<int>(fromIdx), static_cast<int>(toIdx)))
					return MakeErrorJson("Failed to swap slots", "Failed", action);
			}
			else if (action != "status")
			{
				return MakeErrorJson("Unknown formation action: " + action, "InvalidArgument", action);
			}

			const auto& fm = core_.formation;
			std::string slotsJson = "[";
			for (size_t i = 0; i < fm.slots.size(); ++i)
			{
				const auto& s = fm.slots[i];
				if (i) slotsJson += ",";
				MotionPlayer* track = core_.TrackForSlot(s.slotIndex);
				if (!track && !fm.active && s.occupied && s.slotIndex == 0 && core_.player.clip.total_frames > 0)
				{
					track = &core_.player;
				}
				const bool hasTrack = track != nullptr && track->clip.total_frames > 0;
				const bool bound = track != nullptr && track->controller.currentBinding.bindPoseCaptured;
				const bool built = hasTrack || bound;
				std::string sourceMode = "empty";
				if (s.occupied)
				{
					if (hasTrack)
					{
						if (track->currentMotionSlot == s.sourceSlotIndex + 1)
							sourceMode = "slot_" + std::to_string(track->currentMotionSlot);
						else
							sourceMode = "fallback_slot_" + std::to_string(track->currentMotionSlot);
					}
					else
					{
						sourceMode = "unloaded";
					}
				}
				slotsJson += "{\"index\":" + JNum(s.slotIndex) +
					",\"slotIndex\":" + JNum(s.slotIndex) +
					",\"occupied\":" + JBool(s.occupied) +
					",\"actorToken\":" + JUInt(s.actorToken) +
					",\"actorInstanceId\":" + JNum(s.actorInstanceId) +
					",\"character\":" + JStr(s.character) +
					",\"displayName\":" + JStr(s.displayName) +
					",\"shortName\":" + JStr(s.shortName) +
					",\"sourceSlotIndex\":" + JNum(s.sourceSlotIndex) +
					",\"sourceMode\":" + JStr(sourceMode) +
					",\"built\":" + JBool(built) +
					",\"hasTrack\":" + JBool(hasTrack) +
					",\"bound\":" + JBool(bound) +
					",\"isOverflow\":" + JBool(s.isOverflow) +
					",\"physicsBackend\":" + JStr(s.physicsBackend) +
					",\"offset\":{\"x\":" + JNum(s.offset.x) +
					",\"y\":" + JNum(s.offset.y) +
					",\"z\":" + JNum(s.offset.z) +
					",\"yawDegrees\":" + JNum(s.yawDegrees) + "}}";
			}
			slotsJson += "]";

			return "{\"ok\":true,\"action\":" + JStr(action) +
				",\"active\":" + JBool(fm.active) +
				",\"available\":" + JBool(fm.available) +
				",\"verified\":" + JBool(fm.verified) +
				",\"isGroupSong\":" + JBool(fm.isGroupSong) +
				",\"trackPositionCount\":" + JNum(fm.trackPositionCount) +
				",\"slotCount\":" + JNum(fm.slotCount) +
				",\"selectedSlotIndex\":" + JNum(fm.selectedSlotIndex) +
				",\"revision\":" + JUInt(fm.revision) +
				",\"actorEpoch\":" + JUInt(fm.actorEpoch) +
				",\"slots\":" + slotsJson + "}";
		}

		std::string HandleMotionExperiment(const jsonlite::Value& args)
		{
			bool hasBeforeQuartz = false, beforeQuartz = false;
			bool hasBackend = false; int backend = 2;
			bool hasPreserveAnimator = false, preserveAnimator = false;
			bool hasMikuMouthFit = false; int mikuMouthFitTri = -1;
			double mouthWeight = 1.0;
			const auto* mouthWeightParam = FindAlias(args, "mouth_weight", "mouthWeight");
			if (mouthWeightParam && (!IsFiniteNumber(mouthWeightParam, mouthWeight) || mouthWeight < 0 || mouthWeight > 2))
				return MakeErrorJson("mouth_weight must be a number within 0..2", "InvalidArgument");
			bool hasWaistSupport = false, waistSupport = true;

			if (const auto* v = FindAlias(args, "before_quartz_experiment", "beforeQuartz"))
			{
				if (!v->IsBool())
					return MakeErrorJson("before_quartz_experiment must be a boolean", "InvalidArgument");
				hasBeforeQuartz = true;
				beforeQuartz = v->boolValue;
			}
			if (const auto* v = FindAlias(args, "secondary_physics_backend", "secondaryPhysicsBackend"))
			{
				hasBackend = true;
				if (v->IsString())
				{
					if (v->stringValue == "off") backend = 0;
					else if (v->stringValue == "fallback") backend = 1;
					else if (v->stringValue == "native") backend = 2;
					else return MakeErrorJson("secondary_physics_backend must be 0/1/2 or off/fallback/native", "InvalidArgument");
				}
				else
				{
					int64_t iv = -1;
					if (!IsFiniteInt(v, iv) || iv < 0 || iv > 2)
						return MakeErrorJson("secondary_physics_backend must be 0, 1, or 2", "InvalidArgument");
					backend = static_cast<int>(iv);
				}
			}
			if (const auto* v = FindAlias(args, "preserve_animator", "preserveAnimator"))
			{
				if (!v->IsBool())
					return MakeErrorJson("preserve_animator must be a boolean", "InvalidArgument");
				hasPreserveAnimator = true;
				preserveAnimator = v->boolValue;
			}
			if (const auto* v = FindAlias(args, "miku_mouth_fit", "mikuMouthFit"))
			{
				hasMikuMouthFit = true;
				if (v->IsNull()) mikuMouthFitTri = -1;
				else if (v->IsBool()) mikuMouthFitTri = v->boolValue ? 1 : 0;
				else
				{
					int64_t iv = -2;
					if (!IsFiniteInt(v, iv) || iv < -1 || iv > 1)
						return MakeErrorJson("miku_mouth_fit must be boolean, null, or -1/0/1", "InvalidArgument");
					mikuMouthFitTri = static_cast<int>(iv);
				}
			}
			if (const auto* v = FindAlias(args, "waist_support", "waistSupport"))
			{
				if (!v->IsBool())
					return MakeErrorJson("waist_support must be a boolean", "InvalidArgument");
				hasWaistSupport = true;
				waistSupport = v->boolValue;
			}

			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (mouthWeightParam && (core_.captureSuspended || !core_.InjectionAllowed()))
				return MakeErrorJson("Injection is blocked in the current context", "Protected");
			const auto* slot = core_.formation.SelectedSlot();
			auto facialWeights = core_.formation.active && slot ? slot->facialWeights : core_.player.controller.facialWeights;
			if (mouthWeightParam)
			{
				if (!core_.initialized)
					return MakeErrorJson("Core is not initialized", "NotReady");
				facialWeights.mouth = static_cast<float>(mouthWeight);
				if (!core_.SetSlotFacialWeights(core_.formation.selectedSlotIndex, facialWeights))
					core_.player.controller.facialWeights = facialWeights;
			}
			const bool updated = hasBeforeQuartz || hasBackend || hasPreserveAnimator || hasMikuMouthFit || hasWaistSupport || mouthWeightParam;

			auto syncPreserveAnimator = [&](bool enabled)
			{
				core_.config.preserveAnimatorForSecondaryPhysics = enabled ? 1 : 0;
				core_.applied.preserveAnimatorForSecondaryPhysics = enabled ? 1 : 0;
				core_.player.controller.preserveAnimatorForSecondaryPhysics = enabled;
				for (auto& kv : core_.formationTracks)
				{
					if (kv.second) kv.second->controller.preserveAnimatorForSecondaryPhysics = enabled;
				}
			};

			if (hasBackend)
			{
				core_.config.secondaryPhysicsBackend = backend;
				core_.guiPhysicsBackend.store(backend, std::memory_order_release);
				if (!hasBeforeQuartz)
				{
					const bool bq = (backend == 2);
					core_.config.beforeQuartzExperiment = bq;
					core_.guiBeforeQuartzExperiment.store(bq, std::memory_order_release);
				}
				if (!hasPreserveAnimator)
				{
					syncPreserveAnimator(backend == 2);
				}
			}
			if (hasBeforeQuartz)
			{
				core_.config.beforeQuartzExperiment = beforeQuartz;
				core_.guiBeforeQuartzExperiment.store(beforeQuartz, std::memory_order_release);
			}
			if (hasPreserveAnimator)
			{
				syncPreserveAnimator(preserveAnimator);
			}
			if (hasMikuMouthFit)
			{
				core_.config.facialMikuMouthFit = mikuMouthFitTri;
				core_.applied.facialMikuMouthFit = mikuMouthFitTri;
				if (mikuMouthFitTri >= 0)
				{
					const bool enabled = (mikuMouthFitTri != 0);
					auto applyMiku = [enabled](IdolController& ctrl)
					{
						ctrl.facialMikuMouthFit = enabled;
						for (auto& pair : ctrl.bindings) pair.second.face.enableMikuMouthFit = enabled;
						ctrl.currentBinding.face.enableMikuMouthFit = enabled;
					};
					applyMiku(core_.player.controller);
					for (auto& kv : core_.formationTracks)
					{
						if (kv.second) applyMiku(kv.second->controller);
					}
				}
			}
			if (hasWaistSupport)
			{
				core_.config.experimentalWaistSupport = waistSupport ? 1 : 0;
				core_.applied.experimentalWaistSupport = waistSupport ? 1 : 0;
				core_.guiExperimentalWaistSupport.store(waistSupport, std::memory_order_release);
				core_.player.controller.SetExperimentalWaistSupportEnabled(waistSupport);
				for (auto& kv : core_.formationTracks)
				{
					if (kv.second) kv.second->controller.SetExperimentalWaistSupportEnabled(waistSupport);
				}
			}

			const bool curMouth = core_.config.facialMikuMouthFit >= 0
				? (core_.config.facialMikuMouthFit != 0)
				: core_.player.controller.facialMikuMouthFit;

			return "{\"ok\":true,\"updated\":" + JBool(updated) +
				",\"before_quartz_experiment\":" + JBool(core_.config.beforeQuartzExperiment) +
				",\"secondary_physics_backend\":" + JNum(core_.config.secondaryPhysicsBackend) +
				",\"secondary_physics_backend_name\":" + JStr(PhysicsBackendName(core_.config.secondaryPhysicsBackend)) +
				",\"preserve_animator\":" + JBool(core_.player.controller.preserveAnimatorForSecondaryPhysics) +
				",\"miku_mouth_fit\":" + JBool(curMouth) +
				",\"mouth_weight\":" + JNum(facialWeights.mouth) +
				",\"waist_support\":" + JBool(core_.player.controller.IsExperimentalWaistSupportEnabled()) + "}";
		}

		static std::string PhysicsFieldsJson(const GakuPhysicsDiagnosticSnapshot& p)
		{
			return "\"version\":" + JUInt(p.version) +
				",\"requestedBackend\":" + JUInt(p.requested_backend) +
				",\"actualBackend\":" + JUInt(p.actual_backend) +
				",\"bridgeState\":" + JUInt(p.bridge_state) +
				",\"solvedBones\":" + JUInt(p.solved_bones) +
				",\"boundBones\":" + JUInt(p.bound_bones) +
				",\"bridgeFault\":" + JUInt(p.bridge_fault) +
				",\"actor\":" + JUInt(p.actor) +
				",\"sequence\":" + JUInt(p.sequence) +
				",\"unityFrame\":" + JNum(p.unity_frame) +
				",\"playbackState\":" + JNum(p.playback_state) +
				",\"clipTime\":" + JNum(p.clip_time) +
				",\"bodyCalls\":" + JUInt(p.body_calls) +
				",\"dispatchCalls\":" + JUInt(p.dispatch_calls) +
				",\"dispatchLastNs\":" + JUInt(p.dispatch_last_ns) +
				",\"dispatchMaxNs\":" + JUInt(p.dispatch_max_ns) +
				",\"fallbackCalls\":" + JUInt(p.fallback_calls) +
				",\"fallbackClamps\":" + JUInt(p.fallback_clamps) +
				",\"fallbackResets\":" + JUInt(p.fallback_resets) +
				",\"bridgePublishCalls\":" + JUInt(p.bridge_publish_calls) +
				",\"bridgeNativeCalls\":" + JUInt(p.bridge_native_calls) +
				",\"nativeSolverCalls\":" + JUInt(p.native_solver_calls) +
				",\"bridgeFailures\":" + JUInt(p.bridge_failures) +
				",\"bridgeExpiredSkips\":" + JUInt(p.bridge_expired_skips) +
				",\"bridgeIdentitySkips\":" + JUInt(p.bridge_identity_skips);
		}

		std::string HandleDiagPhysics(const jsonlite::Value& args)
		{
			int64_t requestedSlot = -1;
			bool hasRequestedSlot = false;
			if (const auto* si = FindAlias(args, "slot_index", "slotIndex"))
			{
				if (!IsFiniteInt(si, requestedSlot) || requestedSlot < 0)
					return MakeErrorJson("slot_index must be a non-negative integer", "InvalidArgument");
				hasRequestedSlot = true;
			}

			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (core_.formation.active)
			{
				const auto ensemble = hooks_.readEnsemblePhysics
					? hooks_.readEnsemblePhysics()
					: std::vector<PhysicsSlotView>{};
				const size_t totalSlots = (std::max)(core_.formation.slots.size(), ensemble.size());
				int primaryIndex = -1;
				if (hasRequestedSlot && static_cast<size_t>(requestedSlot) < ensemble.size() &&
					ensemble[static_cast<size_t>(requestedSlot)].snapshot.actor != 0)
				{
					primaryIndex = static_cast<int>(requestedSlot);
				}
				if (primaryIndex < 0 && core_.formation.selectedSlotIndex >= 0 &&
					static_cast<size_t>(core_.formation.selectedSlotIndex) < ensemble.size() &&
					ensemble[static_cast<size_t>(core_.formation.selectedSlotIndex)].snapshot.actor != 0)
				{
					primaryIndex = core_.formation.selectedSlotIndex;
				}
				if (primaryIndex < 0)
				{
					for (size_t i = 0; i < ensemble.size(); ++i)
					{
						if (ensemble[i].snapshot.actor != 0)
						{
							primaryIndex = static_cast<int>(i);
							break;
						}
					}
				}

				std::string slotsJson = "[";
				bool firstSlot = true;
				for (size_t i = 0; i < totalSlots; ++i)
				{
					const StageSlotState* fmSlot = i < core_.formation.slots.size() ? &core_.formation.slots[i] : nullptr;
					const PhysicsSlotView* phSlot = i < ensemble.size() ? &ensemble[i] : nullptr;
					const bool hasDiag = phSlot && phSlot->snapshot.actor != 0;
					const bool occupied = fmSlot && fmSlot->occupied;
					if (!occupied && !hasDiag) continue;
					if (!firstSlot) slotsJson += ",";
					firstSlot = false;
					const GakuPhysicsDiagnosticSnapshot emptySnap{};
					const auto& snap = phSlot ? phSlot->snapshot : emptySnap;
					slotsJson += "{\"slotIndex\":" + JNum(static_cast<double>(i)) +
						",\"occupied\":" + JBool(occupied) +
						",\"actorToken\":" + JUInt(fmSlot ? fmSlot->actorToken : 0) +
						",\"character\":" + JStr(fmSlot ? fmSlot->character : "") +
						",\"displayName\":" + JStr(fmSlot ? fmSlot->displayName : "") +
						",\"physicsBackend\":" + JStr(fmSlot ? fmSlot->physicsBackend : "") +
						",\"reason\":" + JStr(phSlot ? phSlot->reason : "") +
						",\"available\":" + JBool(hasDiag) +
						"," + PhysicsFieldsJson(snap) + "}";
				}
				slotsJson += "]";

				if (primaryIndex >= 0)
				{
					const auto& primary = ensemble[static_cast<size_t>(primaryIndex)];
					return "{\"ok\":true,\"available\":true,\"formationActive\":true"
						",\"selectedSlotIndex\":" + JNum(core_.formation.selectedSlotIndex) +
						",\"primarySlotIndex\":" + JNum(primaryIndex) +
						",\"reason\":" + JStr(primary.reason) +
						"," + PhysicsFieldsJson(primary.snapshot) +
						",\"slots\":" + slotsJson + "}";
				}
				return "{\"ok\":false,\"available\":false,\"formationActive\":true"
					",\"selectedSlotIndex\":" + JNum(core_.formation.selectedSlotIndex) +
					",\"reason\":\"no_physics_snapshot_for_actor\""
					",\"slots\":" + slotsJson + "}";
			}

			const PhysicsSlotView single = hooks_.readSinglePhysics ? hooks_.readSinglePhysics() : PhysicsSlotView{};
			if (single.snapshot.actor == 0)
			{
				return "{\"ok\":false,\"available\":false,\"formationActive\":false"
					",\"reason\":\"no_physics_snapshot_for_actor\"}";
			}
			return "{\"ok\":true,\"available\":true,\"formationActive\":false"
				",\"reason\":" + JStr(single.reason) +
				"," + PhysicsFieldsJson(single.snapshot) + "}";
		}

		std::string HandleDiagFace(const jsonlite::Value& args)
		{
			bool onlyActive = true;
			if (const auto* v = FindAlias(args, "only_active", "onlyActive"))
			{
				if (!v->IsBool()) return MakeErrorJson("only_active must be a boolean", "InvalidArgument");
				onlyActive = v->boolValue;
			}
			bool onlyDiff = false;
			if (const auto* v = FindAlias(args, "only_diff", "onlyDiff"))
			{
				if (!v->IsBool()) return MakeErrorJson("only_diff must be a boolean", "InvalidArgument");
				onlyDiff = v->boolValue;
			}
			float eps = 0.0001f;
			if (const auto* v = args.Find("eps"))
			{
				double d = 0.0;
				if (!IsFiniteNumber(v, d) || d < 0.0)
					return MakeErrorJson("eps must be a non-negative finite number", "InvalidArgument");
				eps = static_cast<float>(d);
			}
			std::string filter;
			if (const auto* v = args.Find("filter"))
			{
				if (!v->IsString()) return MakeErrorJson("filter must be a string", "InvalidArgument");
				filter = v->stringValue;
			}
			uint32_t limit = 64u;
			if (const auto* v = args.Find("limit"))
			{
				int64_t lv = 0;
				if (!IsFiniteInt(v, lv) || lv < 1)
					return MakeErrorJson("limit must be a positive integer", "InvalidArgument");
				limit = static_cast<uint32_t>((std::min)(lv, static_cast<int64_t>(GAKU_FACE_DIAGNOSTICS_CAPACITY)));
			}
			int64_t requestedSlot = -1;
			bool hasRequestedSlot = false;
			if (const auto* si = FindAlias(args, "slot_index", "slotIndex"))
			{
				if (!IsFiniteInt(si, requestedSlot) || requestedSlot < 0)
					return MakeErrorJson("slot_index must be a non-negative integer", "InvalidArgument");
				hasRequestedSlot = true;
			}

			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (core_.player.state == PlaybackState::Stopped)
			{
				return "{\"ok\":false,\"available\":false,\"reason\":\"no_face_snapshot_for_actor\"}";
			}

			void* actor = nullptr;
			MotionPlayer* track = nullptr;
			int resolvedSlotIndex = -1;

			auto tryActor = [&](void* candidate, int slotIdx) -> bool
			{
				if (!candidate) return false;
				auto* t = core_.PlayerForActor(candidate);
				if (!t) t = &core_.player;
				if (t->controller.bindings.find(candidate) != t->controller.bindings.end())
				{
					actor = candidate;
					track = t;
					resolvedSlotIndex = slotIdx;
					return true;
				}
				return false;
			};

			if (hasRequestedSlot)
			{
				if (static_cast<size_t>(requestedSlot) < core_.formation.slots.size())
				{
					const auto& s = core_.formation.slots[static_cast<size_t>(requestedSlot)];
					if (s.occupied && s.actorToken != 0)
					{
						tryActor(core_.sceneActorRegistry.Resolve(s.actorToken, core_.formation.actorEpoch),
						         static_cast<int>(requestedSlot));
					}
				}
			}
			else if (core_.formation.active)
			{
				if (const auto* sel = core_.formation.SelectedSlot(); sel && sel->occupied && sel->actorToken != 0)
				{
					tryActor(core_.sceneActorRegistry.Resolve(sel->actorToken, core_.formation.actorEpoch),
					         sel->slotIndex);
				}
				if (!actor)
				{
					for (const auto& s : core_.formation.slots)
					{
						if (s.occupied && s.actorToken != 0 &&
							tryActor(core_.sceneActorRegistry.Resolve(s.actorToken, core_.formation.actorEpoch), s.slotIndex))
							break;
					}
				}
			}
			else
			{
				if (!tryActor(core_.player.controller.targetActor, core_.formation.selectedSlotIndex) &&
					!tryActor(core_.currentActor, core_.formation.selectedSlotIndex))
				{
					if (!core_.player.controller.bindings.empty())
					{
						tryActor(core_.player.controller.bindings.begin()->first, core_.formation.selectedSlotIndex);
					}
				}
			}

			if (!actor || !track)
			{
				return "{\"ok\":false,\"available\":false,\"reason\":\"no_face_snapshot_for_actor\"}";
			}
			auto it = track->controller.bindings.find(actor);
			if (it == track->controller.bindings.end())
			{
				return "{\"ok\":false,\"available\":false,\"reason\":\"no_face_snapshot_for_actor\"}";
			}

			auto& face = it->second.face;
			face.Observe();

			const uint32_t total = static_cast<uint32_t>(
				(std::min)(face.diagnosticChannels.size(), static_cast<size_t>(GAKU_FACE_DIAGNOSTICS_CAPACITY)));
			std::string channelsJson = "[";
			uint32_t emitted = 0;
			uint32_t matched = 0;
			for (uint32_t i = 0; i < total; ++i)
			{
				const auto& ch = face.diagnosticChannels[i];
				if (!filter.empty() && std::string(ch.name).find(filter) == std::string::npos)
					continue;
				if (onlyActive &&
					std::fabs(ch.target) <= eps &&
					std::fabs(ch.before) <= eps &&
					std::fabs(ch.after) <= eps)
					continue;
				if (onlyDiff && (ch.index < 0 || std::fabs(ch.target - ch.after) <= eps))
					continue;

				++matched;
				if (emitted < limit)
				{
					if (emitted > 0) channelsJson += ",";
					++emitted;
					channelsJson += "{\"name\":" + JStr(ch.name) +
						",\"index\":" + JNum(ch.index) +
						",\"target\":" + JNum(ch.target) +
						",\"before\":" + JNum(ch.before) +
						",\"after\":" + JNum(ch.after) + "}";
				}
			}
			channelsJson += "]";

			return "{\"ok\":true,\"available\":true"
				",\"status\":" + JUInt(face.diagnosticStatus) +
				",\"channelCount\":" + JUInt(total) +
				",\"sequence\":" + JUInt(face.diagnosticSequence) +
				",\"unityFrame\":" + JNum(face.diagnosticUnityFrame) +
				",\"playbackState\":" + JNum(static_cast<int>(core_.player.state)) +
				",\"clipTime\":" + JNum(face.diagnosticTime) +
				",\"actor\":" + JUInt(reinterpret_cast<uintptr_t>(actor)) +
				",\"slotIndex\":" + JNum(resolvedSlotIndex) +
				",\"channels\":" + channelsJson +
				",\"matchedCount\":" + JUInt(matched) +
				",\"truncated\":" + JBool(matched > emitted) + "}";
		}

		std::string HandleDiagBurst(const jsonlite::Value& args)
		{
			bool dumpTrace = false;
			if (const auto* dt = FindAlias(args, "dump_trace", "dumpTrace"))
			{
				if (!dt->IsBool()) return MakeErrorJson("dump_trace must be a boolean", "InvalidArgument");
				dumpTrace = dt->boolValue;
			}

			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			const BurstSummaryView summary = hooks_.readBurstSummary
				? hooks_.readBurstSummary()
				: BurstSummaryView{};

			bool traceDumped = false;
			int traceResult = GBB_E_UNAVAILABLE;
			std::string tracePathUtf8;
			if (dumpTrace && !core_.diagnosticsRoot.empty() && summary.dumpTraces)
			{
				std::error_code ec;
				const auto dir = MotionPathFromUtf8(core_.diagnosticsRoot);
				std::filesystem::create_directories(dir, ec);
				const auto tracePath = dir / "before_quartz_trace.json";
				tracePathUtf8 = MotionPathToUtf8(tracePath);
				traceResult = summary.dumpTraces(tracePath.wstring().c_str());
				traceDumped = (traceResult == GBB_OK);
			}

			auto statusFields = [](const GbbStatus& st, const GbbPerf& perf, const std::string& reason) -> std::string
			{
				return "\"state\":" + JUInt(st.state) +
					",\"stateName\":" + JStr(BurstStateName(st.state)) +
					",\"reason\":" + JStr(reason) +
					",\"fault\":" + JBool(st.injection_fault != 0 || st.state == GBB_STATE_FAULTED) +
					",\"injectionFault\":" + JUInt(st.injection_fault) +
					",\"writes\":" + JUInt(st.writes) +
					",\"injected\":" + JBool(st.injected != 0) +
					",\"actor\":" + JUInt(reinterpret_cast<uintptr_t>(st.actor)) +
					",\"sequence\":" + JUInt(st.sequence) +
					",\"generation\":" + JUInt(st.generation) +
					",\"matchedPasses\":" + JUInt(st.matched_passes) +
					",\"failedPasses\":" + JUInt(st.failed_passes) +
					",\"swingCalls\":" + JUInt(st.swing_calls) +
					",\"sourceFrame\":" + JNum(st.source_frame) +
					",\"clipTime\":" + JNum(st.clip_time) +
					",\"deltaTime\":" + JNum(st.delta_time) +
					",\"profile\":" + JStr(st.profile) +
					",\"detail\":" + JStr(st.detail) +
					",\"publishCalls\":" + JUInt(perf.publish_calls) +
					",\"nativePasses\":" + JUInt(perf.native_passes) +
					",\"solverCalls\":" + JUInt(perf.solver_calls) +
					",\"expiredSkips\":" + JUInt(perf.expired_skips) +
					",\"identitySkips\":" + JUInt(perf.identity_skips);
			};

			std::string slotsJson = "[";
			for (size_t i = 0; i < summary.multiSlots.size(); ++i)
			{
				const auto& ms = summary.multiSlots[i];
				if (i) slotsJson += ",";
				slotsJson += "{\"slotIndex\":" + JNum(static_cast<double>(i)) +
					",\"token\":" + JUInt(ms.token) +
					",\"faulted\":" + JBool(ms.faulted) +
					",\"outstanding\":" + JBool(ms.outstanding) +
					",\"minimumFrame\":" + JNum(ms.minimumFrame) +
					"," + statusFields(ms.status, ms.perf, ms.reason) + "}";
			}
			slotsJson += "]";

			bool useMulti = core_.formation.active && !summary.multiSlots.empty();
			const BurstSlotView* primaryMulti = nullptr;
			if (useMulti)
			{
				const int sel = core_.formation.selectedSlotIndex;
				if (sel >= 0 && static_cast<size_t>(sel) < summary.multiSlots.size())
				{
					const auto& candidate = summary.multiSlots[static_cast<size_t>(sel)];
					if (candidate.token != 0 || candidate.actor != nullptr || candidate.reason != "not_requested")
						primaryMulti = &candidate;
				}
				if (!primaryMulti)
				{
					for (const auto& candidate : summary.multiSlots)
					{
						if (candidate.token != 0 || candidate.actor != nullptr || candidate.reason != "not_requested")
						{
							primaryMulti = &candidate;
							break;
						}
					}
				}
				if (!primaryMulti && summary.singleReason != "not_requested")
					useMulti = false;
			}

			const GbbStatus& topStatus = primaryMulti ? primaryMulti->status : summary.singleStatus;
			const GbbPerf& topPerf = primaryMulti ? primaryMulti->perf : summary.singlePerf;
			const std::string& topReason = primaryMulti
				? primaryMulti->reason
				: (useMulti ? summary.multiReason : summary.singleReason);
			const bool available = (topStatus.state != GBB_STATE_UNINITIALIZED &&
			                        topStatus.state != GBB_STATE_UNAVAILABLE) ||
				topReason == "native" || topReason == "discovering" || topReason == "native_pending";

			std::string json = "{\"ok\":true,\"available\":" + JBool(available) +
				",\"mode\":" + JStr(useMulti ? "multi" : "single") +
				",\"beforeQuartz\":" + JBool(summary.beforeQuartz) +
				",\"sleeveDiagnostics\":" + JBool(summary.sleeveDiagnostics) +
				",\"requestedBackend\":" + JNum(summary.requestedBackend) +
				",\"actualBackend\":" + JNum(summary.actualBackend) +
				"," + statusFields(topStatus, topPerf, topReason) +
				",\"multiReason\":" + JStr(summary.multiReason) +
				",\"multiHasCaptureInput\":" + JBool(summary.multiHasCaptureInput) +
				",\"singleReason\":" + JStr(summary.singleReason) +
				",\"singleHasCaptureInput\":" + JBool(summary.singleHasCaptureInput) +
				",\"slots\":" + slotsJson +
				",\"single\":{" + statusFields(summary.singleStatus, summary.singlePerf, summary.singleReason) + "}";
			if (dumpTrace)
			{
				json += ",\"traceDumped\":" + JBool(traceDumped) +
					",\"traceResult\":" + JNum(traceResult) +
					",\"tracePath\":" + JStr(tracePathUtf8);
			}
			json += "}";
			return json;
		}

		Core& core_;
		AdapterHooks hooks_;
		mutable std::mutex connectMutex_;
		mutable std::mutex leaseMutex_;
		GakuAutoToolsRegistryV1 registry_{};
		std::atomic<bool> connected_{false};
		std::atomic<bool> shuttingDown_{false};
		std::atomic<uint64_t> tickCounter_{0};
		std::atomic<uint64_t> nextRetryTick_{0};
		uint64_t sceneNodesLease_{0};
		uint64_t sceneNodesRevision_{0};
	};
} // namespace GakuMotion::autotools
