#pragma once

#include "GkMotionCatalog.hpp"
#include "GkTrackInfoJson.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GakuMotion
{
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
