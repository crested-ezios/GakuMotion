#pragma once

#include "GkJsonLite.hpp"
#include "GkMotionData.hpp"
#include <array>
#include <vector>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <locale>
#include <set>
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

namespace GakuMotion::ensemble
{
    inline constexpr size_t InitialSlots = 1; // Initial slot count; not a capacity limit.

    struct Slot
    {
        bool enabled{false};
        uint64_t actorToken{0}; // Current roster only; never persisted.
        std::string actorHint, motionKey;
        Vector3 offset{};
        float yawDegrees{0};
        bool operator==(const Slot& other) const
        {
            return enabled == other.enabled && actorToken == other.actorToken && actorHint == other.actorHint &&
                motionKey == other.motionKey && offset.x == other.offset.x && offset.y == other.offset.y &&
                offset.z == other.offset.z && yawDegrees == other.yawDegrees;
        }
    };

    struct Settings
    {
        bool enabled{false};
        uint32_t masterSlot{0}, cameraSlot{0};
        std::vector<Slot> slots = std::vector<Slot>(InitialSlots);
        bool operator==(const Settings&) const = default;
    };

    struct Snapshot
    {
        Settings settings;
        uint64_t revision{0}, actorEpoch{0};
        bool available{false}, verified{false}, probeEnabled{false}, active{false};
        std::string notice;
        std::vector<std::string> physics = std::vector<std::string>(InitialSlots);
    };

    inline bool Validate(const Settings& settings, std::string& error)
    {
        error.clear();
        if (settings.masterSlot >= settings.slots.size() || settings.cameraSlot >= settings.slots.size())
        { error = "Ensemble slot selection is out of range"; return false; }
        std::set<uint64_t> tokens;
        size_t active = 0;
        for (const auto& slot : settings.slots)
        {
            if (!std::isfinite(slot.offset.x) || !std::isfinite(slot.offset.y) || !std::isfinite(slot.offset.z) ||
                !std::isfinite(slot.yawDegrees))
            { error = "Ensemble placement must use finite numbers"; return false; }
            if (slot.actorHint.size() > 1024 || slot.actorHint.find('\0') != std::string::npos ||
                slot.motionKey.size() > 512 || slot.motionKey.find('\0') != std::string::npos ||
                slot.motionKey.find_first_of("/\\") != std::string::npos || slot.motionKey == "." || slot.motionKey == "..")
            { error = "Invalid ensemble actor hint or motion library key"; return false; }
            if (slot.actorToken && !tokens.insert(slot.actorToken).second)
            { error = "An actor cannot occupy two ensemble slots"; return false; }
            if (slot.enabled)
            {
                ++active;
                if (settings.enabled && slot.motionKey.empty())
                { error = "Choose a motion for every enabled ensemble slot"; return false; }
            }
        }
        if (settings.enabled && !active)
        { error = "Enable at least one ensemble slot"; return false; }
        return true;
    }

    inline bool Load(const std::string& path, Settings& settings, std::string& error)
    {
        jsonlite::Value root;
        if (!jsonlite::ParseFile(path, root, error)) return false;
        const auto known = [&](const jsonlite::Value& object, std::initializer_list<const char*> names) {
            if (!object.IsObject()) { error = "Ensemble preset must contain JSON objects"; return false; }
            std::set<std::string> seen;
            for (const auto& [name, unused] : object.objectMembers)
            {
                bool allowed = false;
                for (const char* field : names) if (name == field) allowed = true;
                if (!allowed || !seen.insert(name).second)
                { error = "Unknown or duplicate ensemble preset field: " + name; return false; }
            }
            return true;
        };
        if (!known(root, {"version", "enabled", "masterSlot", "cameraSlot", "slots"})) return false;
        const auto* version = root.Find("version");
        if (!version || !version->IsNumber() || version->numberValue != 1)
        { error = "Unsupported ensemble preset version"; return false; }
        Settings candidate;
        const auto boolean = [&](const jsonlite::Value& object, const char* field, bool& target) {
            const auto* value = object.Find(field);
            if (!value || !value->IsBool()) { error = std::string("Expected ensemble boolean: ") + field; return false; }
            target = value->boolValue; return true;
        };
        const auto index = [&](const char* field, uint32_t& target) {
            const auto* value = root.Find(field);
            if (!value || !value->IsNumber() || !std::isfinite(value->numberValue) || value->numberValue < 0 ||
                value->numberValue >= candidate.slots.size() || std::floor(value->numberValue) != value->numberValue)
            { error = std::string("Invalid ensemble slot index: ") + field; return false; }
            target = static_cast<uint32_t>(value->numberValue); return true;
        };
        const auto* slots = root.Find("slots");
        if (!slots || !slots->IsArray() || slots->Size() == 0)
        { error = "Ensemble preset requires at least one logical slot"; return false; }
        candidate.slots.resize(slots->Size());
        if (!boolean(root, "enabled", candidate.enabled) || !index("masterSlot", candidate.masterSlot) ||
            !index("cameraSlot", candidate.cameraSlot)) return false;
        for (size_t i = 0; i < candidate.slots.size(); ++i)
        {
            const auto& object = *slots->At(i);
            auto& slot = candidate.slots[i];
            if (!known(object, {"enabled", "actorHint", "motionKey", "offset", "yawDegrees"}) ||
                !boolean(object, "enabled", slot.enabled)) return false;
            const auto* hint = object.Find("actorHint");
            const auto* motion = object.Find("motionKey");
            const auto* offset = object.Find("offset");
            const auto* yaw = object.Find("yawDegrees");
            if (!hint || !hint->IsString() || !motion || !motion->IsString() || !offset || !offset->IsArray() ||
                offset->Size() != 3 || !yaw || !yaw->IsNumber() || !std::isfinite(yaw->numberValue))
            { error = "Invalid ensemble slot fields"; return false; }
            slot.actorHint = hint->stringValue;
            slot.motionKey = motion->stringValue;
            float components[3]{};
            for (size_t component = 0; component < 3; ++component)
            {
                const auto* value = offset->At(component);
                if (!value->IsNumber() || !std::isfinite(value->numberValue))
                { error = "Ensemble offset must contain three finite numbers"; return false; }
                components[component] = static_cast<float>(value->numberValue);
            }
            slot.offset = {components[0], components[1], components[2]};
            slot.yawDegrees = static_cast<float>(yaw->numberValue);
            // A hint is presentation only. Loading never selects a live actor.
            slot.actorToken = 0;
        }
        if (!Validate(candidate, error)) return false;
        settings = std::move(candidate);
        return true;
    }

