#pragma once

#include "GkAudioPlayback.hpp"
#include "GkCamera.hpp"
#include "GkTrackTiming.hpp"

// =============================================================================
// GkRuntimeConfig.hpp — 运行时控制文件热加载层（阶段一「无 GUI」控制入口）
// =============================================================================
// 设计目标（对应批准计划的「选曲 / 进度 / 视线 / 角色 / 骨骼微调」五项诉求）：
//   在 ImGui 面板尚未接入（宿主未导出 ImGui 符号，见 ARCHITECTURE.md §6）的
//   阶段，先用「可热加载的 JSON 控制文件 + 快捷键」把全部控制能力跑通，
//   阶段二再用图形面板复用同一套状态机，不需要改动本文件的任何语义。
//
// 文件位置： <localRoot>/GakuMotion/control.json     （首次运行自动生成模板）
//
// schema 分两类字段，语义严格区分：
//   1) 声明式（declarative）—— 文件里写什么就是什么，持续生效：
//        selected_motion    : 目标曲目（文件夹名或 info.json 的 title 都可）
//        autoplay           : 选中曲目后是否立刻开始播放
//        gaze.mode          : 0=Auto 1=ForceOff 2=DesktopOn
//        gaze.desktop_enabled / eye_weight / head_weight / body_weight
//        use_rest_rotations : 是否启用「静息姿态相对校准」（true=现网稳健行为）
//        overrides.enabled  : 骨骼覆盖总开关
//        overrides.motion_file / overrides.global_file : 可选的自定义路径
//        poll_interval_seconds : 文件轮询间隔（默认 0.5s）
//   2) 一次性命令（one-shot）—— 靠单调递增的 seq 去重，执行一次即失效：
//        command.seq            : 必须比上一次大，否则整块命令被忽略
//        command.play / pause / stop
//        command.seek_seconds
//        command.reload_overrides
//        command.toggle_overrides
//     之所以用 seq 而不是「执行后回写清空」，是为了绝不改写用户手写的
//     文件（保留注释与排版），行为可预测、可复现。
// =============================================================================

#include "GkJsonLite.hpp"
#include "GkBoneOverride.hpp"

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace GakuMotion
{
	// -----------------------------------------------------------------------------
	// 1. 文件签名（mtime + size）：热加载的变更探测依据
	// -----------------------------------------------------------------------------
	struct FileSignature
	{
		bool exists{false};
		long long writeTime{0};
		long long size{0};

		bool operator==(const FileSignature& rhs) const noexcept
		{
			return exists == rhs.exists && writeTime == rhs.writeTime && size == rhs.size;
		}

		bool operator!=(const FileSignature& rhs) const noexcept { return !(*this == rhs); }

		bool Changed(const FileSignature& rhs) const noexcept { return !(*this == rhs); }
	};

	inline FileSignature StatFile(const std::string& path)
	{
		FileSignature sig;
		std::error_code ec;
		const auto nativePath = MotionPathFromUtf8(path);
		if (!std::filesystem::exists(nativePath, ec)) return sig;

		std::filesystem::file_status st = std::filesystem::status(nativePath, ec);
		if (ec) return sig;

		sig.exists = true;
		auto t = std::filesystem::last_write_time(nativePath, ec);
		if (!ec)
		{
			sig.writeTime = static_cast<long long>(t.time_since_epoch().count());
		}
		auto sz = std::filesystem::file_size(nativePath, ec);
		if (!ec)
		{
			sig.size = static_cast<long long>(sz);
		}
		return sig;
	}

	struct MotionPositionEntry
	{
		int slot{1};
		float x{0.0f};
		float y{0.0f};
		float z{0.0f};
		float yawDegrees{0.0f};
		bool hasMotion{false};
		bool hasFacial{false};
		std::string sourceVmd;
		std::string sourceMorphVmd;
		std::string sourceVmdSha256;
		float sourceFps{0.0f};
		uint32_t frames{0};
		uint32_t bones{0};
		std::string motionFile;
		bool hasOverrides{false};
	};

	struct TrackPositionPlacementUpdate
	{
		int slot{1};
		float x{0.0f};
		float y{0.0f};
		float z{0.0f};
		float yawDegrees{0.0f};
	};

	// -----------------------------------------------------------------------------
	// 2. 曲目库扫描（选曲列表的来源，与 GkGuiOverlay 的既有扫描规则保持一致：
	//    含 motion.gkclip / motion.gk_world_dump / motion.gkmotion 的子目录可播）
	// -----------------------------------------------------------------------------
	struct MotionEntry
	{
		std::string name; // 文件夹名（用作唯一 key）
		std::string dir; // 绝对/相对路径
		std::string title; // info.json 的 title（缺省回退为文件夹名）
		std::string subtitle; // info.json 的 subtitle（副标题/作者，半角逗号分隔）
		float durationSeconds{0.0f};
		uint32_t totalFrames{0};
		uint32_t boneCount{0};
		float fps{30.0f};
		bool hasAudio{false};
		std::string audioFile; // 解析命中的音频文件名（如 song.wav / song.flac）
		float audioDurationSeconds{0.0f};
		bool hasCover{false};
		std::string coverFile; // 解析命中的封面文件名（如 cover.jpg / cover.png）
		bool hasOverrides{false}; // 是否带曲目级 bone_overrides.json
		bool hasFacial{false};
		bool hasCamera{false};
		std::string sourceVmd;
		std::string sourceMorphVmd;
		std::string sourceCameraVmd;
		PerformanceTiming timing;
		bool metadataLoaded{false}; // 是否成功读取 info.json
		std::string motionFile; // 扫描实际命中的动作文件，UTF-8；GUI 不重复探测磁盘
		bool isGroup{false};
		int positionCount{1};
		std::vector<MotionPositionEntry> positions;

		std::string SlotDirectory(int slot1Based) const
		{
			return ResolveMotionSlotDirectory(dir, slot1Based);
		}

		std::string DisplayLabel() const
		{
			char buf[256]{};
			if (metadataLoaded && durationSeconds > 0.0f)
			{
				std::snprintf(buf, sizeof(buf), "%s  (%.2fs / %uf / %ub)",
				              title.c_str(), durationSeconds, totalFrames, boneCount);
			}
			else
			{
				std::snprintf(buf, sizeof(buf), "%s", title.c_str());
			}
			return std::string(buf);
		}
	};

	// Compare complete lexical paths rather than matching a directory-name substring.
	// Missing sources remain comparable; canonical() would require the file to exist.
	inline bool MotionPathEqualsUtf8(const std::string& a, const std::string& b)
	{
		if (a.empty() || b.empty()) return false;
		std::error_code ec;
		auto left = std::filesystem::absolute(MotionPathFromUtf8(a), ec).lexically_normal();
		if (ec) return false;
		auto right = std::filesystem::absolute(MotionPathFromUtf8(b), ec).lexically_normal();
		if (ec) return false;
#ifdef _WIN32
		auto lower = [](std::wstring value) {
			std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
			return value;
		};
		return lower(left.native()) == lower(right.native());
#else
		return left == right;
#endif
	}

	inline int FindMotionKeyIndex(const std::vector<MotionEntry>& entries, const std::string& key)
	{
		for (size_t i = 0; i < entries.size(); ++i)
			if (entries[i].name == key) return static_cast<int>(i);
		return -1;
	}

	enum class MotionLibraryScanStatus { Complete, Partial, Failed };
	struct MotionLibraryScanResult
	{
		MotionLibraryScanStatus status{MotionLibraryScanStatus::Failed};
		std::vector<MotionEntry> motions;
		std::vector<std::string> warnings;
		std::string error;
		bool Succeeded() const { return status != MotionLibraryScanStatus::Failed; }
		bool Complete() const { return status == MotionLibraryScanStatus::Complete; }
	};

	// Low-cost structural probe: do not allocate or inspect every animation sample.
	// Playback still performs full numerical and bone-contract validation on load.
	inline bool ProbeMotionLibraryFile(const std::filesystem::path& path, MotionEntry& motion, std::string& error)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		auto fail = [&](const char* reason) { error = reason; return false; };
		if (!file) return fail("cannot open motion file");
		const auto end = file.tellg();
		if (end < 8 || static_cast<uint64_t>(end) > MotionClip::kDefaultLoadBudget)
			return fail("motion file size/budget");
		file.seekg(0);
		auto read = [&](auto& value) { return bool(file.read(reinterpret_cast<char*>(&value), sizeof(value))); };
		auto skip = [&](uint64_t bytes) {
			const auto position = file.tellg();
			if (position < 0 || position > end || bytes > static_cast<uint64_t>(end - position)) return false;
			file.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
			return bool(file);
		};
		char magic[8]{};
		file.read(magic, 8);
		const auto format = MotionClip::DetectFormat(magic);
		if (format == MotionClip::Format::kUnknown) return fail("unknown motion format");
		file.seekg(format == MotionClip::Format::kLegacyGkmo ? 4 : 8);
		uint32_t version{};
		float rootScale{};
		if (!read(version) || !read(motion.fps) || !read(motion.totalFrames) || !read(motion.durationSeconds) ||
			!read(motion.boneCount) || !read(rootScale)) return fail("incomplete motion header");
		if (!std::isfinite(motion.fps) || motion.fps <= 0 || !std::isfinite(motion.durationSeconds) ||
			motion.durationSeconds < 0 || !std::isfinite(rootScale) || rootScale <= 0 ||
			!motion.totalFrames || motion.totalFrames > 1000000 || !motion.boneCount || motion.boneCount > 256 ||
			version < 1 || version > (format == MotionClip::Format::kGkClip ? 5u : 4u))
			return fail("invalid motion header/version");
		for (uint32_t i = 0; i < motion.boneCount; ++i)
		{
			uint16_t hid{};
			uint8_t length{}, flags{};
			if (!read(hid) || !read(length) || !read(flags) || !length || flags > (version >= 5 ? 3 : 1) || !skip(length))
				return fail("incomplete motion bone table");
		}
		if (!skip(static_cast<uint64_t>(motion.totalFrames) * (sizeof(Vector3) + sizeof(Quaternion) * motion.boneCount)))
			return fail("incomplete motion frames");
		if ((format == MotionClip::Format::kGkClip && version >= 4) || file.tellg() < end)
		{
			uint8_t hasCamera{};
			if (!read(hasCamera) || hasCamera > 1) return fail("incomplete motion camera flag");
			motion.hasCamera = hasCamera != 0;
			if (hasCamera && !skip(static_cast<uint64_t>(motion.totalFrames) * sizeof(LegacyCameraFrame)))
				return fail("incomplete motion camera frames");
		}
		if (format == MotionClip::Format::kGkClip && version >= 4)
		{
			uint32_t count{};
			if (!read(count) || count > 1024) return fail("incomplete motion facial count");
			for (uint32_t i = 0; i < count; ++i)
			{
				uint16_t length{};
				if (!read(length) || !length || !skip(length)) return fail("incomplete motion facial names");
			}
			if (!skip(static_cast<uint64_t>(motion.totalFrames) * count * sizeof(float)))
				return fail("incomplete motion facial frames");
			motion.hasFacial = count != 0;
		}
		if (file.tellg() != end) return fail("unexpected motion payload");
		return true;
	}

	inline std::mutex& InfoJsonMutationMutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	inline bool ReadTrackInfoJsonTextUnlocked(
		const std::filesystem::path& infoPath,
		std::string& outText,
		std::string* outError = nullptr)
	{
		outText.clear();
		if (outError) outError->clear();
		std::error_code ec;
		if (!std::filesystem::is_regular_file(infoPath, ec) || ec)
		{
			if (outError) *outError = ec ? ec.message() : "info.json not found";
			return false;
		}
#ifdef _WIN32
		HANDLE file = CreateFileW(
			infoPath.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			if (outError)
				*outError = std::error_code(static_cast<int>(GetLastError()), std::system_category()).message();
			return false;
		}
		LARGE_INTEGER fileSize{};
		if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 0 ||
			static_cast<uint64_t>(fileSize.QuadPart) > 64ULL * 1024ULL * 1024ULL)
		{
			const DWORD err = GetLastError();
			CloseHandle(file);
			if (outError)
				*outError = err ? std::error_code(static_cast<int>(err), std::system_category()).message() : "invalid info.json size";
			return false;
		}
		outText.resize(static_cast<size_t>(fileSize.QuadPart));
		size_t totalRead = 0;
		while (totalRead < outText.size())
		{
			DWORD bytesRead = 0;
			const auto chunk = static_cast<DWORD>((std::min)(outText.size() - totalRead, size_t{0x40000000}));
			if (!ReadFile(file, outText.data() + totalRead, chunk, &bytesRead, nullptr))
			{
				const DWORD err = GetLastError();
				CloseHandle(file);
				outText.clear();
				if (outError)
					*outError = std::error_code(static_cast<int>(err), std::system_category()).message();
				return false;
			}
			if (bytesRead == 0) break;
			totalRead += bytesRead;
		}
		CloseHandle(file);
		outText.resize(totalRead);
		return true;
