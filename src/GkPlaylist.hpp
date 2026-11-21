#pragma once
#include "GkRuntimeConfig.hpp"
#include <random>
#include <unordered_set>
#include <atomic>
#include <chrono>
#include <sstream>
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
	enum class PlaylistMode { Once, SingleLoop, ListLoop, Shuffle };
	enum class GuiPlaylistAction { Move = 1, SaveList = 3, LoadList = 4, SetMode = 5, DeleteFiles = 6 };

	struct ImportPositionSlot
	{
		int slot{1};
		int sourceSlot{0};
		std::string motionVmd;
		std::string morphVmd;
		bool disableMorph{false};
		bool clearMorph{false};
		float sourceFps{0.0f};
		bool rebakeOutput{false};
		bool preserveExisting{false};
		float x{0.0f};
		float y{0.0f};
		float z{0.0f};
		float yawDegrees{0.0f};
	};

	struct MotionImportRequest
	{
		std::string vmd, audio, title, subtitle, python;
		bool hasSubtitle{false};
		std::string morphVmd;
		std::string cameraVmd;
		std::string cameraRootRegistration{"auto"}; // auto | off | required (camera root pairing)
		float cameraFps{60.0f};
		float cameraScale{0.083f};
		std::string editKey;      // Non-empty when editing an existing library entry (directory key)
		std::string editDir;      // Filled by Core from editKey
		bool disableMorph{false}; // True when user explicitly selects (留空) for facial track
		bool clearCamera{false};  // True when user clears camera in edit mode
		bool clearAudio{false};   // True when user selects (留空) for audio
		std::string motionsRoot, script, log;
		std::string transactionRoot; // Optional managed scratch; same volume as library.
		float fps{0.0f}; // 0 = auto-detect
		float sourceFps{0.0f}; // 0 = auto-detect source timeline
		bool rebakeOutput{false};
		PerformanceTiming timing;
		// 兼容旧请求结构；产品始终使用 NativeAOT，不再回落解释器。
		bool useCsharp{true};
		std::vector<ImportPositionSlot> positions;
		bool positionsExplicit{false};
	};

	struct ImportProgressState
	{
		bool active{false};
		bool finished{false};
		bool succeeded{false};
		float progress{0.0f}; // 0.0f ~ 1.0f
		std::string stage;
		std::string statusText;
		std::string errorDetail;
	};

	inline int FindPlaylistIndex(const std::vector<MotionEntry>& entries, const std::string& key)
	{
		for (size_t i = 0; i < entries.size(); ++i)
			if (entries[i].name == key) return static_cast<int>(i);
		return -1;
	}

	// Entries are identified by directory name, never by a transient row index.
	// Every local track remains visible until its directory is permanently deleted.
	class PlaylistSettings
	{
	public:
		PlaylistMode mode{PlaylistMode::SingleLoop};
		std::vector<std::string> order;
		std::vector<std::string> shuffleRemaining;
		std::mt19937 random{std::random_device{}()};

		void Apply(std::vector<MotionEntry>& entries)
		{
			auto rank = [&](const std::string& key)
			{
				auto it = std::find(order.begin(), order.end(), key);
				return static_cast<size_t>(it - order.begin());
			};
			std::stable_sort(entries.begin(), entries.end(), [&](const MotionEntry& a, const MotionEntry& b)
			{
				return rank(a.name) < rank(b.name);
			});
			Remember(entries);
		}

		void Remember(const std::vector<MotionEntry>& entries)
		{
			order.clear();
			for (const auto& e : entries) order.push_back(e.name);
			shuffleRemaining.clear();
		}

		int Next(const std::vector<MotionEntry>& entries, int current)
		{
			if (entries.empty()) return -1;
			if (mode == PlaylistMode::Once) return -1;
			if (mode == PlaylistMode::SingleLoop) return current;
			if (mode == PlaylistMode::ListLoop) return (current + 1) % static_cast<int>(entries.size());
			if (shuffleRemaining.empty())
			{
				for (const auto& e : entries) shuffleRemaining.push_back(e.name);
				std::shuffle(shuffleRemaining.begin(), shuffleRemaining.end(), random);
				if (entries.size() > 1 && current >= 0 && current < static_cast<int>(entries.size()) &&
					shuffleRemaining.back() == entries[current].name)
					std::swap(shuffleRemaining.front(), shuffleRemaining.back());
			}
			const std::string key = shuffleRemaining.back();
			shuffleRemaining.pop_back();
			return FindPlaylistIndex(entries, key);
		}

		bool Save(const std::string& path, std::string& error) const
		{
			error.clear();
			std::error_code ec;
			const std::filesystem::path destination(MotionPathFromUtf8(path));
			if (!destination.parent_path().empty()) std::filesystem::create_directories(destination.parent_path(), ec);
			if (ec)
			{
				error = ec.message();
				return false;
			}
			std::ostringstream out;
			out << "{\n  \"version\": 1,\n  \"mode\": " << static_cast<int>(mode);
			auto array = [&](const char* field, const std::vector<std::string>& values)
			{
				out << ",\n  \"" << field << "\": [";
				for (size_t i = 0; i < values.size(); ++i)
					out << (i ? ", " : "") << '"' << jsonlite::EscapeString(values[i]) << '"';
				out << ']';
			};
			array("order", order);
			out << "\n}\n";
			const auto bytes = out.str();
			static std::atomic<uint64_t> sequence{0};
			std::filesystem::path temp;
			bool created = false;
#ifdef _WIN32
			HANDLE file = INVALID_HANDLE_VALUE;
#else
			int file = -1;
#endif
			for (unsigned retry = 0; retry < 32 && !created; ++retry)
			{
				temp = destination;
				temp += "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
					std::to_string(++sequence) + ".tmp";
#ifdef _WIN32
				file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
				created = file != INVALID_HANDLE_VALUE;
				if (!created && GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
				{ error = std::error_code(GetLastError(), std::system_category()).message(); return false; }
#else
				file = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
				created = file >= 0;
				if (!created && errno != EEXIST) { error = std::error_code(errno, std::generic_category()).message(); return false; }
#endif
			}
			if (!created) { error = "Cannot create unique playlist staging file"; return false; }
			auto cleanup = [&] { std::error_code ignored; std::filesystem::remove(temp, ignored); };
			bool written = true;
#ifdef _WIN32
			for (size_t offset = 0; offset < bytes.size();)
			{
				DWORD count = 0;
				const auto chunk = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t{0x40000000}));
				if (!WriteFile(file, bytes.data() + offset, chunk, &count, nullptr) || count != chunk)
				{ error = std::error_code(GetLastError(), std::system_category()).message(); written = false; break; }
				offset += count;
			}
			if (written && !FlushFileBuffers(file)) { error = std::error_code(GetLastError(), std::system_category()).message(); written = false; }
			if (!CloseHandle(file)) { error = std::error_code(GetLastError(), std::system_category()).message(); written = false; }
			if (!written) { cleanup(); return false; }
			// Both paths are in the same directory. Never remove the old destination
			// first and never allow MoveFileEx's cross-volume copy fallback.
			if (!MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{ error = std::error_code(GetLastError(), std::system_category()).message(); cleanup(); return false; }
#else
			for (size_t offset = 0; offset < bytes.size();)
			{
				const auto count = ::write(file, bytes.data() + offset, bytes.size() - offset);
				if (count < 0 && errno == EINTR) continue;
				if (count <= 0) { error = std::error_code(errno, std::generic_category()).message(); written = false; break; }
				offset += static_cast<size_t>(count);
			}
			if (written && ::fsync(file) != 0) { error = std::error_code(errno, std::generic_category()).message(); written = false; }
			if (::close(file) != 0) { error = std::error_code(errno, std::generic_category()).message(); written = false; }
			if (!written) { cleanup(); return false; }
			std::filesystem::rename(temp, destination, ec);
			if (ec) { error = ec.message(); cleanup(); return false; }
#endif
			return true;
		}

		bool Load(const std::string& path, std::string& error)
		{
			jsonlite::Value root;
			if (!jsonlite::ParseFile(path, root, error)) return false;
			const auto* o = root.Find("order");
			const int m = root.IntPath("mode", -1);
			if (!root.IsObject() || root.IntPath("version", 0) != 1 || m < 0 || m > 3 ||
				!o || !o->IsArray())
			{
				error = "Invalid GakuMotion playlist (version/mode/order)";
				return false;
			}
			std::vector<std::string> nextOrder;
			auto read = [&](const jsonlite::Value& array, std::vector<std::string>& out)
			{
				for (const auto& v : array.arrayItems)
				{
					if (!v.IsString() || v.stringValue.empty()) return false;
					if (std::find(out.begin(), out.end(), v.stringValue) == out.end()) out.push_back(v.stringValue);
				}
				return true;
			};
			if (!read(*o, nextOrder))
			{
				error = "Invalid song key";
				return false;
			}
			mode = static_cast<PlaylistMode>(m);
			order = std::move(nextOrder);
			// Ignore the retired "removed" field in legacy playlists.
			shuffleRemaining.clear();
			return true;
		}
	};
} // namespace GakuMotion
