#pragma once
#include "GkAudioPlayback.hpp"
#include "GkRuntimeConfig.hpp"
#include <array>
#include <map>

namespace GakuMotion
{
	struct SlotFileVersion
	{
		int slot{0};
		std::array<FileSignature, 9> files{};
		bool operator==(const SlotFileVersion&) const = default;
	};

	// A library refresh observes files; it never changes the loaded performance.
	struct MotionFileVersion
	{
		std::array<FileSignature, 12> files{};
		bool positionsDirExists{false};
		std::vector<SlotFileVersion> slotFiles;
		bool operator==(const MotionFileVersion&) const = default;
	};

	inline MotionFileVersion ReadMotionFileVersion(const std::string& directory)
	{
		MotionFileVersion result;
		const std::filesystem::path root = MotionPathFromUtf8(directory);
		std::string audioHint;
		std::string coverHint;
		{
			jsonlite::Value rootJson;
			std::string parseErr;
			if (ParseTrackInfoJsonFile(root / "info.json", rootJson, parseErr) &&
				rootJson.type == jsonlite::Type::Object)
			{
				audioHint = rootJson.StringPath("audio_file", "");
				coverHint = rootJson.StringPath("cover_file", "");
			}
		}

		std::string resolvedAudio = audio::ResolveTrackAudioFileName(root, audioHint);
		if (resolvedAudio.empty()) resolvedAudio = "song.wav";
		std::string resolvedCover = audio::ResolveTrackCoverFileName(root, coverHint);
		if (resolvedCover.empty()) resolvedCover = "cover.jpg";

		const std::array<std::string, 12> names{
			"motion.gkclip", "motion.gk_world_dump", "motion.gkmotion",
			"camera.gkcamera", resolvedAudio, "info.json",
			"bone_overrides.json", "root_height_calibration.json",
			"aux_replay.json", "source.vmd", "source_morph.vmd", resolvedCover};

		auto statFile = [](const std::filesystem::path& path, FileSignature& signature) {
			std::error_code ec;
			signature.exists = std::filesystem::is_regular_file(path, ec);
			if (!signature.exists || ec) return;
			auto time = std::filesystem::last_write_time(path, ec);
			if (!ec) signature.writeTime = static_cast<long long>(time.time_since_epoch().count());
			auto length = std::filesystem::file_size(path, ec);
			if (!ec) signature.size = static_cast<long long>(length);
		};

		for (size_t i = 0; i < result.files.size(); ++i)
		{
			statFile(root / std::u8string(names[i].begin(), names[i].end()), result.files[i]);
		}

		std::error_code posEc;
		const auto positionsDir = root / "positions";
		result.positionsDirExists = std::filesystem::is_directory(positionsDir, posEc);
		if (result.positionsDirExists && !posEc)
		{
			static constexpr std::array<const char*, 9> kSlotNames{
				"motion.gkclip", "motion.gk_world_dump", "motion.gkmotion",
				"bone_overrides.json", "root_height_calibration.json",
				"aux_replay.json", "source.vmd", "source_morph.vmd",
				"motion.morph_report.json"};
			std::filesystem::directory_iterator it(positionsDir, posEc), end;
			for (; !posEc && it != end; it.increment(posEc))
			{
				if (posEc) break;
				const auto& entry = *it;
				std::error_code dirEc;
				if (!entry.is_directory(dirEc) || dirEc) continue;
				const auto utf8Name = entry.path().filename().u8string();
				const std::string slotName(utf8Name.begin(), utf8Name.end());
				int slot = 0;
				if (!ParseSlotDirectoryIndex(slotName, slot)) continue;
				SlotFileVersion sv;
				sv.slot = slot;
				for (size_t i = 0; i < kSlotNames.size(); ++i)
				{
					statFile(entry.path() / kSlotNames[i], sv.files[i]);
				}
				result.slotFiles.push_back(std::move(sv));
			}
			std::sort(result.slotFiles.begin(), result.slotFiles.end(),
				[](const SlotFileVersion& a, const SlotFileVersion& b) { return a.slot < b.slot; });
		}
		return result;
	}

	struct LibraryScanSnapshot
	{
		MotionLibraryScanResult result;
		std::map<std::string, MotionFileVersion> versions;
	};

	inline LibraryScanSnapshot ReadLibrarySnapshot(const std::string& root)
	{
		LibraryScanSnapshot snapshot;
		snapshot.result = ScanMotionLibraryDetailed(root);
		for (const auto& entry : snapshot.result.motions)
			snapshot.versions.emplace(entry.name, ReadMotionFileVersion(entry.dir));
		return snapshot;
	}
}
