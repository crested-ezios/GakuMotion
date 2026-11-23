// SPDX-License-Identifier: MIT
//
// GakuLoader Public Plugin SDK
// See the applicable MIT license notice.

#pragma once
#include "GakuPaths.h"
#include <windows.h>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Header-only, caller-side C++ helpers. No STL crosses the C ABI. These helpers
// never create directories or migrate data; call ensureRoot explicitly to write.
namespace GakuPaths {
inline GakuStatus Query(const GakuHostApi* host, GakuPathsV1& output) {
    if (!host) return GAKU_E_INVALID_ARGUMENT;
    if (host->abiVersion != GAKU_ABI_VERSION) return GAKU_E_VERSION;
    if (host->structSize < sizeof(GakuHostApi)) return GAKU_E_SIZE;
    if (!host->queryService) return GAKU_E_INVALID_ARGUMENT;
    GakuPathsV1 table{};
    const auto status = host->queryService(host->hostContext, GAKU_SERVICE_PATHS,
        GAKU_PATHS_VERSION, sizeof(table), &table);
    if (status != GAKU_OK) return status;
    if (table.abiVersion != GAKU_PATHS_VERSION || table.structSize != sizeof(table) ||
        !table.context || !table.getRoot || !table.ensureRoot) return GAKU_E_INVALID_ARGUMENT;
    output = table;
    return GAKU_OK;
}

inline GakuStatus GetRoot(const GakuPathsV1& service, GakuPathKind kind, std::filesystem::path& output) {
    if (service.abiVersion != GAKU_PATHS_VERSION || service.structSize != sizeof(service) ||
        !service.getRoot || !service.context) return GAKU_E_INVALID_ARGUMENT;
    uint32_t count = 0;
    const auto status = service.getRoot(service.context, kind, nullptr, 0, &count);
    if (status != GAKU_E_SIZE) return status == GAKU_OK ? GAKU_E_INTERNAL : status;
    if (!count) return GAKU_E_INTERNAL;
    try {
        std::vector<wchar_t> buffer(count);
        uint32_t written = 0;
        const auto read = service.getRoot(service.context, kind, buffer.data(), count, &written);
        if (read != GAKU_OK) return read;
        if (written != count || buffer.back() != L'\0') return GAKU_E_INTERNAL;
        std::filesystem::path root(buffer.data());
        if (!root.is_absolute()) return GAKU_E_INVALID_ARGUMENT;
        output = std::move(root);
        return GAKU_OK;
    } catch (...) { return GAKU_E_INTERNAL; }
}

inline GakuStatus FromUtf8(std::string_view input, std::wstring& output) {
    if (input.find('\0') != std::string_view::npos ||
        input.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return GAKU_E_INVALID_ARGUMENT;
    if (input.empty()) { output.clear(); return GAKU_OK; }
    const auto length = static_cast<int>(input.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), length, nullptr, 0);
    if (!count) return GAKU_E_INVALID_ARGUMENT;
    try {
        std::wstring wide(static_cast<size_t>(count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), length, wide.data(), count) != count)
            return GAKU_E_INVALID_ARGUMENT;
        output = std::move(wide);
        return GAKU_OK;
    } catch (...) { return GAKU_E_INTERNAL; }
}

inline GakuStatus ToUtf8(std::wstring_view input, std::string& output) {
    if (input.find(L'\0') != std::wstring_view::npos ||
        input.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return GAKU_E_INVALID_ARGUMENT;
    if (input.empty()) { output.clear(); return GAKU_OK; }
    const auto length = static_cast<int>(input.size());
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), length, nullptr, 0, nullptr, nullptr);
    if (!count) return GAKU_E_INVALID_ARGUMENT;
    try {
        std::string utf8(static_cast<size_t>(count), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), length, utf8.data(), count, nullptr, nullptr) != count)
            return GAKU_E_INVALID_ARGUMENT;
        output = std::move(utf8);
        return GAKU_OK;
    } catch (...) { return GAKU_E_INTERNAL; }
}

// Cooperative relative-path validation, not a filesystem sandbox: existing
// symlinks/reparse points may redirect I/O. Never use this to trust ZIP entries.
inline GakuStatus JoinRelative(const std::filesystem::path& root, std::wstring_view relative,
                              std::filesystem::path& output) {
    if (!root.is_absolute() || relative.empty()) return GAKU_E_INVALID_ARGUMENT;
    std::string checked;
    if (ToUtf8(relative, checked) != GAKU_OK) return GAKU_E_INVALID_ARGUMENT;
    try {
        size_t start = 0;
        for (size_t i = 0; i <= relative.size(); ++i) {
            if (i != relative.size() && relative[i] != L'/' && relative[i] != L'\\') continue;
            const auto part = relative.substr(start, i - start);
            if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ')
                return GAKU_E_INVALID_ARGUMENT;
            for (wchar_t c : part)
                if (c < 32 || c == L':' || c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'?' || c == L'*')
                    return GAKU_E_INVALID_ARGUMENT;
            auto stem = std::wstring(part.substr(0, part.find(L'.')));
            for (auto& c : stem) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
            if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" ||
                (stem.size() == 4 && (stem.substr(0, 3) == L"com" || stem.substr(0, 3) == L"lpt") &&
                 ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\x00b9' || stem[3] == L'\x00b2' || stem[3] == L'\x00b3')))
                return GAKU_E_INVALID_ARGUMENT;
            start = i + 1;
        }
        output = root / std::filesystem::path(relative);
        return GAKU_OK;
    } catch (...) { return GAKU_E_INTERNAL; }
}
}
