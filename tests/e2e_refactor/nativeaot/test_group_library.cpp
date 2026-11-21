#include "f4_probes.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

GK_E2E_DEFINE_LOG_SINK()

namespace {

namespace fs = std::filesystem;
namespace fix = gkmotion::e2e::fixtures;

struct CliContext {
    fs::path nativeAotDir;
    fs::path workDir;
};

static CliContext g_cli;

#ifdef _WIN32
std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), needed, nullptr, nullptr);
    return out;
}
#endif

bool EnsureNativeAotReady(gke2e::CaseContext& ctx) {
    if (g_cli.nativeAotDir.empty()) {
        ctx.Pending("NativeAOT directory not provided (--nativeaot-dir)");
        return false;
    }
    const fs::path dllPath = g_cli.nativeAotDir / "GakuMotion.VmdImporter.Native.dll";
    std::error_code ec;
    if (!fs::is_regular_file(dllPath, ec)) {
        ctx.Pending("GakuMotion.VmdImporter.Native.dll not found at " + GakuMotion::MotionPathToUtf8(dllPath));
        return false;
    }
    GakuMotion::csharp_import::SetPluginsRoot(GakuMotion::MotionPathToUtf8(g_cli.nativeAotDir));
    if (!GakuMotion::csharp_import::Initialize(GakuMotion::MotionPathToUtf8(g_cli.nativeAotDir))) {
        ctx.Pending("GkCsharpImport unavailable: " + GakuMotion::csharp_import::Detail());
        return false;
    }
    return true;
}

fs::path CaseWork(std::string_view testId) {
    return f4_probes::ResetCaseDir(g_cli.workDir / std::string(testId));
}

template <typename Fn>
void DispatchF4Test(gke2e::CaseContext& ctx, Fn&& implFn) {
    if constexpr (!f4_probes::kAllF4CapabilitiesPresent<
                      GakuMotion::MotionImportRequest,
                      GakuMotion::MotionEntry,
                      GakuMotion::MotionFileVersion,
                      GakuMotion::MotionPlayer,
                      GakuMotion::E2eAdlPath,
                      GakuMotion::E2eAdlStr,
                      GakuMotion::E2eAdlSv>) {
        GKE2E_PENDING(ctx, "M1 F4 group-song C++ APIs not yet present");
    } else {
        implFn.template operator()<
            GakuMotion::MotionImportRequest,
            GakuMotion::MotionEntry,
            GakuMotion::MotionFileVersion,
            GakuMotion::MotionPlayer,
            GakuMotion::E2eAdlPath,
            GakuMotion::E2eAdlStr,
            GakuMotion::E2eAdlSv>(ctx);
    }
}

// =============================================================================
// F4 Tier 1 Tests (6 tests >= 5)
// =============================================================================

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T1_F4_01_SoloBackwardCompatibleScan(gke2e::CaseContext& ctx) {
    const fs::path work = CaseWork("T1-F4-01");
    const fs::path lib = work / "library";

    // 1. Modern solo info.json
    fix::TrackInfoSpec soloSpec;
    soloSpec.title = "SoloModern";
    soloSpec.total_frames = 12;
    soloSpec.fps = 30.0f;
    soloSpec.duration_seconds = 11.0f / 30.0f;
    soloSpec.is_group = false;
    soloSpec.position_count = 1;
    soloSpec.positions = {fix::PositionSlotSpec{1, 0.0f, 0.0f, 0.0f, 0.0f, true, false, "source.vmd", "", "", 30.0f, 12, 1}};
    fix::write_synthetic_track_dir(lib / "01_solo_modern", soloSpec, false, true);

    // 2. Legacy solo info.json without is_group / position_count / positions
    const fs::path legacyDir = lib / "02_solo_legacy";
    fix::WriteBinaryFile(legacyDir / "motion.gkclip", fix::make_minimal_gkclip_bytes(60.0f, 9));
    fix::write_pcm_wav(legacyDir / "song.wav", 0.4);
    fix::WriteTextFileUtf8(legacyDir / "info.json",
        "{\n  \"title\": \"SoloLegacy\",\n  \"total_frames\": 9,\n  \"fps\": 60\n}\n");

    // 3. Minimal solo with no info.json at all
    const fs::path noInfoDir = lib / "03_solo_noinfo";
    fix::WriteBinaryFile(noInfoDir / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 6));

    const auto res = GakuMotion::ScanMotionLibraryDetailed(GakuMotion::MotionPathToUtf8(lib));
    GKE2E_REQUIRE(ctx, res.status == GakuMotion::MotionLibraryScanStatus::Complete);
    GKE2E_REQUIRE_EQ(ctx, res.motions.size(), 3u);

    for (const EntryT& entry : res.motions) {
        GKE2E_CHECK(ctx, !entry.isGroup);
        GKE2E_CHECK_EQ(ctx, entry.positionCount, 1);
        GKE2E_REQUIRE_EQ(ctx, entry.positions.size(), 1u);
        GKE2E_CHECK_EQ(ctx, entry.positions[0].slot, 1);
        GKE2E_CHECK(ctx, entry.positions[0].hasMotion);
        GKE2E_CHECK_EQ(ctx, entry.SlotDirectory(1), entry.dir);
    }
    GKE2E_CHECK_EQ(ctx, res.motions[0].title, std::string("SoloModern"));
    GKE2E_CHECK_EQ(ctx, res.motions[1].title, std::string("SoloLegacy"));
    GKE2E_CHECK_EQ(ctx, res.motions[2].title, std::string("03_solo_noinfo"));
    GKE2E_PASS(ctx, "Solo modern, legacy info.json, and info-less solo tracks scanned cleanly");
}

