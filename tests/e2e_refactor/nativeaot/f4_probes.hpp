#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "GkLog.hpp"
#include "GkMotionData.hpp"
#include "GkPlaylist.hpp"
#include "GkRuntimeConfig.hpp"
#include "GkCsharpImport.hpp"
#include "GkLibraryState.hpp"
#include "GkMotionPlayer.hpp"
#include "GkCamera.hpp"
#include "e2e_report.hpp"
#include "vmd_fixture_gen.hpp"

namespace GakuMotion {

// ADL carrier types so free-function probes/calls stay purely dependent in C++20 templates/concepts
// without triggering phase-1 non-dependent qualified lookup errors against pre-M1 src/ headers.
struct E2eAdlPath : std::filesystem::path {
    using std::filesystem::path::path;
    E2eAdlPath(const std::filesystem::path& p) : std::filesystem::path(p) {}
    E2eAdlPath(std::filesystem::path&& p) : std::filesystem::path(std::move(p)) {}
};

struct E2eAdlStr : std::string {
    using std::string::string;
    E2eAdlStr(const std::string& s) : std::string(s) {}
    E2eAdlStr(std::string&& s) : std::string(std::move(s)) {}
};

struct E2eAdlSv : std::string_view {
    using std::string_view::string_view;
    constexpr E2eAdlSv(std::string_view s) noexcept : std::string_view(s) {}
};

} // namespace GakuMotion

