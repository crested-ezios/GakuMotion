#pragma once
#include "GakuBurstBridge.h"
#include "GkIdolController.hpp"
#include <windows.h>
#include <string>

namespace GakuMotion
{
	// The companion owns every engine-private detail. This client only negotiates
	// a versioned C ABI and copies the body's actual application-level outputs.
	class BurstBridgeClient
	{
		HMODULE module_{};
		bool supportsSceneForceFilter_{};
		GbbApi api_{};
		std::wstring path_;
		bool attempted_{};
		bool faulted_{};
		bool stopped_{};
		bool captureInputOutstanding_{};
		uint64_t lastSequence_{};
		std::chrono::steady_clock::time_point lastMatch_{};
		GbbConfig config_{};

		static HMODULE LoadGuarded(const wchar_t* path)
		{
			__try
			{
				return LoadLibraryExW(path, nullptr,
				                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		}

		static int GetApiGuarded(GbbGetApiFn fn, GbbApi* out)
		{
			__try { return fn(GBB_API_VERSION, out); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
		}

		static int ConfigureGuarded(const GbbApi& api, const GbbConfig* config)
		{
			__try { return api.configure(config); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
		}

		static int PublishGuarded(const GbbApi& api, const GbbFrame* frame)
		{
			__try { return api.publish(frame); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
		}

		static int StatusGuarded(const GbbApi& api, GbbStatus* out)
		{
			__try { return api.get_status(out); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
		}

		static int PerfGuarded(const GbbApi& api, GbbPerf* out)
		{
			__try { return api.get_perf(out); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
		}

		static int DumpGuarded(const GbbApi& api, const wchar_t* path)
		{
			__try { return api.dump_traces(path); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
		}

		static int ClearGuarded(const GbbApi& api, uint32_t flags)
		{
			__try { return api.clear(flags); }
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return GBB_E_FAULT;
			}
		}

		static void ShutdownGuarded(const GbbApi& api)
		{
			__try { api.shutdown(); }
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
		}

	public:
		std::string reason{"not_requested"};
		GbbStatus status{};
		GbbPerf perf{};
		bool HasCaptureInput() const noexcept { return captureInputOutstanding_; }

		// Uses only the already negotiated ABI. The companion's Clear cancels
		// current_ and diagnostic_ under its mutex and advances generation.
		static bool ClearApiForCapture(const GbbApi& api, GbbStatus& after)
		{
			if (!api.clear || !api.get_status) return false;
			GbbStatus before{};
			before.size = sizeof(before); before.version = GBB_API_VERSION;
			if (StatusGuarded(api, &before) != GBB_OK || before.size != sizeof(before) ||
				before.version != GBB_API_VERSION) return false;
			// Do not reset a fault merely to label the capture state clean.
			if (ClearGuarded(api, 0) != GBB_OK) return false;
			after = {}; after.size = sizeof(after); after.version = GBB_API_VERSION;
			if (StatusGuarded(api, &after) != GBB_OK || after.size != sizeof(after) ||
				after.version != GBB_API_VERSION || after.state > GBB_STATE_SHUTDOWN) return false;
			return after.generation > before.generation && !after.actor && !after.injected &&
				!after.writes && !after.sequence && !after.injection_fault &&
				after.state != GBB_STATE_INJECTING && after.state != GBB_STATE_FAULTED &&
				after.state != GBB_STATE_SHUTDOWN;
		}

		bool ClearForCapture()
		{
			if (!captureInputOutstanding_) return true;
			if (!api_.publish) return !captureInputOutstanding_;
			GbbStatus observed{};
			if (!ClearApiForCapture(api_, observed))
			{
				reason = "capture_clear_unconfirmed";
				return false;
			}
			status = observed;
			perf = {};
			captureInputOutstanding_ = false;
			lastSequence_ = 0; lastMatch_ = {};
			reason = "capture_cleared";
			return true;
		}

		bool Fail(const char* detail)
		{
			if (!faulted_ && api_.clear) ClearGuarded(api_, 0);
			faulted_ = true;
			reason = detail;
			status.state = GBB_STATE_FAULTED;
			status.injection_fault = 1;
			return false;
		}

		void Configure(const wchar_t* pluginsRoot, GbbLogFn log, void* context)
		{
			if (pluginsRoot && *pluginsRoot) path_ = std::wstring(pluginsRoot) + L"/GakuBurstBridge.dll";
			config_ = {};
			config_.size = sizeof(config_);
			config_.version = GBB_API_VERSION;
			config_.log = log;
			config_.log_context = context;
		}

		bool Ensure()
		{
			if (stopped_)
			{
				reason = "client_shutdown";
				return false;
			}
			if (api_.publish) return true;
			if (attempted_) return false;
			attempted_ = true;
			if (path_.empty())
			{
				reason = "companion_path_missing";
				return false;
			}
			const DWORD attributes = GetFileAttributesW(path_.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				reason = "companion_missing";
				return false;
			}
			module_ = LoadGuarded(path_.c_str());
			if (!module_)
			{
				reason = "companion_load_failed";
				return false;
			}
			auto getApi = reinterpret_cast<GbbGetApiFn>(GetProcAddress(module_, "GakuBurstBridge_GetApi"));
			GbbApi next{};
			next.size = sizeof(next);
			next.version = GBB_API_VERSION;
			if (!getApi || GetApiGuarded(getApi, &next) != GBB_OK || next.version != GBB_API_VERSION ||
				next.size != sizeof(next) || !next.configure || !next.publish || !next.clear ||
				!next.get_status || !next.get_perf || !next.shutdown || !next.dump_traces)
			{
				reason = "companion_abi_rejected";
				return false;
			}
			// Host shutdown may originate on its quit-guard worker. Keep this
			// client's code and any in-flight companion log callback valid until
			// process exit, even if the host releases the main plugin's handle.
			HMODULE pinned{};
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			                        reinterpret_cast<LPCWSTR>(&LoadGuarded), &pinned))
			{
				reason = "client_pin_failed";
				return false;
			}
			if (ConfigureGuarded(next, &config_) != GBB_OK)
			{
				reason = "companion_configure_failed";
				return false;
			}
			api_ = next;
			supportsSceneForceFilter_ = GetProcAddress(module_, "GakuBurstBridge_SupportsSceneForceFilter") != nullptr;
			reason = "discovering";
			return true;
		}

		bool Publish(const IdolBoneBinding& b, const IdolController& ctrl, float time, int frame,
		             bool beforeQuartz = false, bool sleeveDiagnostics = false, bool filterSceneForces = false)
		{
			if (!Ensure()) return false;
			if (faulted_) return false;
			if (!b.cachedAnimator || !ctrl.fnGetLocalRotation || !ctrl.fnGetLocalPosition)
			{
				// Actor/Animator resolution can trail the first Play command by a
				// frame. Pending binding is not a native fault; cancel any old
				// packet and retry when the normal body binding becomes ready.
				if (api_.clear) ClearGuarded(api_, 0);
				reason = "body_binding_pending";
				status = {};
				perf = {};
				return false;
			}
			GbbFrame packet{};
			packet.size = sizeof(packet);
			packet.version = GBB_API_VERSION;
			packet.flags = GBB_FRAME_INJECT;
			if (beforeQuartz) packet.flags |= GBB_FRAME_BEFORE_QUARTZ;
			if (sleeveDiagnostics) packet.flags |= GBB_FRAME_SLEEVE_DIAGNOSTICS;
			if (filterSceneForces && supportsSceneForceFilter_) packet.flags |= GBB_FRAME_NO_SCENE_FORCES;
			packet.actor = b.actor;
			packet.animator = b.cachedAnimator;
			packet.clip_time = time;
			packet.unity_frame = frame;
			for (size_t i = 0; i < b.boneTransforms.size() && packet.bone_count < GBB_MAX_BONES; ++i)
			{
				if (!b.boneTransforms[i] || i >= b.boneHids.size()) continue;
				auto& pose = packet.bones[packet.bone_count++];
				pose.transform = b.boneTransforms[i];
				pose.humanoid_id = b.boneHids[i];
				ctrl.fnGetLocalRotation(pose.transform, &pose.local_rotation.x);
				ctrl.fnGetLocalPosition(pose.transform, &pose.local_position.x);
			}
			captureInputOutstanding_ = true;
			const int result = PublishGuarded(api_, &packet);
			status = {};
			status.size = sizeof(status);
			status.version = GBB_API_VERSION;
			perf = {};
			perf.size = sizeof(perf);
			perf.version = GBB_API_VERSION;
			if (StatusGuarded(api_, &status) != GBB_OK || PerfGuarded(api_, &perf) != GBB_OK ||
				status.size != sizeof(status) || status.version != GBB_API_VERSION || status.state > GBB_STATE_SHUTDOWN
				||
				status.writes > GBB_MAX_BONES || perf.size != sizeof(perf) || perf.version != GBB_API_VERSION)
				return Fail("companion_observer_fault");
			if (result != GBB_OK && result != GBB_PENDING) return Fail("companion_publish_rejected");
			if (status.injection_fault || status.state == GBB_STATE_FAULTED) return Fail("companion_faulted");
			if (status.state == GBB_STATE_INJECTING && status.injected && status.writes && status.actor == b.actor)
			{
				const auto now = std::chrono::steady_clock::now();
				if (status.sequence != lastSequence_)
				{
					lastSequence_ = status.sequence;
					lastMatch_ = now;
				}
				if (!status.sequence || status.source_frame < 0 || status.source_frame > frame) return Fail(
					"companion_source_invalid");
				if (static_cast<int64_t>(frame) - status.source_frame > 4 || now - lastMatch_ >
					std::chrono::milliseconds(100))
				{
					// A main-thread hitch may expire the preceding packet before
					// a job can consume the fresh one just published above.
					// Leave it queued so the next job can confirm consumption.
					reason = "native_pending_stale";
					return false;
				}
				reason = "native";
				return true;
			}
			reason = "native_pending";
			return false;
		}

		int DumpTraces(const wchar_t* path) const
		{
			return api_.dump_traces ? DumpGuarded(api_, path) : GBB_E_UNAVAILABLE;
		}

		void Clear()
		{
			if (api_.clear && ClearGuarded(api_, GBB_CLEAR_RESET_FAULT) == GBB_OK)
				captureInputOutstanding_ = false;
			status = {};
			perf = {};
			faulted_ = false;
			lastSequence_ = 0;
			lastMatch_ = {};
		}

		void RetryUnavailable() { if (!stopped_ && !api_.publish) attempted_ = false; }

		void Shutdown(bool invokeModuleShutdown = true)
		{
			stopped_ = true;
			if (invokeModuleShutdown && api_.shutdown) ShutdownGuarded(api_);
			api_ = {};
			// Keep the module reference until process exit: a worker may still be
			// returning through the companion's pinned native trampoline.
		}
	};
}