void Test_T1_F4_01_SoloBackwardCompatibleScan(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T1_F4_01_SoloBackwardCompatibleScan<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T1_F4_02_ThreeSlotGroupScanAndIsolatedMetadata(gke2e::CaseContext& ctx) {
    const fs::path work = CaseWork("T1-F4-02");
    const fs::path trackDir = work / "library" / "group_three";

    fix::TrackInfoSpec spec;
    spec.title = "ThreeSlotGroup";
    spec.subtitle = "ProducerUnit";
    spec.fps = 30.0f;
    spec.source_fps = 30.0f;
    spec.total_frames = 24;
    spec.duration_seconds = 23.0f / 30.0f;
    spec.has_facial = true;
    spec.has_camera = true;
    spec.audio_file = "song.wav";
    spec.audio_duration = 0.8f;
    spec.is_group = true;
    spec.position_count = 3;
    spec.positions = {
        fix::PositionSlotSpec{1, 0.0f, 0.0f, 0.0f, 0.0f, true, false, "s1.vmd", "", "", 30.0f, 10, 1},
        fix::PositionSlotSpec{2, 1.25f, 0.1f, -0.5f, 30.0f, true, true, "s2.vmd", "m2.vmd", "", 60.0f, 24, 2},
        fix::PositionSlotSpec{3, -1.5f, 0.0f, 0.75f, -45.0f, true, false, "s3.vmd", "", "", 25.0f, 15, 1},
    };
    fix::write_synthetic_track_dir(trackDir, spec, true, true);
    // Replace slot 2 & 3 clips with distinct FPS/frames/bones/blendshapes to verify slotProbe isolation
    fix::WriteBinaryFile(trackDir / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 10, {{0, "Hips", 1}}));
    fix::WriteBinaryFile(
        trackDir / "positions" / "slot_2" / "motion.gkclip",
        fix::make_minimal_gkclip_bytes(60.0f, 24, {{0, "Hips", 1}, {7, "Spine", 1}}, {"a", "blink"}));
    fix::WriteBinaryFile(trackDir / "positions" / "slot_3" / "motion.gkclip", fix::make_minimal_gkclip_bytes(25.0f, 15));
    f4_probes::WriteSlotSidecars(trackDir / "positions" / "slot_2", "slot2_meta");

    const auto res = GakuMotion::ScanMotionLibraryDetailed(GakuMotion::MotionPathToUtf8(work / "library"));
    GKE2E_REQUIRE(ctx, res.status == GakuMotion::MotionLibraryScanStatus::Complete);
    GKE2E_REQUIRE_EQ(ctx, res.motions.size(), 1u);

    const EntryT& m = res.motions[0];
    GKE2E_CHECK(ctx, m.isGroup);
    GKE2E_CHECK_EQ(ctx, m.positionCount, 3);
    GKE2E_CHECK_NEAR(ctx, m.fps, 30.0f, 1e-4);
    GKE2E_CHECK_EQ(ctx, m.totalFrames, 24u);
    GKE2E_CHECK(ctx, m.hasCamera);
    GKE2E_CHECK(ctx, m.hasAudio);
    GKE2E_CHECK(ctx, m.hasFacial);
    GKE2E_REQUIRE_EQ(ctx, m.positions.size(), 3u);

    GKE2E_CHECK_EQ(ctx, m.positions[0].slot, 1);
    GKE2E_CHECK(ctx, !m.positions[0].hasOverrides);
    GKE2E_CHECK_EQ(ctx, m.positions[1].slot, 2);
    GKE2E_CHECK_NEAR(ctx, m.positions[1].x, 1.25f, 1e-4);
    GKE2E_CHECK_NEAR(ctx, m.positions[1].y, 0.1f, 1e-4);
    GKE2E_CHECK_NEAR(ctx, m.positions[1].z, -0.5f, 1e-4);
    GKE2E_CHECK_NEAR(ctx, m.positions[1].yawDegrees, 30.0f, 1e-4);
    GKE2E_CHECK(ctx, m.positions[1].hasMotion);
    GKE2E_CHECK(ctx, m.positions[1].hasFacial);
    GKE2E_CHECK(ctx, m.positions[1].hasOverrides);
    GKE2E_CHECK_NEAR(ctx, m.positions[1].sourceFps, 60.0f, 1e-4);
    GKE2E_CHECK_EQ(ctx, m.positions[1].frames, 24u);
    GKE2E_CHECK_EQ(ctx, m.positions[1].bones, 2u);

    GKE2E_CHECK_EQ(ctx, m.positions[2].slot, 3);
    GKE2E_CHECK_NEAR(ctx, m.positions[2].x, -1.5f, 1e-4);
    GKE2E_CHECK_NEAR(ctx, m.positions[2].yawDegrees, -45.0f, 1e-4);
    GKE2E_CHECK(ctx, !m.positions[2].hasOverrides);
    GKE2E_PASS(ctx, "3-slot group scan preserved slot 1 fps and isolated slot 2/3 metadata");
}

void Test_T1_F4_02_ThreeSlotGroupScanAndIsolatedMetadata(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T1_F4_02_ThreeSlotGroupScanAndIsolatedMetadata<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T1_F4_03_FallbackGroupDiscoveryWithoutInfoJson(gke2e::CaseContext& ctx) {
    const fs::path work = CaseWork("T1-F4-03");
    const fs::path trackDir = work / "library" / "fallback_group";
    fix::WriteBinaryFile(trackDir / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 5));
    fix::WriteBinaryFile(trackDir / "positions" / "slot_2" / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 7));
    fix::WriteBinaryFile(
        trackDir / "positions" / "slot_3" / "motion.gkclip",
        fix::make_minimal_gkclip_bytes(60.0f, 9, {{0, "Hips", 1}}, {"a"}));

    const auto res = GakuMotion::ScanMotionLibraryDetailed(GakuMotion::MotionPathToUtf8(work / "library"));
    GKE2E_REQUIRE(ctx, res.status == GakuMotion::MotionLibraryScanStatus::Complete);
    GKE2E_REQUIRE_EQ(ctx, res.motions.size(), 1u);
    const EntryT& m = res.motions[0];
    GKE2E_CHECK(ctx, m.isGroup);
    GKE2E_CHECK_EQ(ctx, m.positionCount, 3);
    GKE2E_CHECK(ctx, m.hasFacial);
    GKE2E_REQUIRE_EQ(ctx, m.positions.size(), 3u);
    for (int k = 1; k <= 3; ++k) {
        const std::string resolved = f4_probes::CallResolveMotionSlotDirectory<AdlStrT>(m.dir, k);
        GKE2E_CHECK_EQ(ctx, m.SlotDirectory(k), resolved);
        GKE2E_CHECK(ctx, !f4_probes::CallFindMotionSlotFile<AdlStrT>(m.dir, k).empty());
    }
    GKE2E_PASS(ctx, "Fallback filesystem group discovery without info.json verified");
}