namespace f4_probes {

// 1. GkPlaylist.hpp M1 group import extensions
template <typename Req>
concept HasGroupImportRequest = requires(Req& r) {
    r.positions;
    r.positionsExplicit;
    requires requires(typename decltype(r.positions)::value_type& slot) {
        slot.slot;
        slot.sourceSlot;
        slot.motionVmd;
        slot.morphVmd;
        slot.disableMorph;
        slot.clearMorph;
        slot.sourceFps;
        slot.rebakeOutput;
        slot.preserveExisting;
        slot.x;
        slot.y;
        slot.z;
        slot.yawDegrees;
    };
};

// 2. GkRuntimeConfig.hpp M1 group MotionEntry & MotionPositionEntry extensions
template <typename Entry>
concept HasGroupMotionEntry =
    gke2e::probes::HasIsGroup<Entry> &&
    gke2e::probes::HasPositionCount<Entry> &&
    gke2e::probes::HasPositions<Entry> &&
    requires(const Entry& e, int slot) {
        { e.SlotDirectory(slot) } -> std::convertible_to<std::string>;
        requires requires(const typename decltype(e.positions)::value_type& p) {
            p.slot;
            p.x;
            p.y;
            p.z;
            p.yawDegrees;
            p.hasMotion;
            p.hasFacial;
            p.motionFile;
            p.hasOverrides;
            p.sourceVmd;
            p.sourceMorphVmd;
            p.sourceVmdSha256;
            p.sourceFps;
            p.frames;
            p.bones;
        };
    };

// 3. Zero-rebake info.json placement update helper (unqualified ADL on dependent AdlPathT)
template <typename AdlPathT>
concept HasPlacementUpdater = requires(const AdlPathT& dir, int slot, float v, std::string& err) {
    { UpdateTrackInfoJsonPositionPlacement(dir, slot, v, v, v, v, err) } -> std::same_as<bool>;
    { UpdateTrackInfoJsonPositionsPlacement(dir, {}, err) } -> std::same_as<bool>;
};

// 4. GkLibraryState.hpp M1 group file watcher extensions
template <typename Ver>
concept HasGroupFileVersion = requires(const Ver& v) {
    v.positionsDirExists;
    v.slotFiles;
    requires requires(const typename decltype(v.slotFiles)::value_type& sv) {
        sv.slot;
        sv.files;
    };
};

// 5. GkMotionPlayer.hpp M1 slot loading extensions
template <typename Player>
concept HasSlotMotionPlayer = requires(Player& p, const std::string& dir, int slot) {
    { p.LoadMotionSlot(dir, slot) } -> std::same_as<bool>;
    p.currentMotionSlot;
    p.currentSlotDir;
    { p.CurrentSlotDirectory() } -> std::convertible_to<const std::string&>;
    { p.SlotBoneOverridesPath() } -> std::convertible_to<std::string>;
    { p.SlotAuxReplayPath() } -> std::convertible_to<std::string>;
    { p.SlotRootHeightCalibrationPath() } -> std::convertible_to<std::string>;
};

// 6. GkMotionData.hpp slot directory path helpers (unqualified ADL on dependent AdlStrT/AdlSvT)
template <typename AdlStrT, typename AdlSvT>
concept HasSlotPathHelpers = requires(const AdlStrT& dir, AdlSvT sv, int& outSlot) {
    { ParseSlotDirectoryIndex(sv, outSlot) } -> std::same_as<bool>;
    { ResolveMotionSlotDirectory(dir, 2) } -> std::convertible_to<std::string>;
    { FindMotionSlotFile(dir, 2) } -> std::convertible_to<std::string>;
};

template <typename Req, typename Entry, typename Ver, typename Player,
          typename AdlPathT = GakuMotion::E2eAdlPath,
          typename AdlStrT = GakuMotion::E2eAdlStr,
          typename AdlSvT = GakuMotion::E2eAdlSv>
inline constexpr bool kAllF4CapabilitiesPresent =
    HasGroupImportRequest<Req> &&
    HasGroupMotionEntry<Entry> &&
    HasPlacementUpdater<AdlPathT> &&
    HasGroupFileVersion<Ver> &&
    HasSlotMotionPlayer<Player> &&
    HasSlotPathHelpers<AdlStrT, AdlSvT>;

// Dependent ADL call helpers (never instantiated on pre-M1 trees)
template <typename AdlPathT>
inline bool CallUpdatePlacement(
    const std::filesystem::path& dir, int slot, float x, float y, float z, float yaw, std::string& err) {
    return UpdateTrackInfoJsonPositionPlacement(AdlPathT(dir), slot, x, y, z, yaw, err);
}

template <typename AdlPathT>
inline bool CallUpdatePlacementsEmpty(const std::filesystem::path& dir, std::string& err) {
    return UpdateTrackInfoJsonPositionsPlacement(AdlPathT(dir), {}, err);
}

template <typename AdlSvT>
inline bool CallParseSlotDirectoryIndex(std::string_view sv, int& outSlot) {
    return ParseSlotDirectoryIndex(AdlSvT(sv), outSlot);
}

template <typename AdlStrT>
inline std::string CallResolveMotionSlotDirectory(const std::string& dir, int slot1Based) {
    return ResolveMotionSlotDirectory(AdlStrT(dir), slot1Based);
}

template <typename AdlStrT>
inline std::string CallFindMotionSlotFile(const std::string& dir, int slot1Based) {
    return FindMotionSlotFile(AdlStrT(dir), slot1Based);
}

// Local fixture & sync helpers
inline std::filesystem::path ResetCaseDir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

inline std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return {};
    }
    const auto sz = ifs.tellg();
    if (sz <= 0) {
        return {};
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

inline std::filesystem::path SlotDirectoryPath(const std::filesystem::path& trackDir, int slot1Based) {
    if (slot1Based <= 1) {
        return trackDir;
    }
    return trackDir / "positions" / ("slot_" + std::to_string(slot1Based));
}

inline bool WriteSlotSidecars(const std::filesystem::path& slotDir, std::string_view tag) {
    const bool ok1 = gkmotion::e2e::fixtures::WriteTextFileUtf8(
        slotDir / "bone_overrides.json",
        std::string("{\"tag\":\"") + std::string(tag) + "\",\"overrides\":{\"Spine\":{\"rx\":1.0}}}\n");
    const bool ok2 = gkmotion::e2e::fixtures::WriteTextFileUtf8(
        slotDir / "root_height_calibration.json",
        std::string("{\"tag\":\"") + std::string(tag) + "\",\"offset_m\":0.01}\n");
    const bool ok3 = gkmotion::e2e::fixtures::WriteTextFileUtf8(
        slotDir / "aux_replay.json",
        std::string("{\"tag\":\"") + std::string(tag) + "\",\"channels\":[\"eye\"]}\n");
    return ok1 && ok2 && ok3;
}

inline int RunCsharpImportSync(
    const GakuMotion::MotionImportRequest& req,
    std::string& outMessage,
    GakuMotion::ImportProgressState* outProgress = nullptr,
    int timeoutMs = 20000) {
    if (!GakuMotion::csharp_import::Begin(req, outMessage)) {
        return -2;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const int poll = GakuMotion::csharp_import::Poll(outMessage, outProgress);
        if (poll != 0) {
            return poll;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            outMessage = "Timed out waiting for GkCsharpImport worker";
            return -3;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace f4_probes
