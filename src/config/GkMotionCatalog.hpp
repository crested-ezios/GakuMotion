#pragma once

#include "../GkAudioPlayback.hpp"
#include "../GkTrackTiming.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

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
		TrackTiming motionTiming;
		TrackTiming facialTiming;
		bool hasTiming{false};
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

	/// <summary>Resolves slot-specific motion/facial timing while keeping camera and audio track-global.</summary>
	inline PerformanceTiming ResolveSlotPerformanceTiming(const MotionEntry* motion, int slot1Based,
	                                                      const PerformanceTiming& fallback)
	{
		PerformanceTiming result = motion ? motion->timing : fallback;
		if (!motion || slot1Based < 1) return result;
		const MotionPositionEntry* matched = nullptr;
		for (const auto& pos : motion->positions)
		{
			if (pos.slot == slot1Based)
			{
				matched = &pos;
				break;
			}
		}
		if (!matched && static_cast<size_t>(slot1Based - 1) < motion->positions.size())
		{
			const auto& candidate = motion->positions[static_cast<size_t>(slot1Based - 1)];
			if (candidate.slot <= 0 || (slot1Based > 1 && candidate.slot == 1))
			{
				matched = &candidate;
			}
		}
		if (matched && matched->hasTiming)
		{
			result.motion = matched->motionTiming;
			result.facial = matched->facialTiming;
		}
		return result;
	}

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
} // namespace GakuMotion