void Test_T1_F4_03_FallbackGroupDiscoveryWithoutInfoJson(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T1_F4_03_FallbackGroupDiscoveryWithoutInfoJson<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T1_F4_04_LibraryStateWatcherPerSlotDiff(gke2e::CaseContext& ctx) {
    const fs::path work = CaseWork("T1-F4-04");
    const fs::path lib = work / "library";
    const fs::path trackDir = lib / "watched_group";
    fix::write_synthetic_group_track(trackDir, 3, "WatchedGroup", "Tester", true, true);

    const auto snap1 = GakuMotion::ReadLibrarySnapshot(GakuMotion::MotionPathToUtf8(lib));
    GKE2E_REQUIRE_EQ(ctx, snap1.result.motions.size(), 1u);
    const auto it1 = snap1.versions.find("watched_group");
    GKE2E_REQUIRE(ctx, it1 != snap1.versions.end());
    const VerT& v1 = it1->second;
    GKE2E_CHECK(ctx, v1.positionsDirExists);
    GKE2E_REQUIRE_EQ(ctx, v1.slotFiles.size(), 2u);
    GKE2E_CHECK_EQ(ctx, v1.slotFiles[0].slot, 2);
    GKE2E_CHECK_EQ(ctx, v1.slotFiles[1].slot, 3);

    // Edit slot 2 bone_overrides.json without touching root audio
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    fix::WriteTextFileUtf8(trackDir / "positions" / "slot_2" / "bone_overrides.json", "{\"edited\":true}\n");
    const VerT v2 = GakuMotion::ReadMotionFileVersion(GakuMotion::MotionPathToUtf8(trackDir));
    GKE2E_CHECK(ctx, v2 != v1);
    GKE2E_CHECK(ctx, v2.files[4] == v1.files[4]); // song.wav signature untouched
    GKE2E_CHECK(ctx, v2.slotFiles[0] != v1.slotFiles[0]);
    GKE2E_CHECK(ctx, v2.slotFiles[1] == v1.slotFiles[1]);

    // Edit slot 2 motion.gkclip
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    fix::WriteBinaryFile(trackDir / "positions" / "slot_2" / "motion.gkclip", fix::make_minimal_gkclip_bytes(60.0f, 11));
    const VerT v3 = GakuMotion::ReadMotionFileVersion(GakuMotion::MotionPathToUtf8(trackDir));
    GKE2E_CHECK(ctx, v3 != v2);
    GKE2E_CHECK(ctx, v3.files[4] == v1.files[4]);

    // Delete slot 3
    std::error_code ec;
    fs::remove_all(trackDir / "positions" / "slot_3", ec);
    const VerT v4 = GakuMotion::ReadMotionFileVersion(GakuMotion::MotionPathToUtf8(trackDir));
    GKE2E_CHECK(ctx, v4 != v3);
    GKE2E_REQUIRE_EQ(ctx, v4.slotFiles.size(), 1u);
    GKE2E_CHECK_EQ(ctx, v4.slotFiles[0].slot, 2);
    GKE2E_PASS(ctx, "GkLibraryState watcher detected per-slot sidecar/clip edits and slot deletion");
}

void Test_T1_F4_04_LibraryStateWatcherPerSlotDiff(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T1_F4_04_LibraryStateWatcherPerSlotDiff<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T1_F4_05_MotionPlayerLoadMotionSlot(gke2e::CaseContext& ctx) {
    const fs::path work = CaseWork("T1-F4-05");
    const fs::path trackDir = work / "library" / "player_group";
    fix::write_synthetic_group_track(trackDir, 3, "PlayerGroup", "Author", false, true);

    fix::WriteBinaryFile(trackDir / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 10));
    fix::WriteBinaryFile(trackDir / "positions" / "slot_2" / "motion.gkclip", fix::make_minimal_gkclip_bytes(60.0f, 25));
    fix::WriteBinaryFile(trackDir / "positions" / "slot_3" / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 15));
    f4_probes::WriteSlotSidecars(trackDir, "p_slot1");
    f4_probes::WriteSlotSidecars(trackDir / "positions" / "slot_2", "p_slot2");

    const std::string trackUtf8 = GakuMotion::MotionPathToUtf8(trackDir);
    PlayerT player;
    GKE2E_REQUIRE(ctx, player.LoadMotionSlot(trackUtf8, 1));
    GKE2E_CHECK_EQ(ctx, player.currentMotionSlot, 1);
    GKE2E_CHECK_EQ(ctx, player.clip.total_frames, 10u);
    GKE2E_CHECK_EQ(ctx, player.CurrentSlotDirectory(), trackUtf8);
    GKE2E_CHECK_EQ(ctx, player.SlotBoneOverridesPath(), trackUtf8 + "/bone_overrides.json");
    GKE2E_CHECK_EQ(ctx, player.SlotRootHeightCalibrationPath(), trackUtf8 + "/root_height_calibration.json");
    GKE2E_CHECK_EQ(ctx, player.SlotAuxReplayPath(), trackUtf8 + "/aux_replay.json");
    GKE2E_CHECK_EQ(ctx, player.AudioPath(), trackUtf8 + "/song.wav");

    GKE2E_REQUIRE(ctx, player.LoadMotionSlot(trackUtf8, 2));
    const std::string slot2Utf8 = trackUtf8 + "/positions/slot_2";
    GKE2E_CHECK_EQ(ctx, player.currentMotionSlot, 2);
    GKE2E_CHECK_EQ(ctx, player.clip.total_frames, 25u);
    GKE2E_CHECK_NEAR(ctx, player.clip.fps, 60.0f, 1e-4);
    GKE2E_CHECK_EQ(ctx, player.CurrentSlotDirectory(), slot2Utf8);
    GKE2E_CHECK_EQ(ctx, player.SlotBoneOverridesPath(), slot2Utf8 + "/bone_overrides.json");
    GKE2E_CHECK_EQ(ctx, player.SlotRootHeightCalibrationPath(), slot2Utf8 + "/root_height_calibration.json");
    GKE2E_CHECK_EQ(ctx, player.SlotAuxReplayPath(), slot2Utf8 + "/aux_replay.json");
    GKE2E_CHECK_EQ(ctx, player.AudioPath(), trackUtf8 + "/song.wav");

    GKE2E_REQUIRE(ctx, player.LoadMotionSlot(trackUtf8, 3));
    GKE2E_CHECK_EQ(ctx, player.currentMotionSlot, 3);
    GKE2E_CHECK_EQ(ctx, player.clip.total_frames, 15u);
    GKE2E_CHECK_EQ(ctx, player.AudioPath(), trackUtf8 + "/song.wav");
    GKE2E_PASS(ctx, "MotionPlayer::LoadMotionSlot switched slots 1..3 while keeping root song.wav");
}