#else
		std::ifstream in(infoPath, std::ios::binary);
		if (!in)
		{
			if (outError) *outError = "cannot open info.json for reading";
			return false;
		}
		std::ostringstream ss;
		ss << in.rdbuf();
		if (in.bad())
		{
			if (outError) *outError = "failed reading info.json";
			return false;
		}
		outText = ss.str();
		return true;
#endif
	}

	inline bool ReadTrackInfoJsonText(
		const std::filesystem::path& infoPath,
		std::string& outText,
		std::string* outError = nullptr)
	{
		std::lock_guard<std::mutex> lock(InfoJsonMutationMutex());
		return ReadTrackInfoJsonTextUnlocked(infoPath, outText, outError);
	}

	inline bool ParseTrackInfoJsonFile(
		const std::filesystem::path& infoPath,
		jsonlite::Value& outValue,
		std::string& outError)
	{
		std::string text;
		std::string readErr;
		if (!ReadTrackInfoJsonText(infoPath, text, &readErr))
		{
			outError = readErr.empty() ? "cannot open info.json for reading" : readErr;
			return false;
		}
		return jsonlite::ParseText(text, outValue, outError);
	}

	inline MotionLibraryScanResult ScanMotionLibraryDetailed(const std::string& motionsRoot)
	{
		MotionLibraryScanResult result;
		const auto root = MotionPathFromUtf8(motionsRoot);
		std::error_code ec;
		if (!std::filesystem::is_directory(root, ec) || ec)
		{
			result.error = "cannot access motions directory: " + motionsRoot + (ec ? " (" + ec.message() + ")" : "");
			return result;
		}
		std::filesystem::directory_iterator iterator(root, ec), end;
		if (ec)
		{
			result.error = "cannot scan motions directory: " + ec.message();
			return result;
		}
		result.status = MotionLibraryScanStatus::Complete;
		auto warn = [&](const std::string& message, bool incomplete = true) {
			result.warnings.push_back(message);
			if (incomplete) result.status = MotionLibraryScanStatus::Partial;
		};
		for (; iterator != end; iterator.increment(ec))
		{
			if (ec) break;
			const auto& entry = *iterator;
			if (entry.path().filename() == ".gakumotion-trash") continue; // Reserved legacy recovery storage; never scanned as a track.
			const bool isDirectory = entry.is_directory(ec);
			if (ec) { warn(MotionPathToUtf8(entry.path()) + ": " + ec.message()); ec.clear(); continue; }
			if (!isDirectory) continue;
			const std::string dir = MotionPathToUtf8(entry.path());

			// Clip discovery, in priority order.
			//
			// gkclip is preferred: it is the distributable format and carries no
			// character constants, so it behaves identically on every actor. A
			// world dump is second (it is bound to whoever was recorded). The old
			// ".gkmotion" is last and exists only so existing installs keep working
			// during migration.
			//
			// Exactly one file is used per track. If a directory ships both, the
			// gkclip wins -- mixing the two would mean playing a role-agnostic delta
			// against a world-space expectation.
			std::filesystem::path motionPath;
			for (const char* candidate : {"motion.gkclip", "motion.gk_world_dump", "motion.gkmotion"})
			{
				const auto path = entry.path() / candidate;
				const bool exists = std::filesystem::exists(path, ec);
				if (ec) { warn(MotionPathToUtf8(path) + ": " + ec.message()); ec.clear(); break; }
				if (exists) { motionPath = path; break; }
			}
			if (motionPath.empty()) continue;

			MotionEntry m;
			m.dir = dir;
			m.motionFile = MotionPathToUtf8(motionPath);
			const auto utf8Name = entry.path().filename().u8string();
			m.name.assign(utf8Name.begin(), utf8Name.end());
			m.title = m.name;
			const auto beforeTime = std::filesystem::last_write_time(motionPath, ec);
			if (ec) { warn(m.name + ": " + ec.message()); ec.clear(); continue; }
			const auto beforeSize = std::filesystem::file_size(motionPath, ec);
			if (ec) { warn(m.name + ": " + ec.message()); ec.clear(); continue; }
			std::string probeError;
			if (!ProbeMotionLibraryFile(motionPath, m, probeError)) { warn(m.name + ": " + probeError); continue; }
			auto exists = [&](const char* filename) {
				const bool found = std::filesystem::exists(entry.path() / filename, ec);
				if (ec) { warn(m.name + "/" + filename + ": " + ec.message()); ec.clear(); }
				return found;
			};
			m.audioFile = audio::ResolveTrackAudioFileName(entry.path());
			m.hasAudio = !m.audioFile.empty();
			m.coverFile = audio::ResolveTrackCoverFileName(entry.path());
			m.hasCover = !m.coverFile.empty();
			m.hasOverrides = exists("bone_overrides.json");
			m.hasCamera = exists("camera.gkcamera") || m.hasCamera;

			// info.json 是可选元数据：缺失或损坏都不影响曲目可用性
			bool hasFacialExplicit = false;
			bool infoIsGroup = false;
			std::map<int, MotionPositionEntry> posMap;
			{
				MotionPositionEntry slot1;
				slot1.slot = 1;
				slot1.hasMotion = true;
				slot1.hasFacial = m.hasFacial;
				slot1.sourceFps = m.fps > 0.0f ? m.fps : 30.0f;
				slot1.frames = m.totalFrames;
				slot1.bones = m.boneCount;
				slot1.motionFile = m.motionFile;
				slot1.hasOverrides = m.hasOverrides;
				posMap[1] = std::move(slot1);
			}

			if (exists("info.json"))
			{
				jsonlite::Value info;
				std::string err;
				const bool parsed = ParseTrackInfoJsonFile(entry.path() / "info.json", info, err) && info.IsObject();
				if (parsed)
				{
					m.title = info.StringPath("title", m.name);
					m.subtitle = info.StringPath("subtitle", "");
					m.durationSeconds = info.FloatPath("duration_seconds", m.durationSeconds);
					m.totalFrames = static_cast<uint32_t>(info.IntPath("total_frames", static_cast<int>(m.totalFrames)));
					m.boneCount = static_cast<uint32_t>(info.IntPath("bone_count", static_cast<int>(m.boneCount)));
					m.fps = info.FloatPath("fps", m.fps);
					m.sourceVmd = info.StringPath("source_vmd", "");
					m.sourceMorphVmd = info.StringPath("source_morph_vmd", "");
					m.sourceCameraVmd = info.StringPath("source_camera_vmd", "");
					m.timing = ReadPerformanceTiming(info);
					const std::string hintedAudio = info.StringPath("audio_file", "");
					const std::string resolvedHintedAudio = audio::ResolveTrackAudioFileName(entry.path(), hintedAudio);
					if (!resolvedHintedAudio.empty())
					{
						m.audioFile = resolvedHintedAudio;
						m.hasAudio = true;
					}
					m.audioDurationSeconds = info.FloatPath("audio_duration", 0.0f);
					const std::string hintedCover = info.StringPath("cover_file", "");
					const std::string resolvedHintedCover = audio::ResolveTrackCoverFileName(entry.path(), hintedCover);
					if (!resolvedHintedCover.empty())
					{
						m.coverFile = resolvedHintedCover;
						m.hasCover = true;
					}
					if (info.Find("has_camera"))
					{
						m.hasCamera = m.hasCamera || info.BoolPath("has_camera", false);
					}
					if (info.Find("has_facial"))
					{
						m.hasFacial = info.BoolPath("has_facial", false);
						hasFacialExplicit = true;
					}

					posMap[1].sourceVmd = m.sourceVmd;
					posMap[1].sourceMorphVmd = m.sourceMorphVmd;
					posMap[1].sourceVmdSha256 = info.StringPath("source_vmd_sha256", "");
					const float infoSourceFps = info.FloatPath("source_fps", m.fps);
					if (std::isfinite(infoSourceFps) && infoSourceFps > 0.0f) posMap[1].sourceFps = infoSourceFps;
					posMap[1].hasFacial = m.hasFacial;

					infoIsGroup = info.BoolPath("is_group", false);
					if (const auto* positionsArr = info.Find("positions"); positionsArr && positionsArr->IsArray())
					{
						for (size_t pi = 0; pi < positionsArr->arrayItems.size(); ++pi)
						{
							const auto& item = positionsArr->arrayItems[pi];
							if (!item.IsObject()) continue;
							const int slot = item.IntPath("slot", static_cast<int>(pi) + 1);
							if (slot < 1) continue;
							auto& pe = posMap[slot];
							pe.slot = slot;
							if (slot == 1)
							{
								pe.x = 0.0f;
								pe.y = 0.0f;
								pe.z = 0.0f;
								pe.yawDegrees = 0.0f;
								pe.hasMotion = true;
								pe.motionFile = m.motionFile;
								pe.hasOverrides = m.hasOverrides;
							}
							else
							{
								const float px = item.FloatPath("x", 0.0f);
								const float py = item.FloatPath("y", 0.0f);
								const float pz = item.FloatPath("z", 0.0f);
								const float pyaw = item.FloatPath("yaw_degrees", 0.0f);
								pe.x = std::isfinite(px) ? px : 0.0f;
								pe.y = std::isfinite(py) ? py : 0.0f;
								pe.z = std::isfinite(pz) ? pz : 0.0f;
								pe.yawDegrees = std::isfinite(pyaw) ? pyaw : 0.0f;
								pe.hasMotion = false;
								pe.motionFile.clear();
							}
							pe.hasFacial = item.BoolPath("has_facial", slot == 1 ? m.hasFacial : false);
							pe.sourceVmd = item.StringPath("source_vmd", slot == 1 ? m.sourceVmd : "");
							pe.sourceMorphVmd = item.StringPath("source_morph_vmd", slot == 1 ? m.sourceMorphVmd : "");
							pe.sourceVmdSha256 = item.StringPath("source_vmd_sha256", slot == 1 ? posMap[1].sourceVmdSha256 : "");
							const float sfps = item.FloatPath("source_fps", 0.0f);
							if (std::isfinite(sfps) && sfps > 0.0f) pe.sourceFps = sfps;
							const int pf = item.IntPath("frames", slot == 1 ? static_cast<int>(m.totalFrames) : 0);
							if (pf > 0) pe.frames = static_cast<uint32_t>(pf);
							const int pb = item.IntPath("bones", slot == 1 ? static_cast<int>(m.boneCount) : 0);
							if (pb > 0) pe.bones = static_cast<uint32_t>(pb);
						}
					}
					m.metadataLoaded = true;
				}
				else warn(m.name + "/info.json: " + (err.empty() ? "invalid or unreadable metadata" : err), false);
			}
			if (!hasFacialExplicit && !m.hasFacial) m.hasFacial = exists("motion.morph_report.json");

			// Probe positions/slot_{k}/ (k >= 2) using a temporary MotionEntry so slot 1 metadata stays intact.
			const auto positionsDir = entry.path() / "positions";
			if (std::filesystem::is_directory(positionsDir, ec))
			{
				std::filesystem::directory_iterator posIt(positionsDir, ec), posEnd;
				if (ec)
				{
					warn(m.name + "/positions: " + ec.message());
					ec.clear();
				}
				else
				{
					for (; posIt != posEnd; posIt.increment(ec))
					{
						if (ec) { ec.clear(); break; }
						const auto& slotEntry = *posIt;
						if (!slotEntry.is_directory(ec)) { ec.clear(); continue; }
						const auto slotUtf8 = slotEntry.path().filename().u8string();
						const std::string slotName(slotUtf8.begin(), slotUtf8.end());
						int slotIndex = 0;
						if (!ParseSlotDirectoryIndex(slotName, slotIndex)) continue;

						std::filesystem::path slotMotionPath;
						for (const char* candidate : {"motion.gkclip", "motion.gk_world_dump", "motion.gkmotion"})
						{
							const auto p = slotEntry.path() / candidate;
							if (std::filesystem::exists(p, ec)) { slotMotionPath = p; break; }
							ec.clear();
						}

						auto& pe = posMap[slotIndex];
						pe.slot = slotIndex;
						pe.hasOverrides = std::filesystem::exists(slotEntry.path() / "bone_overrides.json", ec);
						ec.clear();

						if (slotMotionPath.empty())
						{
							pe.hasMotion = false;
							pe.motionFile.clear();
							continue;
						}

						MotionEntry slotProbe;
						std::string slotProbeErr;
						if (!ProbeMotionLibraryFile(slotMotionPath, slotProbe, slotProbeErr))
						{
							warn(m.name + "/positions/" + slotName + ": " + slotProbeErr);
							pe.hasMotion = false;
							pe.motionFile.clear();
							continue;
						}
						pe.hasMotion = true;
						pe.motionFile = MotionPathToUtf8(slotMotionPath);
						if (pe.frames == 0) pe.frames = slotProbe.totalFrames;
						if (pe.bones == 0) pe.bones = slotProbe.boneCount;
						if (pe.sourceFps <= 0.0f && std::isfinite(slotProbe.fps) && slotProbe.fps > 0.0f)
							pe.sourceFps = slotProbe.fps;
						const bool hasMorphReport = std::filesystem::exists(slotEntry.path() / "motion.morph_report.json", ec);
						ec.clear();
						pe.hasFacial = pe.hasFacial || slotProbe.hasFacial || hasMorphReport;
					}
				}
			}
			ec.clear();

			m.positions.clear();
			m.positions.reserve(posMap.size());
			for (auto& [slotKey, pe] : posMap)
			{
				(void)slotKey;
				if (pe.sourceFps <= 0.0f)
					pe.sourceFps = (std::isfinite(m.fps) && m.fps > 0.0f) ? m.fps : 30.0f;
				if (!hasFacialExplicit && pe.hasFacial) m.hasFacial = true;
				m.positions.push_back(std::move(pe));
			}
			m.positionCount = static_cast<int>(m.positions.size());
			m.isGroup = (m.positionCount >= 2) || infoIsGroup;
			const auto afterTime = std::filesystem::last_write_time(motionPath, ec);
			if (ec) { warn(m.name + ": motion changed during scan"); ec.clear(); continue; }
			const auto afterSize = std::filesystem::file_size(motionPath, ec);
			if (ec || beforeTime != afterTime || beforeSize != afterSize)
			{ warn(m.name + ": motion changed during scan"); ec.clear(); continue; }
			result.motions.push_back(std::move(m));
		}
		if (ec) warn("motions scan incomplete: " + ec.message());

		// 稳定排序：保证「上一曲/下一曲」在多次扫描之间顺序一致
		std::sort(result.motions.begin(), result.motions.end(),
		          [](const MotionEntry& a, const MotionEntry& b) { return a.name < b.name; });

		return result;
	}

	// Compatibility entry point; an accessible, successfully scanned empty library
	// is a valid result. Warnings remain available to callers through error.
	inline bool ScanMotionLibrary(const std::string& motionsRoot, std::vector<MotionEntry>& out, std::string& error)
	{
		auto result = ScanMotionLibraryDetailed(motionsRoot);
		out = std::move(result.motions);
		error = result.error;
		for (const auto& warning : result.warnings)
			error += (error.empty() ? "" : "\n") + warning;
		return result.Succeeded();
	}

	// 按「文件夹名优先、其次 title、最后前缀匹配」查找曲目下标
	inline int FindMotionIndex(const std::vector<MotionEntry>& motions, const std::string& key)
	{
		if (key.empty()) return -1;
		for (size_t i = 0; i < motions.size(); ++i)
		{
			if (motions[i].name == key) return static_cast<int>(i);
		}
		for (size_t i = 0; i < motions.size(); ++i)
		{
			if (motions[i].title == key) return static_cast<int>(i);
		}
		// 大小写不敏感回退（Windows 文件系统习惯）
		auto lower = [](std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		};
		std::string lk = lower(key);
		for (size_t i = 0; i < motions.size(); ++i)
		{
			if (lower(motions[i].name) == lk) return static_cast<int>(i);
			if (lower(motions[i].title) == lk) return static_cast<int>(i);
		}
		for (size_t i = 0; i < motions.size(); ++i)
		{
			if (lower(motions[i].name).find(lk) != std::string::npos) return static_cast<int>(i);
		}
		return -1;
	}

	// -----------------------------------------------------------------------------
	// 3. 运行时控制配置
	// -----------------------------------------------------------------------------
	class RuntimeConfig
	{
	public:
		// ---- 位置与节奏 ----
		std::string path; // control.json 全路径
		std::string rootDir; // <localRoot>/GakuMotion
		float pollIntervalSeconds{0.5f}; // 轮询间隔
		float pollInterval{0.5f}; // 兼容别名（外部可直接改）

		// ---- 播放控制 ----
		// 全局播放倍速（默认 1.0x），与每个文件的速度相乘。
		// 音频有效速度非 1 时静音并用本地时钟；配置兼容范围 [0.1, 5.0]。
		float playbackSpeedMultiplier{1.0f};
		CameraSettings camera;
		bool cameraDeclared{false};

		// ---- 声明式状态（-1 / 空 表示「未设置」，不覆盖运行时既有值） ----
		std::string selectedMotion;
		// 兼容旧配置键；产品运行时始终使用 C# NativeAOT，不启动 Python。
		std::string importBackend{"csharp"};
		bool autoPlayOnSelect{false};
		int gazeMode{-1}; // 0=Auto 1=ForceOff 2=DesktopOn
		int desktopLookAtEnabled{-1}; // 三态
		float eyeWeight{-1.0f};
		float headWeight{-1.0f};
		float bodyWeight{-1.0f};
		int useRestRotations{-1}; // 三态
		// gkclip 腿部静息基准开关（2026-10-07 A/B 实验用，见 GkIdolController 调用点）。
		// 三态：-1 未设置（走默认值 false）、0 关、1 开。
		int legRestBasis{-1};
		// gkclip 眼骨按角色实测几何响应缩放（#150 §8）。三态：-1 未设置（默认开）、
		// 0 关（raw 1:1 对照）、1 开。这是眼骨自身的角度缩放，不是 LookAt、也不是晚写。
		int eyePoseRelativeGain{-1};
		// Opt-in experiment: preserve the Animator graph for native hair/cloth jobs.
		// Missing keeps the existing runtime setting; default remains legacy mute.
		int preserveAnimatorForSecondaryPhysics{-1};
		// Native is preferred by default. It requires the optional
		// companion DLL and otherwise uses fallback; no private ABI lives here.
		int secondaryPhysicsBackend{2}; // 0=off, 1=fallback, 2=native
		bool beforeQuartzExperiment{false}; // Explicit opt-in; omission returns to OFF.
		bool sleeveMuscleDiagnostics{false}; // Bounded read-only native-reader samples.
		int overridesEnabled{-1}; // 三态
		int auxMode{-1}; // 三态：辅助骨驱动模式 0=normal 1=silence 2=identity 3=replay
		int experimentalWaistSupport{1}; // default-on Waist_H geometry; missing preserves session state
		int muteNativeSoundEffects{-1}; // 三态：-1 未设置(默认开启), 0 关, 1 开
		std::string overrideGlobalFile; // 可选自定义全局覆盖文件
		std::string overrideMotionFile; // 可选自定义曲目覆盖文件

		// ---- 面部表情默认基线与动态让位 ----
		int facialEnableBaseline{-1}; // 三态：-1 未设置(默认开启), 0 关, 1 开
		int facialMikuMouthFit{-1}; // Missing preserves session; fresh controller/config default is on.
		int facialMikuEyeFit{-1}; // Published eye/brow profile; missing preserves session, fresh default is on.
		int facialMikuFitAllowUnverified{-1}; // 0 refuses every component the bundle did not accept.
		float facialBaselineMultiplier{-1.0f}; // 默认 1.0f (基线强度倍率)
		std::string facialCharacterOverride{"auto"}; // Legacy field: identity always comes from the bound actor.

		// ---- 一次性命令（由 command.seq 去重） ----
		bool cmdPlay{false};
		bool cmdPause{false};
		bool cmdStop{false};
		bool cmdSeek{false};
		float cmdSeekSeconds{0.0f};
		bool cmdReloadOverrides{false};
		bool cmdToggleOverrides{false};

		struct SolePitchCheckCommand
		{
			float initialYM{0}, planeOffsetM{0}, maxPitchDegrees{20};
			bool enabled{false}, hasPlane{false}, useMeshEnvelope{false};
		} cmdSolePitchSettings;

		bool cmdSolePitchCheck{false}, cmdSolePitchSnapshot{false};
		bool cmdBurstTrace{false};

		// ---- 状态与诊断 ----
		bool fileFound{false};
		bool loadedOnce{false};
		std::string lastError;
		std::string lastChangeLog;
		FileSignature signature;
		long long appliedCommandSeq{0};
		long long lastSeenCommandSeq{0};
		uint64_t revision{0};

		bool HasPendingCommands() const noexcept
		{
			return cmdPlay || cmdPause || cmdStop || cmdSeek ||
				cmdReloadOverrides || cmdToggleOverrides || cmdSolePitchCheck || cmdSolePitchSnapshot || cmdBurstTrace;
		}

		// 命令消费后必须调用：一次性命令只允许执行一次
		void ClearCommands() noexcept
		{
			cmdPlay = cmdPause = cmdStop = cmdSeek = false;
			cmdReloadOverrides = cmdToggleOverrides = false;
			cmdSolePitchCheck = cmdSolePitchSnapshot = false;
			cmdBurstTrace = false;
			cmdSeekSeconds = 0.0f;
		}

		void ForceReload() noexcept { forceReload_ = true; }
		bool ReloadRequested() const noexcept { return forceReload_; }

		// ---- 模板生成：首次运行自动落盘，便于用户直接编辑 ----
		static std::string DefaultTemplate()
		{
			return
				"// GakuMotion 运行时控制文件（热加载，改动后约 0.5 秒生效）\n"
				"// ---------------------------------------------------------------\n"
				"// 声明式字段：写什么就是什么，持续生效。\n"
				"// 一次性命令：command.seq 必须比上一次更大才会执行。\n"
				"{\n"
				"  \"poll_interval_seconds\": 0.5,\n"
				"  \"playback_speed_multiplier\": 1.0,   // 播放速度倍率 [0.1, 5.0]；有音频时以音频为主时钟保持同步\n"
				"\n"
				"  \"selected_motion\": \"\",   // 留空 = 启动时不选曲；填入曲库内的曲目名可自动选中\n"
				"  \"import_backend\": \"csharp\",      // VMD 导入后端：csharp（原生导入，需配套 DLL）\n"
				"  \"autoplay\": false,\n"
				"  \"camera\": {\n"
				"    \"enabled\": true, \"mode\": 0, \"follow_behavior\": 1, \"follow_actor\": false,\n"
				"    \"follow_target\": \"hips\", // Follow mode target: hips, chest or head\n"
				"    \"limit_to_scene\": false,\n"
				"    \"bounds_min\": [-5, 0.05, -5], \"bounds_max\": [5, 5, 5],\n"
				"    \"follow_offset\": [0, 0.6, 3], \"target_offset\": [0, 0.4, 0], \"vmd_offset\": [0, 0, 0],\n"
				"    \"follow_factor\": [1.0, 1.0, 1.0], // Captured camera-local displacement fractions\n"
				"    \"follow_rate\": [0.1, 0.1, 0.1], // 0: hold, 0.1: smooth pursuit, 1: immediate\n"
				"    \"follow_fov\": 45, \"vmd_range_scale\": 1.0,\n"
				"    // 可选分模式镜头位姿：\"transform_track\", \"transform_follow_move\", \"transform_look_at\": {\"pos\": [x,y,z], \"rot\": [rx,ry,rz]}\n"
				"    \"use_per_mode_transforms\": false\n"
				"  },\n"
				"\n"
				"  \"gaze\": {\n"
				"    \"mode\": 0,                 // 0=Auto(VR 时避让 VRify) 1=ForceOff(纯 VMD 头动) 2=DesktopOn\n"
				"    \"desktop_enabled\": true,\n"
				"    \"eye_weight\": 0.7,\n"
				"    \"head_weight\": 0.4,\n"
				"    \"body_weight\": 0.0\n"
				"  },\n"
				"\n"
				"  \"use_rest_rotations\": true,\n"
				"  // gkclip 眼骨：按角色实测眼区响应比缩放（#150 §8）；false = raw 1:1 对照\n"
				"  \"eye_pose_relative_gain\": true,\n"
				"  // Experimental: keep the native animation graph for hair/cloth; needs device acceptance.\n"
				"  \"secondary_physics\": { \"backend\": \"native\", \"preserve_animator\": false, \"before_quartz_experiment\": false, \"sleeve_muscle_diagnostics\": false },\n"
				"  \"experimental_waist_support\": { \"enabled\": true },\n"
				"  // 可选：\"native_audio\": { \"mute_sound_effects\": true }（默认开启；界面切换持久化于 native_audio.json）\n"
				"\n"
				"  // 面部默认表情基线与动态让位（Ducking）：\n"
				"  //   enable_baseline: 无明确表情时使用角色官方默认；明确开口时让位（默认 true）\n"
				"  //   baseline_multiplier: 默认表情强度倍率 [0.0, 2.0]（默认 1.0）\n"
				"  //   角色身份由实际绑定对象自动识别，旧 character 覆盖字段不再生效。\n"
				"  \"facial\": {\n"
				"    \"enable_baseline\": true,\n"
				"    \"baseline_multiplier\": 1.0,\n"
				"    \"miku_mouth_fit\": true,\n"
				"    \"miku_eye_fit\": true,\n"
				"    \"miku_fit_allow_unverified\": false\n"
				"  },\n"
				"\n"
				"  // 辅助骨（_Roll_H / _H）驱动模式 —— 实机靶向对照实验\n"
				"  //   0 或 \"normal\"   = 现行 Quartz 半角/半扭转注入（默认）\n"
				"  //   1 或 \"silence\"  = 硬锁初始绑定局部姿态（绝对静默，刚性随父跟动）\n"
				"  //   2 或 \"identity\" = 局部旋转强制归零\n"
				"  //   3 或 \"replay\"   = 直灌官方录制真值（读 <曲目>/aux_replay.json）\n"
				"  \"aux\": { \"mode\": 0 },\n"
				"\n"
				"  \"overrides\": {\n"
				"    \"enabled\": true,\n"
				"    \"global_file\": \"GakuMotion/bone_overrides.json\",\n"
				"    \"motion_file\": \"\"         // 留空 = 使用 <曲目目录>/bone_overrides.json\n"
				"  },\n"
				"\n"
				"  \"command\": {\n"
				"    \"seq\": 0,                  // 每次要执行命令时请 +1\n"
				"    \"play\": false,\n"
				"    \"pause\": false,\n"
				"    \"stop\": false,\n"
				"    \"seek_seconds\": -1,        // >=0 时生效\n"
				"    \"reload_overrides\": false,\n"
				"    \"toggle_overrides\": false\n"
				"  }\n"
				"}\n";
		}

		// 首次运行时生成模板；已存在则不动
		bool EnsureFileExists()
		{
			std::error_code ec;
			if (path.empty()) return false;
			if (std::filesystem::exists(MotionPathFromUtf8(path), ec))
			{
				fileFound = true;
				return false;
			}
			std::filesystem::create_directories(MotionPathFromUtf8(path).parent_path(), ec);
			std::ofstream out(MotionPathFromUtf8(path), std::ios::binary);
			if (!out.is_open())
			{
				lastError = "cannot create control file: " + path;
				return false;
			}
			out << DefaultTemplate();
			out.close();
			fileFound = true;
			return true;
		}

		// -------------------------------------------------------------------------
		// 轮询：低频检查文件签名，变化时重新解析
		// forceReload = true 时立即重读（快捷键 F11 等场景）
		// -------------------------------------------------------------------------
		bool Poll(float deltaTime, bool forceReload = false)
		{
			pollInterval = (pollIntervalSeconds > 0.05f) ? pollIntervalSeconds : 0.05f;

			bool doReload = forceReload || forceReload_;
			forceReload_ = false;

			if (!doReload)
			{
				timer_ += (deltaTime > 0.0f) ? deltaTime : 0.0167f;
				if (timer_ < pollInterval) return false;
			}
			timer_ = 0.0f;

			if (path.empty()) return false;

			FileSignature current = StatFile(path);
			if (!current.exists)
			{
				if (fileFound || loadedOnce)
				{
					lastError = "control file disappeared: " + path;
					fileFound = false;
				}
				return false;
			}
			fileFound = true;

			if (!doReload && loadedOnce && current == signature)
			{
				return false;
			}

			signature = current;
			loadedOnce = true;
			return Reload();
		}

		// -------------------------------------------------------------------------
		// 立即解析并应用声明式字段（命令块按 seq 去重后挂起到 HasPendingCommands）
		// -------------------------------------------------------------------------
		bool Reload()
		{
			jsonlite::Value root;
			std::string err;
			if (!jsonlite::ParseFile(path, root, err))
			{
				lastError = err;
				return false;
			}
			if (!root.IsObject())
			{
				lastError = path + " -> top-level must be an object";
				return false;
			}

			// ---- 变更检测快照 ----
			const std::string prevMotion = selectedMotion;
			const int prevGaze = gazeMode;
			const int prevDesktop = desktopLookAtEnabled;
			const float prevEye = eyeWeight;
			const float prevHead = headWeight;
			const float prevBody = bodyWeight;
			const int prevLegBasis = legRestBasis;
			const int prevEyePoseGain = eyePoseRelativeGain;
			const int prevPreserveAnimator = preserveAnimatorForSecondaryPhysics;
			const int prevPhysicsBackend = secondaryPhysicsBackend;
			const bool prevBeforeQuartz = beforeQuartzExperiment;
			const bool prevSleeveDiagnostics = sleeveMuscleDiagnostics;
			const int prevRest = useRestRotations;
			const int prevOverrides = overridesEnabled;
			const int prevAuxMode = auxMode;
			const std::string prevGlobalFile = overrideGlobalFile;
			const std::string prevMotionFile = overrideMotionFile;
			const float prevPlaybackSpeed = playbackSpeedMultiplier;

			// ---- 声明式字段 ----
			if (const auto* c = root.Find("camera"))
			{
				CameraSettings next = camera;
				auto vec = [&](const char* key, Vector3& out)
				{
					if (const auto* a = c->Find(key))
					{
						if (!a->IsArray() || a->Size() != 3) return false;
						Vector3 v;
						float* fields[] = {&v.x, &v.y, &v.z};
						for (size_t i = 0; i < 3; ++i)
						{
							if (!a->At(i)->IsNumber()) return false;
							*fields[i] = a->At(i)->AsFloat(0);
						}
						if (!CameraFinite(v)) return false;
						out = v;
					}
					return true;
				};
				if (!c->IsObject() || !vec("bounds_min", next.boundsMin) || !vec("bounds_max", next.boundsMax) ||
					!vec("follow_offset", next.followOffset) || !vec("target_offset", next.targetOffset) ||
					!vec("vmd_offset", next.vmdOffset) || !vec("vmd_initial_offset", next.vmdOffset) ||
					!vec("follow_factor", next.followFactor) || !vec("follow_rate", next.followRate))
				{
					lastError = "camera vectors must contain three finite numbers";
					return false;
				}
				if (const auto* target = c->Find("follow_target"))
				{
					if (!target->IsString())
					{
						lastError = "camera follow_target must be hips, chest or head";
						return false;
					}
					if (target->stringValue == "hips") next.followTarget = CameraFollowTarget::Hips;
					else if (target->stringValue == "chest") next.followTarget = CameraFollowTarget::Chest;
					else if (target->stringValue == "head") next.followTarget = CameraFollowTarget::Head;
					else
					{
						lastError = "camera follow_target must be hips, chest or head";
						return false;
					}
				}

				if (!next.usePerModeTransforms)
				{
					next.transformTrack.position = next.vmdOffset;
					next.transformFollowMove.position = next.followOffset;
					next.transformLookAt.position = next.followOffset;
				}

				bool anyTransformFound = false;
				auto parseTransform = [&](const char* key, CameraModeTransform& out, bool& found) -> bool
				{
					const auto* obj = c->Find(key);
					if (!obj) return true;
					if (!obj->IsObject())
					{
						lastError = std::string("camera.") + key + " must be an object";
						return false;
					}
					auto readVec3 = [&](const char* subKey, Vector3& v) -> bool
					{
						const auto* a = obj->Find(subKey);
						if (!a || !a->IsArray() || a->Size() != 3) return false;
						for (size_t i = 0; i < 3; ++i)
						{
							if (!a->At(i)->IsNumber()) return false;
						}
						v.x = a->At(0)->AsFloat(0.0f);
						v.y = a->At(1)->AsFloat(0.0f);
						v.z = a->At(2)->AsFloat(0.0f);
						return CameraFinite(v);
					};
					CameraModeTransform parsed = out;
					if (!readVec3("pos", parsed.position) || !readVec3("rot", parsed.rotation))
					{
						lastError = std::string("camera.") + key + " pos and rot must contain 3 finite numbers";
						return false;
					}
					if (!parsed.IsValid())
					{
						lastError = std::string("invalid transform values in camera.") + key;
						return false;
					}
					out = parsed;
					found = true;
					anyTransformFound = true;
					return true;
				};

				bool foundTrack = false, foundFollowMove = false, foundLookAt = false;
				if (!parseTransform("transform_track", next.transformTrack, foundTrack) ||
				    !parseTransform("transform_follow_move", next.transformFollowMove, foundFollowMove) ||
				    !parseTransform("transform_look_at", next.transformLookAt, foundLookAt))
				{
					return false;
				}
				if (anyTransformFound)
				{
					next.usePerModeTransforms = true;
				}
				next.usePerModeTransforms = c->BoolPath("use_per_mode_transforms", next.usePerModeTransforms);

				next.enabled = c->BoolPath("enabled", next.enabled);
				int mode = c->IntPath("mode", static_cast<int>(next.mode));
				int followBehavior = c->IntPath("follow_behavior", static_cast<int>(next.followBehavior));
				if (const auto* fsi = c->Find("follow_slot_index"))
				{
					if (!fsi->IsNumber()) { lastError = "camera follow_slot_index must be an integer"; return false; }
					const double d = fsi->numberValue;
					if (!std::isfinite(d) || d != std::floor(d) || d < -1.0 || d > 1000000.0)
					{
						lastError = "camera follow_slot_index must be >= -1";
						return false;
					}
					next.followSlotIndex = static_cast<int>(d);
				}
				next.followActor = c->BoolPath("follow_actor", next.followActor);
				next.limitToScene = c->BoolPath("limit_to_scene", next.limitToScene);
				next.followFov = c->FloatPath("follow_fov", next.followFov);
				if (const auto* scale = c->Find("vmd_range_scale"))
				{
					if (!scale->IsNumber()) { lastError = "camera vmd_range_scale must be a number"; return false; }
					next.vmdRangeScale = scale->AsFloat(0);
				}
				next.srpFinalOverwriteFallback = c->BoolPath("srp_final_overwrite_fallback", next.srpFinalOverwriteFallback);
				next.actorShadowExperimentMode = c->IntPath("actor_shadow_experiment_mode", next.actorShadowExperimentMode);
				if (next.actorShadowExperimentMode < 0 || next.actorShadowExperimentMode > 2) next.actorShadowExperimentMode = 0;
				if (mode < 0 || mode > 2 || followBehavior < 0 || followBehavior > 1 ||
					next.boundsMin.x > next.boundsMax.x || next.boundsMin.y > next.boundsMax.y
					||
					next.boundsMin.z > next.boundsMax.z || !CameraFinite(next.vmdOffset) || !CameraFinite(next.followFactor) ||
					next.followFactor.x < 0 || next.followFactor.x > 1.0f ||
					next.followFactor.y < 0 || next.followFactor.y > 1.0f ||
					next.followFactor.z < 0 || next.followFactor.z > 1.0f || !CameraFinite(next.followRate) ||
					next.followRate.x < 0 || next.followRate.x > 1.0f ||
					next.followRate.y < 0 || next.followRate.y > 1.0f ||
					next.followRate.z < 0 || next.followRate.z > 1.0f || !std::isfinite(next.followFov) ||
					next.followFov < 1 || next.followFov > 179 || !std::isfinite(next.vmdRangeScale) ||
					next.vmdRangeScale < 0.3f || next.vmdRangeScale > 1.0f ||
					!next.transformTrack.IsValid() || !next.transformFollowMove.IsValid() || !next.transformLookAt.IsValid())
				{
					lastError = "invalid camera mode, bounds, follow factor, follow rate, FOV or VMD range scale";
					return false;
				}
				// Hold is retired from user camera selection. Preserve its numeric
				// ABI slot, but migrate old files to the VMD mode on load.
				next.mode = mode == static_cast<int>(CameraMode::Hold) ? CameraMode::Track : static_cast<CameraMode>(mode);
				next.followBehavior = static_cast<FollowBehavior>(followBehavior);
				camera = next;
				cameraDeclared = true;
			}
			pollIntervalSeconds = root.FloatPath("poll_interval_seconds", pollIntervalSeconds);
			selectedMotion = root.StringPath("selected_motion", selectedMotion);
			autoPlayOnSelect = root.BoolPath("autoplay", autoPlayOnSelect);
			{
				// 旧 python/cs 配置均迁移到唯一的原生产品后端。
				importBackend = "csharp";
			}

			// 播放速度倍率（方案 C）：夹取到 [0.1, 5.0]，避免手写越界值导致时钟异常
			if (const jsonlite::Value* v = root.Find("playback_speed_multiplier"))
			{
				playbackSpeedMultiplier = v->AsFloat(playbackSpeedMultiplier);
				if (playbackSpeedMultiplier < 0.1f) playbackSpeedMultiplier = 0.1f;
				if (playbackSpeedMultiplier > 5.0f) playbackSpeedMultiplier = 5.0f;
			}

			if (const jsonlite::Value* gaze = root.Find("gaze"))
			{
				gazeMode = gaze->IntPath("mode", gazeMode);
				if (const jsonlite::Value* d = gaze->Find("desktop_enabled"))
				{
					desktopLookAtEnabled = d->AsBool(true) ? 1 : 0;
				}
				if (const jsonlite::Value* w = gaze->Find("eye_weight")) eyeWeight = w->AsFloat(eyeWeight);
				if (const jsonlite::Value* w = gaze->Find("head_weight")) headWeight = w->AsFloat(headWeight);
				if (const jsonlite::Value* w = gaze->Find("body_weight")) bodyWeight = w->AsFloat(bodyWeight);
			}
			if (const jsonlite::Value* v = root.Find("leg_rest_basis"))
			{
				legRestBasis = v->AsBool(false) ? 1 : 0;
				eyePoseRelativeGain = v->AsBool(true) ? 1 : 0;
			}
			if (const jsonlite::Value* v = root.Find("use_rest_rotations"))
			{
				useRestRotations = v->AsBool(true) ? 1 : 0;
			}
			if (const jsonlite::Value* v = root.FindPath("secondary_physics.preserve_animator"))
			{
				if (v->IsBool()) preserveAnimatorForSecondaryPhysics = v->AsBool(false) ? 1 : 0;
			}
			if (const jsonlite::Value* v = root.FindPath("experimental_waist_support.enabled"))
			{
				if (v->IsBool()) experimentalWaistSupport = v->AsBool(false) ? 1 : 0;
			}
			if (const jsonlite::Value* v = root.FindPath("native_audio.mute_sound_effects"))
			{
				if (v->IsBool()) muteNativeSoundEffects = v->AsBool(true) ? 1 : 0;
			}
			if (const jsonlite::Value* v = root.FindPath("secondary_physics.backend"))
			{
				if (v->IsString())
				{
					if (v->stringValue == "off") secondaryPhysicsBackend = 0;
					else if (v->stringValue == "fallback") secondaryPhysicsBackend = 1;
					else if (v->stringValue == "native") secondaryPhysicsBackend = 2;
				}
			}
			beforeQuartzExperiment = false;
			sleeveMuscleDiagnostics = false;
			if (const auto* v = root.FindPath("secondary_physics.before_quartz_experiment"); v && v->IsBool())
				beforeQuartzExperiment = v->boolValue;
			if (const auto* v = root.FindPath("secondary_physics.sleeve_muscle_diagnostics"); v && v->IsBool())
				sleeveMuscleDiagnostics = v->boolValue;
			if (const jsonlite::Value* ov = root.Find("overrides"))
			{
				if (const jsonlite::Value* e = ov->Find("enabled")) overridesEnabled = e->AsBool(true) ? 1 : 0;
				overrideGlobalFile = ov->StringPath("global_file", overrideGlobalFile);
				overrideMotionFile = ov->StringPath("motion_file", overrideMotionFile);
			}

			// 面部表情默认基线与动态让位
			if (const jsonlite::Value* facial = root.Find("facial"))
			{
				if (const jsonlite::Value* fitted = facial->Find("miku_mouth_fit"))
					facialMikuMouthFit = fitted->AsBool(false) ? 1 : 0;
				if (const jsonlite::Value* fitted = facial->Find("miku_eye_fit"))
					facialMikuEyeFit = fitted->AsBool(false) ? 1 : 0;
				if (const jsonlite::Value* allow = facial->Find("miku_fit_allow_unverified"))
					facialMikuFitAllowUnverified = allow->AsBool(false) ? 1 : 0;
				if (const jsonlite::Value* e = facial->Find("enable_baseline"))
				{
					facialEnableBaseline = e->AsBool(true) ? 1 : 0;
				}
				if (const jsonlite::Value* m = facial->Find("baseline_multiplier"))
				{
					facialBaselineMultiplier = m->AsFloat(facialBaselineMultiplier);
				}
				// Ignore legacy manual identity overrides, including those saved by older UIs.
				facialCharacterOverride = "auto";
			}

			// 辅助骨驱动模式（实机靶向对照实验）
			//   0/normal   = 现行 Quartz 半角 / 半扭转注入
			//   1/silence  = 硬锁初始绑定局部姿态（绝对静默）
			//   2/identity = 局部旋转强制归零
			//   3/replay   = 直灌官方录制真值（读 <曲目>/aux_replay.json）
			if (const jsonlite::Value* v = root.Find("aux"))
			{
				if (const jsonlite::Value* m = v->Find("mode")) auxMode = ParseAuxMode(m);
			}
			if (const jsonlite::Value* v = root.Find("aux_mode")) auxMode = ParseAuxMode(v);

			// 数值合法性夹取（避免手写文件里的越界值造成异常姿态）
			if (eyeWeight >= 0.0f) eyeWeight = Clamp01(eyeWeight);
			if (headWeight >= 0.0f) headWeight = Clamp01(headWeight);
			if (bodyWeight >= 0.0f) bodyWeight = Clamp01(bodyWeight);
			if (gazeMode >= 0) gazeMode = (gazeMode > 2) ? 2 : gazeMode;

			// ---- 一次性命令（seq 去重） ----
			bool seqReported = false;
			std::string commandError;
			if (const jsonlite::Value* cmd = root.Find("command"))
			{
				long long seq = 0;
				if (const jsonlite::Value* s = cmd->Find("seq"))
				{
					if (s->IsNumber()) seq = static_cast<long long>(s->numberValue);
					else if (s->IsString()) seq = std::strtoll(s->stringValue.c_str(), nullptr, 10);
				}
				lastSeenCommandSeq = seq;

				if (seq > appliedCommandSeq)
				{
					// Validate the entire one-shot experiment before publishing any
					// command flags or consuming its sequence. Omission is inert.
					SolePitchCheckCommand soleCommand;
					const auto* soleCheck = cmd->Find("sole_pitch_check");
					const auto* snapshot = cmd->Find("sole_pitch_snapshot");
					auto validateSoleCommand = [&]()
					{
						if (soleCheck)
						{
							const auto* y = soleCheck->Find("initial_y_m");
							const auto* enabled = soleCheck->Find("enabled");
							auto number = [](const jsonlite::Value* v, float& out, float limit, float minimum)
							{
								if (!v || !v->IsNumber() || !std::isfinite(v->numberValue) ||
									v->numberValue < minimum || v->numberValue > limit)
									return false;
								out = static_cast<float>(v->numberValue);
								return std::isfinite(out);
							};
							if (!soleCheck->IsObject() || !enabled || !enabled->IsBool() ||
								!number(y, soleCommand.initialYM, 2.f, -2.f))
							{
								commandError =
									"invalid command.sole_pitch_check: initial_y_m [-2,2] and boolean enabled required";
								return false;
							}
							soleCommand.enabled = enabled->boolValue;
							if (const auto* envelope = soleCheck->Find("use_mesh_envelope"))
							{
								if (!envelope->IsBool())
								{
									commandError =
										"invalid command.sole_pitch_check.use_mesh_envelope: boolean required";
									return false;
								}
								soleCommand.useMeshEnvelope = envelope->boolValue;
							}
							if (const auto* plane = soleCheck->Find("plane_offset_m"))
							{
								if (!number(plane, soleCommand.planeOffsetM, 100.f, -100.f))
								{
									commandError = "invalid command.sole_pitch_check.plane_offset_m [-100,100]";
									return false;
								}
								soleCommand.hasPlane = true;
							}
							if (const auto* angle = soleCheck->Find("max_pitch_degrees"))
							{
								if (!number(angle, soleCommand.maxPitchDegrees, 35.f, 0.f))
								{
									commandError = "invalid command.sole_pitch_check.max_pitch_degrees [0,35]";
									return false;
								}
							}
						}
						if (snapshot && !snapshot->IsBool())
						{
							commandError = "command.sole_pitch_snapshot must be boolean";
							return false;
						}
						return true;
					};
					if (!validateSoleCommand())
					{
						// Keep valid declarative edits and report the error on this
						// reload, without replaying any earlier pending command.
						ClearCommands();
					}
					else
					{
						appliedCommandSeq = seq;
						seqReported = true;

						cmdSolePitchCheck = soleCheck != nullptr;
						cmdSolePitchSettings = soleCommand;
						cmdSolePitchSnapshot = snapshot && snapshot->boolValue;

						cmdPlay = cmd->BoolPath("play", false);
						cmdPause = cmd->BoolPath("pause", false);
						cmdStop = cmd->BoolPath("stop", false);
						cmdReloadOverrides = cmd->BoolPath("reload_overrides", false);
						cmdToggleOverrides = cmd->BoolPath("toggle_overrides", false);
						cmdBurstTrace = cmd->BoolPath("burst_trace", false);

						float seek = cmd->FloatPath("seek_seconds", -1.0f);
						cmdSeek = (seek >= 0.0f);
						cmdSeekSeconds = cmdSeek ? seek : 0.0f;
					}
				}
			}

			// ---- 变更日志 ----
			std::string log;
			AppendChange(log, "selected_motion", prevMotion, selectedMotion);
			AppendChange(log, "gaze.mode", prevGaze, gazeMode);
			AppendChange(log, "gaze.desktop_enabled", prevDesktop, desktopLookAtEnabled);
			AppendChange(log, "gaze.eye_weight", prevEye, eyeWeight);
			AppendChange(log, "gaze.head_weight", prevHead, headWeight);
			AppendChange(log, "gaze.body_weight", prevBody, bodyWeight);
			AppendChange(log, "leg_rest_basis", prevLegBasis, legRestBasis);
			AppendChange(log, "eye_pose_relative_gain", prevEyePoseGain, eyePoseRelativeGain);
			AppendChange(log, "secondary_physics.preserve_animator", prevPreserveAnimator,
			             preserveAnimatorForSecondaryPhysics);
			AppendChange(log, "secondary_physics.backend", prevPhysicsBackend, secondaryPhysicsBackend);
			AppendChange(log, "secondary_physics.before_quartz_experiment", prevBeforeQuartz, beforeQuartzExperiment);
			AppendChange(log, "secondary_physics.sleeve_muscle_diagnostics", prevSleeveDiagnostics, sleeveMuscleDiagnostics);
			AppendChange(log, "use_rest_rotations", prevRest, useRestRotations);
			AppendChange(log, "aux.mode", prevAuxMode, auxMode);
			AppendChange(log, "overrides.enabled", prevOverrides, overridesEnabled);
			AppendChange(log, "overrides.global_file", prevGlobalFile, overrideGlobalFile);
			AppendChange(log, "overrides.motion_file", prevMotionFile, overrideMotionFile);
			AppendChange(log, "playback_speed_multiplier", prevPlaybackSpeed, playbackSpeedMultiplier);
			if (seqReported)
			{
				if (!log.empty()) log += " | ";
				log += "command.seq=" + std::to_string(appliedCommandSeq);
			}
			lastChangeLog = log;
			lastError = commandError;
			++revision;
			return true;
		}

	private:
		// "normal"/"silence"/"identity"/"replay" <-> 0/1/2/3；无法识别时返回 -1（表示不改动）
		static int ParseAuxMode(const jsonlite::Value* v) noexcept
		{
			if (!v) return -1;
			if (v->IsNumber())
			{
				int n = v->AsInt(-1);
				return (n >= 0 && n <= 3) ? n : -1;
			}
			if (v->IsBool()) return v->AsBool(true) ? 0 : 1;
			if (!v->IsString()) return -1;
			const std::string& s = v->stringValue;
			if (s == "normal" || s == "quartz") return 0;
			if (s == "silence" || s == "rest" || s == "lock") return 1;
			if (s == "identity" || s == "zero") return 2;
			if (s == "replay" || s == "gt") return 3;
			return -1;
		}

		float timer_{0.0f};
		bool forceReload_{false};

		static float Clamp01(float v) noexcept
		{
			if (v < 0.0f) return 0.0f;
			if (v > 1.0f) return 1.0f;
			return v;
		}

		static void AppendChange(std::string& log, const char* name,
		                         const std::string& before, const std::string& after)
		{
			if (before == after) return;
			if (!log.empty()) log += " | ";
			log += std::string(name) + ":'" + before + "'->'" + after + "'";
		}

		static void AppendChange(std::string& log, const char* name, int before, int after)
		{
			if (before == after) return;
			if (!log.empty()) log += " | ";
			log += std::string(name) + ":" + std::to_string(before) + "->" + std::to_string(after);
		}

		static void AppendChange(std::string& log, const char* name, float before, float after)
		{
			if (before == after) return;
			if (!log.empty()) log += " | ";
			char buf[128]{};
			std::snprintf(buf, sizeof(buf), "%s:%.3f->%.3f", name, before, after);
			log += buf;
		}
	};

	inline void SerializeJsonValue(const jsonlite::Value& value, std::string& out, int indent = 0)
	{
		auto appendIndent = [&](int depth) {
			for (int i = 0; i < depth; ++i) out += "  ";
		};
		switch (value.type)
		{
		case jsonlite::Type::Null:
			out += "null";
			break;
		case jsonlite::Type::Bool:
			out += value.boolValue ? "true" : "false";
			break;
		case jsonlite::Type::Number:
		{
			if (!std::isfinite(value.numberValue))
			{
				out += "0";
			}
			else if (value.numberValue == std::floor(value.numberValue) && std::fabs(value.numberValue) < 9.0e15)
			{
				char buf[48]{};
				std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value.numberValue));
				out += buf;
			}
			else
			{
				char buf[48]{};
				std::snprintf(buf, sizeof(buf), "%.17g", value.numberValue);
				out += buf;
			}
			break;
		}
		case jsonlite::Type::String:
			out.push_back('"');
			out += jsonlite::EscapeString(value.stringValue);
			out.push_back('"');
			break;
		case jsonlite::Type::Array:
			if (value.arrayItems.empty())
			{
				out += "[]";
				break;
			}
			out += "[\n";
			for (size_t i = 0; i < value.arrayItems.size(); ++i)
			{
				appendIndent(indent + 1);
				SerializeJsonValue(value.arrayItems[i], out, indent + 1);
				if (i + 1 < value.arrayItems.size()) out.push_back(',');
				out.push_back('\n');
			}
			appendIndent(indent);
			out.push_back(']');
			break;
		case jsonlite::Type::Object:
			if (value.objectMembers.empty())
			{
				out += "{}";
				break;
			}
			out += "{\n";
			for (size_t i = 0; i < value.objectMembers.size(); ++i)
			{
				appendIndent(indent + 1);
				out.push_back('"');
				out += jsonlite::EscapeString(value.objectMembers[i].first);
				out += "\": ";
				SerializeJsonValue(value.objectMembers[i].second, out, indent + 1);
				if (i + 1 < value.objectMembers.size()) out.push_back(',');
				out.push_back('\n');
			}
			appendIndent(indent);
			out.push_back('}');
			break;
		}
	}

	inline bool DecodeCoverBase64(std::string_view input, std::vector<uint8_t>& outBytes)
	{
		outBytes.clear();
		if (input.empty()) return false;
		static const int kLookup[256] = {
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
			52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
			-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
			15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
			-1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
			41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
		};
		outBytes.reserve((input.size() / 4) * 3);
		uint32_t val = 0;
		int valb = -8;
		for (const unsigned char ch : input)
		{
			if (ch == '=') break;
			if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
			const int d = kLookup[ch];
			if (d == -1) { outBytes.clear(); return false; }
			val = (val << 6) + static_cast<uint32_t>(d);
			valb += 6;
			if (valb >= 0)
			{
				outBytes.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
				valb -= 8;
			}
		}
		return !outBytes.empty();
	}

	inline bool DecodeCoverDataUri(std::string_view uri, std::string& outMime, std::vector<uint8_t>& outBytes)
	{
		outMime.clear();
		outBytes.clear();
		constexpr std::string_view kPrefix = "data:";
		constexpr std::string_view kBase64Tag = ";base64,";
		if (uri.size() > 16ULL * 1024ULL * 1024ULL) return false;
		if (uri.rfind(kPrefix, 0) != 0) return false;
		const size_t marker = uri.find(kBase64Tag, kPrefix.size());
		if (marker == std::string_view::npos) return false;
		std::string mime(uri.substr(kPrefix.size(), marker - kPrefix.size()));
		for (char& ch : mime) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		if (mime != "image/png" && mime != "image/jpeg" && mime != "image/jpg" &&
			mime != "image/webp" && mime != "image/bmp" && mime != "image/gif")
		{
			return false;
		}
		if (!DecodeCoverBase64(uri.substr(marker + kBase64Tag.size()), outBytes)) return false;
		outMime = (mime == "image/jpg") ? "image/jpeg" : mime;
		return true;
	}

	inline void RemoveTrackCoverFiles(const std::filesystem::path& targetDir)
	{
		if (targetDir.empty()) return;
		std::error_code ec;
		for (const char* name : {"cover.jpg", "cover.jpeg", "cover.png", "cover.webp", "cover.bmp"})
		{
			std::filesystem::remove(targetDir / name, ec);
			ec.clear();
		}
	}

	inline bool WriteRawPngCoverAtomic(
		const std::vector<uint8_t>& encoded,
		const std::filesystem::path& targetDir,
		std::string& outFileName,
		std::string& outError)
	{
		outFileName.clear();
		outError.clear();
		if (encoded.empty() || targetDir.empty())
		{
			outError = "Cover bytes or target directory is empty";
			return false;
		}
		std::error_code ec;
		std::filesystem::create_directories(targetDir, ec);
		if (ec)
		{
			outError = "Cannot create track directory: " + ec.message();
			return false;
		}
		const auto tmpPath = targetDir / "cover.png.tmp";
		const auto finalPath = targetDir / "cover.png";
		{
			std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				outError = "Cannot open temporary cover file for writing";
				return false;
			}
			out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
			if (!out.good())
			{
				out.close();
				std::filesystem::remove(tmpPath, ec);
				outError = "Failed to write temporary cover file";
				return false;
			}
		}
		RemoveTrackCoverFiles(targetDir);
		std::filesystem::rename(tmpPath, finalPath, ec);
		if (ec)
		{
			std::filesystem::remove(tmpPath, ec);
			outError = "Failed to commit cover.png: " + ec.message();
			return false;
		}
		outFileName = "cover.png";
		return true;
	}

	inline bool WriteTrackInfoJsonAtomically(
		const std::filesystem::path& infoPath,
		const std::string& serialized,
		std::string& outError)
	{
		static std::atomic<uint64_t> sequence{0};
#ifdef _WIN32
		const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
		const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
		std::filesystem::path temp;
		bool created = false;
#ifdef _WIN32
		HANDLE file = INVALID_HANDLE_VALUE;
#else
		int file = -1;
#endif
		for (unsigned retry = 0; retry < 32 && !created; ++retry)
		{
			temp = infoPath;
			temp += ".tmp." + std::to_string(pid) + "." + std::to_string(++sequence);
#ifdef _WIN32
			file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			created = file != INVALID_HANDLE_VALUE;
			if (!created && GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
			{
				outError = "Cannot open info.json staging file: " + std::error_code(GetLastError(), std::system_category()).message();
				return false;
			}
#else
			file = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
			created = file >= 0;
			if (!created && errno != EEXIST)
			{
				outError = "Cannot open info.json staging file: " + std::error_code(errno, std::generic_category()).message();
				return false;
			}
#endif
		}
		if (!created)
		{
			outError = "Cannot create unique info.json staging file";
			return false;
		}
		auto cleanup = [&] {
			std::error_code ignored;
			std::filesystem::remove(temp, ignored);
		};
		bool written = true;
#ifdef _WIN32
		for (size_t offset = 0; offset < serialized.size();)
		{
			DWORD count = 0;
			const auto chunk = static_cast<DWORD>((std::min)(serialized.size() - offset, size_t{0x40000000}));
			if (!WriteFile(file, serialized.data() + offset, chunk, &count, nullptr) || count != chunk)
			{
				outError = "Failed to write info.json staging file: " + std::error_code(GetLastError(), std::system_category()).message();
				written = false;
				break;
			}
			offset += count;
		}
		if (written && !FlushFileBuffers(file))
		{
			outError = "Failed to flush info.json staging file: " + std::error_code(GetLastError(), std::system_category()).message();
			written = false;
		}
		if (!CloseHandle(file))
		{
			if (written)
				outError = "Failed to close info.json staging file: " + std::error_code(GetLastError(), std::system_category()).message();
			written = false;
		}
		if (!written)
		{
			cleanup();
			return false;
		}
		bool moved = false;
		DWORD moveErr = 0;
		{
			std::unique_lock<std::shared_mutex> commitWriteLock(jsonlite::JsonFileCommitMutex());
			for (int attempt = 0; attempt < 10; ++attempt)
			{
				if (MoveFileExW(temp.c_str(), infoPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				{
					moved = true;
					break;
				}
				moveErr = GetLastError();
				if ((moveErr != ERROR_ACCESS_DENIED && moveErr != ERROR_SHARING_VIOLATION) || attempt == 9)
				{
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}
		if (!moved)
		{
			outError = "Failed to commit info.json: " + std::error_code(static_cast<int>(moveErr), std::system_category()).message();
			cleanup();
			return false;
		}
#else
		for (size_t offset = 0; offset < serialized.size();)
		{
			const auto count = ::write(file, serialized.data() + offset, serialized.size() - offset);
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0)
			{
				outError = "Failed to write info.json staging file: " + std::error_code(errno, std::generic_category()).message();
				written = false;
				break;
			}
			offset += static_cast<size_t>(count);
		}
		if (written && ::fsync(file) != 0)
		{
			outError = "Failed to flush info.json staging file: " + std::error_code(errno, std::generic_category()).message();
			written = false;
		}
		if (::close(file) != 0)
		{
			if (written)
				outError = "Failed to close info.json staging file: " + std::error_code(errno, std::generic_category()).message();
			written = false;
		}
		if (!written)
		{
			cleanup();
			return false;
		}
		std::error_code ec;
		std::filesystem::rename(temp, infoPath, ec);
		if (ec)
		{
			cleanup();
			outError = "Failed to commit info.json: " + ec.message();
			return false;
		}
#endif
		return true;
	}

	inline bool UpdateTrackInfoJsonCover(
		const std::filesystem::path& trackDir,
		bool hasCover,
		const std::string& coverFileName,
		std::string& outError)
	{
		outError.clear();
		if (trackDir.empty())
		{
			outError = "Track directory is empty";
			return false;
		}
		std::lock_guard<std::mutex> lock(InfoJsonMutationMutex());
		const auto infoPath = trackDir / "info.json";
		jsonlite::Value root;
		root.type = jsonlite::Type::Object;
		std::error_code ec;
		if (std::filesystem::is_regular_file(infoPath, ec))
		{
			std::string rawText;
			std::string readErr;
			if (!ReadTrackInfoJsonTextUnlocked(infoPath, rawText, &readErr))
			{
				outError = "Cannot open info.json for reading" + (readErr.empty() ? std::string() : ": " + readErr);
				return false;
			}
			std::string parseErr;
			jsonlite::Value parsed;
			if (jsonlite::ParseText(rawText, parsed, parseErr) && parsed.IsObject())
			{
				root = std::move(parsed);
			}
			else if (!rawText.empty())
			{
				outError = "info.json is malformed: " + parseErr;
				return false;
			}
		}

		auto setOrReplace = [&](const std::string& key, jsonlite::Value val) {
			bool updated = false;
			std::vector<std::pair<std::string, jsonlite::Value>> next;
			next.reserve(root.objectMembers.size() + 1);
			for (auto& kv : root.objectMembers)
			{
				if (kv.first == key)
				{
					if (!updated)
					{
						next.emplace_back(key, std::move(val));
						updated = true;
					}
				}
				else
				{
					next.push_back(std::move(kv));
				}
			}
			if (!updated) next.emplace_back(key, std::move(val));
			root.objectMembers = std::move(next);
		};
		auto removeKey = [&](const std::string& key) {
			root.objectMembers.erase(
				std::remove_if(root.objectMembers.begin(), root.objectMembers.end(),
					[&](const auto& kv) { return kv.first == key; }),
				root.objectMembers.end());
		};

		jsonlite::Value hasCoverVal;
		hasCoverVal.type = jsonlite::Type::Bool;
		hasCoverVal.boolValue = hasCover;
		setOrReplace("has_cover", std::move(hasCoverVal));

		if (hasCover && !coverFileName.empty())
		{
			jsonlite::Value coverFileVal;
			coverFileVal.type = jsonlite::Type::String;
			coverFileVal.stringValue = coverFileName;
			setOrReplace("cover_file", std::move(coverFileVal));
		}
		else
		{
			removeKey("cover_file");
		}

		std::string serialized;
		SerializeJsonValue(root, serialized, 0);
		serialized.push_back('\n');

		return WriteTrackInfoJsonAtomically(infoPath, serialized, outError);
	}

	inline bool UpdateTrackInfoJsonPositionsPlacement(
		const std::filesystem::path& trackDir,
		const std::vector<TrackPositionPlacementUpdate>& updates,
		std::string& outError)
	{
		outError.clear();
		if (trackDir.empty())
		{
			outError = "Track directory is empty";
			return false;
		}
		if (updates.empty())
		{
			outError = "Placement updates list is empty";
			return false;
		}
		for (const auto& u : updates)
		{
			if (u.slot < 1)
			{
				outError = "Slot index must be >= 1";
				return false;
			}
			if (!std::isfinite(u.x) || !std::isfinite(u.y) || !std::isfinite(u.z) || !std::isfinite(u.yawDegrees))
			{
				outError = "Placement coordinates and yaw must be finite";
				return false;
			}
		}

		std::lock_guard<std::mutex> lock(InfoJsonMutationMutex());
		const auto infoPath = trackDir / "info.json";
		std::error_code ec;
		if (!std::filesystem::is_regular_file(infoPath, ec))
		{
			outError = "info.json not found";
			return false;
		}
		jsonlite::Value root;
		{
			std::string rawText;
			std::string readErr;
			if (!ReadTrackInfoJsonTextUnlocked(infoPath, rawText, &readErr))
			{
				outError = "Cannot open info.json for reading" + (readErr.empty() ? std::string() : ": " + readErr);
				return false;
			}
			std::string parseErr;
			if (!jsonlite::ParseText(rawText, root, parseErr) || !root.IsObject())
			{
				outError = "info.json is malformed: " + parseErr;
				return false;
			}
		}

		jsonlite::Value* positionsVal = nullptr;
		for (size_t i = root.objectMembers.size(); i-- > 0;)
		{
			if (root.objectMembers[i].first == "positions")
			{
				positionsVal = &root.objectMembers[i].second;
				break;
			}
		}
		if (!positionsVal || !positionsVal->IsArray())
		{
			outError = "info.json does not contain a positions array";
			return false;
		}

		auto setOrReplace = [](jsonlite::Value& obj, const std::string& key, jsonlite::Value val) {
			bool updated = false;
			std::vector<std::pair<std::string, jsonlite::Value>> next;
			next.reserve(obj.objectMembers.size() + 1);
			for (auto& kv : obj.objectMembers)
			{
				if (kv.first == key)
				{
					if (!updated)
					{
						next.emplace_back(key, std::move(val));
						updated = true;
					}
				}
				else
				{
					next.push_back(std::move(kv));
				}
			}
			if (!updated) next.emplace_back(key, std::move(val));
			obj.objectMembers = std::move(next);
		};

		auto makeCleanNumber = [](float v) {
			const float clean = (v == 0.0f || std::fabs(v) < 1e-7f) ? 0.0f : v;
			std::string formatted = jsonlite::FormatNumber(static_cast<double>(clean), 6);
			if (formatted == "-0") formatted = "0";
			jsonlite::Value val;
			val.type = jsonlite::Type::Number;
			val.numberValue = std::strtod(formatted.c_str(), nullptr);
			if (!std::isfinite(val.numberValue) || val.numberValue == 0.0) val.numberValue = 0.0;
			return val;
		};

		for (const auto& u : updates)
		{
			jsonlite::Value* targetObj = nullptr;
			for (auto& item : positionsVal->arrayItems)
			{
				if (!item.IsObject()) continue;
				const auto* slotField = item.Find("slot");
				if (slotField && slotField->IsNumber() && static_cast<int>(std::llround(slotField->numberValue)) == u.slot)
				{
					targetObj = &item;
					break;
				}
			}
			if (!targetObj && static_cast<size_t>(u.slot - 1) < positionsVal->arrayItems.size())
			{
				auto& candidate = positionsVal->arrayItems[static_cast<size_t>(u.slot - 1)];
				if (candidate.IsObject() && candidate.Find("slot") == nullptr)
					targetObj = &candidate;
			}
			if (!targetObj)
			{
				outError = "Target slot " + std::to_string(u.slot) + " not found in info.json positions";
				return false;
			}
			setOrReplace(*targetObj, "x", makeCleanNumber(u.x));
			setOrReplace(*targetObj, "y", makeCleanNumber(u.y));
			setOrReplace(*targetObj, "z", makeCleanNumber(u.z));
			setOrReplace(*targetObj, "yaw_degrees", makeCleanNumber(u.yawDegrees));
		}

		std::string serialized;
		SerializeJsonValue(root, serialized, 0);
		serialized.push_back('\n');

		return WriteTrackInfoJsonAtomically(infoPath, serialized, outError);
	}

	inline bool UpdateTrackInfoJsonPositionPlacement(
		const std::filesystem::path& trackDir,
		int slot,
		float x,
		float y,
		float z,
		float yawDegrees,
		std::string& outError)
	{
		return UpdateTrackInfoJsonPositionsPlacement(
			trackDir,
			std::vector<TrackPositionPlacementUpdate>{{slot, x, y, z, yawDegrees}},
			outError);
	}
} // namespace GakuMotion
