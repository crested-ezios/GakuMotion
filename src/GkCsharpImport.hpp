#pragma once

// =============================================================================
// GkCsharpImport.hpp — 测试用 VMD 导入后端（Candidate B，NativeAOT）
// =============================================================================
// 运行时仅使用 C# NativeAOT；Python 工具仅供离线研究。
// Body ABI v4；独立镜头 ABI v1。未支持的目标几何请求明确失败，
// 不启动解释器，也不覆盖已有动作。单一工作线程使用 @PROGRESS 协议。
// =============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "GkJsonLite.hpp"
#include "GkPlaylist.hpp"

namespace GakuMotion::csharp_import
{
	using RunFn = int32_t (*)(const char* requestUtf8Json, char* responseUtf8Json, int32_t responseCapacity);
	using AbiFn = int32_t (*)();
	using MouthSourceFn = int32_t (*)(const char*, float, float, int32_t, float*, int32_t);

	namespace detail
	{
		inline std::mutex& Mutex()
		{
			static std::mutex m;
			return m;
		}

		inline std::wstring& ModuleDir()
		{
			static std::wstring dir;
			return dir;
		}

		inline HMODULE& Module()
		{
			static HMODULE module{nullptr};
			return module;
		}

		struct RejectedModule
		{
			std::wstring path;
			HMODULE handle;
			std::string reason;
		};
		inline std::vector<RejectedModule>& RejectedModules()
		{
			static std::vector<RejectedModule> modules;
			return modules;
		}

		inline RunFn& Run()
		{
			static RunFn fn{nullptr};
			return fn;
		}

		inline RunFn& CameraRun()
		{
			static RunFn fn{nullptr};
			return fn;
		}

		inline MouthSourceFn& MouthSourceRead()
		{
			static MouthSourceFn function{nullptr};
			return function;
		}

		inline std::string& Detail()
		{
			static std::string detail;
			return detail;
		}

		inline std::string& PluginsRoot()
		{
			static std::string root;
			return root;
		}

		inline std::string& ModuleDirUtf8()
		{
			static std::string dir;
			return dir;
		}

		inline void Marker() {}

		// Result of the in-flight import.
		struct Job
		{
			bool active{false};
			std::atomic<bool> done{false};
			int32_t code{2};
			std::string response;
			std::thread worker;
			std::string logPath;
		};

		inline Job& Current()
		{
			static Job job;
			return job;
		}

		inline Job& CameraCurrent()
		{
			static Job job;
			return job;
		}

		inline std::string Utf8(const std::wstring& wide)
		{
			if (wide.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
			                                     nullptr, 0, nullptr, nullptr);
			if (size <= 0) return {};
			std::string out(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size,
			                    nullptr, nullptr);
			return out;
		}

		inline std::wstring Wide(const std::string& utf8)
		{
			if (utf8.empty()) return {};
			const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
			if (size <= 0) return {};
			std::wstring out(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), size);
			return out;
		}