void Test_T1_F4_05_MotionPlayerLoadMotionSlot(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T1_F4_05_MotionPlayerLoadMotionSlot<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T1_F4_06_GkCsharpImportRealGroupImport(gke2e::CaseContext& ctx) {
    if (!EnsureNativeAotReady(ctx)) return;
    const fs::path work = CaseWork("T1-F4-06");
    const fs::path inDir = work / "in";
    const fs::path libDir = work / "lib";
    std::error_code ec;
    fs::create_directories(libDir, ec);

    const fs::path v1 = inDir / "s1.vmd";
    const fs::path v2 = inDir / "s2.vmd";
    fix::WriteBinaryFile(v1, fix::make_body_vmd(6, false, 1));
    fix::WriteBinaryFile(v2, fix::make_body_vmd(10, false, 2));

    ReqT req;
    using SlotT = typename std::decay_t<decltype(req.positions)>::value_type;
    req.motionsRoot = GakuMotion::MotionPathToUtf8(libDir);
    req.title = "CppBridgeTwoSlot";
    req.log = GakuMotion::MotionPathToUtf8(work / "import.log");
    req.positionsExplicit = true;
    {
        SlotT p1;
        p1.slot = 1;
        p1.motionVmd = GakuMotion::MotionPathToUtf8(v1);
        p1.disableMorph = true;
        req.positions.push_back(std::move(p1));

        SlotT p2;
        p2.slot = 2;
        p2.motionVmd = GakuMotion::MotionPathToUtf8(v2);
        p2.disableMorph = true;
        p2.x = 1.5f;
        p2.z = -0.5f;
        p2.yawDegrees = 22.5f;
        req.positions.push_back(std::move(p2));
    }

    std::string reason;
    GKE2E_REQUIRE_MSG(ctx, GakuMotion::csharp_import::CanHandle(req, reason), reason);
    GakuMotion::ImportProgressState prog;
    std::string msg;
    const int rc = f4_probes::RunCsharpImportSync(req, msg, &prog);
    GKE2E_REQUIRE_MSG(ctx, rc == 1, msg);
    GKE2E_CHECK(ctx, prog.succeeded);

    const fs::path trackDir = libDir / "CppBridgeTwoSlot";
    GKE2E_CHECK(ctx, fs::is_regular_file(trackDir / "motion.gkclip"));
    GKE2E_CHECK(ctx, fs::is_regular_file(trackDir / "positions" / "slot_2" / "motion.gkclip"));
    GKE2E_CHECK(ctx, fs::is_regular_file(trackDir / "info.json"));
    GKE2E_PASS(ctx, "GkCsharpImport executed real 2-slot NativeAOT group import");
}

void Test_T1_F4_06_GkCsharpImportRealGroupImport(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T1_F4_06_GkCsharpImportRealGroupImport<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

// =============================================================================
// F4 Tier 2 Tests (6 tests >= 5)
// =============================================================================

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T2_F4_01_ParseSlotDirectoryIndexBoundaries(gke2e::CaseContext& ctx) {
    int slot = -1;
    GKE2E_CHECK(ctx, f4_probes::CallParseSlotDirectoryIndex<AdlSvT>("slot_2", slot) && slot == 2);
    GKE2E_CHECK(ctx, f4_probes::CallParseSlotDirectoryIndex<AdlSvT>("slot_256", slot) && slot == 256);

    const char* invalid[] = {"slot_257", "slot_1", "slot_0", "slot_02", "slot_", "slot_abc", "", "pos_2", "slot_-2"};
    for (const char* name : invalid) {
        int out = -1;
        GKE2E_CHECK_MSG(
            ctx,
            !f4_probes::CallParseSlotDirectoryIndex<AdlSvT>(name, out),
            std::string("should reject: ") + name);
    }
    GKE2E_PASS(ctx, "ParseSlotDirectoryIndex BVA [2..256] and invalid formats verified");
}

void Test_T2_F4_01_ParseSlotDirectoryIndexBoundaries(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T2_F4_01_ParseSlotDirectoryIndexBoundaries<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T2_F4_02_CorruptSlot2ClipYieldsPartialStatus(gke2e::CaseContext& ctx) {
    const fs::path work = CaseWork("T2-F4-02");
    const fs::path lib = work / "library";
    const fs::path trackDir = lib / "partial_group";
    fix::write_synthetic_group_track(trackDir, 3, "PartialGroup", "Tester", false, false);
    fix::WriteTextFileUtf8(trackDir / "positions" / "slot_2" / "motion.gkclip", "NOT_A_VALID_GKCLIP_FILE");

    const auto res = GakuMotion::ScanMotionLibraryDetailed(GakuMotion::MotionPathToUtf8(lib));
    GKE2E_CHECK(ctx, res.status == GakuMotion::MotionLibraryScanStatus::Partial);
    GKE2E_CHECK(ctx, !res.warnings.empty());
    GKE2E_REQUIRE_EQ(ctx, res.motions.size(), 1u);

    const EntryT& m = res.motions[0];
    GKE2E_REQUIRE_EQ(ctx, m.positions.size(), 3u);
    GKE2E_CHECK(ctx, m.positions[0].hasMotion);
    GKE2E_CHECK(ctx, !m.positions[1].hasMotion);
    GKE2E_CHECK(ctx, m.positions[1].motionFile.empty());
    GKE2E_CHECK(ctx, m.positions[2].hasMotion);
    GKE2E_PASS(ctx, "Corrupt slot_2 clip yielded Partial status without dropping slots 1 or 3");
}

void Test_T2_F4_02_CorruptSlot2ClipYieldsPartialStatus(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T2_F4_02_CorruptSlot2ClipYieldsPartialStatus<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T2_F4_03_MissingSlotInLoadMotionSlotAndEmptyDir(gke2e::CaseContext& ctx) {
    const fs::path work = CaseWork("T2-F4-03");
    const fs::path lib = work / "library";
    const fs::path trackDir = lib / "empty_slot2_track";
    fix::WriteBinaryFile(trackDir / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 8));
    std::error_code ec;
    fs::create_directories(trackDir / "positions" / "slot_2", ec);

    const std::string trackUtf8 = GakuMotion::MotionPathToUtf8(trackDir);
    PlayerT player;
    GKE2E_REQUIRE(ctx, player.LoadMotionSlot(trackUtf8, 1));
    GKE2E_CHECK_EQ(ctx, player.currentMotionSlot, 1);
    GKE2E_CHECK_EQ(ctx, player.clip.total_frames, 8u);

    GKE2E_CHECK(ctx, !player.LoadMotionSlot(trackUtf8, 2));
    GKE2E_CHECK(ctx, !player.LoadMotionSlot(trackUtf8, 99));
    // Previously loaded slot 1 state remains intact
    GKE2E_CHECK_EQ(ctx, player.currentMotionSlot, 1);
    GKE2E_CHECK_EQ(ctx, player.clip.total_frames, 8u);

    const auto res = GakuMotion::ScanMotionLibraryDetailed(GakuMotion::MotionPathToUtf8(lib));
    GKE2E_REQUIRE_EQ(ctx, res.motions.size(), 1u);
    const EntryT& m = res.motions[0];
    GKE2E_REQUIRE_EQ(ctx, m.positions.size(), 2u);
    GKE2E_CHECK(ctx, m.positions[0].hasMotion);
    GKE2E_CHECK(ctx, !m.positions[1].hasMotion);
    GKE2E_PASS(ctx, "Missing/empty slot rejected by LoadMotionSlot without corrupting prior state");
}

void Test_T2_F4_03_MissingSlotInLoadMotionSlotAndEmptyDir(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T2_F4_03_MissingSlotInLoadMotionSlotAndEmptyDir<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T2_F4_04_InfoJsonOriginClampNonFiniteAndNonContiguous(gke2e::CaseContext& ctx) {
    const fs::path work = CaseWork("T2-F4-04");
    const fs::path lib = work / "library";
    const fs::path trackDir = lib / "dirty_info_group";
    fix::WriteBinaryFile(trackDir / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 5));
    fix::WriteBinaryFile(trackDir / "positions" / "slot_2" / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 5));
    fix::WriteBinaryFile(trackDir / "positions" / "slot_4" / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 5));
    fix::WriteBinaryFile(trackDir / "positions" / "slot_02" / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 5));

    fix::WriteTextFileUtf8(trackDir / "info.json",
        "{\n"
        "  \"title\": \"DirtyInfoGroup\",\n"
        "  \"is_group\": true,\n"
        "  \"position_count\": 3,\n"
        "  \"positions\": [\n"
        "    {\"slot\": 1, \"x\": 5.0, \"y\": -2.0, \"z\": 3.0, \"yaw_degrees\": 90.0},\n"
        "    {\"slot\": 2, \"x\": 1.5, \"y\": \"NaN\", \"z\": 1e999, \"yaw_degrees\": \"invalid\"}\n"
        "  ]\n"
        "}\n");

    const auto res = GakuMotion::ScanMotionLibraryDetailed(GakuMotion::MotionPathToUtf8(lib));
    GKE2E_REQUIRE_EQ(ctx, res.motions.size(), 1u);
    const EntryT& m = res.motions[0];
    GKE2E_CHECK(ctx, m.isGroup);
    GKE2E_REQUIRE_EQ(ctx, m.positions.size(), 3u);
    // Slot 1 forced to 0
    GKE2E_CHECK_EQ(ctx, m.positions[0].slot, 1);
    GKE2E_CHECK_NEAR(ctx, m.positions[0].x, 0.0f, 1e-6);
    GKE2E_CHECK_NEAR(ctx, m.positions[0].y, 0.0f, 1e-6);
    GKE2E_CHECK_NEAR(ctx, m.positions[0].z, 0.0f, 1e-6);
    GKE2E_CHECK_NEAR(ctx, m.positions[0].yawDegrees, 0.0f, 1e-6);
    // Slot 2 sanitized non-finite/non-numeric fields to 0
    GKE2E_CHECK_EQ(ctx, m.positions[1].slot, 2);
    GKE2E_CHECK_NEAR(ctx, m.positions[1].x, 1.5f, 1e-4);
    GKE2E_CHECK_NEAR(ctx, m.positions[1].y, 0.0f, 1e-6);
    GKE2E_CHECK_NEAR(ctx, m.positions[1].z, 0.0f, 1e-6);
    GKE2E_CHECK_NEAR(ctx, m.positions[1].yawDegrees, 0.0f, 1e-6);
    // Slot 4 sorted ascending, slot_02 ignored
    GKE2E_CHECK_EQ(ctx, m.positions[2].slot, 4);
    GKE2E_PASS(ctx, "Scanner clamped slot 1 origin, sanitized non-finite values, and ignored slot_02");
}

