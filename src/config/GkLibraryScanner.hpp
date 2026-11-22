#pragma once

#include "GkMotionCatalog.hpp"
#include "GkTrackInfoJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace GakuMotion
{
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
				slot1.motionTiming = m.timing.motion;
				slot1.facialTiming = m.timing.facial;
				slot1.hasTiming = true;
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
					posMap[1].motionTiming = m.timing.motion;
					posMap[1].facialTiming = m.timing.facial;
					posMap[1].hasTiming = true;

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
							ReadSlotTrackTimings(item, m.timing, pe.motionTiming, pe.facialTiming, &pe.hasTiming);
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

						const bool isNewPos = (posMap.find(slotIndex) == posMap.end());
						auto& pe = posMap[slotIndex];
						pe.slot = slotIndex;
						if (isNewPos)
						{
							pe.motionTiming = m.timing.motion;
							pe.facialTiming = m.timing.facial;
							pe.hasTiming = false;
						}
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
				if (!pe.hasTiming)
				{
					pe.motionTiming = m.timing.motion;
					pe.facialTiming = m.timing.facial;
				}
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
} // namespace GakuMotion