    inline bool Save(const std::string& path, const Settings& settings, std::string& error)
    {
        if (!Validate(settings, error)) return false;
        if (path.empty()) { error = "Ensemble preset path is unavailable"; return false; }
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(9) << std::boolalpha << "{\n  \"version\": 1,\n  \"enabled\": " << settings.enabled <<
            ",\n  \"masterSlot\": " << settings.masterSlot << ",\n  \"cameraSlot\": " << settings.cameraSlot << ",\n  \"slots\": [";
        for (size_t i = 0; i < settings.slots.size(); ++i)
        {
            const auto& slot = settings.slots[i];
            out << (i ? "," : "") << "\n    {\"enabled\": " << slot.enabled << ", \"actorHint\": \"" <<
                jsonlite::EscapeString(slot.actorHint) << "\", \"motionKey\": \"" << jsonlite::EscapeString(slot.motionKey) <<
                "\", \"offset\": [" << slot.offset.x << ", " << slot.offset.y << ", " << slot.offset.z <<
                "], \"yawDegrees\": " << slot.yawDegrees << '}';
        }
        out << "\n  ]\n}\n";
        const auto bytes = out.str();
        const auto destination = MotionPathFromUtf8(path);
        std::error_code ec;
        if (!destination.parent_path().empty()) std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) { error = ec.message(); return false; }
        static std::atomic<uint64_t> sequence{0};
        auto temporary = destination;
        temporary += "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
            std::to_string(++sequence) + ".tmp";
        const auto cleanup = [&] { std::error_code ignored; std::filesystem::remove(temporary, ignored); };
#ifdef _WIN32
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { error = std::error_code(GetLastError(), std::system_category()).message(); return false; }
        DWORD written = 0;
        bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
        if (ok) ok = FlushFileBuffers(file) != 0;
        if (!ok) error = std::error_code(GetLastError(), std::system_category()).message();
        if (!CloseHandle(file) && ok) { ok = false; error = std::error_code(GetLastError(), std::system_category()).message(); }
        if (!ok) { cleanup(); return false; }
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        { error = std::error_code(GetLastError(), std::system_category()).message(); cleanup(); return false; }
#else
        const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (file < 0) { error = std::error_code(errno, std::generic_category()).message(); return false; }
        bool ok = true;
        for (size_t offset = 0; offset < bytes.size();)
        {
            const auto count = ::write(file, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { ok = false; break; }
            offset += static_cast<size_t>(count);
        }
        if (ok) ok = ::fsync(file) == 0;
        if (!ok) error = std::error_code(errno, std::generic_category()).message();
        if (::close(file) != 0 && ok) { ok = false; error = std::error_code(errno, std::generic_category()).message(); }
        if (!ok) { cleanup(); return false; }
        std::filesystem::rename(temporary, destination, ec);
        if (ec) { error = ec.message(); cleanup(); return false; }
#endif
        error.clear();
        return true;
    }
}
