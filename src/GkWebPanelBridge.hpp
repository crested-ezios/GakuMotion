#pragma once

// =============================================================================
// GkWebPanelBridge.hpp — gakumotion.web-panel/v1 provider bridge
// =============================================================================
// Implements the GakuWebPanelServiceV1 domain semantics for the Loader's
// WebView2 manager panel. The wire contract mirrors the frontend authority
// (frontend/src/motion/protocol.ts and
// frontend/docs/motion-bridge-protocol.md):
//   request  {v:1, id, type:"request", method, params}
//   response {v:1, type:"response", id, ok, result | error:{code,message,details?}}
//   event    {v:1, type:"event", name, payload, seq, sid}
//
// Threading: every ABI callback except MotionShutdown's silent Invalidate runs
// on the Loader desktop UI thread. Core state is read/written under
// Core::stateMutex; mutations are queued through Core::QueueGui exactly like
// the ImGui overlay (GuiAction + revision/session guards) and drain on the
// Unity main thread. The event sink is only ever invoked from UI-thread
// callbacks (OnUiTick / HandleMessage / OnShutdown), never from Unity.
//
// The header is Windows-free so headless contract tests can drive it with a
// real Core and fake host hooks (tests/test_web_panel_bridge.cpp).
// =============================================================================

#include "../include/GakuWebPanel.h"
#include "GakuMotion.hpp"
#include "GkJsonLite.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace GakuMotion::webpanel
{
	// -------------------------------------------------------------------------
	// Minimal JSON writer (jsonlite is parse-only by design).
	// -------------------------------------------------------------------------
	inline void AppendEscaped(std::string& out, const std::string& text)
	{
		out.push_back('"');
		for (const unsigned char ch : text)
		{
			switch (ch)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (ch < 0x20)
				{
					char buffer[8]{};
					std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(ch));
					out += buffer;
				}
				else out.push_back(static_cast<char>(ch));
			}
		}
		out.push_back('"');
	}

	inline std::string JString(const std::string& text)
	{
		std::string out;
		AppendEscaped(out, text);
		return out;
	}

	inline std::string JNum(double value)
	{
		if (!std::isfinite(value)) return "0";
		char buffer[48]{};
		if (value == std::floor(value) && std::fabs(value) < 9.0e15)
			std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
		else
			std::snprintf(buffer, sizeof(buffer), "%.17g", value);
		return buffer;
	}

	inline std::string JBool(bool value) { return value ? "true" : "false"; }

	inline std::string JVec(const Vector3& value)
	{
		return "{\"x\":" + JNum(value.x) + ",\"y\":" + JNum(value.y) + ",\"z\":" + JNum(value.z) + "}";
	}

	inline std::string JQuat(const Quaternion& value)
	{
		return "{\"x\":" + JNum(value.x) + ",\"y\":" + JNum(value.y) +
			",\"z\":" + JNum(value.z) + ",\"w\":" + JNum(value.w) + "}";
	}

	inline std::string JColor3(const std::array<float, 3>& value)
	{
		return "[" + JNum(value[0]) + "," + JNum(value[1]) + "," + JNum(value[2]) + "]";
	}

	// -------------------------------------------------------------------------
	// Host hooks supplied by the plugin TU (Windows/services) or by tests.
	// -------------------------------------------------------------------------
	struct DropPeek
	{
		bool ok{false};
		std::string visual{"none"}; // none | armed | hover | invalid
		uint32_t pendingCount{0};
		uint32_t fileCount{0};
		uint32_t accepted{0};
		std::vector<std::string> dragPaths;
		bool hostElevated{false};
		bool hostBlocked{false};
	};

	struct DropBatch
	{
		bool ok{false};
		std::vector<std::string> paths;
		std::string notice;
	};

	struct AppearanceInfo
	{
		bool ok{false};
		int theme{0};
		float uiScale{1.0f};
	};

	struct AudioEffectPresetView
	{
		std::string id;
		std::string name;
		std::string category;
		std::string description;
	};

	struct AudioEffectState
	{
		bool available{false};
		std::string activeId{"flat"};
		std::vector<AudioEffectPresetView> presets;
	};

	struct AudioProbeView
	{
		bool valid{false};
		uint32_t sampleRate{0};
		uint32_t channels{0};
		bool hasCover{false};
		double durationSeconds{0.0};
		std::string codec;
		std::string title;
		std::string artist;
		std::string album;
		std::string coverDataUri;
	};

	struct ScreenshotCapabilitiesView
	{
		bool available{false};
		bool runtimeReady{false};
		bool cameraAvailable{false};
		uint32_t windowWidth{0};
		uint32_t windowHeight{0};
		std::string notice;
	};

	struct ScreenshotCaptureView
	{
		bool ok{false};
		uint32_t width{0};
		uint32_t height{0};
		std::string dataUri;
		std::string error;
	};

	struct HostHooks
	{
		// Ordered asset-directory candidates supplied by the plugin (the plugin
		// module directory's GakuMotion.UI). GetAssets picks the first one that
		// actually contains index.html.
		std::function<std::vector<std::wstring>()> assetsDirs;
		std::function<bool(std::string& notice)> openLibraryFolder;
		std::function<bool(int webKind, bool multiple, std::string& notice)> beginWebFileDialog;
		std::function<void()> releaseFileDialog;
		std::function<bool(bool enabled, std::string& notice)> dropSetTarget;
		std::function<DropPeek()> dropPeek;
		std::function<DropBatch()> dropConsume;
		std::function<AppearanceInfo()> appearance;
		std::function<AudioProbeView(const std::string& pathUtf8, bool includeCover)> probeAudio;
		std::function<std::string(const std::string& trackDirUtf8)> getTrackCoverDataUri;
		std::function<std::string(const std::string& pathUtf8)> readImagePreviewDataUri;
		std::function<ScreenshotCapabilitiesView()> getScreenshotCapabilities;
		std::function<ScreenshotCaptureView()> captureScreenshot;
		std::function<AudioEffectState()> getAudioEffectsState;
		std::function<bool(const std::string& presetId, std::string& error)> setAudioEffectPreset;
		std::string pluginVersion{"0.1.1"};
	};

	// -------------------------------------------------------------------------
	// Command result helpers.
	// -------------------------------------------------------------------------
	struct CmdResult
	{
		bool ok{true};
		std::string body{"{}"};
		std::string code;
		std::string message;
		std::string details;

		static CmdResult Ok(std::string result = "{}") { CmdResult r; r.body = std::move(result); return r; }
		static CmdResult Err(std::string code_, std::string message_, std::string details_ = "")
		{
			CmdResult r;
			r.ok = false;
			r.code = std::move(code_);
			r.message = std::move(message_);
			r.details = std::move(details_);
			return r;
		}
	};

	inline constexpr const char* kSupportedMethods[] = {
		"bridge.hello", "bridge.ping", "motion.hello", "state.get",
		"player.play", "player.pause", "player.resume", "player.stop", "player.cycle",
		"player.seek", "player.seekFrame", "player.stepFrame",
		"player.scrubBegin", "player.scrubUpdate", "player.scrubEnd",
		"player.setSpeed", "player.setVolume", "player.saveVolume", "player.reload",
		"playlist.setMode",
		"library.refresh", "library.select", "library.delete", "library.openFolder",
		"library.import", "library.probeFiles", "library.startEdit",
		"library.probeAudio", "library.getCover", "library.setCover", "library.readImagePreview",
		"screenshot.capabilities", "screenshot.capture",
		"clock.setRunMode", "clock.configure",
		"audioEffect.set",
		"actors.selectTarget",
		"facial.setWeights", "facial.save",
		"position.set", "position.save",
		"camera.set", "camera.save",
		"stage.set", "stage.scan", "stage.toggleNode", "stage.setLoadReplacement",
		"settings.setPhysics",
		"shell.setTextInputActive",
		"appearance.get",
		"dialog.pickFile", "dialog.poll",
		"drop.setTarget", "drop.poll", "drop.peek",
		"ensemble.set", "ensemble.presetSave", "ensemble.presetLoad", "ensemble.probe",
		"bone.get", "bone.set", "bone.reset", "bone.resetAll", "bone.setMaster",
		"bone.reload", "bone.saveToMotion", "bone.saveToGlobal",
		"gaze.get", "gaze.set", "gaze.cycle",
		"sole.get", "sole.set", "sole.save", "sole.load",
		"solePitch.get", "solePitch.set",
		"studio.get", "studio.set", "studio.reset",
		"experiments.get", "experiments.set",
		"telemetry.get",
	};

	inline constexpr const char* kSupportedEvents[] = {
		"player.stateChanged", "permission.changed", "library.changed", "loaded.changed",
		"playlist.changed", "actors.changed", "camera.changed", "stage.changed",
		"stage.nodesChanged",
		"facial.changed", "position.changed", "ensemble.changed", "settings.changed",
		"import.progress", "appearance.changed", "bridge.closed",
		"gaze.changed", "bone.changed", "sole.changed", "solePitch.changed",
		"studio.changed", "experiments.changed",
		"clock.changed", "audioEffect.changed",
	};

	class Bridge
	{
		using GuiAction = Core::GuiAction;
		using GuiCommand = Core::GuiCommand;

	public:
		Bridge(Core& core, HostHooks hooks) : core_(core), hooks_(std::move(hooks))
		{
			sessionId_ = NextSessionId();
		}

		uint64_t SessionId() const { return sessionId_; }
		bool Closed() const { return closed_.load(std::memory_order_acquire); }

		// ---- ABI surface -----------------------------------------------------

		GakuStatus GetAssets(GakuWebPanelAssetsV1* output)
		{
			if (!output) return GAKU_E_INVALID_ARGUMENT;
			if (closed_.load(std::memory_order_acquire)) return GAKU_E_SHUTTING_DOWN;
			if (!hooks_.assetsDirs) return GAKU_E_UNAVAILABLE;
			for (const std::wstring& dir : hooks_.assetsDirs())
			{
				if (dir.empty()) continue;
				std::error_code ec;
				const std::filesystem::path entry = std::filesystem::path(dir) / L"index.html";
				if (!std::filesystem::is_regular_file(entry, ec) || ec) continue;
				assetsDirCache_ = dir;
				output->abiVersion = GAKU_WEB_PANEL_VERSION;
				output->structSize = sizeof(GakuWebPanelAssetsV1);
				output->assetsDir = assetsDirCache_.c_str();
				output->entryPath = L"index.html";
				output->virtualHost = L"gakumotion.invalid";
				return GAKU_OK;
			}
			return GAKU_E_UNAVAILABLE;
		}

		void OnShow()
		{
			visible_ = true;
			baselinePending_ = true;
		}

		void OnHide() { visible_ = false; }

		void SetEventSink(GakuWebPanelEventSink sink, void* userData)
		{
			std::lock_guard<std::mutex> lock(sinkMutex_);
			sink_ = sink;
			sinkUserData_ = userData;
			if (sink) baselinePending_ = true;
		}

		GakuStatus HandleMessage(const char* requestJson, uint32_t responseCapacity,
			char* responseJson, uint32_t* requiredBytes)
		{
			if (!requestJson) return GAKU_E_INVALID_ARGUMENT;
			std::string response;
			try { response = DispatchRequest(requestJson); }
			catch (...) { response = MakeError(0, "internal", "Unhandled bridge exception"); }
			forceDomainRefresh_.store(true, std::memory_order_release);
			const uint64_t needed = static_cast<uint64_t>(response.size()) + 1;
			if (needed > 0xFFFFFFFFull) return GAKU_E_LIMIT;
			if (requiredBytes) *requiredBytes = static_cast<uint32_t>(needed);
			if (!responseJson || responseCapacity < needed) return GAKU_E_SIZE;
			std::memcpy(responseJson, response.c_str(), static_cast<size_t>(needed));
			return GAKU_OK;
		}

		void OnUiTick()
		{
			if (closed_.load(std::memory_order_acquire)) return;
			try { Tick(); }
			catch (...) { /* a diagnostic tick must never break the host UI */ }
		}

		// UI thread: announce UI-host detach and reset per-session state so
		// rebinding the panel later starts a fresh session without unloading.
		void OnShutdown()
		{
			if (closed_.load(std::memory_order_acquire)) return;
			Emit("bridge.closed", "{\"reason\":\"service-shutdown\"}");
			if (dialogPending_ && hooks_.releaseFileDialog) hooks_.releaseFileDialog();
			dialogPending_ = false;
			visible_ = false;
			dropEnabled_ = false;
			if (hooks_.dropSetTarget && lastDropTargetSent_)
			{
				std::string ignored;
				hooks_.dropSetTarget(false, ignored);
			}
			lastDropTargetSent_ = false;
			cachedDropPeek_ = {};
			pendingDrops_.clear();
			{
				std::lock_guard<std::mutex> lock(sinkMutex_);
				sink_ = nullptr;
				sinkUserData_ = nullptr;
				sessionId_ = NextSessionId();
				seq_ = 0;
			}
			ResetChangeTracking();
		}

		// Any thread (MotionShutdown runs on Unity): permanent silent invalidation.
		void Invalidate(const char* /*reason*/)
		{
			closed_.store(true, std::memory_order_release);
			dialogPending_ = false;
			if (hooks_.releaseFileDialog) hooks_.releaseFileDialog();
			std::lock_guard<std::mutex> lock(sinkMutex_);
			sink_ = nullptr;
			sinkUserData_ = nullptr;
		}

		// ---- test helpers ----------------------------------------------------

		std::string HandleMessageForTest(const std::string& request) { return DispatchRequest(request.c_str()); }

	private:
		// ---- wire framing ----------------------------------------------------

		static std::string MakeOk(uint64_t id, const std::string& result)
		{
			return "{\"v\":1,\"type\":\"response\",\"id\":" + JNum(static_cast<double>(id)) +
				",\"ok\":true,\"result\":" + result + "}";
		}

		static std::string MakeError(uint64_t id, const std::string& code, const std::string& message,
			const std::string& details = "")
		{
			std::string error = "{\"code\":" + JString(code) + ",\"message\":" + JString(message);
			if (!details.empty()) error += ",\"details\":" + details;
			error += "}";
			return "{\"v\":1,\"type\":\"response\",\"id\":" + JNum(static_cast<double>(id)) +
				",\"ok\":false,\"error\":" + error + "}";
		}

		void Emit(const std::string& name, const std::string& payloadJson)
		{
			std::lock_guard<std::mutex> lock(sinkMutex_);
			if (!sink_ || closed_.load(std::memory_order_acquire)) return;
			const std::string json = "{\"v\":1,\"type\":\"event\",\"name\":" + JString(name) +
				",\"payload\":" + payloadJson + ",\"seq\":" + JNum(static_cast<double>(++seq_)) +
				",\"sid\":" + JNum(static_cast<double>(sessionId_)) + "}";
			sink_(sinkUserData_, json.c_str());
		}

		std::string DispatchRequest(const char* requestJson)
		{
			jsonlite::Value root;
			std::string parseError;
			if (!jsonlite::ParseText(requestJson, root, parseError) || !root.IsObject())
				return MakeError(0, "invalid_params", "Malformed request JSON");
			const jsonlite::Value* version = root.Find("v");
			const jsonlite::Value* type = root.Find("type");
			const jsonlite::Value* idValue = root.Find("id");
			const jsonlite::Value* methodValue = root.Find("method");
			uint64_t id = 0;
			if (idValue && idValue->IsNumber() && idValue->numberValue >= 0)
				id = static_cast<uint64_t>(idValue->numberValue);
			if (!version || !version->IsNumber() || version->numberValue != 1 ||
				!type || !type->IsString() || type->stringValue != "request" ||
				!idValue || !idValue->IsNumber() ||
				!methodValue || !methodValue->IsString() || methodValue->stringValue.empty())
				return MakeError(id, "invalid_params", "Request must carry v=1, type=request, numeric id and method");
			if (closed_.load(std::memory_order_acquire))
				return MakeError(id, "not_allowed", "Bridge is closed");
			const jsonlite::Value* params = root.Find("params");
			if (params && !params->IsObject() && !params->IsNull())
				return MakeError(id, "invalid_params", "params must be an object");
			static const jsonlite::Value emptyParams{[] { jsonlite::Value v; v.type = jsonlite::Type::Object; return v; }()};
			const std::string& method = methodValue->stringValue;
			CmdResult result;
			try { result = Dispatch(method, params ? *params : emptyParams); }
			catch (...) { result = CmdResult::Err("internal", "Unhandled method exception"); }
			if (result.ok) return MakeOk(id, result.body);
			return MakeError(id, result.code, result.message, result.details);
		}

		// ---- parameter helpers ------------------------------------------------

		static const jsonlite::Value* Param(const jsonlite::Value& params, const char* name)
		{
			return params.Find(name);
		}

		static bool NumberParam(const jsonlite::Value& params, const char* name, double& out)
		{
			const jsonlite::Value* value = params.Find(name);
			if (!value || !value->IsNumber() || !std::isfinite(value->numberValue)) return false;
			out = value->numberValue;
			return true;
		}

		static bool IntParam(const jsonlite::Value& params, const char* name, int64_t& out)
		{
			double number = 0;
			if (!NumberParam(params, name, number) || number != std::floor(number)) return false;
			out = static_cast<int64_t>(number);
			return true;
		}

		static bool BoolParam(const jsonlite::Value& params, const char* name, bool& out)
		{
			const jsonlite::Value* value = params.Find(name);
			if (!value || !value->IsBool()) return false;
			out = value->boolValue;
			return true;
		}

		static bool StringParam(const jsonlite::Value& params, const char* name, std::string& out)
		{
			const jsonlite::Value* value = params.Find(name);
			if (!value || !value->IsString()) return false;
			out = value->stringValue;
			return true;
		}

		static bool VecParam(const jsonlite::Value& params, const char* name, Vector3& out)
		{
			const jsonlite::Value* value = params.Find(name);
			if (!value || !value->IsObject()) return false;
			double x = 0, y = 0, z = 0;
			if (!NumberParam(*value, "x", x) || !NumberParam(*value, "y", y) || !NumberParam(*value, "z", z))
				return false;
			out = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
			return true;
		}

		// ---- dispatch ----------------------------------------------------------

		CmdResult Dispatch(const std::string& method, const jsonlite::Value& params)
		{
			if (method == "bridge.hello")
				return CmdResult::Ok("{\"protocol\":1,\"sessionId\":" + JNum(static_cast<double>(sessionId_)) + "}");
			if (method == "bridge.ping")
				return CmdResult::Ok("{\"pong\":true}");
			if (method == "motion.hello")
				return CmdResult::Ok(BuildHello());
			if (method == "state.get")
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				return CmdResult::Ok(BuildSnapshotLocked());
			}

			// Transport ---------------------------------------------------------
			if (method == "player.play") return TransportSimple(GuiAction::Play, true);
			if (method == "player.pause") return TransportSimple(GuiAction::Pause, false);
			if (method == "player.resume") return TransportSimple(GuiAction::Resume, true);
			if (method == "player.stop") return TransportSimple(GuiAction::Stop, false);
			if (method == "player.reload")
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				if (core_.loadedMotionKey.empty()) return CmdResult::Err("not_ready", "No motion is loaded");
				core_.QueueReloadMotion();
				ForcePlayerEvent();
				return CmdResult::Ok();
			}
			if (method == "player.cycle")
			{
				int64_t delta = 0;
				if (!IntParam(params, "delta", delta) || delta == 0 || delta < -100 || delta > 100)
					return CmdResult::Err("invalid_params", "delta must be a non-zero integer in [-100, 100]");
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				if (core_.EnsembleActive())
					return CmdResult::Err("protected", "Cycle is disabled while the ensemble stage is active");
				if (core_.motions.empty()) return CmdResult::Err("not_ready", "Motion library is empty");
				core_.QueueCycleMotion(static_cast<int>(delta));
				ForcePlayerEvent();
				return CmdResult::Ok();
			}
			if (method == "player.seek") return SeekCommand(params, GuiAction::Seek);
			if (method == "player.scrubBegin") return SeekCommand(params, GuiAction::ScrubBegin);
			if (method == "player.scrubUpdate") return SeekCommand(params, GuiAction::ScrubUpdate);
			if (method == "player.scrubEnd") return SeekCommand(params, GuiAction::ScrubEnd);
			if (method == "player.seekFrame")
			{
				int64_t frame = 0;
				if (!IntParam(params, "frame", frame) || frame < 0)
					return CmdResult::Err("invalid_params", "frame must be a 0-based integer");
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				if (!core_.player.clip.total_frames) return CmdResult::Err("not_ready", "No motion is loaded");
				if (frame >= static_cast<int64_t>(core_.player.clip.total_frames))
					return CmdResult::Err("invalid_params", "frame exceeds totalFrames - 1");
				core_.QueueSeekFrame(frame);
				ForcePlayerEvent();
				return CmdResult::Ok();
			}
			if (method == "player.stepFrame")
			{
				int64_t delta = 0;
				if (!IntParam(params, "delta", delta) || (delta != -1 && delta != 1))
					return CmdResult::Err("invalid_params", "delta must be -1 or 1");
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				if (!core_.player.clip.total_frames) return CmdResult::Err("not_ready", "No motion is loaded");
				core_.QueueStepFrame(static_cast<int>(delta));
				ForcePlayerEvent();
				return CmdResult::Ok();
			}
			if (method == "player.setSpeed")
			{
				double speed = 0;
				if (!NumberParam(params, "speed", speed) || speed < 0.1 || speed > 3.0)
					return CmdResult::Err("invalid_params", "speed must be within 0.1..3.0");
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				core_.QueueGui({GuiAction::Speed, static_cast<float>(speed)});
				ForcePlayerEvent();
				return CmdResult::Ok();
			}
			if (method == "player.setVolume")
			{
				double volume = 0;
				if (!NumberParam(params, "volume", volume) || volume < 0.0 || volume > 1.0)
					return CmdResult::Err("invalid_params", "volume must be within 0..1");
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				core_.QueueGui({GuiAction::MusicVolume, static_cast<float>(volume)});
				ForcePlayerEvent();
				return CmdResult::Ok();
			}
			if (method == "player.saveVolume")
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				core_.QueueGui({GuiAction::SaveMusicVolume});
				return CmdResult::Ok();
			}
			if (method == "playlist.setMode")
			{
				int64_t mode = 1;
				if (!IntParam(params, "mode", mode) || mode < 0 || mode > 3) mode = 1; // protocol §6: invalid -> 1
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				core_.QueueGui({GuiAction::Mode, static_cast<float>(mode)});
				return CmdResult::Ok();
			}

			// Library -----------------------------------------------------------
			if (method == "library.refresh")
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (core_.libraryScan.valid()) return CmdResult::Ok("{\"started\":false}");
				core_.QueueGui({GuiAction::Refresh});
				return CmdResult::Ok("{\"started\":true}");
			}
			if (method == "library.select")
			{
				std::string key;
				bool autoplay = false;
				int64_t revision = -1;
				if (!StringParam(params, "key", key) || key.empty() ||
					!IntParam(params, "revision", revision))
					return CmdResult::Err("invalid_params", "key and revision are required");
				if (!BoolParam(params, "autoplay", autoplay))
					return CmdResult::Err("invalid_params", "autoplay must be a boolean");
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (static_cast<uint64_t>(revision) != core_.libraryRevision)
					return CmdResult::Err("stale_revision", "Motion library changed; refresh before selecting");
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				const int index = FindPlaylistIndex(core_.motions, key);
				if (index < 0) return CmdResult::Err("invalid_params", "Motion key not found: " + key);
				core_.QueueSelectMotion(index, autoplay);
				ForcePlayerEvent();
				return CmdResult::Ok("{\"key\":" + JString(key) + "}");
			}
			if (method == "library.delete")
			{
				std::string key;
				int64_t revision = -1;
				if (!StringParam(params, "key", key) || key.empty() || !IntParam(params, "revision", revision))
					return CmdResult::Err("invalid_params", "key and revision are required");
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (static_cast<uint64_t>(revision) != core_.libraryRevision)
					return CmdResult::Err("stale_revision", "Motion library changed; reopen the delete confirmation");
				if (core_.importBusy || core_.cameraImportBusy || core_.libraryScan.valid())
					return CmdResult::Err("busy", "Import or refresh is in progress");
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				if (FindPlaylistIndex(core_.motions, key) < 0)
					return CmdResult::Err("invalid_params", "Motion key not found: " + key);
				if (core_.EnsembleActive())
					for (const auto& slot : core_.ensembleActiveSettings.slots)
						if (slot.enabled && slot.motionKey == key)
							return CmdResult::Err("protected", "Motion is in use by the ensemble stage");
				GuiCommand command{GuiAction::DeleteFiles, 0, true, key};
				command.libraryRevision = core_.libraryRevision;
				core_.QueueGui(std::move(command));
				return CmdResult::Ok();
			}
			if (method == "library.openFolder")
			{
				if (!hooks_.openLibraryFolder)
					return CmdResult::Err("not_implemented", "Host folder opener is unavailable");
				std::string notice;
				if (!hooks_.openLibraryFolder(notice))
					return CmdResult::Err("failed", notice.empty() ? "Cannot open the motions folder" : notice);
				return CmdResult::Ok();
			}
			if (method == "library.import") return LibraryImport(params);
			if (method == "library.probeFiles") return LibraryProbeFiles(params);
			if (method == "library.startEdit") return LibraryStartEdit(params);
			if (method == "library.probeAudio") return LibraryProbeAudio(params);
			if (method == "library.getCover") return LibraryGetCover(params);
			if (method == "library.setCover") return LibrarySetCover(params);
			if (method == "library.readImagePreview") return LibraryReadImagePreview(params);
			if (method == "screenshot.capabilities") return ScreenshotCapabilitiesMethod();
			if (method == "screenshot.capture") return ScreenshotCaptureMethod();

			// Clock & Audio Effects ---------------------------------------------
			if (method == "clock.setRunMode") return ClockSetRunMode(params);
			if (method == "clock.configure") return ClockConfigure(params);
			if (method == "audioEffect.set") return AudioEffectSet(params);

			// Actors ------------------------------------------------------------
			if (method == "actors.selectTarget")
			{
				int64_t token = 0, epoch = -1;
				if (!IntParam(params, "token", token) || token <= 0 || !IntParam(params, "epoch", epoch) || epoch < 0)
					return CmdResult::Err("invalid_params", "token and epoch are required");
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (static_cast<uint64_t>(epoch) != core_.sceneActorRegistry.Epoch())
					return CmdResult::Err("stale_token", "Actor roster changed; refresh before selecting");
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				if (!core_.sceneActorRegistry.Resolve(static_cast<uint64_t>(token), static_cast<uint64_t>(epoch)))
					return CmdResult::Err("stale_token", "Actor token is no longer present");
				GuiCommand command{GuiAction::SelectTarget};
				command.actorToken = static_cast<uint64_t>(token);
				command.actorEpoch = static_cast<uint64_t>(epoch);
				core_.QueueGui(std::move(command));
				return CmdResult::Ok();
			}

			// Facial / position ---------------------------------------------------
			if (method == "facial.setWeights")
			{
				double overall = 1, brow = 1, eye = 1, mouth = 1;
				if (!NumberParam(params, "overall", overall) || !NumberParam(params, "brow", brow) ||
					!NumberParam(params, "eye", eye) || !NumberParam(params, "mouth", mouth))
					return CmdResult::Err("invalid_params", "overall/brow/eye/mouth numbers are required");
				for (const double value : {overall, brow, eye, mouth})
					if (value < 0.0 || value > 2.0)
						return CmdResult::Err("invalid_params", "Facial weights must be within 0..2");
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				// Same direct write the ImGui overlay performs under stateMutex.
				core_.player.controller.facialWeights = {
					static_cast<float>(overall), static_cast<float>(brow),
					static_cast<float>(eye), static_cast<float>(mouth)};
				return CmdResult::Ok();
			}
			if (method == "facial.save")
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				core_.QueueGui({GuiAction::SaveFacialWeights});
				return CmdResult::Ok();
			}
			if (method == "position.set")
			{
				double x = 0, y = 0, z = 0;
				if (!NumberParam(params, "x", x) || !NumberParam(params, "y", y) || !NumberParam(params, "z", z))
					return CmdResult::Err("invalid_params", "x/y/z numbers are required");
				for (const double value : {x, y, z})
					if (value < -10.0 || value > 10.0)
						return CmdResult::Err("invalid_params", "Position offset must be within -10..10 meters");
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				core_.QueueInitialPosition({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)});
				return CmdResult::Ok();
			}
			if (method == "position.save")
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				core_.QueueGui({GuiAction::SaveInitialPosition});
				return CmdResult::Ok();
			}

			// Camera / stage ------------------------------------------------------
			if (method == "camera.set")
			{
				const jsonlite::Value* settings = Param(params, "settings");
				if (!settings || !settings->IsObject())
					return CmdResult::Err("invalid_params", "settings object is required");
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				CameraSettings parsed = core_.camera.settings;
				std::string error;
				if (!ParseCameraSettings(*settings, parsed, error))
					return CmdResult::Err("invalid_params", error);
				if (!parsed.IsValid())
					return CmdResult::Err("invalid_params", "Camera settings failed validation");
				core_.QueueCameraSettings(parsed);
				return CmdResult::Ok();
			}
			if (method == "camera.save")
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				core_.QueueGui({GuiAction::SaveCameraSettings});
				return CmdResult::Ok();
			}
			if (method == "stage.set") return StageSet(params);
			if (method == "stage.scan")
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				core_.QueueScanStageCandidates();
				return CmdResult::Ok("{\"nodes\":" + StageNodesJsonLocked() + "}");
			}
			if (method == "stage.toggleNode") return StageToggleNode(params);
			if (method == "stage.setLoadReplacement") return StageSetLoadReplacement(params);

			// Settings / shell / appearance ---------------------------------------
			if (method == "settings.setPhysics")
			{
				int64_t backend = -1;
				if (!IntParam(params, "backend", backend) || backend < 0 || backend > 2)
					return CmdResult::Err("invalid_params", "backend must be 0, 1 or 2");
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
				core_.QueuePhysicsBackend(static_cast<int>(backend));
				return CmdResult::Ok();
			}
			if (method == "shell.setTextInputActive")
			{
				bool active = false;
				if (!BoolParam(params, "active", active))
					return CmdResult::Err("invalid_params", "active must be a boolean");
				core_.guiTextInputActive.store(active, std::memory_order_relaxed);
				return CmdResult::Ok();
			}
			if (method == "appearance.get")
				return CmdResult::Ok(BuildAppearanceJson());

			if (method == "dialog.pickFile") return DialogPickFile(params);
			if (method == "dialog.poll") return DialogPoll(params);
			if (method == "drop.setTarget") return DropSetTarget(params);
			if (method == "drop.poll") return DropPoll();
			if (method == "drop.peek") return DropPeekMethod();

			if (method == "ensemble.set") return EnsembleSet(params);
			if (method == "ensemble.preset") return EnsemblePreset(params);
			if (method == "ensemble.presetSave" || method == "ensemble.preset.save") return EnsemblePresetSave(params);
			if (method == "ensemble.presetLoad" || method == "ensemble.preset.load") return EnsemblePresetLoad(params);
			if (method == "ensemble.probe") return EnsembleProbe(params);

			if (method == "bone.tree") return BoneTree();
			if (method == "bone.get") return BoneGet(params);
			if (method == "bone.set") return BoneSet(params);
			if (method == "bone.reset") return BoneReset(params);
			if (method == "bone.resetAll") return BoneResetAll();
			if (method == "bone.setMaster") return BoneSetMaster(params);
			if (method == "bone.toggle") return BoneToggle();
			if (method == "bone.reload") return BoneReload();
			if (method == "bone.saveToMotion") return BoneSaveToMotion();
			if (method == "bone.saveToGlobal") return BoneSaveToGlobal();

			if (method == "gaze.get") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); return CmdResult::Ok(GazeJsonLocked()); }
			if (method == "gaze.set") return GazeSet(params);
			if (method == "gaze.setMode") return GazeSetMode(params);
			if (method == "gaze.setDesktop") return GazeSetDesktop(params);
			if (method == "gaze.setWeights") return GazeSetWeights(params);
			if (method == "gaze.cycle") return GazeCycle();

			if (method == "sole.get" || method == "sole.read") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); return CmdResult::Ok(SoleJsonLocked()); }
			if (method == "sole.set" || method == "sole.apply") return SoleSet(params);
			if (method == "sole.save") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); core_.QueueGui({GuiAction::SaveSoleSettings}); return CmdResult::Ok(); }
			if (method == "sole.load") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); core_.QueueGui({GuiAction::LoadSoleSettings}); return CmdResult::Ok(); }

			if (method == "solePitch.get") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); return CmdResult::Ok(SolePitchJsonLocked()); }
			if (method == "solePitch.set") return SolePitchSet(params);

			if (method == "studio.get") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); return CmdResult::Ok(StudioJsonLocked()); }
			if (method == "studio.set") return StudioSet(params);
			if (method == "studio.setLighting") return StudioSetLighting(params);
			if (method == "studio.setVideo") return StudioSetVideo(params);
			if (method == "studio.reset") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); core_.QueueStudioReset(); return CmdResult::Ok(); }

			if (method == "experiments.get") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); return CmdResult::Ok(ExperimentsJsonLocked()); }
			if (method == "experiments.set") return ExperimentsSet(params);
			if (method == "experiments.setMiku") return ExperimentsSetMiku(params);
			if (method == "experiments.setWaistSupport") return ExperimentsSetWaistSupport(params);
			if (method == "experiments.setBeforeQuartz") return ExperimentsSetBeforeQuartz(params);

			if (method == "telemetry.get") { std::lock_guard<std::recursive_mutex> lock(core_.stateMutex); return CmdResult::Ok(TelemetryJsonLocked()); }

			return CmdResult::Err("unknown_method", "Unknown method: " + method);
		}

		// ---- command helpers ----------------------------------------------------

		// Caller holds stateMutex. Mirrors the ImGui drain admission rules so a
		// refused command reports a structured error instead of silently dropping.
		CmdResult MutationGuardLocked()
		{
			if (!core_.InjectionAllowed())
				return CmdResult::Err("not_allowed",
					core_.injectionStatus.empty() ? "Injection is not allowed in the current scene" : core_.injectionStatus);
			return CmdResult::Ok();
		}

		CmdResult TransportSimple(GuiAction action, bool requiresClip)
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			if (requiresClip && !core_.player.clip.total_frames)
				return CmdResult::Err("not_ready", "No motion is loaded");
			if (action == GuiAction::Pause &&
				core_.player.state != PlaybackState::Playing && core_.player.state != PlaybackState::Paused)
				return CmdResult::Err("not_ready", "Playback is not active");
			core_.QueueGui({action});
			ForcePlayerEvent();
			return CmdResult::Ok();
		}

		CmdResult SeekCommand(const jsonlite::Value& params, GuiAction action)
		{
			double seconds = 0;
			if (!NumberParam(params, "seconds", seconds) || seconds < 0)
				return CmdResult::Err("invalid_params", "seconds must be a finite number >= 0");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			if (!core_.player.clip.total_frames) return CmdResult::Err("not_ready", "No motion is loaded");
			const float duration = core_.player.TimelineDuration();
			if (seconds > duration)
				return CmdResult::Err("invalid_params", "seconds exceeds the timeline duration");
			switch (action)
			{
			case GuiAction::Seek: core_.QueueSeek(static_cast<float>(seconds)); break;
			case GuiAction::ScrubBegin: core_.QueueScrubBegin(static_cast<float>(seconds)); break;
			case GuiAction::ScrubUpdate: core_.QueueScrubUpdate(static_cast<float>(seconds)); break;
			case GuiAction::ScrubEnd: core_.QueueScrubEnd(static_cast<float>(seconds)); break;
			default: break;
			}
			ForcePlayerEvent();
			return CmdResult::Ok();
		}

		void ForcePlayerEvent() { forcePlayerEvent_ = true; }

		CmdResult StageSet(const jsonlite::Value& params)
		{
			const jsonlite::Value* settings = Param(params, "settings");
			if (!settings || !settings->IsObject())
				return CmdResult::Err("invalid_params", "settings object is required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			StageSettings parsed;
			{
				std::lock_guard<std::mutex> stageLock(core_.stageManager.stageMutex);
				parsed = core_.stageManager.settings;
			}
			std::string error;
			if (!ParseStageSettings(*settings, parsed, error))
				return CmdResult::Err("invalid_params", error);
			// Cross-domain side effects formerly owned by the ImGui stage page.
			bool lightingChanged = false;
			bool restoreSkybox = false;
			{
				std::lock_guard<std::mutex> stageLock(core_.stageManager.stageMutex);
				lightingChanged = parsed.lightingEffectsEnabled != core_.stageManager.settings.lightingEffectsEnabled;
				restoreSkybox = core_.stageManager.settings.hideBackground && !parsed.hideBackground;
			}
			if (parsed.hideBackground)
			{
				parsed.greenScreenActive = true;
				parsed.overrideCameraClear = true;
			}
			else
			{
				parsed.greenScreenActive = false;
				parsed.overrideCameraClear = false;
			}
			if (lightingChanged)
			{
				CameraSettings cameraSettings = core_.camera.settings;
				cameraSettings.actorShadowExperimentMode = parsed.lightingEffectsEnabled ? 1 : 2;
				core_.QueueCameraSettings(cameraSettings);
			}
			core_.QueueStageSettings(parsed);
			if (restoreSkybox) core_.QueueRestoreCameraSkybox();
			return CmdResult::Ok();
		}

		// ---- dialogs / drops (implemented, capability-gated) ---------------------

		CmdResult DialogPickFile(const jsonlite::Value& params)
		{
			int64_t kind = -1;
			bool multiple = false;
			if (!IntParam(params, "kind", kind) || kind < 0 || kind > 6)
				return CmdResult::Err("invalid_params", "kind must be 0..6");
			if (!BoolParam(params, "multiple", multiple))
				return CmdResult::Err("invalid_params", "multiple must be a boolean");
			if (!hooks_.beginWebFileDialog && !core_.fnBeginPickFile)
				return CmdResult::Err("not_implemented", "Host file dialog service is unavailable");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (dialogPending_ || core_.gui.browsePendingKind >= 0)
				return CmdResult::Err("busy", "A file dialog is already open");
			std::string notice;
			const int legacyKind = kind == 3 ? 2 : kind == 4 ? 1 : kind == 5 ? 3 : 0;
			const bool started = hooks_.beginWebFileDialog
				? hooks_.beginWebFileDialog(static_cast<int>(kind), multiple, notice)
				: core_.fnBeginPickFile(legacyKind, notice);
			if (!started)
				return CmdResult::Err("failed", notice.empty() ? "Cannot open the host file dialog" : notice);
			dialogPending_ = true;
			dialogKind_ = static_cast<int>(kind);
			dialogId_ += 1;
			dialogState_ = "pending";
			dialogPaths_.clear();
			dialogNotice_.clear();
			return CmdResult::Ok("{\"dialogId\":" + JNum(static_cast<double>(dialogId_)) + "}");
		}

		CmdResult DialogPoll(const jsonlite::Value& params)
		{
			int64_t dialogId = 0;
			if (!IntParam(params, "dialogId", dialogId) || dialogId != static_cast<int64_t>(dialogId_))
				return CmdResult::Err("invalid_params", "dialogId does not match the current dialog");
			PollBridgeDialog();
			std::string paths = "[";
			for (size_t i = 0; i < dialogPaths_.size(); ++i)
			{
				if (i) paths += ",";
				paths += JString(dialogPaths_[i]);
			}
			paths += "]";
			return CmdResult::Ok("{\"state\":" + JString(dialogState_) + ",\"paths\":" + paths +
				",\"notice\":" + JString(dialogNotice_) + "}");
		}

		void PollBridgeDialog()
		{
			if (!dialogPending_ || !core_.fnPollPickFile) return;
			std::string path;
			std::string notice;
			const int state = core_.fnPollPickFile(path, notice);
			if (state == GAKU_FILE_DIALOG_PENDING) return;
			dialogPending_ = false;
			dialogNotice_ = notice;
			if (state == GAKU_FILE_DIALOG_ACCEPTED)
			{
				dialogState_ = "accepted";
				dialogPaths_.assign(1, path);
			}
			else if (state == GAKU_FILE_DIALOG_CANCELLED) dialogState_ = "cancelled";
			else dialogState_ = "failed";
		}

		static bool IsSupportedAudioExtensionPath(const std::string& path)
		{
			std::string ext = WideToUtf8(std::filesystem::path(Utf8ToWide(path)).extension().wstring());
			for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			return ext == ".wav" || ext == ".mp3" || ext == ".flac" ||
			       ext == ".ogg" || ext == ".oga" || ext == ".m4a" ||
			       ext == ".aac" || ext == ".opus" || ext == ".wma";
		}

		static bool IsNetworkAudioPath(const std::string& path)
		{
			return path.find("://") != std::string::npos ||
			       path.rfind("\\\\", 0) == 0 ||
			       path.rfind("//", 0) == 0;
		}

		static bool IsAcceptedImportFile(const std::string& path)
		{
			std::string ext = WideToUtf8(std::filesystem::path(Utf8ToWide(path)).extension().wstring());
			for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			return ext == ".vmd" || ext == ".gkcamera" || IsSupportedAudioExtensionPath(path);
		}

		static DropPeek NormalizeDropPeek(const DropPeek& raw)
		{
			DropPeek peek = raw;
			if (!peek.dragPaths.empty())
			{
				if (peek.fileCount == 0) peek.fileCount = static_cast<uint32_t>(peek.dragPaths.size());
				uint32_t valid = 0;
				for (const auto& path : peek.dragPaths)
					if (IsAcceptedImportFile(path)) ++valid;
				peek.accepted = valid;
				if (peek.visual == "hover" && valid == 0) peek.visual = "invalid";
			}
			else if (peek.fileCount == 0 && peek.pendingCount > 0)
			{
				peek.fileCount = peek.pendingCount;
			}
			return peek;
		}

		CmdResult DropSetTarget(const jsonlite::Value& params)
		{
			bool enabled = false;
			if (!BoolParam(params, "enabled", enabled))
				return CmdResult::Err("invalid_params", "enabled must be a boolean");
			if (!hooks_.dropSetTarget && !hooks_.dropPeek && !hooks_.dropConsume)
				return CmdResult::Err("not_implemented", "Host file-drop service is unavailable");
			dropEnabled_ = enabled;
			if (!enabled) cachedDropPeek_ = {};
			return CmdResult::Ok();
		}

		CmdResult DropPoll()
		{
			if (!hooks_.dropConsume && !hooks_.dropPeek)
				return CmdResult::Err("not_implemented", "Host file-drop service is unavailable");
			std::string files = "[";
			for (size_t i = 0; i < pendingDrops_.size(); ++i)
			{
				if (i) files += ",";
				files += JString(pendingDrops_[i]);
			}
			files += "]";
			pendingDrops_.clear();
			return CmdResult::Ok("{\"files\":" + files + "}");
		}

		CmdResult DropPeekMethod()
		{
			if (!hooks_.dropPeek)
				return CmdResult::Err("not_implemented", "Host file-drop service is unavailable");
			DropPeek peek;
			if (hooks_.dropSetTarget)
			{
				if (dropEnabled_) peek = cachedDropPeek_;
			}
			else
			{
				// Test hooks without dropSetTarget may invoke drop.peek directly.
				peek = NormalizeDropPeek(hooks_.dropPeek());
			}
			if (peek.visual.empty()) peek.visual = "none";
			return CmdResult::Ok("{\"visual\":" + JString(peek.visual) +
				",\"fileCount\":" + JNum(peek.fileCount) +
				",\"accepted\":" + JNum(peek.accepted) +
				",\"hostElevated\":" + JBool(peek.hostElevated) +
				",\"hostBlocked\":" + JBool(peek.hostBlocked) + "}");
		}

		static const char* VmdTendencyName(VmdTendency tendency)
		{
			switch (tendency)
			{
			case VmdTendency::MotionOnly: return "motionOnly";
			case VmdTendency::MotionAndFacial: return "motionAndFacial";
			case VmdTendency::FacialOnly: return "facialOnly";
			case VmdTendency::CameraOnly: return "cameraOnly";
			case VmdTendency::Empty: return "empty";
			default: return "invalid";
			}
		}

		static std::string TimingJson(const PerformanceTiming& timing)
		{
			const auto track = [](const TrackTiming& value) {
				return "{\"speed\":" + JNum(value.speed) + ",\"offsetSeconds\":" + JNum(value.offsetSeconds) + "}";
			};
			return "{\"motion\":" + track(timing.motion) + ",\"facial\":" + track(timing.facial) +
				",\"camera\":" + track(timing.camera) + ",\"audio\":" + track(timing.audio) + "}";
		}

		static bool ParseTiming(const jsonlite::Value& value, PerformanceTiming& timing, std::string& error)
		{
			const auto track = [&](const char* name, TrackTiming& target) {
				const jsonlite::Value* field = value.Find(name);
				if (!field) return true;
				if (!field->IsObject()) { error = std::string(name) + " timing must be an object"; return false; }
				double speed = target.speed, offset = target.offsetSeconds;
				if (!NumberParam(*field, "speed", speed)) { error = std::string(name) + ".speed must be a finite number"; return false; }
				const jsonlite::Value* offsetValue = field->Find("offsetSeconds");
				if (!offsetValue) offsetValue = field->Find("offset_seconds");
				if (offsetValue)
				{
					if (!offsetValue->IsNumber() || !std::isfinite(offsetValue->numberValue))
					{ error = std::string(name) + ".offsetSeconds must be a finite number"; return false; }
					offset = offsetValue->numberValue;
				}
				if (speed < 0.0) { error = std::string(name) + ".speed must be >= 0"; return false; }
				target.speed = static_cast<float>(speed);
				target.offsetSeconds = static_cast<float>(offset);
				return true;
			};
			return track("motion", timing.motion) && track("facial", timing.facial) &&
				track("camera", timing.camera) && track("audio", timing.audio);
		}

		CmdResult LibraryProbeFiles(const jsonlite::Value& params)
		{
			const jsonlite::Value* paths = Param(params, "paths");
			if (!paths || !paths->IsArray() || paths->Size() == 0 || paths->Size() > 256)
				return CmdResult::Err("invalid_params", "paths must be a non-empty array of at most 256 strings");
			std::vector<std::string> files;
			for (size_t i = 0; i < paths->Size(); ++i)
			{
				const jsonlite::Value* item = paths->At(i);
				if (!item || !item->IsString() || item->stringValue.empty() || item->stringValue.size() > 32767)
					return CmdResult::Err("invalid_params", "each path must be a non-empty string");
				files.push_back(item->stringValue);
			}
			const ImportClassification classification = ClassifyImportFiles(files);
			std::string probes = "[";
			for (size_t i = 0; i < files.size(); ++i)
			{
				const VmdProbeInfo info = ProbeCameraOrVmdFile(files[i]);
				if (i) probes += ",";
				probes += "{\"path\":" + JString(files[i]) +
					",\"valid\":" + JBool(info.valid) +
					",\"isGkCamera\":" + JBool(info.isGkCamera) +
					",\"tendency\":" + JString(VmdTendencyName(info.tendency)) +
					",\"boneFrames\":" + JNum(info.boneFrames) +
					",\"bodyBoneFrames\":" + JNum(info.bodyBoneFrames) +
					",\"eyeBoneFrames\":" + JNum(info.eyeBoneFrames) +
					",\"morphFrames\":" + JNum(info.morphFrames) +
					",\"cameraFrames\":" + JNum(info.cameraFrames) + "}";
			}
			probes += "]";
			const std::string classificationJson = "{\"suggestedTitle\":" + JString(classification.suggestedTitle) +
				",\"motionCandidate\":" + JString(classification.motionCandidate) +
				",\"facialCandidate\":" + JString(classification.facialCandidate) +
				",\"cameraCandidate\":" + JString(classification.cameraCandidate) +
				",\"audioCandidate\":" + JString(classification.audioCandidate) +
				",\"unsupportedAudioCount\":" + JNum(classification.unsupportedAudioCount) + "}";
			return CmdResult::Ok("{\"classification\":" + classificationJson + ",\"probes\":" + probes + "}");
		}

		CmdResult LibraryStartEdit(const jsonlite::Value& params)
		{
			std::string key;
			int64_t revision = -1;
			if (!StringParam(params, "key", key) || key.empty() || !IntParam(params, "revision", revision))
				return CmdResult::Err("invalid_params", "key and revision are required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (static_cast<uint64_t>(revision) != core_.libraryRevision)
				return CmdResult::Err("stale_revision", "曲库已变化，请刷新后重新选择曲目");
			if (core_.importBusy || core_.cameraImportBusy || core_.libraryScan.valid())
				return CmdResult::Err("busy", "正在导入或刷新，请等待完成");
			const int index = FindPlaylistIndex(core_.motions, key);
			if (index < 0) return CmdResult::Err("invalid_params", "Motion key not found: " + key);
			const MotionEntry& entry = core_.motions[index];
			return CmdResult::Ok("{\"key\":" + JString(key) +
				",\"title\":" + JString(entry.title) +
				",\"subtitle\":" + JString(entry.subtitle) +
				",\"hasFacial\":" + JBool(entry.hasFacial) +
				",\"hasCamera\":" + JBool(entry.hasCamera) +
				",\"hasAudio\":" + JBool(entry.hasAudio) +
				",\"audioFile\":" + JString(entry.audioFile) +
				",\"hasCover\":" + JBool(entry.hasCover) +
				",\"timing\":" + TimingJson(entry.timing) + "}");
		}

		CmdResult LibraryProbeAudio(const jsonlite::Value& params)
		{
			std::string path;
			if (!StringParam(params, "path", path) || path.empty() || path.size() > 32767)
				return CmdResult::Err("invalid_params", "path must be a non-empty string");
			if (IsNetworkAudioPath(path))
				return CmdResult::Err("invalid_params", "不支持网络或流媒体路径，请选择本地音频文件");
			if (!IsSupportedAudioExtensionPath(path))
				return CmdResult::Err("invalid_params", "音频格式不支持（支持 .wav/.mp3/.flac/.ogg/.m4a/.aac/.opus）");
			bool includeCover = true;
			if (params.Find("includeCover") && !BoolParam(params, "includeCover", includeCover))
				return CmdResult::Err("invalid_params", "includeCover must be a boolean");

			AudioProbeView probed;
			if (hooks_.probeAudio) probed = hooks_.probeAudio(path, includeCover);
			return CmdResult::Ok("{\"valid\":" + JBool(probed.valid) +
				",\"sampleRate\":" + JNum(probed.sampleRate) +
				",\"channels\":" + JNum(probed.channels) +
				",\"durationSeconds\":" + JNum(probed.durationSeconds) +
				",\"codec\":" + JString(probed.codec) +
				",\"title\":" + JString(probed.title) +
				",\"artist\":" + JString(probed.artist) +
				",\"album\":" + JString(probed.album) +
				",\"hasCover\":" + JBool(probed.hasCover) +
				",\"coverDataUri\":" + JString(probed.coverDataUri) + "}");
		}

		CmdResult LibraryGetCover(const jsonlite::Value& params)
		{
			std::string key, directory;
			StringParam(params, "key", key);
			StringParam(params, "directory", directory);
			if (key.empty() && directory.empty())
				return CmdResult::Err("invalid_params", "key or directory is required");

			std::string resolvedDir;
			bool hasCoverHint = false;
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				for (const auto& entry : core_.motions)
				{
					if ((!key.empty() && entry.name == key) ||
						(!directory.empty() && entry.dir == directory))
					{
						resolvedDir = entry.dir;
						hasCoverHint = entry.hasCover || entry.hasAudio;
						break;
					}
				}
			}
			if (resolvedDir.empty())
				return CmdResult::Err("not_found", "Track not found in library");

			std::string dataUri;
			if (hasCoverHint && hooks_.getTrackCoverDataUri)
				dataUri = hooks_.getTrackCoverDataUri(resolvedDir);
			return CmdResult::Ok("{\"hasCover\":" + JBool(!dataUri.empty()) +
				",\"coverDataUri\":" + JString(dataUri) + "}");
		}

		CmdResult LibrarySetCover(const jsonlite::Value& params)
		{
			std::string key;
			if (!StringParam(params, "key", key) || key.empty())
				return CmdResult::Err("invalid_params", "key is required");
			bool clear = false;
			if (params.Find("clear") && !BoolParam(params, "clear", clear))
				return CmdResult::Err("invalid_params", "clear must be a boolean");
			std::string coverDataUri;
			if (!clear)
			{
				if (!StringParam(params, "coverDataUri", coverDataUri) || coverDataUri.empty())
					return CmdResult::Err("invalid_params", "coverDataUri is required when clear is false");
				if (coverDataUri.size() > 16ULL * 1024ULL * 1024ULL ||
					coverDataUri.rfind("data:image/", 0) != 0)
					return CmdResult::Err("invalid_params", "coverDataUri must be a valid image data URI");
			}

			std::string thumbnailUri;
			std::string error;
			if (!core_.SetTrackCover(key, clear, coverDataUri, thumbnailUri, error))
			{
				if (error.find("正在导入") != std::string::npos)
					return CmdResult::Err("busy", error);
				if (error.find("not found") != std::string::npos)
					return CmdResult::Err("not_found", error);
				return CmdResult::Err("failed", error.empty() ? "Failed to save track cover" : error);
			}
			forceDomainRefresh_.store(true, std::memory_order_release);
			uint64_t rev = 0;
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				rev = core_.libraryRevision;
			}
			return CmdResult::Ok("{\"hasCover\":" + JBool(!clear) +
				",\"coverDataUri\":" + JString(thumbnailUri) +
				",\"revision\":" + JNum(static_cast<double>(rev)) + "}");
		}

		CmdResult LibraryReadImagePreview(const jsonlite::Value& params)
		{
			std::string path;
			if (!StringParam(params, "path", path) || path.empty() || path.size() > 32767)
				return CmdResult::Err("invalid_params", "path must be a non-empty string");
			if (IsNetworkAudioPath(path))
				return CmdResult::Err("invalid_params", "不支持网络路径，请选择本地图片文件");
			if (!hooks_.readImagePreviewDataUri)
				return CmdResult::Err("not_implemented", "Image preview hook is unavailable");
			const std::string dataUri = hooks_.readImagePreviewDataUri(path);
			if (dataUri.empty())
				return CmdResult::Err("failed", "无法读取或解码所选图片文件");
			return CmdResult::Ok("{\"dataUri\":" + JString(dataUri) + "}");
		}

		CmdResult ScreenshotCapabilitiesMethod()
		{
			ScreenshotCapabilitiesView caps;
			if (hooks_.getScreenshotCapabilities)
			{
				caps = hooks_.getScreenshotCapabilities();
			}
			else
			{
				caps.available = false;
				caps.notice = "ScreenshotManager 联动服务不可用";
			}
			return CmdResult::Ok("{\"available\":" + JBool(caps.available) +
				",\"runtimeReady\":" + JBool(caps.runtimeReady) +
				",\"cameraAvailable\":" + JBool(caps.cameraAvailable) +
				",\"windowWidth\":" + JNum(caps.windowWidth) +
				",\"windowHeight\":" + JNum(caps.windowHeight) +
				",\"notice\":" + JString(caps.notice) + "}");
		}

		CmdResult ScreenshotCaptureMethod()
		{
			if (!hooks_.captureScreenshot)
				return CmdResult::Err("not_implemented", "ScreenshotManager 联动拍照服务不可用");
			const ScreenshotCaptureView captured = hooks_.captureScreenshot();
			if (!captured.ok || captured.dataUri.empty())
			{
				return CmdResult::Err("failed",
					captured.error.empty() ? "ScreenshotManager 拍照失败" : captured.error);
			}
			return CmdResult::Ok("{\"dataUri\":" + JString(captured.dataUri) +
				",\"width\":" + JNum(captured.width) +
				",\"height\":" + JNum(captured.height) + "}");
		}

		CmdResult ClockSetRunMode(const jsonlite::Value& params)
		{
			uint32_t rawMode = GAKUMOTION_RUN_REALTIME;
			if (const auto* rm = params.Find("runMode"))
			{
				if (rm->IsString())
				{
					if (rm->stringValue == "recording") rawMode = GAKUMOTION_RUN_RECORDING;
					else if (rm->stringValue == "realtime") rawMode = GAKUMOTION_RUN_REALTIME;
					else return CmdResult::Err("invalid_params", "runMode must be realtime or recording");
				}
				else if (rm->IsNumber())
				{
					const int v = rm->AsInt(-1);
					if (v != 0 && v != 1) return CmdResult::Err("invalid_params", "runMode must be 0 or 1");
					rawMode = static_cast<uint32_t>(v);
				}
				else return CmdResult::Err("invalid_params", "runMode must be realtime or recording");
			}
			else return CmdResult::Err("invalid_params", "runMode is required");

			if (core_.SetClockRunMode(rawMode) != GAKU_OK)
				return CmdResult::Err("failed", "Failed to update run mode");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			return CmdResult::Ok(ClockJsonLocked());
		}

		CmdResult ClockConfigure(const jsonlite::Value& params)
		{
			GakuMotionClockSnapshotV1 cur{};
			core_.ReadClockSnapshot(&cur);
			uint32_t source = cur.configuredSource;
			uint32_t fpsNum = cur.fpsNumerator > 0 ? cur.fpsNumerator : 60u;
			uint32_t fpsDen = cur.fpsDenominator > 0 ? cur.fpsDenominator : 1u;

			if (const auto* s = params.Find("source"))
			{
				if (s->IsString())
				{
					if (s->stringValue == "audio") source = GAKUMOTION_CLOCK_AUDIO;
					else if (s->stringValue == "engine_delta") source = GAKUMOTION_CLOCK_ENGINE_DELTA;
					else if (s->stringValue == "fixed_step") source = GAKUMOTION_CLOCK_FIXED_STEP;
					else if (s->stringValue == "external") source = GAKUMOTION_CLOCK_EXTERNAL;
					else return CmdResult::Err("invalid_params", "unknown clock source");
				}
				else if (s->IsNumber())
				{
					const int v = s->AsInt(-1);
					if (v < 0 || v > 3) return CmdResult::Err("invalid_params", "clock source out of range");
					source = static_cast<uint32_t>(v);
				}
				else return CmdResult::Err("invalid_params", "source must be string or number");
			}
			int64_t num = 0, den = 0;
			if (params.Find("fpsNumerator"))
			{
				if (!IntParam(params, "fpsNumerator", num) || num < 1 || num > 1000)
					return CmdResult::Err("invalid_params", "fpsNumerator must be in 1..1000");
				fpsNum = static_cast<uint32_t>(num);
			}
			if (params.Find("fpsDenominator"))
			{
				if (!IntParam(params, "fpsDenominator", den) || den < 1 || den > 100)
					return CmdResult::Err("invalid_params", "fpsDenominator must be in 1..100");
				fpsDen = static_cast<uint32_t>(den);
			}

			if (core_.ConfigureClock(source, fpsNum, fpsDen) != GAKU_OK)
				return CmdResult::Err("failed", "Failed to configure clock");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			return CmdResult::Ok(ClockJsonLocked());
		}

		CmdResult AudioEffectSet(const jsonlite::Value& params)
		{
			std::string presetId;
			if (!StringParam(params, "presetId", presetId) || presetId.empty() || presetId.size() > 63)
				return CmdResult::Err("invalid_params", "presetId is required");
			if (!hooks_.setAudioEffectPreset)
				return CmdResult::Err("not_implemented", "GakuAudio service hook unavailable");
			std::string error;
			if (!hooks_.setAudioEffectPreset(presetId, error))
				return CmdResult::Err("failed", error.empty() ? "Failed to switch audio effect" : error);
			forceAudioEffectRefresh_.store(true, std::memory_order_release);
			return CmdResult::Ok(BuildAudioEffectJson());
		}

		static std::string ImportSlotString(const jsonlite::Value& draft, const char* fileField,
			const char* modeField, const char* pathField)
		{
			const jsonlite::Value* mode = draft.Find(modeField);
			if (mode && mode->IsString())
			{
				const std::string& token = mode->stringValue;
				if (token == "keep" || token == "keepTrack") return "[保持当前已导入轨道]";
				if (token == "keepMotion") return "[保持当前动作]";
				if (token == "clear") return "（留空 / 清除）";
				if (token == "skip") return "（不导入）";
				if (token == "auto") return "";
				const jsonlite::Value* path = draft.Find(pathField);
				if (path && path->IsString()) return path->stringValue;
				return "";
			}
			const jsonlite::Value* file = draft.Find(fileField);
			return file && file->IsString() ? file->stringValue : "";
		}

		static bool IsImportSlotFile(const std::string& value)
		{
			return !value.empty() && value != "（留空）" && value != "（留空 / 清除）" &&
				value != "[保持当前已导入轨道]" && value != "[保持当前动作]" &&
				value != "（自动：沿用动作 VMD 内表情）" &&
				value != "（自动：同名 WAV 或 song.wav）" &&
				value != "（自动：同名音频或 song.*）" &&
				value != "（不导入）";
		}

		static CmdResult ValidateImportTrackFile(const std::string& path, const char* label, int kind)
		{
			if (path.size() > 32767) return CmdResult::Err("invalid_params", std::string(label) + "路径过长");
			if (kind == 3)
			{
				if (IsNetworkAudioPath(path))
					return CmdResult::Err("invalid_params", "不支持网络或流媒体路径，请选择本地音频文件");
				if (!IsSupportedAudioExtensionPath(path))
					return CmdResult::Err("invalid_params", "音频轨道仅支持 .wav / .mp3 / .flac / .ogg / .m4a / .aac / .opus 本地文件");
				return CmdResult::Ok();
			}
			const VmdProbeInfo info = ProbeCameraOrVmdFile(path);
			if (kind == 0 && !info.CanUseAsMotion())
				return CmdResult::Err("invalid_params", "所选动作文件不包含身体骨骼动作关键帧");
			if (kind == 1 && !info.CanUseAsFacial())
				return CmdResult::Err("invalid_params", "所选表情文件不包含表情或眼神关键帧");
			if (kind == 2 && !info.CanUseAsCamera())
				return CmdResult::Err("invalid_params", "所选镜头文件不包含镜头关键帧");
			return CmdResult::Ok();
		}

		CmdResult LibraryImport(const jsonlite::Value& params)
		{
			const jsonlite::Value* requestObj = Param(params, "request");
			const jsonlite::Value* entries = Param(params, "entries");
			const bool usingRequestSchema = requestObj != nullptr;
			const jsonlite::Value* draft = requestObj;
			if (!usingRequestSchema)
			{
				if (!entries || !entries->IsArray() || entries->Size() != 1)
					return CmdResult::Err("invalid_params", "request object or single entry is required");
				draft = entries->At(0);
			}
			if (!draft || !draft->IsObject())
				return CmdResult::Err("invalid_params", "import request must be an object");

			std::string title, editKey;
			StringParam(*draft, "title", title);
			StringParam(*draft, "editKey", editKey);
			bool hasTitle = false;
			for (unsigned char ch : title) if (!std::isspace(ch)) { hasTitle = true; break; }
			if (!hasTitle) return CmdResult::Err("invalid_params", "请输入有效的演出曲目名称");

			float resolvedFps = 0.0f;
			if (const jsonlite::Value* fpsVal = draft->Find("fps"))
			{
				double fpsNum = 0.0;
				if (!fpsVal->IsNumber() || !std::isfinite(fpsVal->numberValue) || fpsVal->numberValue < 0.0)
					return CmdResult::Err("invalid_params", "fps must be a non-negative finite number");
				fpsNum = fpsVal->numberValue;
				resolvedFps = static_cast<float>(fpsNum);
			}
			else
			{
				int64_t fpsIndex = 0;
				if (!IntParam(*draft, "fpsIndex", fpsIndex) || fpsIndex < 0 || fpsIndex > 2)
					return CmdResult::Err("invalid_params", "fpsIndex must be 0 (auto), 1 (30) or 2 (60)");
				resolvedFps = fpsIndex == 1 ? 30.0f : fpsIndex == 2 ? 60.0f : 0.0f;
			}

			float resolvedSourceFps = 0.0f;
			if (const jsonlite::Value* sourceFpsVal = draft->Find("sourceFps"))
			{
				if (!sourceFpsVal->IsNumber() || !std::isfinite(sourceFpsVal->numberValue) || sourceFpsVal->numberValue < 0.0)
					return CmdResult::Err("invalid_params", "sourceFps must be a non-negative finite number");
				resolvedSourceFps = static_cast<float>(sourceFpsVal->numberValue);
			}
			else if (draft->Find("sourceFpsIndex"))
			{
				int64_t sourceFpsIndex = 0;
				if (!IntParam(*draft, "sourceFpsIndex", sourceFpsIndex) || sourceFpsIndex < 0 || sourceFpsIndex > 2)
					return CmdResult::Err("invalid_params", "sourceFpsIndex must be 0 (auto), 1 (30) or 2 (60)");
				resolvedSourceFps = sourceFpsIndex == 1 ? 30.0f : sourceFpsIndex == 2 ? 60.0f : 0.0f;
			}
			if (resolvedSourceFps != 0.0f && resolvedSourceFps != 25.0f && resolvedSourceFps != 30.0f && resolvedSourceFps != 60.0f)
				return CmdResult::Err("invalid_params", "源动作帧率只支持自动、25、30 或 60 fps");

			PerformanceTiming timing;
			std::string error;
			const jsonlite::Value* timingValue = draft->Find("timing");
			if (timingValue && !ParseTiming(*timingValue, timing, error))
				return CmdResult::Err("invalid_params", error);
			if (!ValidPerformanceTiming(timing))
				return CmdResult::Err("invalid_params", "轨道速度必须 >= 0，偏移必须为有限数值");

			const bool edit = !editKey.empty();
			bool rebakeOutput = edit && resolvedFps > 0.0f;
			if (draft->Find("rebakeOutput") && !BoolParam(*draft, "rebakeOutput", rebakeOutput))
				return CmdResult::Err("invalid_params", "rebakeOutput must be a boolean");
			const std::string motion = ImportSlotString(*draft, "motionFile", "motionMode", "motionPath");
			const std::string facial = ImportSlotString(*draft, "facialFile", "facialMode", "facialPath");
			const std::string camera = ImportSlotString(*draft, "cameraFile", "cameraMode", "cameraPath");
			const std::string audio = ImportSlotString(*draft, "audioFile", "audioMode", "audioPath");

			MotionImportRequest request;
			request.title = title;
			std::string subtitle;
			if (draft->Find("subtitle"))
			{
				if (!StringParam(*draft, "subtitle", subtitle))
					return CmdResult::Err("invalid_params", "subtitle must be a string");
				request.hasSubtitle = true;
				request.subtitle = subtitle;
			}
			request.fps = resolvedFps;
			request.sourceFps = resolvedSourceFps;
			request.rebakeOutput = rebakeOutput;
			request.timing = timing;
			request.editKey = editKey;

			if (usingRequestSchema)
			{
				bool disableMorph = false, clearCamera = false, clearAudio = false;
				if (draft->Find("disableMorph") && !BoolParam(*draft, "disableMorph", disableMorph))
					return CmdResult::Err("invalid_params", "disableMorph must be a boolean");
				if (draft->Find("clearCamera") && !BoolParam(*draft, "clearCamera", clearCamera))
					return CmdResult::Err("invalid_params", "clearCamera must be a boolean");
				if (draft->Find("clearAudio") && !BoolParam(*draft, "clearAudio", clearAudio))
					return CmdResult::Err("invalid_params", "clearAudio must be a boolean");

				if (edit)
				{
					if (IsImportSlotFile(motion))
					{
						if (auto check = ValidateImportTrackFile(motion, "动作", 0); !check.ok) return check;
						request.vmd = motion;
					}
					request.disableMorph = disableMorph;
					if (!disableMorph && IsImportSlotFile(facial))
					{
						if (auto check = ValidateImportTrackFile(facial, "表情", 1); !check.ok) return check;
						request.morphVmd = facial;
					}
					request.clearCamera = clearCamera;
					if (!clearCamera && IsImportSlotFile(camera))
					{
						if (auto check = ValidateImportTrackFile(camera, "镜头", 2); !check.ok) return check;
						request.cameraVmd = camera;
					}
					request.clearAudio = clearAudio;
					if (!clearAudio && IsImportSlotFile(audio))
					{
						if (auto check = ValidateImportTrackFile(audio, "音频", 3); !check.ok) return check;
						request.audio = audio;
					}
				}
				else
				{
					if (!IsImportSlotFile(motion))
						return CmdResult::Err("invalid_params", "请选择有效的身体动作 (.vmd) 文件");
					if (auto check = ValidateImportTrackFile(motion, "动作", 0); !check.ok) return check;
					request.vmd = motion;
					request.disableMorph = disableMorph || facial == "（不导入）" || facial == "（留空 / 清除）";
					if (!request.disableMorph && IsImportSlotFile(facial))
					{
						if (auto check = ValidateImportTrackFile(facial, "表情", 1); !check.ok) return check;
						request.morphVmd = facial;
					}
					if (IsImportSlotFile(camera))
					{
						if (auto check = ValidateImportTrackFile(camera, "镜头", 2); !check.ok) return check;
						request.cameraVmd = camera;
					}
					request.clearAudio = clearAudio || audio == "（不导入）" || audio == "（留空 / 清除）";
					if (!request.clearAudio && IsImportSlotFile(audio))
					{
						if (auto check = ValidateImportTrackFile(audio, "音频", 3); !check.ok) return check;
						request.audio = audio;
					}
				}
			}
			else
			{
				if (edit)
				{
					if (IsImportSlotFile(motion)) { if (auto check = ValidateImportTrackFile(motion, "动作", 0); !check.ok) return check; request.vmd = motion; }
					if (facial == "[保持当前已导入轨道]") {}
					else if (IsImportSlotFile(facial)) { if (auto check = ValidateImportTrackFile(facial, "表情", 1); !check.ok) return check; request.morphVmd = facial; }
					else request.disableMorph = true;
					if (camera == "[保持当前已导入轨道]") {}
					else if (IsImportSlotFile(camera)) { if (auto check = ValidateImportTrackFile(camera, "镜头", 2); !check.ok) return check; request.cameraVmd = camera; }
					else request.clearCamera = true;
					if (audio == "[保持当前已导入轨道]") {}
					else if (IsImportSlotFile(audio)) { if (auto check = ValidateImportTrackFile(audio, "音频", 3); !check.ok) return check; request.audio = audio; }
					else request.clearAudio = true;
				}
				else
				{
					if (!IsImportSlotFile(motion)) return CmdResult::Err("invalid_params", "请选择有效的身体动作 (.vmd) 文件");
					if (auto check = ValidateImportTrackFile(motion, "动作", 0); !check.ok) return check;
					request.vmd = motion;
					if (facial == "（不导入）" || facial == "（留空 / 清除）") request.disableMorph = true;
					else if (IsImportSlotFile(facial)) { if (auto check = ValidateImportTrackFile(facial, "表情", 1); !check.ok) return check; request.morphVmd = facial; }
					if (IsImportSlotFile(camera)) { if (auto check = ValidateImportTrackFile(camera, "镜头", 2); !check.ok) return check; request.cameraVmd = camera; }
					if (audio == "（不导入）" || audio == "（留空 / 清除）") request.clearAudio = true;
					else if (IsImportSlotFile(audio)) { if (auto check = ValidateImportTrackFile(audio, "音频", 3); !check.ok) return check; request.audio = audio; }
				}
			}

			bool clearCover = false;
			if (draft->Find("clearCover") && !BoolParam(*draft, "clearCover", clearCover))
				return CmdResult::Err("invalid_params", "clearCover must be a boolean");
			std::string coverDataUri;
			bool hasCoverDataUri = false;
			if (draft->Find("coverDataUri"))
			{
				if (!StringParam(*draft, "coverDataUri", coverDataUri))
					return CmdResult::Err("invalid_params", "coverDataUri must be a string");
				if (!coverDataUri.empty())
				{
					if (coverDataUri.size() > 16ULL * 1024ULL * 1024ULL ||
						coverDataUri.rfind("data:image/", 0) != 0)
						return CmdResult::Err("invalid_params", "coverDataUri must be a valid image data URI");
					hasCoverDataUri = true;
				}
			}

			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (core_.importBusy || core_.cameraImportBusy || core_.libraryScan.valid())
				return CmdResult::Err("busy", "正在导入，请等待完成后保存");
			if (edit)
			{
				int64_t revision = -1;
				const bool hasRevision = IntParam(*draft, "revision", revision) || IntParam(params, "revision", revision);
				if (!usingRequestSchema && !hasRevision)
					return CmdResult::Err("invalid_params", "编辑已有曲目必须携带 revision");
				if (hasRevision && static_cast<uint64_t>(revision) != core_.libraryRevision)
					return CmdResult::Err("stale_revision", "曲库已变化，请刷新后重试");
				const int index = FindPlaylistIndex(core_.motions, editKey);
				if (index < 0) return CmdResult::Err("invalid_params", "原曲目已不在曲库中；请刷新后重试");
				const MotionEntry& entry = core_.motions[index];
				const bool editingActiveTrack =
					(core_.loadedMotionKey == editKey ||
						(!core_.player.currentMotionDir.empty() && MotionPathEqualsUtf8(core_.player.currentMotionDir, entry.dir))) &&
					(core_.player.state == PlaybackState::Playing || core_.player.state == PlaybackState::Paused);
				if (editingActiveTrack)
					return CmdResult::Err("protected", "当前曲目正在播放，无法替换其资源文件。请停止播放后重试。你的修改尚未丢失。");
			}
			GuiCommand command{GuiAction::Import};
			command.import = std::move(request);
			command.importCoverSet = hasCoverDataUri && !clearCover;
			command.importCoverClear = clearCover;
			command.importCoverDataUri = clearCover ? std::string() : std::move(coverDataUri);
			core_.QueueGui(std::move(command));
			return CmdResult::Ok("{\"started\":true}");
		}

		static bool ParseEnsembleSettings(const jsonlite::Value& value, ensemble::Settings& settings, std::string& error)
		{
			if (!value.IsObject()) { error = "settings object is required"; return false; }
			const auto boolField = [&](const char* name, bool& target) {
				const jsonlite::Value* field = value.Find(name);
				if (!field) return true;
				if (!field->IsBool()) { error = std::string(name) + " must be a boolean"; return false; }
				target = field->boolValue; return true;
			};
			const jsonlite::Value* slots = value.Find("slots");
			if (!slots || !slots->IsArray() || slots->Size() == 0 || slots->Size() > 32)
			{ error = "Ensemble preset requires at least one logical slot"; return false; }
			settings.slots.resize(slots->Size());
			const auto indexField = [&](const char* name, uint32_t& target) {
				int64_t number = 0;
				if (!IntParam(value, name, number) || number < 0 || number >= static_cast<int64_t>(settings.slots.size()))
				{ error = std::string(name) + " is out of range"; return false; }
				target = static_cast<uint32_t>(number); return true;
			};
			if (!boolField("enabled", settings.enabled) || !indexField("masterSlot", settings.masterSlot) ||
				!indexField("cameraSlot", settings.cameraSlot)) return false;
			for (size_t i = 0; i < settings.slots.size(); ++i)
			{
				const jsonlite::Value* object = slots->At(i);
				if (!object || !object->IsObject()) { error = "ensemble slot must be an object"; return false; }
				auto& slot = settings.slots[i];
				if (!BoolParam(*object, "enabled", slot.enabled)) { error = "slot.enabled must be a boolean"; return false; }
				int64_t token = 0;
				if (!IntParam(*object, "actorToken", token) || token < 0) { error = "slot.actorToken must be a non-negative integer"; return false; }
				slot.actorToken = static_cast<uint64_t>(token);
				if (!StringParam(*object, "actorHint", slot.actorHint) || !StringParam(*object, "motionKey", slot.motionKey))
				{ error = "slot.actorHint and slot.motionKey must be strings"; return false; }
				if (!VecParam(*object, "offset", slot.offset)) { error = "slot.offset must carry finite x/y/z"; return false; }
				double yaw = 0;
				if (!NumberParam(*object, "yawDegrees", yaw)) { error = "slot.yawDegrees must be a finite number"; return false; }
				slot.yawDegrees = static_cast<float>(yaw);
			}
			return ensemble::Validate(settings, error);
		}

		CmdResult EnsembleSet(const jsonlite::Value& params)
		{
			const jsonlite::Value* settingsValue = Param(params, "settings");
			int64_t revision = -1, epoch = -1;
			if (!settingsValue || !IntParam(params, "revision", revision) || !IntParam(params, "actorEpoch", epoch))
				return CmdResult::Err("invalid_params", "settings, revision and actorEpoch are required");
			ensemble::Settings settings;
			std::string error;
			if (!ParseEnsembleSettings(*settingsValue, settings, error))
				return CmdResult::Err("invalid_params", error);
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (static_cast<uint64_t>(revision) != core_.ensembleSnapshot.revision)
				return CmdResult::Err("stale_revision", "名单或组合已变化，请重新应用");
			if (static_cast<uint64_t>(epoch) != core_.sceneActorRegistry.Epoch())
				return CmdResult::Err("stale_token", "演员名单已变化，请重新绑定");
			if (core_.EnsembleActive() && core_.player.state == PlaybackState::Playing)
				return CmdResult::Err("protected", "多人舞台正在播放，请先暂停再修改配置");
			if (settings.enabled)
			{
				if (!settings.slots[settings.masterSlot].enabled || !settings.slots[settings.cameraSlot].enabled)
					return CmdResult::Err("invalid_params", "主槽与镜头跟随槽必须启用");
				if (!core_.ensembleSnapshot.available || !core_.ensembleSnapshot.verified)
					return CmdResult::Err("not_ready", "多人观察尚未通过：请在摄影场景开启观察探针并确认全部演员");
				for (const auto& slot : settings.slots)
				{
					if (!slot.enabled) continue;
					if (!core_.sceneActorRegistry.Resolve(slot.actorToken, static_cast<uint64_t>(epoch)))
						return CmdResult::Err("stale_token", "槽位演员未绑定或已失效");
					if (FindPlaylistIndex(core_.motions, slot.motionKey) < 0)
						return CmdResult::Err("invalid_params", "曲库动作不存在：" + slot.motionKey);
				}
			}
			core_.QueueEnsemble(settings, static_cast<uint64_t>(revision), static_cast<uint64_t>(epoch));
			return CmdResult::Ok();
		}

		static bool ValidPresetName(const std::string& name)
		{
			return !name.empty() && name.size() <= 80 && name != "." && name != ".." &&
				name.find_first_of("/\\:*?\"<>|") == std::string::npos;
		}

		static std::string EnsembleSettingsJson(const ensemble::Settings& settings)
		{
			std::string slots = "[";
			for (size_t i = 0; i < settings.slots.size(); ++i)
			{
				const auto& slot = settings.slots[i];
				if (i) slots += ",";
				slots += "{\"enabled\":" + JBool(slot.enabled) +
					",\"actorToken\":" + JNum(static_cast<double>(slot.actorToken)) +
					",\"actorHint\":" + JString(slot.actorHint) +
					",\"motionKey\":" + JString(slot.motionKey) +
					",\"offset\":" + JVec(slot.offset) +
					",\"yawDegrees\":" + JNum(slot.yawDegrees) + "}";
			}
			slots += "]";
			return "{\"enabled\":" + JBool(settings.enabled) +
				",\"masterSlot\":" + JNum(settings.masterSlot) +
				",\"cameraSlot\":" + JNum(settings.cameraSlot) +
				",\"slots\":" + slots + "}";
		}

		CmdResult EnsemblePresetSave(const jsonlite::Value& params)
		{
			std::string name;
			const jsonlite::Value* settingsValue = Param(params, "settings");
			if (!StringParam(params, "name", name) || !ValidPresetName(name))
				return CmdResult::Err("invalid_params", "预设名称无效");
			if (!settingsValue) return CmdResult::Err("invalid_params", "settings object is required");
			ensemble::Settings settings;
			std::string error;
			if (!ParseEnsembleSettings(*settingsValue, settings, error))
				return CmdResult::Err("invalid_params", error);
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (core_.configRoot.empty())
				return CmdResult::Err("not_ready", "配置目录尚未就绪");
			const std::string path = core_.configRoot + "/ensembles/" + name + ".json";
			if (!ensemble::Save(path, settings, error))
			{
				core_.ensembleSnapshot.notice = error;
				return CmdResult::Err("failed", error);
			}
			core_.ensembleSnapshot.notice = "组合预设已保存";
			return CmdResult::Ok();
		}

		CmdResult EnsemblePresetLoad(const jsonlite::Value& params)
		{
			std::string name;
			if (!StringParam(params, "name", name) || !ValidPresetName(name))
				return CmdResult::Err("invalid_params", "预设名称无效");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (core_.configRoot.empty())
				return CmdResult::Err("not_ready", "配置目录尚未就绪");
			if (core_.EnsembleActive() && core_.player.state == PlaybackState::Playing)
				return CmdResult::Err("protected", "多人舞台正在播放，请先暂停再载入预设");
			ensemble::Settings draft;
			std::string error;
			const std::string path = core_.configRoot + "/ensembles/" + name + ".json";
			if (!ensemble::Load(path, draft, error))
			{
				core_.ensembleSnapshot.notice = error;
				return CmdResult::Err("failed", error);
			}
			for (auto& slot : draft.slots) slot.actorToken = 0;
			core_.ensembleSnapshot.notice = "已载入草稿；请逐槽绑定当前演员后应用，原组合仍保留";
			return CmdResult::Ok("{\"settings\":" + EnsembleSettingsJson(draft) + "}");
		}

		CmdResult EnsemblePreset(const jsonlite::Value& params)
		{
			bool save = false;
			if (!BoolParam(params, "save", save))
				return CmdResult::Err("invalid_params", "save must be a boolean");
			return save ? EnsemblePresetSave(params) : EnsemblePresetLoad(params);
		}

		CmdResult EnsembleProbe(const jsonlite::Value& params)
		{
			bool enabled = false;
			if (!BoolParam(params, "enabled", enabled))
				return CmdResult::Err("invalid_params", "enabled must be a boolean");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			GuiCommand command{GuiAction::EnsembleProbe};
			command.flag = enabled;
			core_.QueueGui(std::move(command));
			return CmdResult::Ok();
		}

		CmdResult StageToggleNode(const jsonlite::Value& params)
		{
			int64_t index = -1;
			bool active = false;
			if (!IntParam(params, "index", index) || !BoolParam(params, "active", active))
				return CmdResult::Err("invalid_params", "index and active are required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			std::lock_guard<std::mutex> stageLock(core_.stageManager.stageMutex);
			const auto& candidates = core_.stageManager.candidates;
			if (index < 0 || index >= static_cast<int64_t>(candidates.size()))
				return CmdResult::Err("invalid_params", "场景节点索引无效，请重新扫描");
			const StageCandidate& candidate = candidates[static_cast<size_t>(index)];
			if (candidate.isColliderLocked)
				return CmdResult::Err("protected", "安全保护：碰撞与相机活动边界锁定开启，禁止修改");
			int ancestor = candidate.parentIndex;
			int depth = 0;
			while (ancestor >= 0 && ancestor < static_cast<int>(candidates.size()) && depth++ < static_cast<int>(candidates.size()))
			{
				if (!candidates[static_cast<size_t>(ancestor)].currentlyActive)
					return CmdResult::Err("protected", "父级节点已隐藏，请先恢复父级再修改子节点");
				ancestor = candidates[static_cast<size_t>(ancestor)].parentIndex;
			}
			core_.QueueToggleStageCandidate(static_cast<int>(index), active);
			return CmdResult::Ok();
		}

		CmdResult StageSetLoadReplacement(const jsonlite::Value& params)
		{
			std::string key;
			if (!StringParam(params, "key", key))
				return CmdResult::Err("invalid_params", "key must be a string");
			if (!key.empty() && !stage_load::IsTarget(key))
				return CmdResult::Err("invalid_params", "舞台选择无效");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (core_.configRoot.empty())
				return CmdResult::Err("not_ready", "配置目录尚未就绪");
			if (!core_.SetStageLoadReplacement(key))
				return CmdResult::Err("failed", core_.stageLoadNotice);
			return CmdResult::Ok("{\"notice\":" + JString(core_.stageLoadNotice) + "}");
		}

		CmdResult BoneTree()
		{
			struct BoneItem { int hid; const char* label; };
			struct BoneGroup { const char* title; std::vector<BoneItem> items; };
			static const BoneGroup groups[] = {
				{"躯干 (Torso)", {{0,"骨盆 (Hips)"},{7,"脊柱 (Spine)"},{8,"胸腔 (Chest)"}}},
				{"头颈 (Head & Neck)", {{9,"颈部 (Neck)"},{10,"头部 (Head)"}}},
				{"上肢双臂 (Arms)", {{11,"左肩 (L Shoulder)"},{12,"右肩 (R Shoulder)"},{13,"左上臂 (L UpperArm)"},{14,"右上臂 (R UpperArm)"},{15,"左小臂/手肘 (L Forearm)"},{16,"右小臂/手肘 (R Forearm)"},{17,"左手 (L Hand)"},{18,"右手 (R Hand)"}}},
				{"下肢双腿 (Legs)", {{1,"左大腿 (L Thigh)"},{2,"右大腿 (R Thigh)"},{3,"左小腿/膝盖 (L Calf)"},{4,"右小腿/膝盖 (R Calf)"},{5,"左足踝 (L Foot)"},{6,"右足踝 (R Foot)"},{19,"左脚趾 (L Toes)"},{20,"右脚趾 (R Toes)"}}},
				{"左手手指 (L Fingers)", {{24,"左拇指根"},{25,"左拇指中"},{26,"左拇指尖"},{27,"左食指根"},{28,"左食指中"},{29,"左食指尖"},{30,"左中指根"},{31,"左中指中"},{32,"左中指尖"},{33,"左无名根"},{34,"左无名中"},{35,"左无名尖"},{36,"左小指根"},{37,"左小指中"},{38,"左小指尖"}}},
				{"右手手指 (R Fingers)", {{39,"右拇指根"},{40,"右拇指中"},{41,"右拇指尖"},{42,"右食指根"},{43,"右食指中"},{44,"右食指尖"},{45,"右中指根"},{46,"右中指中"},{47,"右中指尖"},{48,"右无名根"},{49,"右无名中"},{50,"右无名尖"},{51,"右小指根"},{52,"右小指中"},{53,"右小指尖"}}},
			};
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			std::string json = "{\"masterEnabled\":" + JBool(core_.masterOverridesEnabled) +
				",\"revision\":" + JNum(static_cast<double>(core_.effectiveOverrides.revision)) + ",\"groups\":[";
			for (size_t g = 0; g < std::size(groups); ++g)
			{
				if (g) json += ",";
				json += "{\"title\":" + JString(groups[g].title) + ",\"bones\":[";
				for (size_t i = 0; i < groups[g].items.size(); ++i)
				{
					const auto& item = groups[g].items[i];
					if (i) json += ",";
					const int symm = BoneOverrideSet::GetSymmetricalHumanoidId(item.hid);
					float pitch = 0, yaw = 0, roll = 0;
					bool world = false, enabled = true;
					const bool found = core_.GetLiveBoneOverride(item.hid, BoneOverrideSet::GetBoneNameByHumanoidId(item.hid),
						&pitch, &yaw, &roll, &world, &enabled);
					json += "{\"hid\":" + JNum(item.hid) +
						",\"name\":" + JString(BoneOverrideSet::GetBoneNameByHumanoidId(item.hid)) +
						",\"label\":" + JString(item.label) +
						",\"symmetricalHid\":" + JNum(symm) +
						",\"hasOverride\":" + JBool(found) + "}";
				}
				json += "]}";
			}
			json += "]}";
			return CmdResult::Ok(json);
		}

		static bool BoneNameParam(const jsonlite::Value& params, int64_t& hid, std::string& name)
		{
			if (!IntParam(params, "humanoidId", hid) && !IntParam(params, "hid", hid)) return false;
			if (!StringParam(params, "boneName", name)) StringParam(params, "name", name);
			if (name.empty()) name = BoneOverrideSet::GetBoneNameByHumanoidId(static_cast<int>(hid));
			return !name.empty();
		}

		CmdResult BoneGet(const jsonlite::Value& params)
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (!params.Find("humanoidId") && !params.Find("hid"))
				return CmdResult::Ok(BoneJsonLocked());
			int64_t hid = -1;
			std::string name;
			if (!BoneNameParam(params, hid, name))
				return CmdResult::Err("invalid_params", "hid/humanoidId and a known bone name are required");
			float pitch = 0, yaw = 0, roll = 0;
			bool world = false, enabled = true;
			const bool found = core_.GetLiveBoneOverride(static_cast<int>(hid), name.c_str(), &pitch, &yaw, &roll, &world, &enabled);
			return CmdResult::Ok("{\"humanoidId\":" + JNum(static_cast<double>(hid)) +
				",\"hid\":" + JNum(static_cast<double>(hid)) +
				",\"boneName\":" + JString(name) +
				",\"name\":" + JString(name) +
				",\"found\":" + JBool(found) +
				",\"pitch\":" + JNum(pitch) + ",\"yaw\":" + JNum(yaw) + ",\"roll\":" + JNum(roll) +
				",\"spaceWorld\":" + JBool(world) + ",\"enabled\":" + JBool(enabled) +
				",\"symmetricalHid\":" + JNum(BoneOverrideSet::GetSymmetricalHumanoidId(static_cast<int>(hid))) + "}");
		}

		CmdResult BoneSet(const jsonlite::Value& params)
		{
			int64_t hid = -1;
			std::string name;
			double pitch = 0, yaw = 0, roll = 0;
			bool world = false, enabled = true, symmetrical = false;
			if (!BoneNameParam(params, hid, name) ||
				!NumberParam(params, "pitch", pitch) || !NumberParam(params, "yaw", yaw) || !NumberParam(params, "roll", roll) ||
				!BoolParam(params, "spaceWorld", world) || !BoolParam(params, "enabled", enabled) ||
				!BoolParam(params, "symmetrical", symmetrical))
				return CmdResult::Err("invalid_params", "humanoidId/boneName/pitch/yaw/roll/spaceWorld/enabled/symmetrical are required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			core_.SetLiveBoneOverride(static_cast<int>(hid), name.c_str(), static_cast<float>(pitch),
				static_cast<float>(yaw), static_cast<float>(roll), world, enabled, symmetrical);
			boneNotice_.clear();
			return CmdResult::Ok();
		}

		CmdResult BoneReset(const jsonlite::Value& params)
		{
			int64_t hid = -1;
			std::string name;
			bool symmetrical = false;
			if (!BoneNameParam(params, hid, name) || !BoolParam(params, "symmetrical", symmetrical))
				return CmdResult::Err("invalid_params", "humanoidId/boneName and symmetrical are required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			core_.ResetLiveBoneOverride(static_cast<int>(hid), name.c_str(), symmetrical);
			boneNotice_.clear();
			return CmdResult::Ok();
		}

		CmdResult BoneResetAll()
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			core_.ResetAllLiveOverrides();
			boneNotice_ = "已清空全部运行时骨骼调整";
			return CmdResult::Ok();
		}

		CmdResult BoneSetMaster(const jsonlite::Value& params)
		{
			bool enabled = false;
			if (!BoolParam(params, "enabled", enabled))
				return CmdResult::Err("invalid_params", "enabled must be a boolean");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			if (core_.masterOverridesEnabled != enabled) core_.ToggleOverrides();
			boneNotice_ = core_.masterOverridesEnabled ? "骨骼覆盖总开关已开启" : "骨骼覆盖总开关已关闭";
			return CmdResult::Ok();
		}

		CmdResult BoneToggle()
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (const auto guard = MutationGuardLocked(); !guard.ok) return guard;
			const bool enabled = core_.ToggleOverrides();
			boneNotice_ = enabled ? "骨骼覆盖总开关已开启" : "骨骼覆盖总开关已关闭";
			return CmdResult::Ok("{\"enabled\":" + JBool(enabled) + "}");
		}

		CmdResult BoneReload()
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			core_.LoadOverrides(true);
			boneNotice_ = "已重新加载磁盘骨骼覆盖配置";
			return CmdResult::Ok();
		}

		CmdResult BoneSaveToMotion()
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (!core_.SaveLiveOverridesToMotion())
				return CmdResult::Err("failed", "保存失败：无法写入当前曲目目录");
			boneNotice_ = "已保存到当前曲目目录";
			return CmdResult::Ok();
		}

		CmdResult BoneSaveToGlobal()
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (!core_.SaveLiveOverridesToGlobal())
				return CmdResult::Err("failed", "保存失败：无法写入全局文件");
			boneNotice_ = "已保存到全局默认配置";
			return CmdResult::Ok();
		}

		bool GazeRuntimeAvailable() const
		{
			return core_.lookAt.fnSetTarget && core_.lookAt.fnSetWeights && core_.lookAt.fnSetEnabled;
		}

		CmdResult GazeSet(const jsonlite::Value& params)
		{
			const jsonlite::Value* settingsValue = Param(params, "settings");
			if (!settingsValue || !settingsValue->IsObject())
				return CmdResult::Err("invalid_params", "settings object is required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			int64_t mode = static_cast<int64_t>(core_.lookAt.gazeMode.load());
			bool desktop = core_.lookAt.desktopLookAtEnabled.load();
			double eye = core_.lookAt.eyeWeight.load();
			double head = core_.lookAt.headWeight.load();
			double body = core_.lookAt.bodyWeight.load();
			if (settingsValue->Find("mode") && (!IntParam(*settingsValue, "mode", mode) || mode < 0 || mode > 2))
				return CmdResult::Err("invalid_params", "mode must be 0 (Auto), 1 (ForceOff) or 2 (DesktopOn)");
			if (settingsValue->Find("desktopLookAtEnabled") && !BoolParam(*settingsValue, "desktopLookAtEnabled", desktop))
				return CmdResult::Err("invalid_params", "desktopLookAtEnabled must be a boolean");
			if (settingsValue->Find("eyeWeight") && !NumberParam(*settingsValue, "eyeWeight", eye))
				return CmdResult::Err("invalid_params", "eyeWeight must be a finite number");
			if (settingsValue->Find("headWeight") && !NumberParam(*settingsValue, "headWeight", head))
				return CmdResult::Err("invalid_params", "headWeight must be a finite number");
			if (settingsValue->Find("bodyWeight") && !NumberParam(*settingsValue, "bodyWeight", body))
				return CmdResult::Err("invalid_params", "bodyWeight must be a finite number");
			for (double value : {eye, head, body})
				if (value < 0.0 || value > 1.0)
					return CmdResult::Err("invalid_params", "视线权重必须在 0..1 范围内");
			if (!GazeRuntimeAvailable()) return CmdResult::Err("not_ready", "视线控制器接口尚未就绪");
			core_.lookAt.gazeMode.store(static_cast<GazeMode>(mode));
			core_.config.gazeMode = static_cast<int>(mode);
			core_.lookAt.desktopLookAtEnabled.store(desktop);
			core_.config.desktopLookAtEnabled = desktop ? 1 : 0;
			core_.lookAt.eyeWeight.store(static_cast<float>(eye));
			core_.lookAt.headWeight.store(static_cast<float>(head));
			core_.lookAt.bodyWeight.store(static_cast<float>(body));
			core_.config.eyeWeight = static_cast<float>(eye);
			core_.config.headWeight = static_cast<float>(head);
			core_.config.bodyWeight = static_cast<float>(body);
			return CmdResult::Ok();
		}

		CmdResult GazeSetMode(const jsonlite::Value& params)
		{
			int64_t mode = -1;
			if (!IntParam(params, "mode", mode) || mode < 0 || mode > 2)
				return CmdResult::Err("invalid_params", "mode must be 0 (Auto), 1 (ForceOff) or 2 (DesktopOn)");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (!GazeRuntimeAvailable()) return CmdResult::Err("not_ready", "视线控制器接口尚未就绪");
			core_.lookAt.gazeMode.store(static_cast<GazeMode>(mode));
			core_.config.gazeMode = static_cast<int>(mode);
			return CmdResult::Ok();
		}

		CmdResult GazeSetDesktop(const jsonlite::Value& params)
		{
			bool enabled = false;
			if (!BoolParam(params, "enabled", enabled))
				return CmdResult::Err("invalid_params", "enabled must be a boolean");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (!GazeRuntimeAvailable()) return CmdResult::Err("not_ready", "视线控制器接口尚未就绪");
			core_.lookAt.desktopLookAtEnabled.store(enabled);
			core_.config.desktopLookAtEnabled = enabled ? 1 : 0;
			return CmdResult::Ok();
		}

		CmdResult GazeSetWeights(const jsonlite::Value& params)
		{
			double eye = 0, head = 0, body = 0;
			if (!NumberParam(params, "eye", eye) || !NumberParam(params, "head", head) || !NumberParam(params, "body", body))
				return CmdResult::Err("invalid_params", "eye/head/body numbers are required");
			for (double value : {eye, head, body})
				if (value < 0.0 || value > 1.0)
					return CmdResult::Err("invalid_params", "视线权重必须在 0..1 范围内");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (!GazeRuntimeAvailable()) return CmdResult::Err("not_ready", "视线控制器接口尚未就绪");
			core_.lookAt.eyeWeight.store(static_cast<float>(eye));
			core_.lookAt.headWeight.store(static_cast<float>(head));
			core_.lookAt.bodyWeight.store(static_cast<float>(body));
			core_.config.eyeWeight = static_cast<float>(eye);
			core_.config.headWeight = static_cast<float>(head);
			core_.config.bodyWeight = static_cast<float>(body);
			return CmdResult::Ok();
		}

		CmdResult GazeCycle()
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (!GazeRuntimeAvailable()) return CmdResult::Err("not_ready", "视线控制器接口尚未就绪");
			const int mode = core_.CycleGazeMode();
			return CmdResult::Ok("{\"mode\":" + JNum(mode) + "}");
		}

		static bool ParseSoleSettingsValue(const jsonlite::Value& value, SoleMeasurementSettings& settings, std::string& error)
		{
			if (!value.IsObject()) { error = "settings object is required"; return false; }
			const auto boolField = [&](const char* name, bool& target) {
				const jsonlite::Value* field = value.Find(name);
				if (!field) return true;
				if (!field->IsBool()) { error = std::string(name) + " must be a boolean"; return false; }
				target = field->boolValue; return true;
			};
			const auto floatField = [&](const char* name, float& target) {
				const jsonlite::Value* field = value.Find(name);
				if (!field) return true;
				if (!field->IsNumber() || !std::isfinite(field->numberValue))
				{ error = std::string(name) + " must be a finite number"; return false; }
				target = static_cast<float>(field->numberValue); return true;
			};
			if (!boolField("enabled", settings.enabled) || !boolField("planeCalibrated", settings.planeCalibrated) ||
				!boolField("shoeCalibrated", settings.shoeCalibrated) || !boolField("useBootProxy", settings.useBootProxy) ||
				!floatField("planeOffsetM", settings.planeOffsetM) || !floatField("displayBandM", settings.displayBandM))
				return false;
			if (const jsonlite::Value* normal = value.Find("planeNormal"))
			{
				if (!normal->IsObject()) { error = "planeNormal must be an {x,y,z} object"; return false; }
				double x = 0, y = 0, z = 0;
				if (!NumberParam(*normal, "x", x) || !NumberParam(*normal, "y", y) || !NumberParam(*normal, "z", z))
				{ error = "planeNormal must carry finite x/y/z"; return false; }
				settings.planeNormal = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
			}
			if (const jsonlite::Value* points = value.Find("points"))
			{
				if (!points->IsArray() || points->Size() != 2) { error = "points must be a 2x3 array"; return false; }
				for (size_t side = 0; side < 2; ++side)
				{
					const jsonlite::Value* foot = points->At(side);
					if (!foot || !foot->IsArray() || foot->Size() != 3) { error = "each foot needs three points"; return false; }
					for (size_t i = 0; i < 3; ++i)
					{
						const jsonlite::Value* point = foot->At(i);
						if (!point || !point->IsObject()) { error = "point must be an {x,y,z} object"; return false; }
						double x = 0, y = 0, z = 0;
						if (!NumberParam(*point, "x", x) || !NumberParam(*point, "y", y) || !NumberParam(*point, "z", z))
						{ error = "point must carry finite x/y/z"; return false; }
						settings.points[side][i] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
					}
				}
			}
			if (!settings.IsValid()) { error = "鞋底测量设置无效"; return false; }
			return true;
		}

		CmdResult SoleSet(const jsonlite::Value& params)
		{
			const jsonlite::Value* settingsValue = Param(params, "settings");
			if (!settingsValue) return CmdResult::Err("invalid_params", "settings object is required");
			SoleMeasurementSettings settings;
			std::string error;
			if (!ParseSoleSettingsValue(*settingsValue, settings, error))
				return CmdResult::Err("invalid_params", error);
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			core_.QueueSoleSettings(settings);
			return CmdResult::Ok();
		}

		CmdResult SolePitchSet(const jsonlite::Value& params)
		{
			const jsonlite::Value* settingsValue = Param(params, "settings");
			if (!settingsValue || !settingsValue->IsObject())
				return CmdResult::Err("invalid_params", "settings object is required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			sole_pitch::Settings settings = core_.experimentalSolePitchSettings;
			if (const jsonlite::Value* enabled = settingsValue->Find("enabled"))
			{
				if (!enabled->IsBool()) return CmdResult::Err("invalid_params", "enabled must be a boolean");
				settings.experimentalSolePitchCorrection = enabled->boolValue;
			}
			else if (const jsonlite::Value* correction = settingsValue->Find("experimentalSolePitchCorrection"))
			{
				if (!correction->IsBool()) return CmdResult::Err("invalid_params", "experimentalSolePitchCorrection must be a boolean");
				settings.experimentalSolePitchCorrection = correction->boolValue;
			}
			if (const jsonlite::Value* envelope = settingsValue->Find("useMeshEnvelope"))
			{
				if (!envelope->IsBool()) return CmdResult::Err("invalid_params", "useMeshEnvelope must be a boolean");
				settings.useMeshEnvelope = envelope->boolValue;
			}
			if (const jsonlite::Value* field = settingsValue->Find("maxPitchDegrees"))
			{
				if (!field->IsNumber() || !std::isfinite(field->numberValue))
					return CmdResult::Err("invalid_params", "maxPitchDegrees must be a finite number");
				settings.maxPitchDegrees = static_cast<float>(field->numberValue);
			}
			if (!settings.IsValid())
				return CmdResult::Err("invalid_params", "maxPitchDegrees 必须在 0..35 范围内");
			core_.QueueExperimentalSolePitch(settings);
			return CmdResult::Ok();
		}

		static bool ParseStudioSettings(const jsonlite::Value& value, studio::Settings& settings, std::string& error)
		{
			if (!value.IsObject()) { error = "settings object is required"; return false; }
			const auto boolField = [&](const jsonlite::Value& object, const char* name, bool& target) {
				const jsonlite::Value* field = object.Find(name);
				if (!field) return true;
				if (!field->IsBool()) { error = std::string(name) + " must be a boolean"; return false; }
				target = field->boolValue; return true;
			};
			const auto floatField = [&](const jsonlite::Value& object, const char* name, float& target) {
				const jsonlite::Value* field = object.Find(name);
				if (!field) return true;
				if (!field->IsNumber() || !std::isfinite(field->numberValue))
				{ error = std::string(name) + " must be a finite number"; return false; }
				target = static_cast<float>(field->numberValue); return true;
			};
			const auto colorField = [&](const jsonlite::Value& object, const char* name, std::array<float, 3>& target) {
				const jsonlite::Value* field = object.Find(name);
				if (!field) return true;
				if (!field->IsArray() || field->Size() != 3) { error = std::string(name) + " must be an RGB array"; return false; }
				for (size_t i = 0; i < 3; ++i)
				{
					const jsonlite::Value* component = field->At(i);
					if (!component || !component->IsNumber() || !std::isfinite(component->numberValue))
					{ error = std::string(name) + " must contain finite numbers"; return false; }
					target[i] = static_cast<float>(component->numberValue);
				}
				return true;
			};
			if (const jsonlite::Value* lighting = value.Find("lighting"))
			{
				if (!lighting->IsObject()) { error = "lighting must be an object"; return false; }
				auto& l = settings.lighting;
				if (!boolField(*lighting, "enabled", l.enabled) || !boolField(*lighting, "actorStyle", l.actorStyle) ||
					!boolField(*lighting, "decalEnabled", l.decalEnabled) ||
					!colorField(*lighting, "keyColor", l.keyColor) || !colorField(*lighting, "fillColor", l.fillColor) ||
					!colorField(*lighting, "rimColor", l.rimColor) || !colorField(*lighting, "decalTint", l.decalTint))
					return false;
				for (auto [name, target] : {std::pair{"keyIntensity", &l.keyIntensity}, {"fillIntensity", &l.fillIntensity},
					{"rimIntensity", &l.rimIntensity}, {"keyYaw", &l.keyYaw}, {"keyPitch", &l.keyPitch},
					{"fillYaw", &l.fillYaw}, {"fillPitch", &l.fillPitch}, {"rimYaw", &l.rimYaw}, {"rimPitch", &l.rimPitch},
					{"actorIntensity", &l.actorIntensity}, {"rimPower", &l.rimPower}, {"decalIntensityScale", &l.decalIntensityScale}})
					if (!floatField(*lighting, name, *target)) return false;
			}
			if (const jsonlite::Value* video = value.Find("video"))
			{
				if (!video->IsObject()) { error = "video must be an object"; return false; }
				auto& v = settings.video;
				if (!boolField(*video, "enabled", v.enabled) || !boolField(*video, "paused", v.paused) ||
					!boolField(*video, "loop", v.loop)) return false;
				if (const jsonlite::Value* path = video->Find("path"))
				{
					if (!path->IsString()) { error = "video.path must be a string"; return false; }
					v.path = path->stringValue;
				}
				if (!floatField(*video, "distance", v.distance) || !floatField(*video, "width", v.width) ||
					!floatField(*video, "offsetX", v.offsetX) || !floatField(*video, "offsetY", v.offsetY))
					return false;
			}
			return settings.Validate(error);
		}

		CmdResult StudioSet(const jsonlite::Value& params)
		{
			const jsonlite::Value* settingsValue = Param(params, "settings");
			if (!settingsValue) return CmdResult::Err("invalid_params", "settings object is required");
			studio::Settings settings;
			std::string error;
			if (!ParseStudioSettings(*settingsValue, settings, error))
				return CmdResult::Err("invalid_params", error);
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (!core_.QueueStudioSettings(settings))
				return CmdResult::Err("invalid_params", "布光与视频设置无效");
			return CmdResult::Ok();
		}

		CmdResult StudioSetLighting(const jsonlite::Value& params)
		{
			const jsonlite::Value* lighting = Param(params, "lighting");
			if (!lighting || !lighting->IsObject())
				return CmdResult::Err("invalid_params", "lighting object is required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			studio::Settings settings = core_.studioSettings;
			std::string error;
			jsonlite::Value wrapper;
			wrapper.type = jsonlite::Type::Object;
			wrapper.objectMembers.emplace_back("lighting", *lighting);
			if (!ParseStudioSettings(wrapper, settings, error))
				return CmdResult::Err("invalid_params", error);
			if (!core_.QueueStudioSettings(settings))
				return CmdResult::Err("invalid_params", "布光设置无效");
			return CmdResult::Ok();
		}

		CmdResult StudioSetVideo(const jsonlite::Value& params)
		{
			const jsonlite::Value* video = Param(params, "video");
			if (!video || !video->IsObject())
				return CmdResult::Err("invalid_params", "video object is required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			studio::Settings settings = core_.studioSettings;
			std::string error;
			jsonlite::Value wrapper;
			wrapper.type = jsonlite::Type::Object;
			wrapper.objectMembers.emplace_back("video", *video);
			if (!ParseStudioSettings(wrapper, settings, error))
				return CmdResult::Err("invalid_params", error);
			if (!core_.QueueStudioSettings(settings))
				return CmdResult::Err("invalid_params", "视频设置无效");
			return CmdResult::Ok();
		}

		static bool TriStateParam(const jsonlite::Value& params, const char* name, int& target)
		{
			const jsonlite::Value* value = params.Find(name);
			if (!value) return true;
			if (value->IsNull()) { target = -1; return true; }
			if (value->IsBool()) { target = value->boolValue ? 1 : 0; return true; }
			if (value->IsNumber() && value->numberValue == std::floor(value->numberValue) &&
				value->numberValue >= -1 && value->numberValue <= 1)
			{ target = static_cast<int>(value->numberValue); return true; }
			return false;
		}

		CmdResult ExperimentsSet(const jsonlite::Value& params)
		{
			const jsonlite::Value* patch = Param(params, "patch");
			if (!patch || !patch->IsObject())
				return CmdResult::Err("invalid_params", "patch object is required");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			int mouth = core_.config.facialMikuMouthFit;
			int eye = core_.config.facialMikuEyeFit;
			int allow = core_.config.facialMikuFitAllowUnverified;
			if (!TriStateParam(*patch, "mikuMouthFit", mouth) ||
				!TriStateParam(*patch, "mikuEyeFit", eye) ||
				!TriStateParam(*patch, "mikuFitAllowUnverified", allow))
				return CmdResult::Err("invalid_params", "Miku 拟合开关必须为 boolean 或 -1/0/1");
			bool hasWaist = patch->Find("waistSupport") != nullptr;
			bool waist = core_.guiExperimentalWaistSupport.load();
			if (hasWaist && !BoolParam(*patch, "waistSupport", waist))
				return CmdResult::Err("invalid_params", "waistSupport must be a boolean");
			bool hasBeforeQuartz = patch->Find("beforeQuartz") != nullptr;
			bool beforeQuartz = core_.guiBeforeQuartzExperiment.load();
			if (hasBeforeQuartz && !BoolParam(*patch, "beforeQuartz", beforeQuartz))
				return CmdResult::Err("invalid_params", "beforeQuartz must be a boolean");
			if (hasBeforeQuartz && beforeQuartz && core_.guiPhysicsBackend.load() != 2)
				return CmdResult::Err("invalid_params", "BeforeQuartz 仅在原生注入后端可用");
			bool hasMuteSe = patch->Find("muteNativeSoundEffects") != nullptr;
			bool muteSe = core_.guiMuteNativeSoundEffects.load();
			if (hasMuteSe && !BoolParam(*patch, "muteNativeSoundEffects", muteSe))
				return CmdResult::Err("invalid_params", "muteNativeSoundEffects must be a boolean");
			core_.config.facialMikuMouthFit = mouth;
			core_.config.facialMikuEyeFit = eye;
			core_.config.facialMikuFitAllowUnverified = allow;
			if (hasWaist)
			{
				core_.guiExperimentalWaistSupport.store(waist);
				core_.QueueExperimentalWaistSupport(waist);
			}
			if (hasBeforeQuartz)
			{
				core_.guiBeforeQuartzExperiment.store(beforeQuartz);
				core_.QueueBeforeQuartzExperiment(beforeQuartz);
			}
			if (hasMuteSe)
			{
				core_.guiMuteNativeSoundEffects.store(muteSe);
				core_.QueueMuteNativeSoundEffects(muteSe);
			}
			return CmdResult::Ok();
		}

		CmdResult ExperimentsSetMiku(const jsonlite::Value& params)
		{
			int mouth = core_.config.facialMikuMouthFit;
			int eye = core_.config.facialMikuEyeFit;
			int allow = core_.config.facialMikuFitAllowUnverified;
			if (!TriStateParam(params, "mouthFit", mouth) || !TriStateParam(params, "eyeFit", eye) ||
				!TriStateParam(params, "allowUnverified", allow))
				return CmdResult::Err("invalid_params", "Miku 拟合开关必须为 true/false/null 或 -1/0/1");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			core_.config.facialMikuMouthFit = mouth;
			core_.config.facialMikuEyeFit = eye;
			core_.config.facialMikuFitAllowUnverified = allow;
			return CmdResult::Ok();
		}

		CmdResult ExperimentsSetWaistSupport(const jsonlite::Value& params)
		{
			bool enabled = false;
			if (!BoolParam(params, "enabled", enabled))
				return CmdResult::Err("invalid_params", "enabled must be a boolean");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			core_.QueueExperimentalWaistSupport(enabled);
			return CmdResult::Ok();
		}

		CmdResult ExperimentsSetBeforeQuartz(const jsonlite::Value& params)
		{
			bool enabled = false;
			if (!BoolParam(params, "enabled", enabled))
				return CmdResult::Err("invalid_params", "enabled must be a boolean");
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			if (enabled && core_.guiPhysicsBackend.load() != 2)
				return CmdResult::Err("invalid_params", "BeforeQuartz 仅在原生注入后端可用");
			core_.QueueBeforeQuartzExperiment(enabled);
			return CmdResult::Ok();
		}

		// ---- parsing structured settings -----------------------------------------

		static bool ParseCameraSettings(const jsonlite::Value& value, CameraSettings& settings, std::string& error)
		{
			const auto boolField = [&](const char* name, bool& target) {
				const jsonlite::Value* field = value.Find(name);
				if (!field) return true;
				if (!field->IsBool()) { error = std::string(name) + " must be a boolean"; return false; }
				target = field->boolValue;
				return true;
			};
			const auto intField = [&](const char* name, int& target, int low, int high) {
				const jsonlite::Value* field = value.Find(name);
				if (!field) return true;
				if (!field->IsNumber() || field->numberValue != std::floor(field->numberValue) ||
					field->numberValue < low || field->numberValue > high)
				{ error = std::string(name) + " is out of range"; return false; }
				target = static_cast<int>(field->numberValue);
				return true;
			};
			const auto floatField = [&](const char* name, float& target) {
				const jsonlite::Value* field = value.Find(name);
				if (!field) return true;
				if (!field->IsNumber() || !std::isfinite(field->numberValue))
				{ error = std::string(name) + " must be a finite number"; return false; }
				target = static_cast<float>(field->numberValue);
				return true;
			};
			const auto vecField = [&](const char* name, Vector3& target) {
				const jsonlite::Value* field = value.Find(name);
				if (!field) return true;
				if (!field->IsObject()) { error = std::string(name) + " must be an {x,y,z} object"; return false; }
				double x = 0, y = 0, z = 0;
				if (!NumberParam(*field, "x", x) || !NumberParam(*field, "y", y) || !NumberParam(*field, "z", z))
				{ error = std::string(name) + " must carry finite x/y/z"; return false; }
				target = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
				return true;
			};
			const auto transformField = [&](const char* name, CameraModeTransform& target) {
				const jsonlite::Value* field = value.Find(name);
				if (!field) return true;
				if (!field->IsObject()) { error = std::string(name) + " must be an object"; return false; }
				const jsonlite::Value* position = field->Find("position");
				const jsonlite::Value* rotation = field->Find("rotation");
				if (!position || !rotation) { error = std::string(name) + " needs position and rotation"; return false; }
				const auto parseVec = [&](const jsonlite::Value* object, Vector3& out, const char* label) {
					double x = 0, y = 0, z = 0;
					if (!object->IsObject() || !NumberParam(*object, "x", x) ||
						!NumberParam(*object, "y", y) || !NumberParam(*object, "z", z))
					{ error = std::string(name) + label + " must carry finite x/y/z"; return false; }
					out = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
					return true;
				};
				if (!parseVec(position, target.position, ".position") ||
					!parseVec(rotation, target.rotation, ".rotation"))
					return false;
				return true;
			};
			int mode = static_cast<int>(settings.mode);
			int followBehavior = static_cast<int>(settings.followBehavior);
			int followTarget = static_cast<int>(settings.followTarget);
			if (!boolField("enabled", settings.enabled) ||
				!intField("mode", mode, 0, 2) ||
				!intField("followBehavior", followBehavior, 0, 1) ||
				!intField("followTarget", followTarget, 0, 2) ||
				!boolField("followActor", settings.followActor) ||
				!boolField("limitToScene", settings.limitToScene) ||
				!vecField("boundsMin", settings.boundsMin) ||
				!vecField("boundsMax", settings.boundsMax) ||
				!vecField("followOffset", settings.followOffset) ||
				!vecField("targetOffset", settings.targetOffset) ||
				!vecField("vmdOffset", settings.vmdOffset) ||
				!floatField("vmdRangeScale", settings.vmdRangeScale) ||
				!vecField("followFactor", settings.followFactor) ||
				!vecField("followRate", settings.followRate) ||
				!floatField("followFov", settings.followFov) ||
				!boolField("srpFinalOverwriteFallback", settings.srpFinalOverwriteFallback) ||
				!intField("actorShadowExperimentMode", settings.actorShadowExperimentMode, 0, 2) ||
				!transformField("transformTrack", settings.transformTrack) ||
				!transformField("transformFollowMove", settings.transformFollowMove) ||
				!transformField("transformLookAt", settings.transformLookAt) ||
				!boolField("usePerModeTransforms", settings.usePerModeTransforms))
				return false;
			settings.mode = static_cast<CameraMode>(mode);
			settings.followBehavior = static_cast<FollowBehavior>(followBehavior);
			settings.followTarget = static_cast<CameraFollowTarget>(followTarget);
			return true;
		}

		static bool ParseStageSettings(const jsonlite::Value& value, StageSettings& settings, std::string& error)
		{
			const auto boolField = [&](const char* name, bool& target) {
				const jsonlite::Value* field = value.Find(name);
				if (!field) return true;
				if (!field->IsBool()) { error = std::string(name) + " must be a boolean"; return false; }
				target = field->boolValue;
				return true;
			};
			const auto floatField = [&](const char* name, float& target) {
				const jsonlite::Value* field = value.Find(name);
				if (!field) return true;
				if (!field->IsNumber() || !std::isfinite(field->numberValue))
				{ error = std::string(name) + " must be a finite number"; return false; }
				target = static_cast<float>(field->numberValue);
				return true;
			};
			const jsonlite::Value* color = value.Find("greenScreenColor");
			if (color)
			{
				if (!color->IsArray() || color->Size() != 4)
				{ error = "greenScreenColor must be an RGBA array of 4 numbers"; return false; }
				for (size_t i = 0; i < 4; ++i)
				{
					const jsonlite::Value* component = color->At(i);
					if (!component || !component->IsNumber() || !std::isfinite(component->numberValue) ||
						component->numberValue < 0 || component->numberValue > 1)
					{ error = "greenScreenColor components must be within 0..1"; return false; }
					settings.greenScreenColor[i] = static_cast<float>(component->numberValue);
				}
			}
			const jsonlite::Value* preset = value.Find("selectedPresetIndex");
			if (preset)
			{
				if (!preset->IsNumber() || preset->numberValue != std::floor(preset->numberValue) ||
					preset->numberValue < 0 || preset->numberValue > 64)
				{ error = "selectedPresetIndex is out of range"; return false; }
				settings.selectedPresetIndex = static_cast<int>(preset->numberValue);
			}
			const auto stringField = [&](const char* name, std::string& target) {
				const jsonlite::Value* field = value.Find(name);
				if (!field) return true;
				if (!field->IsString() || field->stringValue.size() > 512 ||
					field->stringValue.find('\0') != std::string::npos)
				{ error = std::string(name) + " must be a short string"; return false; }
				target = field->stringValue;
				return true;
			};
			return boolField("hideBackground", settings.hideBackground) &&
				boolField("greenScreenActive", settings.greenScreenActive) &&
				floatField("screenDistance", settings.screenDistance) &&
				floatField("screenScale", settings.screenScale) &&
				floatField("screenHeightOffset", settings.screenHeightOffset) &&
				boolField("overrideCameraClear", settings.overrideCameraClear) &&
				stringField("customStageKey", settings.customStageKey) &&
				floatField("stageFloorYOffset", settings.stageFloorYOffset) &&
				floatField("stageYawOffset", settings.stageYawOffset) &&
				floatField("stagePosXOffset", settings.stagePosXOffset) &&
				floatField("stagePosZOffset", settings.stagePosZOffset) &&
				boolField("lightingEffectsEnabled", settings.lightingEffectsEnabled) &&
				boolField("customNodeControl", settings.customNodeControl);
		}

		// ---- snapshot serializers (caller holds stateMutex) ------------------------

		std::string BuildHello() const
		{
			std::string methods = "[";
			for (size_t i = 0; i < std::size(kSupportedMethods); ++i)
			{
				if (i) methods += ",";
				methods += JString(kSupportedMethods[i]);
			}
			methods += "]";
			std::string events = "[";
			for (size_t i = 0; i < std::size(kSupportedEvents); ++i)
			{
				if (i) events += ",";
				events += JString(kSupportedEvents[i]);
			}
			events += "]";
			return "{\"protocol\":1,\"sessionId\":" + JNum(static_cast<double>(sessionId_)) +
				",\"pluginVersion\":" + JString(hooks_.pluginVersion) +
				",\"supportedMethods\":" + methods + ",\"supportedEvents\":" + events + "}";
		}

		std::string PlayerStateJsonLocked() const
		{
			const auto& player = core_.player;
			const char* state = "stopped";
			switch (player.state)
			{
			case PlaybackState::Playing: state = "playing"; break;
			case PlaybackState::Paused: state = "paused"; break;
			case PlaybackState::Preview: state = "preview"; break;
			default: break;
			}
			std::string title = "未加载曲目";
			if (!core_.loadedMotionKey.empty())
			{
				const int index = FindPlaylistIndex(core_.motions, core_.loadedMotionKey);
				title = index >= 0 && !core_.motions[index].title.empty()
					? core_.motions[index].title : core_.loadedMotionKey;
			}
			const int playMode = core_.EnsembleActive() ? (player.loop ? 1 : 0)
				: static_cast<int>(core_.playlist.mode);
			return "{\"state\":" + JString(state) + ",\"title\":" + JString(title) +
				",\"time\":" + JNum(player.currentTime) +
				",\"duration\":" + JNum(player.TimelineDuration()) +
				",\"totalFrames\":" + JNum(player.clip.total_frames) +
				",\"fps\":" + JNum(player.clip.fps) +
				",\"speed\":" + JNum(player.playbackSpeedMultiplier) +
				",\"volume\":" + JNum(player.musicVolume) +
				",\"playMode\":" + JNum(playMode) + "}";
		}

		std::string PermissionJsonLocked() const
		{
			const bool allowed = core_.InjectionAllowed();
			const std::string notice = core_.injectionStatus.empty()
				? "仅偶像摄影与更衣室可以使用；其他页面保留设置但不生效。" : core_.injectionStatus;
			return "{\"injectionAllowed\":" + JBool(allowed) + ",\"notice\":" + JString(notice) + "}";
		}

		std::string LibraryJsonLocked() const
		{
			std::string entries = "[";
			for (size_t i = 0; i < core_.motions.size(); ++i)
			{
				const auto& entry = core_.motions[i];
				if (i) entries += ",";
				entries += "{\"id\":" + JString(entry.name) + ",\"title\":" + JString(entry.title) +
					",\"subtitle\":" + JString(entry.subtitle) +
					",\"directory\":" + JString(entry.dir) +
					",\"durationSeconds\":" + JNum(entry.durationSeconds) +
					",\"totalFrames\":" + JNum(entry.totalFrames) +
					",\"hasMotion\":true,\"hasFacial\":" + JBool(entry.hasFacial) +
					",\"hasCamera\":" + JBool(entry.hasCamera) +
					",\"hasAudio\":" + JBool(entry.hasAudio) +
					",\"audioFile\":" + JString(entry.audioFile) +
					",\"hasCover\":" + JBool(entry.hasCover) + "}";
			}
			entries += "]";
			return "{\"revision\":" + JNum(static_cast<double>(core_.libraryRevision)) +
				",\"entries\":" + entries +
				",\"refreshing\":" + JBool(core_.libraryScan.valid()) +
				",\"notice\":" + JString(core_.playlistNotice) + "}";
		}

		std::string LoadedJsonLocked() const
		{
			return "{\"motionKey\":" + JString(core_.loadedMotionKey) +
				",\"generation\":" + JNum(static_cast<double>(core_.loadedMotionGeneration)) +
				",\"injectionSession\":" + JNum(static_cast<double>(core_.injectionSession)) +
				",\"sourceMissing\":" + JBool(core_.loadedSourceMissing) +
				",\"sourceChanged\":" + JBool(core_.loadedSourceChanged) + "}";
		}

		std::string PlaylistJsonLocked() const
		{
			const int mode = core_.EnsembleActive() ? (core_.player.loop ? 1 : 0)
				: static_cast<int>(core_.playlist.mode);
			return "{\"mode\":" + JNum(mode) + ",\"notice\":" + JString(core_.playlistNotice) + "}";
		}

		std::string ActorsJsonLocked() const
		{
			const auto& entries = core_.sceneActorRegistry.Entries();
			std::string actors = "[";
			for (size_t i = 0; i < entries.size(); ++i)
			{
				const auto& item = entries[i];
				const std::string label = FormatSceneActorLabel(item, i + 1);
				if (i) actors += ",";
				actors += "{\"token\":" + JNum(static_cast<double>(item.token)) +
					",\"label\":" + JString(label) +
					",\"selected\":" + JBool(item.actor == core_.player.controller.targetActor) + "}";
			}
			actors += "]";
			const auto labelFor = [&](void* actor) -> std::string {
				for (size_t i = 0; i < entries.size(); ++i)
					if (entries[i].actor == actor) return FormatSceneActorLabel(entries[i], i + 1);
				return "尚未绑定角色";
			};
			return "{\"epoch\":" + JNum(static_cast<double>(core_.sceneActorRegistry.Epoch())) +
				",\"actors\":" + actors +
				",\"targetLabel\":" + JString(labelFor(core_.player.controller.targetActor)) +
				",\"boundLabel\":" + JString(labelFor(core_.player.controller.currentBinding.actor)) + "}";
		}

		static std::string CameraTransformJson(const CameraModeTransform& transform)
		{
			return "{\"position\":" + JVec(transform.position) + ",\"rotation\":" + JVec(transform.rotation) + "}";
		}

		static std::string CameraSettingsJson(const CameraSettings& settings)
		{
			return "{\"enabled\":" + JBool(settings.enabled) +
				",\"mode\":" + JNum(static_cast<int>(settings.mode)) +
				",\"followBehavior\":" + JNum(static_cast<int>(settings.followBehavior)) +
				",\"followTarget\":" + JNum(static_cast<int>(settings.followTarget)) +
				",\"followActor\":" + JBool(settings.followActor) +
				",\"limitToScene\":" + JBool(settings.limitToScene) +
				",\"boundsMin\":" + JVec(settings.boundsMin) +
				",\"boundsMax\":" + JVec(settings.boundsMax) +
				",\"followOffset\":" + JVec(settings.followOffset) +
				",\"targetOffset\":" + JVec(settings.targetOffset) +
				",\"vmdOffset\":" + JVec(settings.vmdOffset) +
				",\"vmdRangeScale\":" + JNum(settings.vmdRangeScale) +
				",\"followFactor\":" + JVec(settings.followFactor) +
				",\"followRate\":" + JVec(settings.followRate) +
				",\"followFov\":" + JNum(settings.followFov) +
				",\"srpFinalOverwriteFallback\":" + JBool(settings.srpFinalOverwriteFallback) +
				",\"actorShadowExperimentMode\":" + JNum(settings.actorShadowExperimentMode) +
				",\"transformTrack\":" + CameraTransformJson(settings.transformTrack) +
				",\"transformFollowMove\":" + CameraTransformJson(settings.transformFollowMove) +
				",\"transformLookAt\":" + CameraTransformJson(settings.transformLookAt) +
				",\"usePerModeTransforms\":" + JBool(settings.usePerModeTransforms) + "}";
		}

		std::string CameraJsonLocked() const
		{
			const auto snapshot = core_.camera.ReadSnapshot();
			return "{\"settings\":" + CameraSettingsJson(snapshot.settings) +
				",\"available\":" + JBool(snapshot.available) +
				",\"owned\":" + JBool(snapshot.owned) +
				",\"hasTrack\":" + JBool(snapshot.hasTrack) +
				",\"error\":" + JString(snapshot.error) +
				",\"followTargetError\":" + JString(snapshot.followTargetError) +
				",\"notice\":" + JString(core_.cameraNotice) + "}";
		}

		static std::string StageSettingsJson(const StageSettings& settings)
		{
			std::string color = "[";
			for (int i = 0; i < 4; ++i)
			{
				if (i) color += ",";
				color += JNum(settings.greenScreenColor[i]);
			}
			color += "]";
			return "{\"hideBackground\":" + JBool(settings.hideBackground) +
				",\"greenScreenActive\":" + JBool(settings.greenScreenActive) +
				",\"greenScreenColor\":" + color +
				",\"screenDistance\":" + JNum(settings.screenDistance) +
				",\"screenScale\":" + JNum(settings.screenScale) +
				",\"screenHeightOffset\":" + JNum(settings.screenHeightOffset) +
				",\"overrideCameraClear\":" + JBool(settings.overrideCameraClear) +
				",\"selectedPresetIndex\":" + JNum(settings.selectedPresetIndex) +
				",\"customStageKey\":" + JString(settings.customStageKey) +
				",\"loadReplacementKey\":" + JString(settings.loadReplacementKey) +
				",\"stageFloorYOffset\":" + JNum(settings.stageFloorYOffset) +
				",\"stageYawOffset\":" + JNum(settings.stageYawOffset) +
				",\"stagePosXOffset\":" + JNum(settings.stagePosXOffset) +
				",\"stagePosZOffset\":" + JNum(settings.stagePosZOffset) +
				",\"lightingEffectsEnabled\":" + JBool(settings.lightingEffectsEnabled) +
				",\"customNodeControl\":" + JBool(settings.customNodeControl) + "}";
		}

		std::string StageNodesJsonLocked() const
		{
			std::vector<StageCandidate> candidates;
			{
				std::lock_guard<std::mutex> stageLock(core_.stageManager.stageMutex);
				candidates = core_.stageManager.candidates;
			}
			std::string nodes = "[";
			for (size_t i = 0; i < candidates.size(); ++i)
			{
				const auto& candidate = candidates[i];
				if (i) nodes += ",";
				nodes += "{\"index\":" + JNum(static_cast<double>(i)) +
					",\"instanceId\":" + JNum(candidate.instanceId) +
					",\"name\":" + JString(candidate.name) +
					",\"displayName\":" + JString(candidate.displayName) +
					",\"parentIndex\":" + JNum(candidate.parentIndex) +
					",\"depth\":" + JNum(candidate.depth) +
					",\"hasChildren\":" + JBool(candidate.hasChildren) +
					",\"originalActive\":" + JBool(candidate.originalActive) +
					",\"currentlyActive\":" + JBool(candidate.currentlyActive) +
					",\"userMuted\":" + JBool(candidate.userMuted) +
					",\"isIdentifiedBackground\":" + JBool(candidate.isIdentifiedBackground) +
					",\"isColliderLocked\":" + JBool(candidate.isColliderLocked) +
					",\"isLightingRoot\":" + JBool(candidate.isLightingRoot) +
					",\"isLightingNode\":" + JBool(candidate.isLightingNode) + "}";
			}
			nodes += "]";
			return nodes;
		}

		std::string StageJsonWithNodesLocked(const std::string& nodesJson) const
		{
			StageSettings settings;
			std::string sceneName, statusNotice, mountedKey;
			{
				std::lock_guard<std::mutex> stageLock(core_.stageManager.stageMutex);
				settings = core_.stageManager.settings;
				sceneName = core_.stageManager.currentSceneName;
				statusNotice = core_.stageManager.statusNotice;
				mountedKey = core_.stageManager.mountedStageKey;
			}
			return "{\"settings\":" + StageSettingsJson(settings) +
				",\"sceneName\":" + JString(sceneName) +
				",\"statusNotice\":" + JString(statusNotice) +
				",\"mountedKey\":" + JString(mountedKey) +
				",\"loadNotice\":" + JString(core_.stageLoadNotice) +
				",\"candidates\":" + nodesJson + "}";
		}

		std::string StageJsonLocked() const
		{
			return StageJsonWithNodesLocked(StageNodesJsonLocked());
		}

		std::string FacialJsonLocked() const
		{
			const auto& weights = core_.player.controller.facialWeights;
			return "{\"weights\":{\"overall\":" + JNum(weights.overall) +
				",\"brow\":" + JNum(weights.brow) + ",\"eye\":" + JNum(weights.eye) +
				",\"mouth\":" + JNum(weights.mouth) + "},\"notice\":" + JString(core_.facialWeightsNotice) + "}";
		}

		std::string PositionJsonLocked() const
		{
			return "{\"offset\":" + JVec(core_.player.controller.initialPositionOffset) +
				",\"notice\":" + JString(core_.initialPositionNotice) + "}";
		}

		std::string EnsembleJsonLocked() const
		{
			const auto& snapshot = core_.ensembleSnapshot;
			std::string physics = "[";
			for (size_t i = 0; i < snapshot.physics.size(); ++i)
			{
				if (i) physics += ",";
				physics += JString(snapshot.physics[i]);
			}
			physics += "]";
			return "{\"settings\":" + EnsembleSettingsJson(snapshot.settings) +
				",\"revision\":" + JNum(static_cast<double>(snapshot.revision)) +
				",\"actorEpoch\":" + JNum(static_cast<double>(core_.sceneActorRegistry.Epoch())) +
				",\"available\":" + JBool(snapshot.available) +
				",\"verified\":" + JBool(snapshot.verified) +
				",\"probeEnabled\":" + JBool(snapshot.probeEnabled) +
				",\"active\":" + JBool(snapshot.active) +
				",\"notice\":" + JString(snapshot.notice) +
				",\"physics\":" + physics + "}";
		}

		std::string ImportProgressJsonLocked() const
		{
			const auto& progress = core_.importProgress;
			if (!progress.active && !progress.finished) return "null";
			std::string json = "{\"active\":" + JBool(progress.active) +
				",\"finished\":" + JBool(progress.finished) +
				",\"succeeded\":" + JBool(progress.succeeded) +
				",\"progress\":" + JNum(progress.progress) +
				",\"stage\":" + JString(progress.stage) +
				",\"statusText\":" + JString(progress.statusText) +
				",\"errorDetail\":" + JString(progress.errorDetail);
			if (progress.finished && progress.succeeded && !core_.lastImportedKey.empty())
			{
				json += ",\"importedKey\":" + JString(core_.lastImportedKey);
			}
			json += "}";
			return json;
		}

		std::string GazeJsonLocked() const
		{
			const auto& lookAt = core_.lookAt;
			const std::string settings = "{\"mode\":" + JNum(static_cast<int>(lookAt.gazeMode.load())) +
				",\"desktopLookAtEnabled\":" + JBool(lookAt.desktopLookAtEnabled.load()) +
				",\"eyeWeight\":" + JNum(lookAt.eyeWeight.load()) +
				",\"headWeight\":" + JNum(lookAt.headWeight.load()) +
				",\"bodyWeight\":" + JNum(lookAt.bodyWeight.load()) + "}";
			return "{\"settings\":" + settings +
				",\"runtimeAvailable\":" + JBool(GazeRuntimeAvailable()) +
				",\"vrifyQueryAvailable\":" + JBool(lookAt.fnQueryVRifyGaze != nullptr) +
				",\"vrifyLiveGazeActive\":" + JBool(lookAt.IsVRifyLiveGazeActive()) +
				",\"notice\":" + JString(gazeNotice_) + "}";
		}

		std::string BoneJsonLocked() const
		{
			const auto& overrides = core_.effectiveOverrides;
			std::string entries = "[";
			for (size_t i = 0; i < overrides.offsets.size(); ++i)
			{
				const auto& entry = overrides.offsets[i];
				if (i) entries += ",";
				entries += "{\"humanoidId\":" + JNum(entry.humanoidId) +
					",\"boneName\":" + JString(entry.boneName) +
					",\"enabled\":" + JBool(entry.enabled) +
					",\"spaceWorld\":" + JBool(entry.spaceWorld) +
					",\"eulerDeg\":" + JVec(entry.eulerDeg) +
					",\"isDynamic\":" + JBool(entry.isDynamic) + "}";
			}
			entries += "]";
			return "{\"masterEnabled\":" + JBool(core_.masterOverridesEnabled) +
				",\"overrides\":" + entries +
				",\"notice\":" + JString(boneNotice_) + "}";
		}

		static std::string SoleSettingsJson(const SoleMeasurementSettings& settings)
		{
			std::string points = "[";
			for (size_t side = 0; side < 2; ++side)
			{
				if (side) points += ",";
				points += "[";
				for (size_t i = 0; i < 3; ++i)
				{
					if (i) points += ",";
					points += JVec(settings.points[side][i]);
				}
				points += "]";
			}
			points += "]";
			return "{\"enabled\":" + JBool(settings.enabled) +
				",\"planeCalibrated\":" + JBool(settings.planeCalibrated) +
				",\"shoeCalibrated\":" + JBool(settings.shoeCalibrated) +
				",\"useBootProxy\":" + JBool(settings.useBootProxy) +
				",\"planeNormal\":" + JVec(settings.planeNormal) +
				",\"planeOffsetM\":" + JNum(settings.planeOffsetM) +
				",\"displayBandM\":" + JNum(settings.displayBandM) +
				",\"points\":" + points + "}";
		}

		static std::string SoleFootJson(const SoleFootMeasurement& foot)
		{
			std::string points = "[";
			for (size_t i = 0; i < foot.points.size(); ++i)
			{
				if (i) points += ",";
				const auto& point = foot.points[i];
				points += "{\"world\":" + JVec(point.world) +
					",\"signedDistanceM\":" + JNum(point.signedDistanceM) +
					",\"penetrationM\":" + JNum(point.penetrationM) + "}";
			}
			points += "]";
			std::string patches = "[";
			for (size_t p = 0; p < foot.patches.size(); ++p)
			{
				if (p) patches += ",";
				const auto& patch = foot.patches[p];
				std::string corners = "[";
				for (size_t c = 0; c < patch.corners.size(); ++c)
				{
					if (c) corners += ",";
					const auto& corner = patch.corners[c];
					corners += "{\"world\":" + JVec(corner.world) +
						",\"signedDistanceM\":" + JNum(corner.signedDistanceM) +
						",\"penetrationM\":" + JNum(corner.penetrationM) + "}";
				}
				corners += "]";
				patches += "{\"corners\":" + corners +
					",\"minimumDistanceM\":" + JNum(patch.minimumDistanceM) +
					",\"maximumDistanceM\":" + JNum(patch.maximumDistanceM) +
					",\"maximumPenetrationM\":" + JNum(patch.maximumPenetrationM) + "}";
			}
			patches += "]";
			return "{\"valid\":" + JBool(foot.valid) +
				",\"usingBootProxy\":" + JBool(foot.usingBootProxy) +
				",\"pose\":{\"valid\":" + JBool(foot.pose.valid) +
				",\"hip\":" + JVec(foot.pose.hip) +
				",\"knee\":" + JVec(foot.pose.knee) +
				",\"ankle\":" + JVec(foot.pose.ankle) +
				",\"toe\":" + JVec(foot.pose.toe) +
				",\"footRotation\":" + JQuat(foot.pose.footRotation) + "}" +
				",\"points\":" + points + ",\"patches\":" + patches +
				",\"minimumDistanceM\":" + JNum(foot.minimumDistanceM) +
				",\"maximumPenetrationM\":" + JNum(foot.maximumPenetrationM) + "}";
		}

		std::string SoleJsonLocked() const
		{
			const auto& snapshot = core_.soleSnapshot;
			const auto* actor = core_.sceneActorRegistry.Find(snapshot.actor);
			std::string feet = "[";
			for (size_t i = 0; i < snapshot.feet.size(); ++i)
			{
				if (i) feet += ",";
				feet += SoleFootJson(snapshot.feet[i]);
			}
			feet += "]";
			return "{\"settings\":" + SoleSettingsJson(core_.soleSettings) +
				",\"generation\":" + JNum(static_cast<double>(core_.soleCalibrationGeneration)) +
				",\"notice\":" + JString(core_.soleNotice) +
				",\"snapshot\":{\"available\":" + JBool(snapshot.available) +
				",\"calibrated\":" + JBool(snapshot.calibrated) +
				",\"actorToken\":" + JNum(static_cast<double>(actor ? actor->token : 0)) +
				",\"clipTime\":" + JNum(snapshot.clipTime) +
				",\"feet\":" + feet + "}}";
		}

		static std::string SolePitchSettingsJson(const sole_pitch::Settings& settings)
		{
			return "{\"enabled\":" + JBool(settings.experimentalSolePitchCorrection) +
				",\"maxPitchDegrees\":" + JNum(settings.maxPitchDegrees) +
				",\"useMeshEnvelope\":" + JBool(settings.useMeshEnvelope) + "}";
		}

		std::string SolePitchJsonLocked() const
		{
			const auto& snapshot = core_.experimentalSolePitchSnapshot;
			std::string feet = "[";
			for (size_t i = 0; i < snapshot.feet.size(); ++i)
			{
				const auto& foot = snapshot.feet[i];
				if (i) feet += ",";
				const auto& solution = foot.solution;
				feet += "{\"freshPose\":" + JBool(foot.freshPose) +
					",\"applied\":" + JBool(foot.applied) +
					",\"setterReadbackMatches\":" + JBool(foot.setterReadbackMatches) +
					",\"measuredAfterMinimumDistanceM\":" + JNum(foot.measuredAfterMinimumDistanceM) +
					",\"measuredAfterValid\":" + JBool(foot.measuredAfterValid) +
					",\"status\":" + JString(foot.status ? foot.status : "") +
					",\"solution\":{\"valid\":" + JBool(solution.valid) +
					",\"actionable\":" + JBool(solution.actionable) +
					",\"fullyCleared\":" + JBool(solution.fullyCleared) +
					",\"usedMeshEnvelope\":" + JBool(solution.usedMeshEnvelope) +
					",\"localAxis\":" + JVec(solution.localAxis) +
					",\"pitchDegrees\":" + JNum(solution.pitchDegrees) +
					",\"localDelta\":" + JQuat(solution.localDelta) +
					",\"correctedWorldRotation\":" + JQuat(solution.correctedWorldRotation) +
					",\"beforeMinimumDistanceM\":" + JNum(solution.beforeMinimumDistanceM) +
					",\"afterMinimumDistanceM\":" + JNum(solution.afterMinimumDistanceM) +
					",\"beforePenetrationM\":" + JNum(solution.beforePenetrationM) +
					",\"afterPenetrationM\":" + JNum(solution.afterPenetrationM) + "}" +
					",\"after\":" + SoleFootJson(foot.after) + "}";
			}
			feet += "]";
			return "{\"settings\":" + SolePitchSettingsJson(core_.experimentalSolePitchSettings) +
				",\"clipTime\":" + JNum(snapshot.clipTime) +
				",\"feet\":" + feet +
				",\"notice\":" + JString(solePitchNotice_) + "}";
		}

		static std::string StudioSettingsJson(const studio::Settings& settings)
		{
			const auto& l = settings.lighting;
			const auto& v = settings.video;
			return "{\"lighting\":{\"enabled\":" + JBool(l.enabled) +
				",\"actorStyle\":" + JBool(l.actorStyle) +
				",\"decalEnabled\":" + JBool(l.decalEnabled) +
				",\"keyColor\":" + JColor3(l.keyColor) +
				",\"fillColor\":" + JColor3(l.fillColor) +
				",\"rimColor\":" + JColor3(l.rimColor) +
				",\"decalTint\":" + JColor3(l.decalTint) +
				",\"keyIntensity\":" + JNum(l.keyIntensity) +
				",\"fillIntensity\":" + JNum(l.fillIntensity) +
				",\"rimIntensity\":" + JNum(l.rimIntensity) +
				",\"keyYaw\":" + JNum(l.keyYaw) + ",\"keyPitch\":" + JNum(l.keyPitch) +
				",\"fillYaw\":" + JNum(l.fillYaw) + ",\"fillPitch\":" + JNum(l.fillPitch) +
				",\"rimYaw\":" + JNum(l.rimYaw) + ",\"rimPitch\":" + JNum(l.rimPitch) +
				",\"actorIntensity\":" + JNum(l.actorIntensity) +
				",\"rimPower\":" + JNum(l.rimPower) +
				",\"decalIntensityScale\":" + JNum(l.decalIntensityScale) + "}" +
				",\"video\":{\"enabled\":" + JBool(v.enabled) +
				",\"paused\":" + JBool(v.paused) + ",\"loop\":" + JBool(v.loop) +
				",\"path\":" + JString(v.path) +
				",\"distance\":" + JNum(v.distance) + ",\"width\":" + JNum(v.width) +
				",\"offsetX\":" + JNum(v.offsetX) + ",\"offsetY\":" + JNum(v.offsetY) + "}}";
		}

		std::string StudioJsonLocked() const
		{
			return "{\"settings\":" + StudioSettingsJson(core_.studioSettings) +
				",\"revision\":" + JNum(static_cast<double>(core_.studioRevision)) +
				",\"lightingNotice\":" + JString(core_.studioLightingNotice) +
				",\"videoNotice\":" + JString(core_.studioVideoNotice) + "}";
		}

		static const char* MikuStatusText(MikuFitDecision decision)
		{
			switch (decision)
			{
			case MikuFitDecision::Applied: return "已应用";
			case MikuFitDecision::ExternalApplied: return "已应用（Rinha 发布配置）";
			case MikuFitDecision::ExternalUnverified: return "已回退：该配置未经实测验收";
			case MikuFitDecision::MissingRawSource: return "已回退：旧曲目缺原始五元音";
			case MikuFitDecision::MissingRawEyeSource: return "已回退：曲目缺原始眼眉源（需重新导入）";
			case MikuFitDecision::InvalidRawSource: return "已回退：源数据不完整";
			case MikuFitDecision::UnsupportedSource: return "已回退：源嘴形未覆盖";
			case MikuFitDecision::UnsupportedEyeSource: return "已回退：源眼眉未覆盖";
			case MikuFitDecision::NativeAuthored: return "已回退：保留显式原生通道";
			case MikuFitDecision::UnknownFace: return "已回退：实际脸型未匹配";
			case MikuFitDecision::CharacterMismatch: return "已回退：角色身份不一致";
			case MikuFitDecision::MissingOutput: return "已回退：目标通道不完整";
			case MikuFitDecision::OutsideDomain: return "已回退：源混合超出拟合范围";
			case MikuFitDecision::InvalidModel: return "已回退：拟合数据无效";
			case MikuFitDecision::Disabled: return "已关闭";
			}
			return "未知状态";
		}

		std::string ExperimentsJsonLocked() const
		{
			const auto& controller = core_.player.controller;
			const unsigned snapshot = controller.guiMikuFitSnapshot.load(std::memory_order_relaxed);
			const auto& config = core_.config;
			const bool mouth = config.facialMikuMouthFit >= 0 ? config.facialMikuMouthFit != 0 : controller.facialMikuMouthFit;
			const bool eye = config.facialMikuEyeFit >= 0 ? config.facialMikuEyeFit != 0 : controller.facialMikuEyeFit;
			const bool allow = config.facialMikuFitAllowUnverified >= 0 ? config.facialMikuFitAllowUnverified != 0 : controller.facialMikuFitAllowUnverified;
			const unsigned fittedRole = (snapshot >> 8) & 0xff;
			const auto fittedDecision = static_cast<MikuFitDecision>(snapshot & 0xff);
			const auto eyeDecision = static_cast<MikuFitDecision>((snapshot >> 16) & 0xff);
			const bool playing = core_.player.state != PlaybackState::Stopped;
			const std::string mouthFitStatus = !mouth ? "已关闭" : playing ? MikuStatusText(fittedDecision) : "尚未接管";
			const std::string eyeFitStatus = !eye ? "已关闭" : playing ? MikuStatusText(eyeDecision) : "尚未接管";
			std::string fittedFaceLabel;
			if (fittedRole > 0 && fittedRole <= kMikuMouthFits.size())
				fittedFaceLabel = FormatIdolDisplayName(kMikuMouthFits[fittedRole - 1].character) + "（base-0000）";
			else if (fittedRole == kMikuMouthFits.size() + 1)
				fittedFaceLabel = "发布配置（RinhaReplacement）";
			const std::string settingsJson = "{\"mikuMouthFit\":" + JBool(mouth) +
				",\"mikuEyeFit\":" + JBool(eye) +
				",\"mikuFitAllowUnverified\":" + JBool(allow) +
				",\"waistSupport\":" + JBool(core_.guiExperimentalWaistSupport.load()) +
				",\"beforeQuartz\":" + JBool(core_.guiBeforeQuartzExperiment.load()) +
				",\"muteNativeSoundEffects\":" + JBool(core_.guiMuteNativeSoundEffects.load()) + "}";
			return "{\"settings\":" + settingsJson +
				",\"mouthFitStatus\":" + JString(mouthFitStatus) +
				",\"eyeFitStatus\":" + JString(eyeFitStatus) +
				",\"fittedFaceLabel\":" + JString(fittedFaceLabel) +
				",\"notice\":" + JString(experimentsNotice_) + "}";
		}

		std::string TelemetryJsonLocked() const
		{
			const auto& player = core_.player;
			const auto& controller = player.controller;
			const auto* actor = core_.sceneActorRegistry.Find(controller.currentBinding.actor);
			const auto& entries = core_.sceneActorRegistry.Entries();
			std::string actorLabel = "尚未绑定角色";
			for (size_t i = 0; i < entries.size(); ++i)
			{
				if (entries[i].actor == controller.currentBinding.actor)
				{
					const auto& item = entries[i];
					const std::string name = !item.displayName.empty()
						? item.displayName + " (" + item.character + ")"
						: item.character.empty() ? "未识别角色" : FormatIdolDisplayName(item.character.c_str());
					actorLabel = name + " · " + std::to_string(i + 1);
					break;
				}
			}
			const char* stateName = "已停止";
			switch (player.state)
			{
			case PlaybackState::Playing: stateName = "播放中"; break;
			case PlaybackState::Paused: stateName = "已暂停"; break;
			case PlaybackState::Preview: stateName = "预览中"; break;
			default: break;
			}
			const auto& gui = core_.gui;
			return "{\"statusLine\":" + JString(core_.StatusSummary()) +
				",\"player\":{\"state\":" + JNum(static_cast<int>(player.state)) +
				",\"stateName\":" + JString(stateName) +
				",\"time\":" + JNum(player.currentTime) +
				",\"duration\":" + JNum(player.TimelineDuration()) +
				",\"totalFrames\":" + JNum(player.clip.total_frames) +
				",\"fps\":" + JNum(player.clip.fps) +
				",\"boneCount\":" + JNum(player.clip.bone_count) +
				",\"loadedMotionFile\":" + JString(player.loadedMotionFile) +
				",\"seekCount\":" + JNum(player.SeekCount()) +
				",\"audioSyncOffsetSeconds\":" + JNum(player.AudioSyncOffsetSeconds()) +
				",\"lastClockSource\":" + JString(player.lastClockSource ? player.lastClockSource : "") + "}" +
				",\"actor\":{\"token\":" + JNum(static_cast<double>(actor ? actor->token : 0)) +
				",\"actorLabel\":" + JString(actorLabel) +
				",\"boundBoneCount\":" + JNum(static_cast<double>(controller.currentBinding.boundBoneCount)) +
				",\"totalBoneCount\":" + JNum(static_cast<double>(controller.currentBinding.boneTransforms.size())) + "}" +
				",\"scrub\":{\"begin\":" + JNum(gui.probeScrubBegin) +
				",\"update\":" + JNum(gui.probeScrubUpdate) +
				",\"end\":" + JNum(gui.probeScrubEnd) +
				",\"lastValue\":" + JNum(gui.probeLastScrubValue) +
				",\"active\":" + JBool(gui.scrub.active) + "}" +
				",\"overrides\":{\"masterEnabled\":" + JBool(core_.masterOverridesEnabled) +
				",\"effectiveCount\":" + JNum(static_cast<double>(core_.effectiveOverrides.offsets.size())) + "}" +
				",\"gazeMode\":" + JNum(static_cast<int>(core_.lookAt.gazeMode.load())) +
				",\"physicsBackend\":" + JNum(core_.guiPhysicsBackend.load()) + "}";
		}

		std::string BuildAppearanceJson() const
		{
			AppearanceInfo info;
			if (hooks_.appearance) info = hooks_.appearance();
			return "{\"theme\":" + JNum(info.theme) +
				",\"uiScale\":" + JNum(info.uiScale) +
				",\"dpiScale\":1,\"scaleLimited\":false}";
		}

		std::string ClockJsonLocked(std::string* outMeta = nullptr) const
		{
			GakuMotionClockSnapshotV1 snap{};
			core_.ReadClockSnapshot(&snap, false);
			const char* rm = snap.runMode == GAKUMOTION_RUN_RECORDING ? "recording" : "realtime";
			auto srcName = [](uint32_t s) -> const char*
			{
				switch (s)
				{
				case GAKUMOTION_CLOCK_ENGINE_DELTA: return "engine_delta";
				case GAKUMOTION_CLOCK_FIXED_STEP: return "fixed_step";
				case GAKUMOTION_CLOCK_EXTERNAL: return "external";
				default: return "audio";
				}
			};
			std::string meta = "{\"runMode\":" + JString(rm) +
				",\"configuredSource\":" + JString(srcName(snap.configuredSource)) +
				",\"effectiveSource\":" + JString(srcName(snap.effectiveSource)) +
				",\"fpsNumerator\":" + JNum(snap.fpsNumerator) +
				",\"fpsDenominator\":" + JNum(snap.fpsDenominator) +
				",\"waitingForRenderComplete\":" + JBool(snap.waitingForRenderComplete != 0) +
				",\"leaseId\":" + JNum(static_cast<double>(snap.leaseId)) +
				",\"leaseOwner\":" + JString(snap.leaseOwner);
			if (outMeta) *outMeta = meta;
			return meta + ",\"evaluatedFrameSerial\":" + JNum(static_cast<double>(snap.evaluatedFrameSerial)) + "}";
		}

		std::string BuildAudioEffectJson() const
		{
			AudioEffectState st;
			if (hooks_.getAudioEffectsState) st = hooks_.getAudioEffectsState();
			if (st.activeId.empty()) st.activeId = "flat";
			if (st.presets.empty())
				st.presets.push_back({"flat", "原声", "flat", "直通输出"});
			std::string list = "[";
			for (size_t i = 0; i < st.presets.size(); ++i)
			{
				if (i) list += ",";
				const auto& p = st.presets[i];
				list += "{\"id\":" + JString(p.id) +
					",\"name\":" + JString(p.name) +
					",\"category\":" + JString(p.category) +
					",\"description\":" + JString(p.description) + "}";
			}
			list += "]";
			return "{\"available\":" + JBool(st.available) +
				",\"activeId\":" + JString(st.activeId) +
				",\"presets\":" + list + "}";
		}

		std::string BuildSnapshotLocked() const
		{
			const std::string stageNodes = StageNodesJsonLocked();
			return "{\"permission\":" + PermissionJsonLocked() +
				",\"player\":" + PlayerStateJsonLocked() +
				",\"library\":" + LibraryJsonLocked() +
				",\"loaded\":" + LoadedJsonLocked() +
				",\"playlist\":" + PlaylistJsonLocked() +
				",\"actors\":" + ActorsJsonLocked() +
				",\"camera\":" + CameraJsonLocked() +
				",\"stage\":" + StageJsonWithNodesLocked(stageNodes) +
				",\"stageNodes\":" + stageNodes +
				",\"facial\":" + FacialJsonLocked() +
				",\"position\":" + PositionJsonLocked() +
				",\"ensemble\":" + EnsembleJsonLocked() +
				",\"physicsBackend\":" + JNum(core_.guiPhysicsBackend.load()) +
				",\"musicVolumeSupport\":" + JNum(core_.player.fnAudioVolumeSupport ? core_.player.fnAudioVolumeSupport() : 0) +
				",\"musicVolumeNotice\":" + JString(core_.musicVolumeNotice) +
				",\"importProgress\":" + ImportProgressJsonLocked() +
				",\"motionsRoot\":" + JString(core_.motionsRoot) +
				",\"gaze\":" + GazeJsonLocked() +
				",\"bone\":" + BoneJsonLocked() +
				",\"sole\":" + SoleJsonLocked() +
				",\"solePitch\":" + SolePitchJsonLocked() +
				",\"studio\":" + StudioJsonLocked() +
				",\"experiments\":" + ExperimentsJsonLocked() +
				",\"clock\":" + ClockJsonLocked() +
				",\"audioEffect\":" + BuildAudioEffectJson() + "}";
		}

		// ---- UI tick: async polling + change events -------------------------------

		static uint64_t NextSessionId()
		{
			static std::atomic<uint64_t> sessionCounter{0};
			return ++sessionCounter;
		}

		void ResetChangeTracking()
		{
			baselinePending_ = true;
			forcePlayerEvent_ = false;
			forceDomainRefresh_.store(true, std::memory_order_release);
			forceAudioEffectRefresh_.store(true, std::memory_order_release);
			lastPermissionAllowed_ = false;
			lastInjectionSession_ = 0;
			lastPlayerEmit_ = {};
			lastClockEmit_ = {};
			lastSlowDomainPoll_ = {};
			lastAudioEffectPoll_ = {};
			lastPhysicsBackend_ = -1;
			lastPermission_.clear();
			lastPlayer_.clear();
			lastLibrary_.clear();
			lastLoaded_.clear();
			lastPlaylist_.clear();
			lastActors_.clear();
			lastCamera_.clear();
			lastStage_.clear();
			lastStageNodes_.clear();
			lastFacial_.clear();
			lastPosition_.clear();
			lastEnsemble_.clear();
			lastImportProgress_.clear();
			lastAppearance_.clear();
			lastGaze_.clear();
			lastBone_.clear();
			lastSole_.clear();
			lastSolePitch_.clear();
			lastStudio_.clear();
			lastExperiments_.clear();
			lastClock_.clear();
			lastClockMeta_.clear();
			lastAudioEffect_.clear();
		}

		void Tick()
		{
			PollBridgeDialog();
			PollImGuiDialog();
			const bool wantDropTarget = visible_ && dropEnabled_;
			if (hooks_.dropSetTarget && (wantDropTarget || lastDropTargetSent_))
			{
				std::string dropNotice;
				hooks_.dropSetTarget(wantDropTarget, dropNotice);
				lastDropTargetSent_ = wantDropTarget;
			}
			if (wantDropTarget)
			{
				if (hooks_.dropPeek) cachedDropPeek_ = NormalizeDropPeek(hooks_.dropPeek());
				if (hooks_.dropConsume && (!hooks_.dropPeek || cachedDropPeek_.pendingCount > 0))
				{
					const DropBatch batch = hooks_.dropConsume();
					if (batch.ok && !batch.paths.empty())
					{
						for (const auto& path : batch.paths)
						{
							if (pendingDrops_.size() >= 512) break;
							pendingDrops_.push_back(path);
						}
					}
				}
			}
			else if (!dropEnabled_)
			{
				cachedDropPeek_ = {};
			}
			if (!visible_ && !baselinePending_) return;

			const auto now = std::chrono::steady_clock::now();
			std::string permission, player, library, loaded, playlist, actors, camera, stage, stageNodes,
				facial, position, ensemble, importProgress, appearance,
				gaze, bone, sole, solePitch, studio, experiments, clock, clockMeta, audioEffect;
			int physicsBackend = 0;
			bool injectionAllowed = false;
			bool scrubbing = false;
			float scrubTime = 0.0f;
			uint64_t injectionSession = 0;
			bool playing = false;
			bool refreshSlowDomains = false;
			bool refreshAudioEffect = false;
			{
				std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
				injectionAllowed = core_.InjectionAllowed();
				injectionSession = core_.injectionSession;
				scrubbing = core_.player.scrubbing;
				scrubTime = core_.player.currentTime;
				playing = core_.player.state == PlaybackState::Playing;
				permission = PermissionJsonLocked();
				player = PlayerStateJsonLocked();
				clock = ClockJsonLocked(&clockMeta);
				physicsBackend = core_.guiPhysicsBackend.load();

				const bool forceSlowRefresh = forceDomainRefresh_.exchange(false, std::memory_order_acq_rel);
				const bool permissionOrSessionChanged =
					permission != lastPermission_ ||
					(lastPermissionAllowed_ && !injectionAllowed) ||
					(injectionSession != lastInjectionSession_ && lastInjectionSession_ != 0);
				refreshSlowDomains = baselinePending_ || forceSlowRefresh ||
					!playing || permissionOrSessionChanged ||
					(now - lastSlowDomainPoll_ >= std::chrono::milliseconds(100));
				if (refreshSlowDomains)
				{
					lastSlowDomainPoll_ = now;
					library = LibraryJsonLocked();
					loaded = LoadedJsonLocked();
					playlist = PlaylistJsonLocked();
					actors = ActorsJsonLocked();
					camera = CameraJsonLocked();
					stageNodes = StageNodesJsonLocked();
					stage = StageJsonWithNodesLocked(stageNodes);
					facial = FacialJsonLocked();
					position = PositionJsonLocked();
					ensemble = EnsembleJsonLocked();
					importProgress = ImportProgressJsonLocked();
					gaze = GazeJsonLocked();
					bone = BoneJsonLocked();
					sole = SoleJsonLocked();
					solePitch = SolePitchJsonLocked();
					studio = StudioJsonLocked();
					experiments = ExperimentsJsonLocked();
				}
			}
			if (refreshSlowDomains)
			{
				appearance = BuildAppearanceJson();
				const bool forceAudioRefresh = forceAudioEffectRefresh_.exchange(false, std::memory_order_acq_rel);
				refreshAudioEffect = baselinePending_ || forceAudioRefresh ||
					!playing || lastAudioEffect_.empty() ||
					(now - lastAudioEffectPoll_ >= std::chrono::milliseconds(250));
				if (refreshAudioEffect)
				{
					lastAudioEffectPoll_ = now;
					audioEffect = BuildAudioEffectJson();
				}
			}

			// Permission loss / session change: the frontend drops its scrub without
			// sending scrubEnd, so finish the native scrub state through the queue.
			if ((lastPermissionAllowed_ && !injectionAllowed) ||
				(injectionSession != lastInjectionSession_ && lastInjectionSession_ != 0))
			{
				if (scrubbing)
				{
					std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
					core_.QueueScrubEnd(scrubTime);
				}
			}
			lastPermissionAllowed_ = injectionAllowed;
			lastInjectionSession_ = injectionSession;

			const bool baseline = baselinePending_;
			baselinePending_ = false;
			const auto emitChanged = [&](const char* name, const std::string& json, std::string& last) {
				if (baseline || json != last)
				{
					last = json;
					Emit(name, json);
				}
			};
			emitChanged("permission.changed", permission, lastPermission_);
			if (refreshSlowDomains)
			{
				emitChanged("library.changed", library, lastLibrary_);
				emitChanged("loaded.changed", loaded, lastLoaded_);
				emitChanged("playlist.changed", playlist, lastPlaylist_);
				emitChanged("actors.changed", actors, lastActors_);
				emitChanged("camera.changed", camera, lastCamera_);
				emitChanged("stage.changed", stage, lastStage_);
				if (baseline || stageNodes != lastStageNodes_)
				{
					lastStageNodes_ = stageNodes;
					Emit("stage.nodesChanged", "{\"nodes\":" + stageNodes + "}");
				}
				emitChanged("facial.changed", facial, lastFacial_);
				emitChanged("position.changed", position, lastPosition_);
				emitChanged("ensemble.changed", ensemble, lastEnsemble_);
				if (importProgress != "null")
					emitChanged("import.progress", importProgress, lastImportProgress_);
				emitChanged("appearance.changed", appearance, lastAppearance_);
				emitChanged("gaze.changed", gaze, lastGaze_);
				emitChanged("bone.changed", bone, lastBone_);
				emitChanged("sole.changed", sole, lastSole_);
				emitChanged("solePitch.changed", solePitch, lastSolePitch_);
				emitChanged("studio.changed", studio, lastStudio_);
				emitChanged("experiments.changed", experiments, lastExperiments_);
				if (refreshAudioEffect)
					emitChanged("audioEffect.changed", audioEffect, lastAudioEffect_);
			}
			// clock.changed: immediate on baseline or structural clock config change,
			// throttled to ~10 Hz while playing for high-frequency evaluatedFrameSerial ticks.
			const bool clockThrottled = playing && !baseline && clockMeta == lastClockMeta_ &&
				now - lastClockEmit_ < std::chrono::milliseconds(100);
			if (!clockThrottled && (baseline || clock != lastClock_))
			{
				lastClock_ = clock;
				lastClockMeta_ = clockMeta;
				lastClockEmit_ = now;
				Emit("clock.changed", clock);
			}
			if (baseline || physicsBackend != lastPhysicsBackend_)
			{
				lastPhysicsBackend_ = physicsBackend;
				Emit("settings.changed", "{\"physicsBackend\":" + JNum(physicsBackend) + "}");
			}

			// player.stateChanged: immediate after commands, ~10 Hz while playing,
			// otherwise only on change.
			const bool throttled = playing && !forcePlayerEvent_ && !baseline &&
				now - lastPlayerEmit_ < std::chrono::milliseconds(100);
			if (!throttled && (baseline || forcePlayerEvent_ || player != lastPlayer_))
			{
				lastPlayer_ = player;
				lastPlayerEmit_ = now;
				forcePlayerEvent_ = false;
				Emit("player.stateChanged", player);
			}
		}

		// Keep an ImGui-started dialog progressing while the WebView panel is
		// selected (the ImGui draw loop that normally polls it is not running).
		void PollImGuiDialog()
		{
			std::lock_guard<std::recursive_mutex> lock(core_.stateMutex);
			auto& gui = core_.gui;
			if (gui.browsePendingKind < 0 || !core_.fnPollPickFile) return;
			std::string path;
			const int state = core_.fnPollPickFile(path, gui.browseNotice);
			if (state == GAKU_FILE_DIALOG_PENDING) return;
			if (state == GAKU_FILE_DIALOG_ACCEPTED)
			{
				char* target = gui.browsePendingKind == 0 ? gui.importVmd
					: gui.browsePendingKind == 1 ? gui.importAudio
					: gui.browsePendingKind == 3 ? gui.studioVideoPath : gui.importCameraSource;
				const size_t capacity = gui.browsePendingKind == 0 ? sizeof(gui.importVmd)
					: gui.browsePendingKind == 1 ? sizeof(gui.importAudio)
					: gui.browsePendingKind == 3 ? sizeof(gui.studioVideoPath) : sizeof(gui.importCameraSource);
				if (path.size() >= capacity) gui.browseNotice = "所选路径过长，未修改输入路径";
				else std::memcpy(target, path.c_str(), path.size() + 1);
			}
			gui.browsePendingKind = -1;
		}

		// ---- state ---------------------------------------------------------------

		Core& core_;
		HostHooks hooks_;
		uint64_t sessionId_{0};
		uint64_t seq_{0};
		std::atomic<bool> closed_{false};
		bool visible_{false};
		bool baselinePending_{true};
		mutable std::mutex sinkMutex_;
		GakuWebPanelEventSink sink_{nullptr};
		void* sinkUserData_{nullptr};
		std::wstring assetsDirCache_;

		// Dialog state (bridge-owned ticket; host service owns the OS dialog).
		bool dialogPending_{false};
		int dialogKind_{-1};
		uint64_t dialogId_{0};
		std::string dialogState_{"pending"};
		std::vector<std::string> dialogPaths_;
		std::string dialogNotice_;
		bool dropEnabled_{false};
		bool lastDropTargetSent_{false};
		DropPeek cachedDropPeek_{};
		std::vector<std::string> pendingDrops_;
		std::string boneNotice_, gazeNotice_, solePitchNotice_, experimentsNotice_;

		// Event change detection.
		bool forcePlayerEvent_{false};
		std::atomic<bool> forceDomainRefresh_{true};
		std::atomic<bool> forceAudioEffectRefresh_{true};
		bool lastPermissionAllowed_{false};
		uint64_t lastInjectionSession_{0};
		std::chrono::steady_clock::time_point lastPlayerEmit_{};
		std::chrono::steady_clock::time_point lastClockEmit_{};
		std::chrono::steady_clock::time_point lastSlowDomainPoll_{};
		std::chrono::steady_clock::time_point lastAudioEffectPoll_{};
		std::string lastPermission_, lastPlayer_, lastLibrary_, lastLoaded_, lastPlaylist_,
			lastActors_, lastCamera_, lastStage_, lastStageNodes_, lastFacial_, lastPosition_,
			lastEnsemble_, lastImportProgress_, lastAppearance_, lastGaze_, lastBone_, lastSole_,
			lastSolePitch_, lastStudio_, lastExperiments_, lastClock_, lastClockMeta_, lastAudioEffect_;
		int lastPhysicsBackend_{-1};
	};
} // namespace GakuMotion::webpanel
