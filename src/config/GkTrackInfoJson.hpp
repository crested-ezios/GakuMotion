#pragma once

#include "../GkJsonLite.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
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
} // namespace GakuMotion