void Test_T2_F4_04_InfoJsonOriginClampNonFiniteAndNonContiguous(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T2_F4_04_InfoJsonOriginClampNonFiniteAndNonContiguous<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T2_F4_05_GkCsharpImportInvalidGroupRequests(gke2e::CaseContext& ctx) {
    using SlotT = typename std::decay_t<decltype(std::declval<ReqT&>().positions)>::value_type;
    std::string reason;
    // 1. positionsExplicit=true with empty positions
    {
        ReqT req;
        req.vmd = "a.vmd";
        req.positionsExplicit = true;
        GKE2E_CHECK(ctx, !GakuMotion::csharp_import::CanHandle(req, reason) && !reason.empty());
        GKE2E_CHECK(ctx, !GakuMotion::csharp_import::Begin(req, reason) && !reason.empty());
    }
    // 2. slot < 1
    {
        ReqT req;
        SlotT p;
        p.slot = 0;
        p.motionVmd = "a.vmd";
        req.positions.push_back(p);
        GKE2E_CHECK(ctx, !GakuMotion::csharp_import::CanHandle(req, reason) && !reason.empty());
    }
    // 3. sourceSlot < 0
    {
        ReqT req;
        SlotT p;
        p.slot = 1;
        p.sourceSlot = -1;
        p.motionVmd = "a.vmd";
        req.positions.push_back(p);
        GKE2E_CHECK(ctx, !GakuMotion::csharp_import::CanHandle(req, reason) && !reason.empty());
    }
    // 4. NaN / Inf coordinates
    {
        ReqT req;
        SlotT p;
        p.slot = 1;
        p.motionVmd = "a.vmd";
        p.x = std::numeric_limits<float>::quiet_NaN();
        req.positions.push_back(p);
        GKE2E_CHECK(ctx, !GakuMotion::csharp_import::CanHandle(req, reason) && !reason.empty());
        req.positions[0].x = 0.0f;
        req.positions[0].yawDegrees = std::numeric_limits<float>::infinity();
        GKE2E_CHECK(ctx, !GakuMotion::csharp_import::CanHandle(req, reason) && !reason.empty());
    }
    // 5. Negative sourceFps
    {
        ReqT req;
        SlotT p;
        p.slot = 1;
        p.motionVmd = "a.vmd";
        p.sourceFps = -30.0f;
        req.positions.push_back(p);
        GKE2E_CHECK(ctx, !GakuMotion::csharp_import::CanHandle(req, reason) && !reason.empty());
    }
    // 6. Empty vmd, empty positions, empty editDir
    {
        ReqT req;
        GKE2E_CHECK(ctx, !GakuMotion::csharp_import::CanHandle(req, reason) && !reason.empty());
    }
    GKE2E_PASS(ctx, "GkCsharpImport::CanHandle and Begin rejected all invalid group requests");
}

void Test_T2_F4_05_GkCsharpImportInvalidGroupRequests(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T2_F4_05_GkCsharpImportInvalidGroupRequests<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T2_F4_06_UpdateTrackPlacementNegativesAndKeyPreservation(gke2e::CaseContext& ctx) {
    const fs::path work = CaseWork("T2-F4-06");
    std::string err;

    // 1. Empty dir & missing info.json
    GKE2E_CHECK(ctx, !f4_probes::CallUpdatePlacement<AdlPathT>(fs::path{}, 2, 1.0f, 0.0f, 0.0f, 0.0f, err) && !err.empty());
    GKE2E_CHECK(ctx, !f4_probes::CallUpdatePlacement<AdlPathT>(work / "missing", 2, 1.0f, 0.0f, 0.0f, 0.0f, err) && !err.empty());

    // 2. Empty updates vector
    GKE2E_CHECK(ctx, !f4_probes::CallUpdatePlacementsEmpty<AdlPathT>(work, err) && !err.empty());

    // 3. info.json missing positions array
    const fs::path noPosDir = work / "no_pos";
    fix::WriteTextFileUtf8(noPosDir / "info.json", "{\"title\": \"NoPositions\"}\n");
    GKE2E_CHECK(ctx, !f4_probes::CallUpdatePlacement<AdlPathT>(noPosDir, 2, 1.0f, 0.0f, 0.0f, 0.0f, err) && !err.empty());

    // 4. Valid group info.json with custom unknown keys
    const fs::path grpDir = work / "grp";
    fix::WriteTextFileUtf8(grpDir / "info.json",
        "{\n"
        "  \"title\": \"KeepUnknownKeys\",\n"
        "  \"custom_top\": \"preserve_me\",\n"
        "  \"is_group\": true,\n"
        "  \"position_count\": 2,\n"
        "  \"positions\": [\n"
        "    {\"slot\": 1, \"x\": 0, \"y\": 0, \"z\": 0, \"yaw_degrees\": 0},\n"
        "    {\"slot\": 2, \"x\": 1.0, \"y\": 0.0, \"z\": 0.0, \"yaw_degrees\": 15.0, \"custom_note\": \"keep_slot2\"}\n"
        "  ]\n"
        "}\n");

    const auto beforeBytes = f4_probes::ReadFileBytes(grpDir / "info.json");
    GKE2E_CHECK(ctx, !f4_probes::CallUpdatePlacement<AdlPathT>(grpDir, 0, 1.0f, 0.0f, 0.0f, 0.0f, err));
    GKE2E_CHECK(ctx, !f4_probes::CallUpdatePlacement<AdlPathT>(grpDir, 99, 1.0f, 0.0f, 0.0f, 0.0f, err));
    GKE2E_CHECK(ctx, !f4_probes::CallUpdatePlacement<AdlPathT>(grpDir, 2, std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f, err));
    GKE2E_CHECK(ctx, !f4_probes::CallUpdatePlacement<AdlPathT>(grpDir, 2, 0.0f, 0.0f, 0.0f, std::numeric_limits<float>::infinity(), err));
    GKE2E_CHECK(ctx, f4_probes::ReadFileBytes(grpDir / "info.json") == beforeBytes);

    // 5. Update with -0.0f and check unknown keys preserved
    GKE2E_REQUIRE_MSG(ctx, f4_probes::CallUpdatePlacement<AdlPathT>(grpDir, 2, -0.0f, 0.5f, -2.25f, -90.0f, err), err);
    const auto afterBytes = f4_probes::ReadFileBytes(grpDir / "info.json");
    const std::string afterStr(afterBytes.begin(), afterBytes.end());
    GKE2E_CHECK(ctx, afterStr.find("preserve_me") != std::string::npos);
    GKE2E_CHECK(ctx, afterStr.find("keep_slot2") != std::string::npos);
    GKE2E_CHECK(ctx, afterStr.find("-0,") == std::string::npos);
    GKE2E_CHECK(ctx, afterStr.find("-0\n") == std::string::npos);
    GKE2E_CHECK(ctx, afterStr.find("-0\r") == std::string::npos);
    GKE2E_CHECK(ctx, afterStr.find("-0 ") == std::string::npos);
    GKE2E_CHECK(ctx, afterStr.find("-0}") == std::string::npos);
    GKE2E_PASS(ctx, "UpdateTrackInfoJsonPositionPlacement negatives, -0 normalization, and key preservation verified");
}

void Test_T2_F4_06_UpdateTrackPlacementNegativesAndKeyPreservation(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T2_F4_06_UpdateTrackPlacementNegativesAndKeyPreservation<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

// =============================================================================
// Tier 3 Pairwise Cross-Feature Tests (2 C++ tests)
// =============================================================================

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T3_F2F4_01_NativeAot4SlotToScanAndLoad(gke2e::CaseContext& ctx) {
    if (!EnsureNativeAotReady(ctx)) return;
    const fs::path work = CaseWork("T3-F2F4-01");
    const fs::path inDir = work / "in";
    const fs::path libDir = work / "lib";
    std::error_code ec;
    fs::create_directories(libDir, ec);

    const fs::path camVmd = inDir / "cam.vmd";
    const fs::path wavFile = work / "audio" / "bgm.wav";
    const fs::path morph2 = work / "morph" / "m2.vmd";
    fix::WriteBinaryFile(camVmd, fix::make_camera_vmd(12, 42));
    fix::write_pcm_wav(wavFile, 0.7);
    fix::WriteBinaryFile(morph2, fix::make_morph_vmd(8));

    ReqT req;
    using SlotT = typename std::decay_t<decltype(req.positions)>::value_type;
    req.motionsRoot = GakuMotion::MotionPathToUtf8(libDir);
    req.title = "CrossF2F4FourSlot";
    req.cameraVmd = GakuMotion::MotionPathToUtf8(camVmd);
    req.audio = GakuMotion::MotionPathToUtf8(wavFile);
    req.log = GakuMotion::MotionPathToUtf8(work / "import.log");
    req.positionsExplicit = true;

    for (int k = 1; k <= 4; ++k) {
        const fs::path v = inDir / ("slot_" + std::to_string(k) + ".vmd");
        fix::WriteBinaryFile(v, fix::make_body_vmd(4 + static_cast<uint32_t>(k) * 3, false, static_cast<uint32_t>(k)));
        SlotT p;
        p.slot = k;
        p.motionVmd = GakuMotion::MotionPathToUtf8(v);
        if (k == 2) p.morphVmd = GakuMotion::MotionPathToUtf8(morph2);
        else p.disableMorph = true;
        if (k >= 2) {
            p.x = static_cast<float>(k - 1) * 0.9f;
            p.z = static_cast<float>(k - 1) * -0.4f;
            p.yawDegrees = static_cast<float>(k - 1) * 12.0f;
        }
        req.positions.push_back(std::move(p));
    }

    std::string msg;
    GKE2E_REQUIRE_MSG(ctx, f4_probes::RunCsharpImportSync(req, msg) == 1, msg);

    const auto snap = GakuMotion::ReadLibrarySnapshot(GakuMotion::MotionPathToUtf8(libDir));
    GKE2E_REQUIRE(ctx, snap.result.status == GakuMotion::MotionLibraryScanStatus::Complete);
    GKE2E_REQUIRE_EQ(ctx, snap.result.motions.size(), 1u);
    const EntryT& m = snap.result.motions[0];
    GKE2E_CHECK(ctx, m.isGroup);
    GKE2E_CHECK_EQ(ctx, m.positionCount, 4);
    GKE2E_CHECK(ctx, m.hasCamera);
    GKE2E_CHECK(ctx, m.hasAudio);
    GKE2E_CHECK(ctx, m.hasFacial);
    GKE2E_REQUIRE_EQ(ctx, m.positions.size(), 4u);
    GKE2E_CHECK(ctx, !m.positions[0].hasFacial);
    GKE2E_CHECK(ctx, m.positions[1].hasFacial);

    PlayerT player;
    for (int k = 1; k <= 4; ++k) {
        GKE2E_REQUIRE(ctx, player.LoadMotionSlot(m.dir, k));
        GKE2E_CHECK_EQ(ctx, player.currentMotionSlot, k);
        GKE2E_CHECK_EQ(ctx, player.clip.total_frames, m.positions[static_cast<size_t>(k - 1)].frames);
        GKE2E_CHECK_EQ(ctx, player.AudioPath(), m.dir + "/" + m.audioFile);
    }

    GakuMotion::CameraTrack camTrack;
    GKE2E_CHECK(ctx, camTrack.Load(m.dir + "/camera.gkcamera"));
    GKE2E_CHECK(ctx, !camTrack.frames.empty());
    GKE2E_PASS(ctx, "F2 4-slot NativeAOT import -> F4 scan, snapshot, LoadMotionSlot(1..4), CameraTrack::Load verified");
}

void Test_T3_F2F4_01_NativeAot4SlotToScanAndLoad(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T3_F2F4_01_NativeAot4SlotToScanAndLoad<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T3_F3F4_01_SelectiveEditAndPlacementSyncWithWatcher(gke2e::CaseContext& ctx) {
    if (!EnsureNativeAotReady(ctx)) return;
    const fs::path work = CaseWork("T3-F3F4-01");
    const fs::path inDir = work / "in";
    const fs::path libDir = work / "lib";
    std::error_code ec;
    fs::create_directories(libDir, ec);

    ReqT req;
    using SlotT = typename std::decay_t<decltype(req.positions)>::value_type;
    req.motionsRoot = GakuMotion::MotionPathToUtf8(libDir);
    req.title = "CrossF3F4";
    req.log = GakuMotion::MotionPathToUtf8(work / "import.log");
    req.positionsExplicit = true;
    for (int k = 1; k <= 4; ++k) {
        const fs::path v = inDir / ("init_" + std::to_string(k) + ".vmd");
        fix::WriteBinaryFile(v, fix::make_body_vmd(6, false, static_cast<uint32_t>(k)));
        SlotT p;
        p.slot = k;
        p.motionVmd = GakuMotion::MotionPathToUtf8(v);
        p.disableMorph = true;
        p.x = static_cast<float>(k - 1);
        req.positions.push_back(std::move(p));
    }
    std::string msg;
    GKE2E_REQUIRE_MSG(ctx, f4_probes::RunCsharpImportSync(req, msg) == 1, msg);
    const fs::path trackDir = libDir / "CrossF3F4";
    const std::string trackUtf8 = GakuMotion::MotionPathToUtf8(trackDir);

    const VerT ver1 = GakuMotion::ReadMotionFileVersion(trackUtf8);
    const auto clip1Bytes = f4_probes::ReadFileBytes(trackDir / "motion.gkclip");
    const auto clip4Bytes = f4_probes::ReadFileBytes(trackDir / "positions" / "slot_4" / "motion.gkclip");

    // 1. Zero-rebake C++ UpdateTrackInfoJsonPositionPlacement on slot 3
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::string err;
    GKE2E_REQUIRE_MSG(ctx, f4_probes::CallUpdatePlacement<AdlPathT>(trackDir, 3, 4.5f, 0.2f, -1.0f, 35.0f, err), err);
    const VerT ver2 = GakuMotion::ReadMotionFileVersion(trackUtf8);
    GKE2E_CHECK(ctx, ver2 != ver1);
    GKE2E_CHECK(ctx, ver2.files[0] == ver1.files[0]); // motion.gkclip unchanged
    GKE2E_CHECK(ctx, ver2.slotFiles == ver1.slotFiles); // slot dirs unchanged

    // 2. F3 selective slot-2 swap + delete slot 3 (slot 4 -> slot 3) via GkCsharpImport
    const fs::path swapVmd = inDir / "swap2.vmd";
    fix::WriteBinaryFile(swapVmd, fix::make_body_vmd(20, false, 99));
    ReqT editReq;
    editReq.editDir = trackUtf8;
    editReq.log = GakuMotion::MotionPathToUtf8(work / "edit_import.log");
    editReq.positionsExplicit = true;
    {
        SlotT s1; s1.slot = 1; s1.sourceSlot = 1; s1.preserveExisting = true;
        SlotT s2; s2.slot = 2; s2.sourceSlot = 2; s2.motionVmd = GakuMotion::MotionPathToUtf8(swapVmd); s2.x = 1.0f;
        SlotT s3; s3.slot = 3; s3.sourceSlot = 4; s3.preserveExisting = true; s3.x = 3.0f;
        editReq.positions = {s1, s2, s3};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    GKE2E_REQUIRE_MSG(ctx, f4_probes::RunCsharpImportSync(editReq, msg) == 1, msg);

    GKE2E_CHECK(ctx, f4_probes::ReadFileBytes(trackDir / "motion.gkclip") == clip1Bytes);
    GKE2E_CHECK(ctx, f4_probes::ReadFileBytes(trackDir / "positions" / "slot_3" / "motion.gkclip") == clip4Bytes);

    const VerT ver3 = GakuMotion::ReadMotionFileVersion(trackUtf8);
    GKE2E_CHECK(ctx, ver3 != ver2);
    GKE2E_REQUIRE_EQ(ctx, ver3.slotFiles.size(), 2u);
    GKE2E_CHECK_EQ(ctx, ver3.slotFiles[0].slot, 2);
    GKE2E_CHECK_EQ(ctx, ver3.slotFiles[1].slot, 3);

    const auto scan = GakuMotion::ScanMotionLibraryDetailed(GakuMotion::MotionPathToUtf8(libDir));
    GKE2E_REQUIRE_EQ(ctx, scan.motions.size(), 1u);
    const EntryT& scanEntry = scan.motions[0];
    GKE2E_CHECK_EQ(ctx, scanEntry.positionCount, 3);

    PlayerT player;
    GKE2E_REQUIRE(ctx, player.LoadMotionSlot(trackUtf8, 2));
    GKE2E_CHECK(ctx, player.clip.total_frames > 10u);
    GKE2E_REQUIRE(ctx, player.LoadMotionSlot(trackUtf8, 3));
    GKE2E_PASS(ctx, "F3 placement update & selective swap/renumber -> F4 watcher diff and LoadMotionSlot verified");
}

void Test_T3_F3F4_01_SelectiveEditAndPlacementSyncWithWatcher(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T3_F3F4_01_SelectiveEditAndPlacementSyncWithWatcher<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

// =============================================================================
// Tier 4 Real-World Scenario (1 C++ scenario)
// =============================================================================

template <typename ReqT, typename EntryT, typename VerT, typename PlayerT,
          typename AdlPathT, typename AdlStrT, typename AdlSvT>
void Impl_T4_NA_CPP_01_FullCppLibraryLifecycle(gke2e::CaseContext& ctx) {
    if (!EnsureNativeAotReady(ctx)) return;
    const fs::path work = CaseWork("T4-NA-CPP-01");
    const fs::path inDir = work / "in";
    const fs::path libDir = work / "library";
    std::error_code ec;
    fs::create_directories(libDir, ec);

    // 1. Import UTF-8 CJK 3-idol group song via GkCsharpImport
    const fs::path camVmd = inDir / "cam.vmd";
    const fs::path wavFile = work / "audio" / "stage.wav";
    fix::WriteBinaryFile(camVmd, fix::make_camera_vmd(16, 45));
    fix::write_pcm_wav(wavFile, 0.9);

    ReqT grpReq;
    using SlotT = typename std::decay_t<decltype(grpReq.positions)>::value_type;
    grpReq.motionsRoot = GakuMotion::MotionPathToUtf8(libDir);
    grpReq.title = "\xE5\x9B\xA2\xE6\x9B\xB2\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88_\xE6\xA1\x9C"; // 团曲テスト_桜
    grpReq.cameraVmd = GakuMotion::MotionPathToUtf8(camVmd);
    grpReq.audio = GakuMotion::MotionPathToUtf8(wavFile);
    grpReq.log = GakuMotion::MotionPathToUtf8(work / "import.log");
    grpReq.positionsExplicit = true;
    for (int k = 1; k <= 3; ++k) {
        const fs::path v = inDir / ("idol_" + std::to_string(k) + ".vmd");
        fix::WriteBinaryFile(v, fix::make_body_vmd(6 + static_cast<uint32_t>(k) * 2, k == 1, static_cast<uint32_t>(k)));
        SlotT p;
        p.slot = k;
        p.motionVmd = GakuMotion::MotionPathToUtf8(v);
        p.x = (k == 1) ? 0.0f : static_cast<float>(k - 1) * 1.1f;
        grpReq.positions.push_back(std::move(p));
    }

    std::string msg;
    GKE2E_REQUIRE_MSG(ctx, f4_probes::RunCsharpImportSync(grpReq, msg) == 1, msg);

    // 2. Add a solo track and a fallback group track (without info.json)
    fix::TrackInfoSpec soloSpec;
    soloSpec.title = "LegacySolo";
    soloSpec.is_group = false;
    soloSpec.position_count = 1;
    soloSpec.positions = {fix::PositionSlotSpec{1}};
    fix::write_synthetic_track_dir(libDir / "zz_solo", soloSpec, false, true);

    const fs::path fbDir = libDir / "zz_fallback_grp";
    fix::WriteBinaryFile(fbDir / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 7));
    fix::WriteBinaryFile(fbDir / "positions" / "slot_2" / "motion.gkclip", fix::make_minimal_gkclip_bytes(30.0f, 7));

    // 3. ReadLibrarySnapshot across all 3 tracks
    const auto snap1 = GakuMotion::ReadLibrarySnapshot(GakuMotion::MotionPathToUtf8(libDir));
    GKE2E_REQUIRE(ctx, snap1.result.status == GakuMotion::MotionLibraryScanStatus::Complete);
    GKE2E_REQUIRE_EQ(ctx, snap1.result.motions.size(), 3u);

    const EntryT* cjkEntry = nullptr;
    for (const EntryT& entry : snap1.result.motions) {
        if (entry.title == grpReq.title) {
            cjkEntry = &entry;
            break;
        }
    }
    GKE2E_REQUIRE(ctx, cjkEntry != nullptr);
    GKE2E_CHECK(ctx, cjkEntry->isGroup);
    GKE2E_CHECK_EQ(ctx, cjkEntry->positionCount, 3);
    const fs::path cjkTrackPath = GakuMotion::MotionPathFromUtf8(cjkEntry->dir);
    const auto cjkClip1 = f4_probes::ReadFileBytes(cjkTrackPath / "motion.gkclip");
    const auto cjkClip3 = f4_probes::ReadFileBytes(cjkTrackPath / "positions" / "slot_3" / "motion.gkclip");

    // 4. Zero-rebake C++ placement update + per-slot sidecar edit + watcher diff
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::string err;
    GKE2E_REQUIRE_MSG(ctx, f4_probes::CallUpdatePlacement<AdlPathT>(cjkTrackPath, 2, 2.25f, 0.0f, -0.75f, 20.0f, err), err);
    f4_probes::WriteSlotSidecars(cjkTrackPath / "positions" / "slot_3", "cjk_s3");

    const auto snap2 = GakuMotion::ReadLibrarySnapshot(GakuMotion::MotionPathToUtf8(libDir));
    const auto vBeforeIt = snap1.versions.find(cjkEntry->name);
    const auto vAfterIt = snap2.versions.find(cjkEntry->name);
    GKE2E_REQUIRE(ctx, vBeforeIt != snap1.versions.end() && vAfterIt != snap2.versions.end());
    const VerT& vBefore = vBeforeIt->second;
    const VerT& vAfter = vAfterIt->second;
    GKE2E_CHECK(ctx, vBefore != vAfter);
    GKE2E_CHECK(ctx, vBefore.files[4] == vAfter.files[4]); // audio unchanged

    // 5. Selective slot 2 deletion (slot 3 -> slot 2) via GkCsharpImport & reload
    ReqT delReq;
    delReq.editDir = cjkEntry->dir;
    delReq.log = GakuMotion::MotionPathToUtf8(work / "del_import.log");
    delReq.positionsExplicit = true;
    {
        SlotT s1; s1.slot = 1; s1.sourceSlot = 1; s1.preserveExisting = true;
        SlotT s2; s2.slot = 2; s2.sourceSlot = 3; s2.preserveExisting = true; s2.x = 1.75f;
        delReq.positions = {s1, s2};
    }
    GKE2E_REQUIRE_MSG(ctx, f4_probes::RunCsharpImportSync(delReq, msg) == 1, msg);
    GKE2E_CHECK(ctx, f4_probes::ReadFileBytes(cjkTrackPath / "motion.gkclip") == cjkClip1);
    GKE2E_CHECK(ctx, f4_probes::ReadFileBytes(cjkTrackPath / "positions" / "slot_2" / "motion.gkclip") == cjkClip3);

    // 6. MotionPlayer switches across group slots 1..2 and solo track + CameraTrack load
    PlayerT player;
    GKE2E_REQUIRE(ctx, player.LoadMotionSlot(cjkEntry->dir, 1));
    GKE2E_CHECK_EQ(ctx, player.currentMotionSlot, 1);
    GKE2E_REQUIRE(ctx, player.LoadMotionSlot(cjkEntry->dir, 2));
    GKE2E_CHECK_EQ(ctx, player.currentMotionSlot, 2);
    GKE2E_CHECK_EQ(ctx, player.AudioPath(), cjkEntry->dir + "/" + cjkEntry->audioFile);

    GakuMotion::CameraTrack camTrack;
    GKE2E_CHECK(ctx, camTrack.Load(cjkEntry->dir + "/camera.gkcamera"));
    GKE2E_CHECK(ctx, !camTrack.frames.empty());

    GKE2E_REQUIRE(ctx, player.LoadMotionSlot(GakuMotion::MotionPathToUtf8(libDir / "zz_solo"), 1));
    GKE2E_CHECK_EQ(ctx, player.currentMotionSlot, 1);
    GKE2E_PASS(ctx, "Full C++ + NativeAOT UTF-8 CJK lifecycle verified across scan/watch/edit/load/camera");
}

void Test_T4_NA_CPP_01_FullCppLibraryLifecycle(gke2e::CaseContext& ctx) {
    DispatchF4Test(ctx, []<typename ReqT, typename EntryT, typename VerT, typename PlayerT,
                           typename AdlPathT, typename AdlStrT, typename AdlSvT>(gke2e::CaseContext& c) {
        Impl_T4_NA_CPP_01_FullCppLibraryLifecycle<ReqT, EntryT, VerT, PlayerT, AdlPathT, AdlStrT, AdlSvT>(c);
    });
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> utf8Args;
#ifdef _WIN32
    int wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        utf8Args.reserve(static_cast<std::size_t>(wargc));
        for (int i = 0; i < wargc; ++i) {
            utf8Args.push_back(WideToUtf8(wargv[i]));
        }
        LocalFree(wargv);
    }
#endif
    if (utf8Args.empty()) {
        utf8Args.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            utf8Args.emplace_back(argv[i] ? argv[i] : "");
        }
    }

    for (std::size_t i = 1; i < utf8Args.size(); ++i) {
        const std::string_view arg = utf8Args[i];
        if (arg == "--nativeaot-dir" && i + 1 < utf8Args.size()) {
            g_cli.nativeAotDir = GakuMotion::MotionPathFromUtf8(utf8Args[++i]);
        } else if (arg == "--work-dir" && i + 1 < utf8Args.size()) {
            g_cli.workDir = GakuMotion::MotionPathFromUtf8(utf8Args[++i]);
        }
    }
    if (g_cli.workDir.empty()) {
        g_cli.workDir = fs::current_path() / "cpp_work";
    }
    std::error_code dirEc;
    fs::create_directories(g_cli.workDir, dirEc);
    if (dirEc) {
        std::cerr << "[gke2e] Failed to create --work-dir '"
                  << GakuMotion::MotionPathToUtf8(g_cli.workDir)
                  << "': " << dirEc.message() << '\n';
        return 2;
    }

    gke2e::Runner runner;
    // F4 Tier 1
    runner.Add("T1-F4-01", 1, "F4", "Solo song backward-compatible scan", Test_T1_F4_01_SoloBackwardCompatibleScan);
    runner.Add("T1-F4-02", 1, "F4", "3-slot group scan and isolated slot metadata", Test_T1_F4_02_ThreeSlotGroupScanAndIsolatedMetadata);
    runner.Add("T1-F4-03", 1, "F4", "Fallback group discovery without info.json", Test_T1_F4_03_FallbackGroupDiscoveryWithoutInfoJson);
    runner.Add("T1-F4-04", 1, "F4", "GkLibraryState watcher per-slot diff preserving root audio", Test_T1_F4_04_LibraryStateWatcherPerSlotDiff);
    runner.Add("T1-F4-05", 1, "F4", "MotionPlayer::LoadMotionSlot across slots 1..N with shared audio", Test_T1_F4_05_MotionPlayerLoadMotionSlot);
    runner.Add("T1-F4-06", 1, "F4", "GkCsharpImport real NativeAOT 2-slot group import", Test_T1_F4_06_GkCsharpImportRealGroupImport);
    // F4 Tier 2
    runner.Add("T2-F4-01", 2, "F4", "ParseSlotDirectoryIndex boundaries and invalid formats", Test_T2_F4_01_ParseSlotDirectoryIndexBoundaries);
    runner.Add("T2-F4-02", 2, "F4", "Corrupt positions/slot_2/motion.gkclip yields Partial status", Test_T2_F4_02_CorruptSlot2ClipYieldsPartialStatus);
    runner.Add("T2-F4-03", 2, "F4", "Missing/empty slot in LoadMotionSlot and scanner", Test_T2_F4_03_MissingSlotInLoadMotionSlotAndEmptyDir);
    runner.Add("T2-F4-04", 2, "F4", "info.json slot-1 clamp, non-finite coords, non-contiguous slots", Test_T2_F4_04_InfoJsonOriginClampNonFiniteAndNonContiguous);
    runner.Add("T2-F4-05", 2, "F4", "GkCsharpImport::CanHandle and Begin invalid group requests", Test_T2_F4_05_GkCsharpImportInvalidGroupRequests);
    runner.Add("T2-F4-06", 2, "F4", "UpdateTrackInfoJsonPositionPlacement negatives, -0, key preservation", Test_T2_F4_06_UpdateTrackPlacementNegativesAndKeyPreservation);
    // Tier 3
    runner.Add("T3-F2F4-01", 3, "F4", "F2 NativeAOT 4-slot import -> F4 scan, snapshot, LoadMotionSlot, CameraTrack", Test_T3_F2F4_01_NativeAot4SlotToScanAndLoad);
    runner.Add("T3-F3F4-01", 3, "F4", "F3 selective edit & placement sync -> F4 watcher diff and LoadMotionSlot", Test_T3_F3F4_01_SelectiveEditAndPlacementSyncWithWatcher);
    // Tier 4
    runner.Add("T4-NA-CPP-01", 4, "F4", "Full C++ + NativeAOT UTF-8 CJK group song lifecycle", Test_T4_NA_CPP_01_FullCppLibraryLifecycle);

    std::vector<char*> utf8Ptrs;
    utf8Ptrs.reserve(utf8Args.size());
    for (auto& s : utf8Args) {
        utf8Ptrs.push_back(s.data());
    }
    const int rc = runner.Run(static_cast<int>(utf8Ptrs.size()), utf8Ptrs.data());
    GakuMotion::csharp_import::Shutdown();
    return rc;
}
