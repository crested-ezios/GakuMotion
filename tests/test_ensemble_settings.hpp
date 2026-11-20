#pragma once

#include "GkEnsembleSettings.hpp"

static void TestEnsembleSettings(const std::string& repoRoot)
{
    BeginTest("ensemble presets: logical slots, unbound actor hints and atomic replacement");
    using namespace GakuMotion;
    namespace fs = std::filesystem;
    const auto root = fs::path(repoRoot) / "build/tests" / ("ensemble-settings-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = root / MotionPathFromUtf8("组合 舞台.json");
    const auto read = [](const fs::path& file) {
        std::ifstream input(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    std::string error;
    ensemble::Settings original;
    Check(ensemble::Validate(original, error) && !original.enabled,
        "ensemble defaults are disabled and valid without actors or motions");
    original.slots.resize(3);
    original.enabled = true;
    original.masterSlot = 1;
    original.cameraSlot = 2;
    for (size_t i = 0; i < size_t{3}; ++i)
    {
        auto& slot = original.slots[i];
        slot.enabled = true;
        slot.actorToken = 101 + i;
        slot.actorHint = "摄影角色 " + std::to_string(i + 1);
        slot.motionKey = i == 1 ? "动作 \"B\"" : "动作 A";
        slot.offset = {static_cast<float>(i) - 1.25f, .04f, -.3f};
        slot.yawDegrees = static_cast<float>(i) * 45 - 90;
    }
    Check(ensemble::Save(MotionPathToUtf8(path), original, error), "three independent logical slots save to a Unicode preset path");
    const auto bytes = read(path);
    Check(bytes.find("actorToken") == std::string::npos && bytes.find("101") == std::string::npos,
        "preset bytes never contain transient actor token fields or values");
    ensemble::Settings expected = original;
    for (auto& slot : expected.slots) slot.actorToken = 0;
    ensemble::Settings loaded;
    Check(ensemble::Load(MotionPathToUtf8(path), loaded, error) && loaded == expected,
        "motion keys, Unicode hints and placements round-trip while every actor stays unbound");
    auto expanded = original;
    expanded.slots.resize(5);
    expanded.slots[3].enabled = true;
    expanded.slots[3].actorToken = 104;
    expanded.slots[3].motionKey = "motion-D";
    expanded.slots[3].offset = {128.0f, -256.0f, 256.0f};
    const auto expandedPath = root / "expanded.json";
    Check(ensemble::Save(MotionPathToUtf8(expandedPath), expanded, error), "more than three slots can be saved");
    expanded.slots[0].actorToken = expanded.slots[1].actorToken = expanded.slots[2].actorToken = expanded.slots[3].actorToken = 0;
    Check(ensemble::Load(MotionPathToUtf8(expandedPath), loaded, error) && loaded == expanded,
        "expanded presets preserve all positions and leave every actor unbound");
    auto invalid = original;
    invalid.slots[2].actorToken = invalid.slots[0].actorToken;
    Check(!ensemble::Validate(invalid, error), "the same actor cannot occupy two logical slots");
    invalid = original;
    invalid.masterSlot = 3;
    Check(!ensemble::Validate(invalid, error), "out-of-range master slot is rejected");
    invalid = original;
    invalid.cameraSlot = UINT32_MAX;
    Check(!ensemble::Validate(invalid, error), "out-of-range follow slot is rejected");
    invalid = original;
    invalid.slots[0].offset.y = std::numeric_limits<float>::quiet_NaN();
    Check(!ensemble::Save(MotionPathToUtf8(path), invalid, error) && read(path) == bytes,
        "non-finite placement is rejected without overwriting the last preset");
    invalid = original;
    invalid.slots[0].yawDegrees = std::numeric_limits<float>::infinity();
    Check(!ensemble::Validate(invalid, error), "non-finite yaw is rejected");
    invalid = original;
    invalid.slots[0].motionKey = "../outside";
    Check(!ensemble::Validate(invalid, error), "preset motion references are library keys, not relative paths");
    invalid = original;
    invalid.slots[0].motionKey.clear();
    Check(!ensemble::Validate(invalid, error), "an enabled slot in an enabled ensemble requires a motion");
    invalid = original;
    for (auto& slot : invalid.slots) slot.enabled = false;
    Check(!ensemble::Validate(invalid, error), "an enabled ensemble requires at least one enabled slot");
    invalid.enabled = false;
    Check(ensemble::Validate(invalid, error), "a disabled draft may retain unfinished logical slots");
    const auto badPath = root / "invalid.json";
    const auto reject = [&](const std::string& malformed, const char* message) {
        WriteTextFile(badPath, malformed);
        const auto before = loaded;
        Check(!ensemble::Load(MotionPathToUtf8(badPath), loaded, error) && loaded == before, message);
    };
    auto replace = [&](const std::string& from, const std::string& to) {
        auto modified = bytes;
        const auto at = modified.find(from);
        if (at != std::string::npos) modified.replace(at, from.size(), to);
        return modified;
    };
    reject(replace("\"version\": 1", "\"version\": 2"), "unknown versions preserve the previous in-memory draft");
    reject(replace("\"masterSlot\": 1", "\"masterSlot\": 1.5"), "fractional logical slot indices are rejected");
    reject(replace("\"version\": 1", "\"version\": 1, \"version\": 1"), "duplicate preset fields are rejected");
    reject(replace("\"version\": 1", "\"version\": 1, \"actorToken\": 101"), "transient identity fields in preset input are rejected");
    reject(replace("\"actorHint\":", "\"actorToken\": 101, \"actorHint\":"), "persisted actor identities inside a logical slot are rejected");
    auto malformedOffset = bytes;
    const auto offsetAt = malformedOffset.find("\"offset\": [");
    const auto offsetEnd = malformedOffset.find(']', offsetAt);
    malformedOffset.replace(offsetAt, offsetEnd - offsetAt + 1, "\"offset\": [0, 0]");
    reject(malformedOffset, "invalid placement dimensions preserve the previous draft");
#ifdef _WIN32
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock preset against replacement");
    if (locked != INVALID_HANDLE_VALUE)
    {
        auto replacement = original;
        replacement.slots[0].yawDegrees = 27;
        Check(!ensemble::Save(MotionPathToUtf8(path), replacement, error) && read(path) == bytes,
            "failed atomic replacement preserves the previous bytes");
        CloseHandle(locked);
        bool temporaryLeft = false;
        for (const auto& entry : fs::directory_iterator(root))
            if (entry.path().extension() == ".tmp") temporaryLeft = true;
        Check(!temporaryLeft, "failed save removes only its own temporary file");
    }
#endif
    auto replacement = expected;
    replacement.slots[0].yawDegrees = 27;
    Check(ensemble::Save(MotionPathToUtf8(path), replacement, error) &&
        ensemble::Load(MotionPathToUtf8(path), loaded, error) && loaded == replacement,
        "an unlocked existing preset is atomically replaced with the complete new draft");
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