		inline void AppendJsonString(std::string& out, const std::string& value)
		{
			out.push_back('"');
			for (char raw : value)
			{
				const unsigned char c = static_cast<unsigned char>(raw);
				switch (c)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (c < 0x20)
					{
						char buffer[8]{};
						std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
						out += buffer;
					}
					else
					{
						out.push_back(raw);
					}
					break;
				}
			}
			out.push_back('"');
		}

		inline std::wstring CanonicalLibraryPath(const std::wstring& path)
		{
			const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
			if (!required) return path;
			std::wstring full(required, L'\0');
			const DWORD written = GetFullPathNameW(path.c_str(), required, full.data(), nullptr);
			if (!written || written >= required) return path;
			full.resize(written);
			std::replace(full.begin(), full.end(), L'/', L'\\');
			return full;
		}

		inline bool FileExists(const std::string& path)
		{
			const DWORD attributes = GetFileAttributesW(Wide(path).c_str());
			return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
		}

	}

	/// <summary>Remembers where the companion DLL lives; does not load it yet.</summary>
	inline void SetPluginsRoot(const std::string& pluginsRootUtf8)
	{
		std::lock_guard<std::mutex> lock(detail::Mutex());
		detail::PluginsRoot() = pluginsRootUtf8;
	}

	namespace detail
	{
		/// <summary>Loads the companion DLL once. Caller must hold the mutex.</summary>
		inline bool LoadLocked(const std::string& pluginsRootUtf8)
		{
			if (Module()) return true;
			Detail().clear();

			wchar_t modulePath[MAX_PATH * 4]{};
			HMODULE self{nullptr};
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                       reinterpret_cast<LPCWSTR>(&Marker), &self))
			{
				const DWORD length = GetModuleFileNameW(self, modulePath, static_cast<DWORD>(std::size(modulePath)));
				if (length > 0 && length < std::size(modulePath))
				{
					std::wstring path(modulePath, length);
					const size_t slash = path.find_last_of(L"/\\");
					ModuleDir() = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
				}
			}

			std::vector<std::wstring> candidates;
			if (!ModuleDir().empty())
				candidates.push_back(ModuleDir() + L"\\GakuMotion.VmdImporter.Native.dll");
			if (!pluginsRootUtf8.empty())
				candidates.push_back(Wide(pluginsRootUtf8) + L"\\GakuMotion.VmdImporter.Native.dll");

			for (const auto& unnormalized : candidates)
			{
				const auto candidate = CanonicalLibraryPath(unnormalized);
				// NativeAOT libraries cannot be unloaded after initialization. A
				// rejected image therefore remains pinned too, and is never probed
				// again at this path. Replacing it requires a process restart.
				const auto rejected = std::find_if(RejectedModules().begin(), RejectedModules().end(),
					[&](const RejectedModule& entry) { return _wcsicmp(entry.path.c_str(), candidate.c_str()) == 0; });
				if (rejected != RejectedModules().end())
				{
					Detail() = rejected->reason;
					continue;
				}
				HMODULE module = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
				if (!module)
				{
					const DWORD error = GetLastError();
					if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
						Detail() = "GakuMotion.VmdImporter.Native.dll 无法加载（Win32 错误 " + std::to_string(error) + "）";
					continue;
				}
				HMODULE pinned{nullptr};
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
					reinterpret_cast<LPCWSTR>(module), &pinned);
				auto run = reinterpret_cast<RunFn>(GetProcAddress(module, "GkVmdImport_Run"));
				auto abi = reinterpret_cast<AbiFn>(GetProcAddress(module, "GkVmdImporter_AbiVersion"));
				if (!run || !abi || abi() < 4)
				{
					Detail() = "GakuMotion.VmdImporter.Native.dll 缺少 ABI v4 导出（旧 DLL 会丢源帧率或重新烘焙请求）";
					RejectedModules().push_back({candidate, module, Detail()});
					continue;
				}
				Module() = module;
				Run() = run;
				auto cameraAbi = reinterpret_cast<AbiFn>(GetProcAddress(module, "GkCameraImporter_AbiVersion"));
				CameraRun() = cameraAbi && cameraAbi() == 1
					? reinterpret_cast<RunFn>(GetProcAddress(module, "GkCameraImport_Run")) : nullptr;
				auto mouthAbi = reinterpret_cast<AbiFn>(GetProcAddress(module, "GkMouthSource_AbiVersion"));
				MouthSourceRead() = mouthAbi && mouthAbi() == 1
					? reinterpret_cast<MouthSourceFn>(GetProcAddress(module, "GkMouthSource_Read")) : nullptr;
				const size_t slash = candidate.find_last_of(L"/\\");
				ModuleDir() = slash == std::wstring::npos ? std::wstring() : candidate.substr(0, slash);
				ModuleDirUtf8() = Utf8(ModuleDir());
				Detail().clear();
				return true;
			}

			if (Detail().empty()) Detail() = "未找到 GakuMotion.VmdImporter.Native.dll";
			return false;
		}
	}

	/// <summary>Resolves and loads the companion DLL from the plugin directory.</summary>
	inline bool Initialize(const std::string& pluginsRootUtf8)
	{
		std::lock_guard<std::mutex> lock(detail::Mutex());
		return detail::LoadLocked(pluginsRootUtf8);
	}

	inline bool Available()
	{
		std::lock_guard<std::mutex> lock(detail::Mutex());
		return detail::Module() != nullptr && detail::Run() != nullptr;
	}

	inline std::string Detail()
	{
		std::lock_guard<std::mutex> lock(detail::Mutex());
		return detail::Detail();
	}

	inline int32_t ReadMouthSource(const char* path, float sourceFps, float clipFps,
		int32_t frames, float* output, int32_t outputFloats)
	{
		std::lock_guard<std::mutex> lock(detail::Mutex());
		if (!detail::LoadLocked(detail::PluginsRoot()) || !detail::MouthSourceRead()) return 5;
		return detail::MouthSourceRead()(path, sourceFps, clipFps, frames, output, outputFloats);
	}

	inline bool Busy()
	{
		std::lock_guard<std::mutex> lock(detail::Mutex());
		return detail::Current().active;
	}

	inline bool CameraBusy()
	{
		std::lock_guard<std::mutex> lock(detail::Mutex());
		return detail::CameraCurrent().active;
	}

	/// <summary>Camera-only import is always native; missing ABI is an explicit deployment error.</summary>
	inline bool BeginCamera(const std::string& source, const std::string& directory,
		float fps, float scale, bool detectCuts, const std::string& logPath, std::string& message)
	{
		std::lock_guard<std::mutex> lock(detail::Mutex());
		auto& job = detail::CameraCurrent();
		if (job.active || detail::Current().active)
		{
			message = "已有导入任务正在运行";
			return false;
		}
		if (!detail::LoadLocked(detail::PluginsRoot()))
		{
			message = "原生镜头导入后端未加载: " + detail::Detail();
			return false;
		}
		if (!detail::CameraRun())
		{
			message = "GakuMotion.VmdImporter.Native.dll 缺少独立镜头 ABI v1，请部署配套新版 DLL";
			return false;
		}
		std::string json = "{\"source\":";
		detail::AppendJsonString(json, source);
		json += ",\"motion_dir\":";
		detail::AppendJsonString(json, directory);
		char settings[128]{};
		std::snprintf(settings, sizeof(settings), ",\"fps\":%.6f,\"scale\":%.6f,\"detect_cuts\":%s",
			fps, scale, detectCuts ? "true" : "false");
		json += settings;
		json += ",\"root_registration\":\"auto\",\"log_path\":";
		detail::AppendJsonString(json, logPath);
		json += "}";
		job.response.assign(65536, '\0');
		job.code = 2;
		job.done = false;
		job.logPath = logPath;
		job.active = true;
		RunFn run = detail::CameraRun();
		try
		{
			job.worker = std::thread([&job, run, json = std::move(json)]()
			{
				job.code = run(json.c_str(), job.response.data(), static_cast<int32_t>(job.response.size()));
				job.done = true;
			});
		}
		catch (...)
		{
			job.active = false;
			message = "无法创建镜头导入线程";
			return false;
		}
		message.clear();
		return true;
	}

	inline int PollCamera(std::string& message)
	{
		std::lock_guard<std::mutex> lock(detail::Mutex());
		auto& job = detail::CameraCurrent();
		if (!job.active || !job.done.load(std::memory_order_acquire)) return 0;
		if (job.worker.joinable()) job.worker.join();
		job.active = false;
		jsonlite::Value root;
		std::string error;
		const size_t length = job.response.find('\0');
		const bool parsed = length != std::string::npos && length > 0 &&
			jsonlite::ParseText(job.response.substr(0, length), root, error);
		const auto* ok = parsed ? root.Find("ok") : nullptr;
		const bool succeeded = job.code == 0 && ok && ok->IsBool() && ok->boolValue;
		if (succeeded) message = "镜头导入完成";
		else
		{
			const auto* detail = parsed ? root.Find("error") : nullptr;
			message = "镜头导入失败: " + (detail && detail->IsString() && !detail->stringValue.empty()
				? detail->stringValue : "原生后端响应无效");
		}
		if (!job.logPath.empty()) message += "（日志: " + job.logPath + "）";
		return succeeded ? 1 : -1;
	}

	namespace detail
	{
		inline bool ValidatePositions(const MotionImportRequest& request, std::string& reason)
		{
			if (request.positionsExplicit && request.positions.empty())
			{
				reason = "团曲位置列表不能为空";
				return false;
			}
			for (const auto& pos : request.positions)
			{
				if (pos.slot < 1 || pos.sourceSlot < 0)
				{
					reason = "团曲槽位序号无效";
					return false;
				}
				if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) ||
				    !std::isfinite(pos.yawDegrees) || !std::isfinite(pos.sourceFps) || pos.sourceFps < 0.0f)
				{
					reason = "团曲位置参数或源帧率必须为有限数值";
					return false;
				}
				if (!ValidTrackTiming(pos.motionTiming) || !ValidTrackTiming(pos.facialTiming))
				{
					reason = "团曲槽位播放速度必须为不小于 0 的有限数值，初始偏移量必须为有限数值";
					return false;
				}
			}
			return true;
		}

		inline std::string ResolvePrimaryVmd(const MotionImportRequest& request)
		{
			if (!request.vmd.empty()) return request.vmd;
			for (const auto& pos : request.positions)
			{
				if (pos.slot == 1 && !pos.motionVmd.empty()) return pos.motionVmd;
			}
			return std::string();
		}

		inline std::string FormatFiniteFloat(float value)
		{
			const float clean = (value == 0.0f || std::fabs(value) < 1e-7f) ? 0.0f : value;
			std::string s = jsonlite::FormatNumber(static_cast<double>(clean), 6);
			if (s == "-0") s = "0";
			return s;
		}

		inline std::string SerializeImportRequestJson(const MotionImportRequest& request)
		{
			const std::string primaryVmd = request.editDir.empty()
				? ResolvePrimaryVmd(request)
				: request.vmd;

			std::string json = "{";
			json += "\"motion_vmd\":";
			AppendJsonString(json, primaryVmd);
			json += ",\"edit_dir\":";
			AppendJsonString(json, request.editDir);
			json += ",\"morph_vmd\":";
			AppendJsonString(json, request.morphVmd);
			json += ",\"disable_morph\":";
			json += request.disableMorph ? "true" : "false";
			json += ",\"motions_root\":";
			AppendJsonString(json, request.motionsRoot);
			json += ",\"transaction_root\":";
			AppendJsonString(json, request.transactionRoot);
			json += ",\"title\":";
			AppendJsonString(json, request.title);
			if (request.hasSubtitle)
			{
				json += ",\"subtitle\":";
				AppendJsonString(json, request.subtitle);
			}
			char fpsBuffer[64]{};
			std::snprintf(fpsBuffer, sizeof(fpsBuffer), "%.6f", request.fps > 0.0f ? request.fps : 0.0f);
			json += ",\"fps\":";
			json += fpsBuffer;
			json += ",\"rebake_output\":";
			json += request.rebakeOutput ? "true" : "false";
			std::snprintf(fpsBuffer, sizeof(fpsBuffer), "%.6f", request.sourceFps > 0.0f ? request.sourceFps : 0.0f);
			json += ",\"source_fps\":";
			json += fpsBuffer;
			json += ",\"audio\":";
			AppendJsonString(json, request.audio);
			json += ",\"clear_audio\":";
			json += request.clearAudio ? "true" : "false";
			json += ",\"track_timing\":" + PerformanceTimingJson(request.timing);
			json += ",\"camera_vmd\":";
			AppendJsonString(json, request.cameraVmd);
			json += ",\"clear_camera\":";
			json += request.clearCamera ? "true" : "false";
			char cameraFpsBuffer[64]{};
			std::snprintf(cameraFpsBuffer, sizeof(cameraFpsBuffer), "%.6f", request.cameraFps > 0.0f ? request.cameraFps : 60.0f);
			json += ",\"camera_fps\":";
			json += cameraFpsBuffer;
			char cameraScaleBuffer[64]{};
			std::snprintf(cameraScaleBuffer, sizeof(cameraScaleBuffer), "%.6f", request.cameraScale > 0.0f ? request.cameraScale : 0.083f);
			json += ",\"camera_scale\":";
			json += cameraScaleBuffer;
			json += ",\"camera_root_registration\":";
			AppendJsonString(json, request.cameraRootRegistration);
			if (!request.positions.empty() || request.positionsExplicit)
			{
				json += ",\"positions\":[";
				for (size_t i = 0; i < request.positions.size(); ++i)
				{
					if (i > 0) json.push_back(',');
					const auto& pos = request.positions[i];
					json += "{\"slot\":";
					json += std::to_string(pos.slot);
					if (pos.sourceSlot > 0)
					{
						json += ",\"source_slot\":";
						json += std::to_string(pos.sourceSlot);
					}
					json += ",\"motion_vmd\":";
					AppendJsonString(json, pos.motionVmd);
					json += ",\"morph_vmd\":";
					AppendJsonString(json, pos.morphVmd);
					json += ",\"disable_morph\":";
					json += pos.disableMorph ? "true" : "false";
					json += ",\"clear_morph\":";
					json += pos.clearMorph ? "true" : "false";
					json += ",\"source_fps\":";
					json += FormatFiniteFloat(pos.sourceFps > 0.0f ? pos.sourceFps : 0.0f);
					json += ",\"rebake_output\":";
					json += pos.rebakeOutput ? "true" : "false";
					json += ",\"preserve_existing\":";
					json += pos.preserveExisting ? "true" : "false";
					json += ",\"x\":";
					json += FormatFiniteFloat(pos.x);
					json += ",\"y\":";
					json += FormatFiniteFloat(pos.y);
					json += ",\"z\":";
					json += FormatFiniteFloat(pos.z);
					json += ",\"yaw_degrees\":";
					json += FormatFiniteFloat(pos.yawDegrees);
					json += ",\"has_explicit_placement\":";
					json += pos.hasExplicitPlacement ? "true" : "false";
					if (pos.hasTiming)
					{
						json += ",\"track_timing\":";
						json += SlotTrackTimingJson(pos.motionTiming, pos.facialTiming);
					}
					json.push_back('}');
				}
				json.push_back(']');
			}
			json += ",\"log_path\":";
			AppendJsonString(json, request.log);
			json += ",\"timing_jsonl\":";
			{
				// Collect real import timings next to the import log
				// (<localRoot>/GakuMotion/vmd_import_timing.jsonl), never in plugins/.
				const size_t slash = request.log.find_last_of("/\\");
				const std::string timing = (slash == std::string::npos ? std::string() : request.log.substr(0, slash + 1))
					+ "vmd_import_timing.jsonl";
				AppendJsonString(json, timing);
			}
			json += "}";
			return json;
		}
	}

	/// <summary>
	/// Requests admitted by the native product pipeline. Unsupported target
	/// geometry is an explicit error; existing clips remain playable.
	/// </summary>
	inline bool CanHandle(const MotionImportRequest& request, std::string& reason)
	{
		if (!ValidPerformanceTiming(request.timing))
		{
			reason = "播放速度必须为不小于 0 的有限数值，初始偏移量必须为有限数值";
			return false;
		}
		if (!detail::ValidatePositions(request, reason))
		{
			return false;
		}
		if (detail::ResolvePrimaryVmd(request).empty() && request.editDir.empty())
		{
			reason = "未指定 VMD 或待编辑曲目";
			return false;
		}
		// The native pipeline alone verifies adjacent geometry against the VMD's
		// actual SHA256. Merely sharing a directory is not a binding.
		return true;
	}

	/// <summary>Starts the import on one worker thread; returns false if it could not start.</summary>
	inline bool Begin(const MotionImportRequest& request, std::string& message)
	{
		if (!ValidPerformanceTiming(request.timing))
		{
			message = "播放速度必须为不小于 0 的有限数值，初始偏移量必须为有限数值";
			return false;
		}
		if (!detail::ValidatePositions(request, message))
		{
			return false;
		}
		std::lock_guard<std::mutex> lock(detail::Mutex());
		auto& job = detail::Current();
		if (job.active || detail::CameraCurrent().active)
		{
			message = "已有导入任务正在运行";
			return false;
		}
		if (!detail::LoadLocked(detail::PluginsRoot()))
		{
			message = "C# 导入后端未加载: " + detail::Detail();
			return false;
		}
		if (!detail::Run())
		{
			message = "C# 导入后端未加载: " + detail::Detail();
			return false;
		}

		std::string json = detail::SerializeImportRequestJson(request);

		job.response.assign(65536, '\0');
		job.code = 2;
		job.done = false;
		job.active = true;
		job.logPath = request.log;
		RunFn run = detail::Run();

		// Truncate before the worker starts so the panel never drains a stale log.
		if (!request.log.empty())
		{
			HANDLE handle = CreateFileW(detail::Wide(request.log).c_str(), GENERIC_WRITE,
			                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
			                            FILE_ATTRIBUTE_NORMAL, nullptr);
			if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
		}

		try
		{
			job.worker = std::thread([&job, run, json = std::move(json)]()
			{
				job.code = run(json.c_str(), job.response.data(), static_cast<int32_t>(job.response.size()));
				job.done = true;
			});
		}
		catch (...)
		{
			job.active = false;
			message = "无法创建导入线程";
			return false;
		}

		message.clear();
		return true;
	}

	/// <summary>Polls once: 0 = running, 1 = done, -1 = failed.</summary>
	inline int Poll(std::string& message, ImportProgressState* progress)
	{
		std::lock_guard<std::mutex> lock(detail::Mutex());
		auto& job = detail::Current();
		if (!job.active) return 0;
		if (!job.done.load(std::memory_order_acquire)) return 0;

		if (job.worker.joinable()) job.worker.join();
		job.active = false;

		jsonlite::Value root;
		std::string error;
		bool parsed = false;
		const size_t length = job.response.find('\0');
		if (length != std::string::npos && length > 0)
		{
			parsed = jsonlite::ParseText(std::string(job.response.c_str(), length), root, error);
		}

		auto stringField = [&](const char* key)
		{
			const jsonlite::Value* value = parsed ? root.Find(key) : nullptr;
			return value != nullptr && value->IsString() ? value->stringValue : std::string();
		};
		auto numberField = [&](const char* key)
		{
			const jsonlite::Value* value = parsed ? root.Find(key) : nullptr;
			return value != nullptr && value->IsNumber() ? value->numberValue : 0.0;
		};

		const bool ok = job.code == 0 && parsed && root.Find("ok") != nullptr && root.Find("ok")->IsBool() &&
		                root.Find("ok")->boolValue;
		if (ok)
		{
			const std::string dir = stringField("dir");
			message = "导入完成，已加入播放列表";
			if (progress)
			{
				progress->active = false;
				progress->finished = true;
				progress->succeeded = true;
				progress->progress = 1.0f;
				progress->statusText = "导入完成";
			}
			(void)dir;
			return 1;
		}

		std::string detail = stringField("error");
		if (detail.empty())
		{
			if (!parsed && job.code == 3) detail = "C# 导入后端响应缓冲区不足";
			else detail = parsed ? "C# 导入后端返回失败" : "C# 导入后端响应无法解析";
		}
		std::string code = stringField("code");
		if (code.empty() && !parsed && job.code == 3) code = "buffer_too_small";
		message = "导入失败: " + detail;
		if (!job.logPath.empty()) message += "（日志: " + job.logPath + "）";
		if (progress)
		{
			progress->active = false;
			progress->finished = true;
			progress->succeeded = false;
			progress->statusText = code.empty() ? "导入失败" : "导入失败 (" + code + ")";
			progress->errorDetail = message;
		}
		return -1;
	}

	/// <summary>Best-effort shutdown: joins a running worker so no thread outlives the plugin.</summary>
	inline void Shutdown()
	{
		std::lock_guard<std::mutex> lock(detail::Mutex());
		auto& job = detail::Current();
		if (job.worker.joinable()) job.worker.join();
		job.active = false;
		auto& cameraJob = detail::CameraCurrent();
		if (cameraJob.worker.joinable()) cameraJob.worker.join();
		cameraJob.active = false;
		// Only join jobs here. NativeAOT does not support FreeLibrary/dlclose;
		// keep the accepted module and its exports for this process's lifetime.
		// https://learn.microsoft.com/dotnet/core/deploying/native-aot/libraries
	}
}
