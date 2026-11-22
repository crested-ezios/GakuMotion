#include "multi_actor_fixture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

GK_E2E_DEFINE_LOG_SINK()

namespace {

using namespace GakuMotion;
using namespace gkmotion::e2e::multi_actor;
namespace fx = gkmotion::e2e::fixtures;

void ReopenCoreWorkspace(Core& core, const TempWorkspace& ws) {
    core.initialized = true;
    core.SetControlActive(true);
    core.configRoot = MotionPathToUtf8(ws.Sub("config"));
    core.motionsRoot = MotionPathToUtf8(ws.Sub("motions"));
    core.playlistPath = MotionPathToUtf8(ws.Sub("config/playlist.json"));
    core.initialPositionPath = MotionPathToUtf8(ws.Sub("config/initial_position.json"));
    core.globalOverridePath = MotionPathToUtf8(ws.Sub("config/bone_overrides.json"));
    WireIdolController(core.player.controller);
    WireCoreAudioCounters(core);
    core.RefreshMotionLibrary();
}

void InitCoreWorkspace(Core& core, const TempWorkspace& ws, int groupSlots = 0) {
    std::error_code ec;
    std::filesystem::create_directories(ws.Sub("config"), ec);
    std::filesystem::create_directories(ws.Sub("motions"), ec);

    if (groupSlots >= 2) {
        const auto trackDir = ws.Sub("motions/group_song");
        fx::write_synthetic_group_track(trackDir, groupSlots, "Group Song", "Tester", true, true);
    } else {
        fx::TrackInfoSpec solo;
        solo.title = "Solo Song";
        solo.subtitle = "Tester";
        solo.total_frames = 5;
        solo.fps = 30.0f;
        solo.duration_seconds = 4.0f / 30.0f;
        solo.is_group = false;
        solo.position_count = 1;
        solo.audio_file = "song.wav";
        solo.audio_duration = 0.5f;
        fx::write_synthetic_track_dir(ws.Sub("motions/solo_song"), solo, true, true);
    }
    ReopenCoreWorkspace(core, ws);
}

template <typename CoreT>
FakeActor* FindActorByToken(const CoreT& core, uint64_t token) {
    if (token == 0) return nullptr;
    return static_cast<FakeActor*>(
        core.sceneActorRegistry.Resolve(token, core.sceneActorRegistry.Epoch()));
}

template <typename SlotT>
Vector3 SlotOffsetVec3(const SlotT& slot) {
    return Vector3{slot.offset.x, slot.offset.y, slot.offset.z};
}

template <typename SlotT>
float SlotYawDeg(const SlotT& slot) {
    if constexpr (requires(const SlotT& s) { s.yawDegrees; }) {
        return slot.yawDegrees;
    } else if constexpr (requires(const SlotT& s) { s.offset.yawDegrees; }) {
        return slot.offset.yawDegrees;
    } else {
        return 0.0f;
    }
}

inline MotionClip MakeTestHipsClip(const Vector3& vmdRootPos = {0.0f, 0.0f, 0.0f},
                                    const Quaternion& hipsRot = Quaternion::Identity()) {
    MotionClip clip;
    clip.fps = 30.0f;
    clip.total_frames = 1;
    clip.duration_seconds = 0.0f;
    clip.bones = {{0, "Hips", false}};
    clip.bone_count = 1;
    clip.root_positions = {vmdRootPos};
    clip.bone_rotations = {hipsRot};
    return clip;
}

inline void ConfigureDirectFollowCamera(CameraController& cam) {
    cam.settings.enabled = true;
    cam.settings.mode = CameraMode::Follow;
    cam.settings.followBehavior = FollowBehavior::FollowMove;
    cam.settings.followOffset = {0.0f, 0.0f, 0.0f};
    cam.settings.transformFollowMove.position = {0.0f, 0.0f, 0.0f};
    cam.settings.followFactor = {1.0f, 1.0f, 1.0f};
    cam.settings.followRate = {1.0f, 1.0f, 1.0f};
}

template <typename Ctrl>
bool TrySetAnchor(Ctrl& ctrl, const Vector3& pos, const Quaternion& rot) {
    if constexpr (probes::HasSetReferenceAnchor<Ctrl>) {
        ctrl.SetReferenceAnchor(pos, rot);
        return true;
    } else if constexpr (probes::HasSetAnchorOverride<Ctrl>) {
        ctrl.SetAnchorOverride(pos, rot);
        return true;
    } else {
        (void)ctrl; (void)pos; (void)rot;
        return false;
    }
}

template <typename Ctrl>
bool TryClearAnchor(Ctrl& ctrl) {
    if constexpr (probes::HasClearReferenceAnchor<Ctrl>) {
        ctrl.ClearReferenceAnchor();
        return true;
    } else if constexpr (probes::HasClearAnchorOverride<Ctrl>) {
        ctrl.ClearAnchorOverride();
        return true;
    } else {
        (void)ctrl;
        return false;
    }
}

template <typename CoreT>
bool TrySelectSlot(CoreT& c, int slotIndex) {
    if constexpr (probes::HasSelectFormationSlot<CoreT>) {
        return c.SelectFormationSlot(slotIndex);
    } else if constexpr (probes::HasSelectSlot<CoreT>) {
        return c.SelectSlot(slotIndex);
    } else if constexpr (probes::HasFormationState<CoreT>) {
        if constexpr (probes::HasSelectSlot<decltype(c.formation)>) {
            return c.formation.SelectSlot(slotIndex);
        }
        return false;
    } else if constexpr (probes::HasStageFormationMember<CoreT>) {
        if constexpr (probes::HasSelectSlot<decltype(c.stageFormation)>) {
            return c.stageFormation.SelectSlot(slotIndex);
        }
        return false;
    } else {
        (void)c; (void)slotIndex;
        return false;
    }
}

template <typename CoreT>
bool TrySwapSlots(CoreT& c, int fromSlot, int toSlot) {
    if constexpr (probes::HasSwapFormationSlots<CoreT>) {
        return c.SwapFormationSlots(fromSlot, toSlot);
    } else if constexpr (probes::HasSwapSlots<CoreT>) {
        return c.SwapSlots(fromSlot, toSlot);
    } else if constexpr (probes::HasFormationState<CoreT>) {
        if constexpr (probes::HasSwapSlots<decltype(c.formation)>) {
            return c.formation.SwapSlots(fromSlot, toSlot);
        }
        return false;
    } else if constexpr (probes::HasStageFormationMember<CoreT>) {
        if constexpr (probes::HasSwapSlots<decltype(c.stageFormation)>) {
            return c.stageFormation.SwapSlots(fromSlot, toSlot);
        }
        return false;
    } else {
        (void)c; (void)fromSlot; (void)toSlot;
        return false;
    }
}

template <typename CoreT>
bool TrySetSlotOffset(CoreT& c, int slotIndex, const Vector3& offset, float yawDeg) {
    if constexpr (probes::HasSetFormationSlotOffset<CoreT>) {
        return c.SetFormationSlotOffset(slotIndex, offset, yawDeg);
    } else if constexpr (probes::HasSetSlotOffset<CoreT>) {
        return c.SetSlotOffset(slotIndex, offset, yawDeg);
    } else if constexpr (probes::HasFormationState<CoreT>) {
        if constexpr (probes::HasSetSlotOffset<decltype(c.formation)>) {
            return c.formation.SetSlotOffset(slotIndex, offset, yawDeg);
        }
        return false;
    } else if constexpr (probes::HasStageFormationMember<CoreT>) {
        if constexpr (probes::HasSetSlotOffset<decltype(c.stageFormation)>) {
            return c.stageFormation.SetSlotOffset(slotIndex, offset, yawDeg);
        }
        return false;
    } else {
        (void)c; (void)slotIndex; (void)offset; (void)yawDeg;
        return false;
    }
}

template <typename CoreT>
bool TrySaveSlotOffset(CoreT& c, int slotIndex) {
    if constexpr (probes::HasSaveFormationSlotOffset<CoreT>) {
        return c.SaveFormationSlotOffset(slotIndex);
    } else if constexpr (probes::HasSaveSlotOffset<CoreT>) {
        return c.SaveSlotOffset(slotIndex);
    } else if constexpr (probes::HasFormationState<CoreT>) {
        if constexpr (probes::HasSaveSlotOffset<decltype(c.formation)>) {
            return c.formation.SaveSlotOffset(slotIndex);
        }
        return false;
    } else if constexpr (probes::HasStageFormationMember<CoreT>) {
        if constexpr (probes::HasSaveSlotOffset<decltype(c.stageFormation)>) {
            return c.stageFormation.SaveSlotOffset(slotIndex);
        }
        return false;
    } else {
        (void)c; (void)slotIndex;
        return false;
    }
}

template <typename CoreT>
bool TrySetSlotSource(CoreT& c, int slotIndex, int sourceSlotIndex) {
    if constexpr (probes::HasSetFormationSlotSource<CoreT>) {
        return c.SetFormationSlotSource(slotIndex, sourceSlotIndex);
    } else if constexpr (probes::HasSetSlotSource<CoreT>) {
        return c.SetSlotSource(slotIndex, sourceSlotIndex);
    } else if constexpr (probes::HasFormationState<CoreT>) {
        if constexpr (probes::HasSetSlotSource<decltype(c.formation)>) {
            return c.formation.SetSlotSource(slotIndex, sourceSlotIndex);
        }
        return false;
    } else if constexpr (probes::HasStageFormationMember<CoreT>) {
        if constexpr (probes::HasSetSlotSource<decltype(c.stageFormation)>) {
            return c.stageFormation.SetSlotSource(slotIndex, sourceSlotIndex);
        }
        return false;
    } else {
        (void)c; (void)slotIndex; (void)sourceSlotIndex;
        return false;
    }
}

template <typename CoreT>
bool TrySetSlotFacialWeights(CoreT& c, int slotIndex, const FacialWeights& w) {
    if constexpr (probes::HasSetSlotFacialWeights<CoreT>) {
        return c.SetSlotFacialWeights(slotIndex, w);
    } else {
        if (!TrySelectSlot(c, slotIndex)) return false;
        c.player.controller.facialWeights = w;
        return true;
    }
}

template <typename CoreT>
void RunFrameAll(CoreT& core, std::vector<FakeActor>& actors, uint64_t frame, float dt,
                 const std::vector<size_t>& order) {
    for (size_t i = 0; i < order.size(); ++i) {
        auto& a = actors[order[i]];
        core.OnLateUpdate(&a, &a.animator, nullptr, dt, i == 0, dt, frame);
    }
}

// Dependent-type probe helpers: only instantiated when Concept<T> is true.
template <typename CoreT, typename Fn>
bool InspectFormation(CoreT& c, gke2e::CaseContext& ctx, std::string_view reason, Fn&& fn) {
    if constexpr (probes::HasFormationState<CoreT>) {
        fn(c.formation);
        return true;
    } else if constexpr (probes::HasStageFormationMember<CoreT>) {
        fn(c.stageFormation);
        return true;
    } else {
        (void)c; (void)fn;
        ctx.Pending(reason);
        return false;
    }
}

template <typename CamSettingsT>
bool TrySetFollowSlotIndex(CamSettingsT& s, int slotIndex, gke2e::CaseContext& ctx, std::string_view reason) {
    if constexpr (probes::HasFollowSlotIndex<CamSettingsT>) {
        s.followSlotIndex = slotIndex;
        return true;
    } else {
        (void)s; (void)slotIndex;
        ctx.Pending(reason);
        return false;
    }
}

template <typename CamSettingsT>
bool TryGetFollowSlotIndex(const CamSettingsT& s, int& outSlotIndex, gke2e::CaseContext& ctx, std::string_view reason) {
    if constexpr (probes::HasFollowSlotIndex<CamSettingsT>) {
        outSlotIndex = s.followSlotIndex;
        return true;
    } else {
        (void)s; (void)outSlotIndex;
        ctx.Pending(reason);
        return false;
    }
}

// ============================================================================
// F7: Unified Automatic Multi-Actor Driving (6 T1 + 6 T2)
// ============================================================================

void Test_T1_F7_01(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F7 auto-activation on >=2 actors")) return;
    TempWorkspace ws("t1_f7_01");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    std::vector<FakeActor> actors(2);
    actors[0].Init(701, "hski", "Saki", {0.0f, 0.0f, 0.0f});
    actors[1].Init(702, "ttmr", "Temari", {1.5f, 0.0f, 0.0f});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    RunFrameAll(core, actors, 1, 1.0f / 30.0f, {0, 1});
    GKE2E_CHECK(ctx, actors[0].writes > 0);
    GKE2E_CHECK(ctx, actors[1].writes > 0);
    core.AbandonScene();
}

void Test_T1_F7_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F7 5-idol simultaneous driving")) return;
    TempWorkspace ws("t1_f7_02");
    Core core;
    InitCoreWorkspace(core, ws, 5);
    std::vector<FakeActor> actors(5);
    const char* codes[5] = {"hski", "ttmr", "fktn", "amao", "kllj"};
    for (int i = 0; i < 5; ++i) {
        actors[i].Init(710 + i, codes[i], codes[i], {static_cast<float>(i) * 1.2f, 0.0f, 0.0f});
    }
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    RunFrameAll(core, actors, 1, 1.0f / 30.0f, {0, 1, 2, 3, 4});
    for (int i = 0; i < 5; ++i) {
        GKE2E_CHECK(ctx, actors[i].writes > 0);
    }
    core.AbandonScene();
}

void Test_T1_F7_03(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F7 preview frame-0 multi-actor pose")) return;
    TempWorkspace ws("t1_f7_03");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) {
        actors[i].Init(720 + i, "hski", "Actor", {static_cast<float>(i), 0.0f, 0.0f});
    }
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(core.player.state), static_cast<int>(PlaybackState::Preview));
    RunFrameAll(core, actors, 1, 0.0f, {0, 1, 2});
    for (int i = 0; i < 3; ++i) {
        GKE2E_CHECK(ctx, actors[i].writes > 0);
    }
    core.AbandonScene();
}

void Test_T1_F7_04(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F7 shared clock and single audio owner")) return;
    TempWorkspace ws("t1_f7_04");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) {
        actors[i].Init(730 + i, "hski", "Actor", {static_cast<float>(i), 0.0f, 0.0f});
    }
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    const float t0 = core.player.currentTime;
    RunFrameAll(core, actors, 10, 0.02f, {2, 0, 1});
    GKE2E_CHECK_NEAR(ctx, core.player.currentTime, t0 + 0.02f, 1e-4f);
    GKE2E_CHECK_EQ(ctx, GlobalAudioCounters().plays, 1);
    core.AbandonScene();
}

template <typename CoreT>
void ExerciseEligibilityFilterIfPresent(CoreT& core, std::vector<FakeActor>& actors, gke2e::CaseContext& ctx) {
    if constexpr (requires(CoreT& c) { c.fnEnsembleActorEligible; }) {
        core.fnEnsembleActorEligible = [](void* actorPtr) -> bool {
            const auto* a = static_cast<const FakeActor*>(actorPtr);
            return a && a->instanceId != 741;
        };
        core.PublishSceneActors(MakeRoster(actors));
        GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
        RunFrameAll(core, actors, 2, 0.0f, {0, 1});
        GKE2E_CHECK(ctx, actors[0].writes > 0);
        GKE2E_CHECK_EQ(ctx, actors[1].writes, 0);
        core.fnEnsembleActorEligible = nullptr;
        core.PublishSceneActors(MakeRoster(actors));
    } else {
        (void)core;
        (void)actors;
        (void)ctx;
    }
}

void Test_T1_F7_05(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F7 automatic background read-only validation")) return;
    TempWorkspace ws("t1_f7_05");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    std::vector<FakeActor> actors(2);
    actors[0].Init(740, "hski", "Saki", {0, 0, 0});
    actors[1].Init(741, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(core.player.state), static_cast<int>(PlaybackState::Stopped));
    // Read-only background validation before Play/Preview must not mutate actor bones/transforms.
    RunFrameAll(core, actors, 1, 0.0f, {0, 1});
    GKE2E_CHECK_EQ(ctx, actors[0].writes, 0);
    GKE2E_CHECK_EQ(ctx, actors[1].writes, 0);
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, f.active);
        if constexpr (probes::HasFormationVerified<decltype(f)>) {
            GKE2E_CHECK(ctx, f.verified);
        } else if constexpr (probes::HasFormationValidated<decltype(f)>) {
            GKE2E_CHECK(ctx, f.validated);
        }
    });

    ExerciseEligibilityFilterIfPresent(core, actors, ctx);

    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    RunFrameAll(core, actors, 3, 0.0f, {0, 1});
    GKE2E_CHECK(ctx, actors[0].writes > 0);
    GKE2E_CHECK(ctx, actors[1].writes > 0);
    GKE2E_CHECK(ctx, !probes::HasLegacyEnsembleProbe<GuiContext>);
    core.AbandonScene();
}

void Test_T1_F7_06(gke2e::CaseContext& ctx) {
    TempWorkspace ws("t1_f7_06");
    BurstBridgeMultiClient client;
    std::array<const BurstBridgeMultiClient::Slot*, 5> slotPtrs{};
    for (size_t i = 0; i < 5; ++i) {
        auto& s = client.State(i);
        slotPtrs[i] = &s;
        GKE2E_CHECK(ctx, !s.faulted);
        GKE2E_CHECK(ctx, !s.outstanding);
        GKE2E_CHECK_EQ(ctx, s.reason, std::string("not_requested"));
    }
    for (size_t i = 0; i < 5; ++i) {
        for (size_t j = i + 1; j < 5; ++j) {
            GKE2E_CHECK(ctx, slotPtrs[i] != slotPtrs[j]);
        }
    }
    GKE2E_CHECK(ctx, !client.HasCaptureInput());
    const std::wstring missingDir = ws.Sub("missing_companion").wstring();
    client.Configure(missingDir.c_str(), nullptr, nullptr);
    GKE2E_CHECK(ctx, !client.Ensure());
    GKE2E_CHECK_EQ(ctx, client.reason, std::string("companion_missing"));

    IdolBoneBinding binding;
    FakeActor actor;
    actor.Init(750, "hski", "Saki", {0, 0, 0});
    binding.actor = &actor;
    binding.cachedAnimator = &actor.animator;
    IdolController ctrl;
    WireIdolController(ctrl);
    GKE2E_CHECK(ctx, !client.Publish(2, binding, ctrl, 0.0f, 1));
    GKE2E_CHECK(ctx, client.ClearForCapture());
    GKE2E_CHECK(ctx, client.Clear());
    GKE2E_CHECK(ctx, !client.Shutdown());
    GKE2E_CHECK(ctx, !client.Ensure());
    GKE2E_CHECK_EQ(ctx, client.reason, std::string("client_shutdown"));
    GKE2E_PASS(ctx, "Verified 5-slot BurstBridgeMultiClient isolation, ClearForCapture/Clear, and Shutdown");
}

void Test_T2_F7_01(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F7 1-actor solo vs 2-actor auto-switch")) return;
    TempWorkspace ws("t2_f7_01");
    Core core;
    InitCoreWorkspace(core, ws, 0);
    std::vector<FakeActor> actors(2);
    actors[0].Init(760, "hski", "Saki", {0, 0, 0});
    actors[1].Init(761, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors({MakeRoster(actors)[0]});
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    if (!InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, !f.active);
    })) return;
    core.PublishSceneActors(MakeRoster(actors));
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, f.active);
    });
    core.AbandonScene();
}

void Test_T2_F7_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F7 legacy probe toggle and standalone panel removed")) return;
    GKE2E_CHECK(ctx, !probes::HasLegacyEnsembleProbe<GuiContext>);
    GKE2E_CHECK(ctx, !probes::HasLegacyEnsembleSet<GuiContext>);
    GKE2E_CHECK(ctx, !probes::HasLegacyEnsembleSnapshot<GuiContext>);
    GKE2E_CHECK(ctx, !probes::HasLegacyEnsembleActive<Core>);
}

void Test_T2_F7_03(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F7 legacy group presets and cross-song mix-match removed")) return;
    GKE2E_CHECK(ctx, !probes::HasLegacyEnsemblePreset<GuiContext>);
    GKE2E_CHECK(ctx, !probes::HasLegacyEnsembleDraft<GuiOverlay>);
    GKE2E_CHECK(ctx, !probes::HasLegacyEnsembleActive<Core>);

    TempWorkspace ws("t2_f7_03");
    fx::WriteTextFileUtf8(ws.Sub("config/ensembles/legacy_stage.json"),
        R"({"version":1,"enabled":true,"masterSlot":0,"cameraSlot":0,"slots":[{"enabled":true,"motionKey":"nonexistent_legacy_song"}]})");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    std::vector<FakeActor> actors(2);
    actors[0].Init(765, "hski", "Saki", {0, 0, 0});
    actors[1].Init(766, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK_EQ(ctx, core.loadedMotionKey, std::string("group_song"));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(core.motions.size()), 1);
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, f.active);
        GKE2E_CHECK(ctx, f.isGroupSong);
    });
    core.AbandonScene();
}

void Test_T2_F7_04(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F7 actor removal and pointer-reuse safety")) return;
    TempWorkspace ws("t2_f7_04");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) actors[i].Init(770 + i, "hski", "Actor", {static_cast<float>(i), 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    RunFrameAll(core, actors, 1, 0.016f, {0, 1, 2});
    actors[1].alive = false;
    const int beforeInvalid = actors[1].invalidTouches;
    std::vector<SceneActorObservation> nextRoster{MakeRoster(actors)[0], MakeRoster(actors)[2]};
    SceneActorObservation reused{&actors[1], 9999, "fktn", "Kotone", "", ""};
    nextRoster.push_back(reused);
    core.PublishSceneActors(nextRoster);
    GKE2E_CHECK_EQ(ctx, actors[1].invalidTouches, beforeInvalid);
    core.AbandonScene();
}

void Test_T2_F7_05(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F7 capture suspend/resume across 3+ actors")) return;
    TempWorkspace ws("t2_f7_05");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) actors[i].Init(780 + i, "hski", "Actor", {static_cast<float>(i), 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    RunFrameAll(core, actors, 1, 0.02f, {0, 1, 2});
    GKE2E_CHECK(ctx, core.SuspendForCapture());
    GKE2E_CHECK(ctx, core.ResumeAfterCapture());
    core.AbandonScene();
}

void Test_T2_F7_06(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F7 undrivable actor exclusion")) return;
    TempWorkspace ws("t2_f7_06");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) actors[i].Init(790 + i, "hski", "Actor", {static_cast<float>(i), 0, 0});
    actors[1].alive = false;
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    RunFrameAll(core, actors, 1, 0.02f, {0, 1, 2});
    GKE2E_CHECK(ctx, actors[0].writes > 0);
    GKE2E_CHECK(ctx, actors[2].writes > 0);
    GKE2E_CHECK_EQ(ctx, actors[1].invalidTouches, 0);
    core.AbandonScene();
}

// ============================================================================
// F8: Slot-1 Reference Anchor & Local Coordinates (6 T1 + 5 T2)
// ============================================================================

template <typename IdolCtrlT = IdolController>
void Run_T1_F8_01(gke2e::CaseContext& ctx) {
    FakeActor slot1, slot2;
    slot1.Init(801, "hski", "Saki", {10.0f, 1.0f, -5.0f}, Quaternion::Identity());
    slot2.Init(802, "ttmr", "Temari", {0.0f, 0.0f, 0.0f}, Quaternion::Identity());
    IdolCtrlT ctrl;
    WireIdolController(ctrl);
    GKE2E_CHECK(ctx, TrySetAnchor(ctrl, WorldPosition(&slot1.root), WorldRotation(&slot1.root)));
    GKE2E_CHECK(ctx, ctrl.SetInitialPositionOffset({1.5f, 0.2f, -0.8f}));
    MotionClip clip;
    clip.fps = 30.0f; clip.total_frames = 1; clip.duration_seconds = 0.0f;
    clip.bones = {{0, "Hips", false}}; clip.bone_count = 1;
    clip.root_positions = {{0.0f, 0.0f, 0.0f}};
    clip.bone_rotations = {Quaternion::Identity()};
    ctrl.BindActor(&slot2, clip, &slot2.animator);
    ctrl.ApplyMotion(clip, 0.0f);
    const auto pos = WorldPosition(&slot2.hips);
    GKE2E_CHECK_NEAR(ctx, pos.x, 11.5f, 1e-3f);
    GKE2E_CHECK_NEAR(ctx, pos.z, -5.8f, 1e-3f);
}

void Test_T1_F8_01(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F8 slot-1 anchor origin + identity rotation local +X")) return;
    if constexpr (probes::HasAnchorOverride<IdolController>) {
        Run_T1_F8_01<IdolController>(ctx);
    } else {
        ctx.Pending("Missing IdolController anchor API");
    }
}

void Test_T1_F8_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F8 anchor rotated +90 deg Y maps local +X to (0,0,-1)")) return;
    FakeActor slot1, slot2;
    const Quaternion anchorRot = Quaternion::FromEulerDegreesXYZ(0.0f, 90.0f, 0.0f);
    slot1.Init(803, "hski", "Saki", {0.0f, 0.0f, 0.0f}, anchorRot);
    slot2.Init(804, "ttmr", "Temari", {5.0f, 0.0f, 5.0f}, Quaternion::Identity());
    IdolController ctrl;
    WireIdolController(ctrl);
    GKE2E_CHECK(ctx, TrySetAnchor(ctrl, WorldPosition(&slot1.root), WorldRotation(&slot1.root)));
    GKE2E_CHECK(ctx, ctrl.SetInitialPositionOffset({1.0f, 0.0f, 0.0f}));
    const MotionClip clip = MakeTestHipsClip();
    ctrl.BindActor(&slot2, clip, &slot2.animator);
    ctrl.ApplyMotion(clip, 0.0f);
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&slot2.hips), {0.0f, 1.0f, -1.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearQuat(WorldRotation(&slot2.hips), anchorRot, 1e-3f));
}

void Test_T1_F8_03(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F8 arbitrary anchor yaw (37 deg) + origin")) return;
    FakeActor slot2;
    slot2.Init(805, "ttmr", "Temari", {-4.0f, 0.0f, 2.0f}, Quaternion::Identity());
    IdolController ctrl;
    WireIdolController(ctrl);
    const Vector3 anchorOrigin{2.0f, 0.5f, -3.0f};
    const Quaternion anchorRot = Quaternion::FromEulerDegreesXYZ(0.0f, 37.0f, 0.0f);
    const Vector3 localOffset{1.25f, 0.1f, -0.75f};
    GKE2E_CHECK(ctx, TrySetAnchor(ctrl, anchorOrigin, anchorRot));
    GKE2E_CHECK(ctx, ctrl.SetInitialPositionOffset(localOffset));
    const MotionClip clip = MakeTestHipsClip();
    ctrl.BindActor(&slot2, clip, &slot2.animator);
    ctrl.ApplyMotion(clip, 0.0f);
    const Vector3 expectedHips = anchorOrigin + anchorRot.Rotate(Vector3{0.0f, 1.0f, 0.0f} + localOffset);
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&slot2.hips), expectedHips, 1e-3f));
    GKE2E_CHECK(ctx, NearQuat(WorldRotation(&slot2.hips), anchorRot, 1e-3f));
}

void Test_T1_F8_04(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F8 yaw pivot around anchored slot position")) return;
    FakeActor slot2;
    slot2.Init(8060, "ttmr", "Temari", {0.0f, 0.0f, 0.0f}, Quaternion::Identity());
    IdolController ctrl;
    WireIdolController(ctrl);
    const Vector3 anchorOrigin{1.0f, 0.0f, 2.0f};
    const Quaternion anchorRot = Quaternion::FromEulerDegreesXYZ(0.0f, 45.0f, 0.0f);
    const Vector3 localOffset{1.0f, 0.0f, 0.0f};
    GKE2E_CHECK(ctx, TrySetAnchor(ctrl, anchorOrigin, anchorRot));
    GKE2E_CHECK(ctx, ctrl.SetInitialPositionOffset(localOffset));
    GKE2E_CHECK(ctx, ctrl.SetPlacementYawDegrees(90.0f));
    GKE2E_CHECK_NEAR(ctx, ctrl.placementYawDegrees, 90.0f, 1e-4f);

    const MotionClip clipZero = MakeTestHipsClip({0.0f, 0.0f, 0.0f});
    ctrl.BindActor(&slot2, clipZero, &slot2.animator);
    ctrl.ApplyMotion(clipZero, 0.0f);
    const Vector3 slotPivot = anchorOrigin + anchorRot.Rotate(localOffset);
    const Quaternion expectedRot = (Quaternion::FromEulerDegreesXYZ(0.0f, 90.0f, 0.0f) * anchorRot).Normalized();
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&slot2.hips), slotPivot + Vector3{0.0f, 1.0f, 0.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearQuat(WorldRotation(&slot2.hips), expectedRot, 1e-3f));

    const MotionClip clipMoved = MakeTestHipsClip({0.5f, 0.0f, 0.0f});
    ctrl.ApplyMotion(clipMoved, 0.0f);
    const Vector3 expectedMovedHips = slotPivot + expectedRot.Rotate(Vector3{0.5f, 1.0f, 0.0f});
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&slot2.hips), expectedMovedHips, 1e-3f));
}

void Test_T1_F8_05(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F8 dynamic slot-1 movement updates dependents without drift")) return;
    FakeActor slot1, slot2;
    slot1.Init(807, "hski", "Saki", {0.0f, 0.0f, 0.0f}, Quaternion::Identity());
    slot2.Init(808, "ttmr", "Temari", {0.0f, 0.0f, 0.0f}, Quaternion::Identity());
    IdolController ctrl;
    WireIdolController(ctrl);
    const Vector3 localOffset{1.0f, 0.0f, 0.5f};
    GKE2E_CHECK(ctx, ctrl.SetInitialPositionOffset(localOffset));
    const MotionClip clip = MakeTestHipsClip();
    ctrl.BindActor(&slot2, clip, &slot2.animator);
    for (int i = 0; i < 25; ++i) {
        slot1.root.localPosition = {static_cast<float>(i) * 0.1f, 0.0f, -static_cast<float>(i) * 0.05f};
        slot1.root.localRotation = Quaternion::FromEulerDegreesXYZ(0.0f, static_cast<float>(i) * 3.0f, 0.0f);
        GKE2E_CHECK(ctx, TrySetAnchor(ctrl, WorldPosition(&slot1.root), WorldRotation(&slot1.root)));
        ctrl.ApplyMotion(clip, 0.0f);
        const Vector3 expected = WorldPosition(&slot1.root) +
            WorldRotation(&slot1.root).Rotate(Vector3{0.0f, 1.0f, 0.0f} + localOffset);
        GKE2E_CHECK(ctx, NearVec3(WorldPosition(&slot2.hips), expected, 1e-3f));
    }
}

void Test_T1_F8_06(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F8 Stop/ResetToRestPose restores original pose without anchor residue")) return;
    IdolController ctrl;
    FakeActor actor;
    actor.Init(806, "hski", "Saki", {3.0f, 0.0f, -2.0f});
    WireIdolController(ctrl);
    const MotionClip clip = MakeTestHipsClip();
    ctrl.BindActor(&actor, clip, &actor.animator);
    GKE2E_CHECK(ctx, TrySetAnchor(ctrl, {10.0f, 0.0f, 10.0f}, Quaternion::FromEulerDegreesXYZ(0, 90, 0)));
    GKE2E_CHECK(ctx, ctrl.SetInitialPositionOffset({2.0f, 0.5f, -1.0f}));
    GKE2E_CHECK(ctx, ctrl.SetPlacementYawDegrees(45.0f));
    ctrl.ApplyMotion(clip, 0.0f);
    GKE2E_CHECK(ctx, !NearVec3(WorldPosition(&actor.hips), {3.0f, 1.0f, -2.0f}, 1e-2f));
    ctrl.ResetToRestPose(ctrl.currentBinding);
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&actor.hips), {3.0f, 1.0f, -2.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearQuat(WorldRotation(&actor.hips), Quaternion::Identity(), 1e-3f));
}

void Test_T2_F8_01(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F8 boundary yaw angles (+-180, 360, 450)")) return;
    IdolController ctrl;
    GKE2E_CHECK(ctx, ctrl.SetPlacementYawDegrees(180.0f));
    GKE2E_CHECK(ctx, ctrl.SetPlacementYawDegrees(-180.0f));
    GKE2E_CHECK(ctx, ctrl.SetPlacementYawDegrees(360.0f));
    GKE2E_CHECK(ctx, ctrl.SetPlacementYawDegrees(450.0f));
    GKE2E_CHECK_NEAR(ctx, ctrl.placementYawDegrees, 90.0f, 1e-4f);
}

void Test_T2_F8_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F8 non-finite local offset/yaw rejected")) return;
    IdolController ctrl;
    GKE2E_CHECK(ctx, ctrl.SetInitialPositionOffset({1.0f, 2.0f, 3.0f}));
    GKE2E_CHECK(ctx, ctrl.SetPlacementYawDegrees(30.0f));
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    GKE2E_CHECK(ctx, !ctrl.SetInitialPositionOffset({qnan, 0.0f, 0.0f}));
    GKE2E_CHECK(ctx, !ctrl.SetInitialPositionOffset({0.0f, inf, 0.0f}));
    GKE2E_CHECK(ctx, !ctrl.SetPlacementYawDegrees(qnan));
    GKE2E_CHECK(ctx, NearVec3(ctrl.initialPositionOffset, {1.0f, 2.0f, 3.0f}));
    GKE2E_CHECK_NEAR(ctx, ctrl.placementYawDegrees, 30.0f, 1e-4f);
}

void Test_T2_F8_03(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F8 zero offset (0,0,0,0) preserves native VMD path")) return;
    FakeActor slot2;
    slot2.Init(823, "ttmr", "Temari", {5.0f, 0.0f, 5.0f});
    IdolController ctrl;
    WireIdolController(ctrl);
    const Vector3 anchorOrigin{2.0f, 0.0f, -1.0f};
    const Quaternion anchorRot = Quaternion::FromEulerDegreesXYZ(0.0f, 90.0f, 0.0f);
    GKE2E_CHECK(ctx, TrySetAnchor(ctrl, anchorOrigin, anchorRot));
    GKE2E_CHECK(ctx, ctrl.SetInitialPositionOffset({0.0f, 0.0f, 0.0f}));
    GKE2E_CHECK(ctx, ctrl.SetPlacementYawDegrees(0.0f));
    const MotionClip clip = MakeTestHipsClip({0.4f, 0.1f, -0.2f});
    ctrl.BindActor(&slot2, clip, &slot2.animator);
    ctrl.ApplyMotion(clip, 0.0f);
    const Vector3 expected = anchorOrigin + anchorRot.Rotate(Vector3{0.4f, 1.1f, -0.2f});
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&slot2.hips), expected, 1e-3f));
    GKE2E_CHECK(ctx, NearQuat(WorldRotation(&slot2.hips), anchorRot, 1e-3f));
}

void Test_T2_F8_04(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F8 null/cleared anchor fallback")) return;
    FakeActor actor;
    const Quaternion actorRot = Quaternion::FromEulerDegreesXYZ(0.0f, 30.0f, 0.0f);
    actor.Init(824, "hski", "Saki", {3.0f, 0.0f, -1.0f}, actorRot);
    IdolController ctrl;
    WireIdolController(ctrl);
    const MotionClip clip = MakeTestHipsClip();
    ctrl.BindActor(&actor, clip, &actor.animator);
    GKE2E_CHECK(ctx, TrySetAnchor(ctrl, {10.0f, 0.0f, 10.0f}, Quaternion::FromEulerDegreesXYZ(0.0f, 90.0f, 0.0f)));
    ctrl.ApplyMotion(clip, 0.0f);
    GKE2E_CHECK(ctx, !NearVec3(WorldPosition(&actor.hips), {3.0f, 1.0f, -1.0f}, 1e-1f));
    GKE2E_CHECK(ctx, TryClearAnchor(ctrl));
    GKE2E_CHECK(ctx, ctrl.SetInitialPositionOffset({0.0f, 0.0f, 0.0f}));
    ctrl.ApplyMotion(clip, 0.0f);
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&actor.hips), {3.0f, 1.0f, -1.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearQuat(WorldRotation(&actor.hips), actorRot, 1e-3f));
}

void Test_T2_F8_05(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F8 3D anchor orientation local-basis check")) return;
    FakeActor actor;
    actor.Init(825, "ttmr", "Temari", {0.0f, 0.0f, 0.0f});
    IdolController ctrl;
    WireIdolController(ctrl);
    const Vector3 anchorOrigin{1.0f, 0.5f, -2.0f};
    const Quaternion q3d = Quaternion::FromEulerDegreesXYZ(10.0f, 60.0f, -5.0f).Normalized();
    const Vector3 localOffset{1.0f, 0.25f, -0.5f};
    GKE2E_CHECK(ctx, TrySetAnchor(ctrl, anchorOrigin, q3d));
    GKE2E_CHECK(ctx, ctrl.SetInitialPositionOffset(localOffset));
    const MotionClip clip = MakeTestHipsClip();
    ctrl.BindActor(&actor, clip, &actor.animator);
    ctrl.ApplyMotion(clip, 0.0f);
    const Vector3 expected = anchorOrigin + q3d.Rotate(Vector3{0.0f, 1.0f, 0.0f} + localOffset);
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&actor.hips), expected, 1e-3f));
    GKE2E_CHECK(ctx, NearQuat(WorldRotation(&actor.hips), q3d, 1e-3f));
}

// ============================================================================
// F9: Formation Placement, Swap & Reuse (6 T1 + 6 T2)
// ============================================================================

void Test_T1_F9_01(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F9 group song 2+ default (0,0,0,0)")) return;
    TempWorkspace ws("t1_f9_01");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) actors[i].Init(901 + i, "hski", "Actor", {static_cast<float>(i), 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_EQ(ctx, static_cast<int>(f.slots.size()), 3);
        for (size_t i = 1; i < f.slots.size(); ++i) {
            GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[i]), {0.0f, 0.0f, 0.0f}));
            GKE2E_CHECK_NEAR(ctx, SlotYawDeg(f.slots[i]), 0.0f, 1e-4f);
        }
    });
    core.AbandonScene();
}

void Test_T1_F9_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F9 group song explicit info.json placement")) return;
    TempWorkspace ws("t1_f9_02");
    fx::TrackInfoSpec spec;
    spec.title = "Explicit Group";
    spec.is_group = true;
    spec.position_count = 2;
    spec.positions = {
        {1, 0.0f, 0.0f, 0.0f, 0.0f, true, false, "slot_1.vmd", "", "", 30.0f, 3, 1},
        {2, -1.2f, 0.1f, 0.5f, 30.0f, true, false, "slot_2.vmd", "", "", 30.0f, 3, 1}
    };
    fx::write_synthetic_track_dir(ws.Sub("motions/explicit_group"), spec, true, true);
    Core core;
    core.initialized = true; core.SetControlActive(true);
    core.configRoot = MotionPathToUtf8(ws.Sub("config"));
    core.motionsRoot = MotionPathToUtf8(ws.Sub("motions"));
    WireIdolController(core.player.controller);
    core.RefreshMotionLibrary();
    std::vector<FakeActor> actors(2);
    actors[0].Init(910, "hski", "Saki", {0, 0, 0});
    actors[1].Init(911, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_NEAR(ctx, SlotOffsetVec3(f.slots[1]).x, -1.2f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, SlotOffsetVec3(f.slots[1]).y, 0.1f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, SlotOffsetVec3(f.slots[1]).z, 0.5f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, SlotYawDeg(f.slots[1]), 30.0f, 1e-4f);
    });
    core.AbandonScene();
}

void Test_T1_F9_03(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F9 single-song 2+ auto-placement (x+0.5,y,z)")) return;
    TempWorkspace ws("t1_f9_03");
    Core core;
    InitCoreWorkspace(core, ws, 0);
    std::vector<FakeActor> actors(3);
    actors[0].Init(920, "hski", "Saki", {2.0f, 0.3f, -1.0f});
    actors[1].Init(921, "ttmr", "Temari", {0.0f, 0.0f, 0.0f});
    actors[2].Init(922, "fktn", "Kotone", {0.0f, 0.0f, 0.0f});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    RunFrameAll(core, actors, 1, 0.0f, {0, 1, 2});
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_EQ(ctx, static_cast<int>(f.slots.size()), 3);
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[1]), {0.5f, 0.0f, 0.0f}, 1e-3f));
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[2]), {1.0f, 0.0f, 0.0f}, 1e-3f));
    });
    const Vector3 pos0 = WorldPosition(&actors[0].hips);
    GKE2E_CHECK(ctx, NearVec3(pos0, {2.0f, 1.3f, -1.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&actors[1].hips), pos0 + Vector3{0.5f, 0.0f, 0.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&actors[2].hips), pos0 + Vector3{1.0f, 0.0f, 0.0f}, 1e-3f));
    core.AbandonScene();
}

void Test_T1_F9_04(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F9 group-song overflow auto-place & sourceSlotIndex")) return;
    TempWorkspace ws("t1_f9_04");
    fx::TrackInfoSpec spec;
    spec.title = "Overflow Group";
    spec.is_group = true;
    spec.position_count = 2;
    spec.positions = {
        {1, 0.0f, 0.0f, 0.0f, 0.0f, true, false, "slot_1.vmd", "", "", 30.0f, 3, 1},
        {2, 1.0f, 0.0f, 0.25f, 0.0f, true, false, "slot_2.vmd", "", "", 30.0f, 3, 1}
    };
    fx::write_synthetic_track_dir(ws.Sub("motions/overflow_group"), spec, true, true);
    Core core;
    core.initialized = true; core.SetControlActive(true);
    core.configRoot = MotionPathToUtf8(ws.Sub("config"));
    core.motionsRoot = MotionPathToUtf8(ws.Sub("motions"));
    WireIdolController(core.player.controller);
    core.RefreshMotionLibrary();

    std::vector<FakeActor> actors(4);
    actors[0].Init(930, "hski", "Saki", {1.0f, 0.2f, -0.5f});
    actors[1].Init(931, "ttmr", "Temari", {0.0f, 0.0f, 0.0f});
    actors[2].Init(932, "fktn", "Kotone", {0.0f, 0.0f, 0.0f});
    actors[3].Init(933, "amao", "Mao", {0.0f, 0.0f, 0.0f});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    RunFrameAll(core, actors, 1, 0.0f, {0, 1, 2, 3});
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_EQ(ctx, static_cast<int>(f.slots.size()), 4);
        GKE2E_CHECK(ctx, !f.slots[0].isOverflow);
        GKE2E_CHECK(ctx, !f.slots[1].isOverflow);
        GKE2E_CHECK(ctx, f.slots[2].isOverflow);
        GKE2E_CHECK(ctx, f.slots[3].isOverflow);
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[2]), {1.5f, 0.0f, 0.25f}, 1e-3f));
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[3]), {2.0f, 0.0f, 0.25f}, 1e-3f));
    });
    const Vector3 pos1 = WorldPosition(&actors[1].hips);
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&actors[2].hips), pos1 + Vector3{0.5f, 0.0f, 0.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&actors[3].hips), pos1 + Vector3{1.0f, 0.0f, 0.0f}, 1e-3f));

    GKE2E_CHECK(ctx, TrySetSlotSource(core, 2, 1));
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, f.slots[2].isOverflow);
        GKE2E_CHECK_EQ(ctx, f.slots[2].sourceSlotIndex, 1);
    });
    core.AbandonScene();
}

void Test_T1_F9_05(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F9 drag-drop swap occupied <-> occupied")) return;
    TempWorkspace ws("t1_f9_05");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    std::vector<FakeActor> actors(4);
    actors[0].Init(940, "hski", "Saki", {0, 0, 0});
    actors[1].Init(941, "ttmr", "Temari", {1, 0, 0});
    actors[2].Init(942, "fktn", "Kotone", {2, 0, 0});
    actors[3].Init(943, "amao", "Mao", {3, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, TrySetSlotSource(core, 2, 0));
    GKE2E_CHECK(ctx, TrySetSlotSource(core, 3, 1));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 2, {0.4f, 0.0f, 0.2f}, 15.0f));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 3, {-0.6f, 0.1f, -0.3f}, -20.0f));

    GKE2E_CHECK(ctx, TrySelectSlot(core, 2));
    GKE2E_CHECK(ctx, TrySetSlotFacialWeights(core, 2, {1.6f, 1.1f, 0.9f, 1.4f}));
    core.SetLiveBoneOverride(13, "LeftUpperArm", 5.0f, 0.0f, 0.0f, false, true, false);

    GKE2E_CHECK(ctx, TrySelectSlot(core, 3));
    GKE2E_CHECK(ctx, TrySetSlotFacialWeights(core, 3, {0.7f, 0.8f, 1.2f, 0.5f}));

    GKE2E_CHECK(ctx, TrySwapSlots(core, 2, 3));
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_EQ(ctx, f.slots[2].character, std::string("amao"));
        GKE2E_CHECK_EQ(ctx, f.slots[3].character, std::string("fktn"));
        // Offset, yawDegrees, and sourceSlotIndex stay with the slot.
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[2]), {0.4f, 0.0f, 0.2f}, 1e-4f));
        GKE2E_CHECK_NEAR(ctx, SlotYawDeg(f.slots[2]), 15.0f, 1e-4f);
        GKE2E_CHECK_EQ(ctx, f.slots[2].sourceSlotIndex, 0);
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[3]), {-0.6f, 0.1f, -0.3f}, 1e-4f));
        GKE2E_CHECK_NEAR(ctx, SlotYawDeg(f.slots[3]), -20.0f, 1e-4f);
        GKE2E_CHECK_EQ(ctx, f.slots[3].sourceSlotIndex, 1);
        // Facial weights and bone overrides follow the actor entity.
        GKE2E_CHECK_NEAR(ctx, f.slots[2].facialWeights.overall, 0.7f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, f.slots[2].facialWeights.mouth, 0.5f, 1e-4f);
        GKE2E_CHECK(ctx, !f.slots[2].hasActorBoneOverrides);
        GKE2E_CHECK_NEAR(ctx, f.slots[3].facialWeights.overall, 1.6f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, f.slots[3].facialWeights.mouth, 1.4f, 1e-4f);
        GKE2E_CHECK(ctx, f.slots[3].hasActorBoneOverrides);
    });
    core.AbandonScene();
}

void Test_T1_F9_06(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F9 drag-drop swap occupied <-> empty + slot")) return;
    TempWorkspace ws("t1_f9_06");
    Core core;
    InitCoreWorkspace(core, ws, 4);
    std::vector<FakeActor> actors(2);
    actors[0].Init(950, "hski", "Saki", {0, 0, 0});
    actors[1].Init(951, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {0.3f, 0.0f, 0.1f}, 22.0f));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 3, {-0.9f, 0.0f, 0.4f}, -10.0f));
    GKE2E_CHECK(ctx, TrySelectSlot(core, 1));
    GKE2E_CHECK(ctx, TrySetSlotFacialWeights(core, 1, {1.7f, 1.3f, 0.6f, 1.9f}));
    core.SetLiveBoneOverride(13, "LeftUpperArm", 6.0f, 0.0f, 0.0f, false, true, false);

    GKE2E_CHECK(ctx, TrySwapSlots(core, 1, 3));
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, !f.slots[1].occupied);
        GKE2E_CHECK(ctx, f.slots[3].occupied);
        GKE2E_CHECK_EQ(ctx, f.slots[3].character, std::string("ttmr"));
        // Offset, yawDegrees, and sourceSlotIndex stay with the slot.
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[1]), {0.3f, 0.0f, 0.1f}, 1e-4f));
        GKE2E_CHECK_NEAR(ctx, SlotYawDeg(f.slots[1]), 22.0f, 1e-4f);
        GKE2E_CHECK_EQ(ctx, f.slots[1].sourceSlotIndex, 1);
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[3]), {-0.9f, 0.0f, 0.4f}, 1e-4f));
        GKE2E_CHECK_NEAR(ctx, SlotYawDeg(f.slots[3]), -10.0f, 1e-4f);
        GKE2E_CHECK_EQ(ctx, f.slots[3].sourceSlotIndex, 3);
        // Facial weights and bone overrides follow the actor entity to slot 3.
        GKE2E_CHECK(ctx, !f.slots[1].hasActorBoneOverrides);
        GKE2E_CHECK_NEAR(ctx, f.slots[3].facialWeights.overall, 1.7f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, f.slots[3].facialWeights.mouth, 1.9f, 1e-4f);
        GKE2E_CHECK(ctx, f.slots[3].hasActorBoneOverrides);
    });
    core.AbandonScene();
}

void Test_T2_F9_01(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F9 slotCount invariant across solo/group/overflow")) return;
    TempWorkspace ws("t2_f9_01");
    Core core;
    InitCoreWorkspace(core, ws, 4);
    std::vector<FakeActor> actors(2);
    actors[0].Init(960, "hski", "Saki", {0, 0, 0});
    actors[1].Init(961, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_EQ(ctx, static_cast<int>(f.slots.size()), 4);
    });
    core.AbandonScene();
}

void Test_T2_F9_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F9 out-of-range slot operations rejected")) return;
    TempWorkspace ws("t2_f9_02");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    GKE2E_CHECK(ctx, !TrySwapSlots(core, -1, 0));
    GKE2E_CHECK(ctx, !TrySwapSlots(core, 0, 99));
    GKE2E_CHECK(ctx, !TrySelectSlot(core, 99));
    GKE2E_CHECK(ctx, !TrySetSlotSource(core, 0, 99));
}

void Test_T2_F9_03(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F9 self-swap is safe no-op")) return;
    TempWorkspace ws("t2_f9_03");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    std::vector<FakeActor> actors(2);
    actors[0].Init(970, "hski", "Saki", {0, 0, 0});
    actors[1].Init(971, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, TrySwapSlots(core, 0, 0));
    core.AbandonScene();
}

void Test_T2_F9_04(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F9 independent post-load slot offset & dirty flag")) return;
    TempWorkspace ws("t2_f9_04");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) actors[i].Init(980 + i, "hski", "Actor", {0, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {0.75f, 0.0f, -0.25f}, 12.0f));
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, f.slots[1].placementDirty);
        GKE2E_CHECK(ctx, !f.slots[0].placementDirty);
        GKE2E_CHECK(ctx, !f.slots[2].placementDirty);
    });
    core.AbandonScene();
}

void Test_T2_F9_05(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F9 group track missing slot clip fallback")) return;
    TempWorkspace ws("t2_f9_05");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    std::error_code ec;
    std::filesystem::remove(ws.Sub("motions/group_song/positions/slot_3/motion.gkclip"), ec);
    core.RefreshMotionLibrary();
    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) actors[i].Init(990 + i, "hski", "Actor", {0, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    core.AbandonScene();
}

void Test_T2_F9_06(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F9 mid-session roster shrink/grow stability")) return;
    TempWorkspace ws("t2_f9_06");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    std::vector<FakeActor> actors(3);
    actors[0].Init(995, "hski", "Saki", {0, 0, 0});
    actors[1].Init(996, "ttmr", "Temari", {1, 0, 0});
    actors[2].Init(997, "fktn", "Kotone", {2, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    core.PublishSceneActors({MakeRoster(actors)[0]});
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_EQ(ctx, static_cast<int>(f.slots.size()), 3);
        GKE2E_CHECK(ctx, f.slots[0].occupied);
        GKE2E_CHECK(ctx, !f.slots[1].occupied);
        GKE2E_CHECK(ctx, !f.slots[2].occupied);
    });
    RunFrameAll(core, actors, 1, 0.016f, {0});
    GKE2E_CHECK(ctx, actors[0].writes > 0);

    core.PublishSceneActors(MakeRoster(actors));
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, f.active);
        GKE2E_CHECK_EQ(ctx, static_cast<int>(f.slots.size()), 3);
        GKE2E_CHECK(ctx, f.slots[0].occupied && f.slots[1].occupied && f.slots[2].occupied);
        GKE2E_CHECK(ctx, f.slots[0].actorToken != f.slots[1].actorToken);
        GKE2E_CHECK(ctx, f.slots[1].actorToken != f.slots[2].actorToken);
    });
    RunFrameAll(core, actors, 2, 0.016f, {0, 1, 2});
    for (int i = 0; i < 3; ++i) {
        GKE2E_CHECK(ctx, actors[i].writes > 0);
        GKE2E_CHECK_EQ(ctx, actors[i].invalidTouches, 0);
    }
    core.AbandonScene();
}

// ============================================================================
// F10: Layered Persistence (6 T1 + 6 T2)
// ============================================================================

void Test_T1_F10_01(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F10 facial_weights.json by_character round-trip")) return;
    TempWorkspace ws("t1_f10_01");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    std::vector<FakeActor> actors(2);
    actors[0].Init(1001, "hski", "Saki", {0, 0, 0});
    actors[1].Init(1002, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, TrySelectSlot(core, 0));
    core.player.controller.facialWeights = {1.5f, 1.2f, 0.8f, 1.8f};
    GKE2E_CHECK(ctx, core.SaveFacialWeights());

    jsonlite::Value fw;
    std::string err;
    GKE2E_REQUIRE(ctx, jsonlite::ParseFile(MotionPathToUtf8(ws.Sub("config/facial_weights.json")), fw, err));
    const auto* hskiNode = fw.FindPath("by_character.hski");
    GKE2E_REQUIRE(ctx, hskiNode != nullptr);
    GKE2E_CHECK_NEAR(ctx, hskiNode->FloatPath("overall", -1.0f), 1.5f, 1e-4f);
    GKE2E_CHECK_NEAR(ctx, hskiNode->FloatPath("brow", -1.0f), 1.2f, 1e-4f);
    GKE2E_CHECK_NEAR(ctx, hskiNode->FloatPath("eye", -1.0f), 0.8f, 1e-4f);
    GKE2E_CHECK_NEAR(ctx, hskiNode->FloatPath("mouth", -1.0f), 1.8f, 1e-4f);

    Core reopened;
    ReopenCoreWorkspace(reopened, ws);
    reopened.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, reopened.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, reopened.LoadFacialWeights());
    GKE2E_CHECK(ctx, TrySelectSlot(reopened, 0));
    GKE2E_CHECK_NEAR(ctx, reopened.player.controller.facialWeights.overall, 1.5f, 1e-4f);
    GKE2E_CHECK_NEAR(ctx, reopened.player.controller.facialWeights.brow, 1.2f, 1e-4f);
    GKE2E_CHECK_NEAR(ctx, reopened.player.controller.facialWeights.eye, 0.8f, 1e-4f);
    GKE2E_CHECK_NEAR(ctx, reopened.player.controller.facialWeights.mouth, 1.8f, 1e-4f);
    InspectFormation(reopened, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_NEAR(ctx, f.slots[0].facialWeights.overall, 1.5f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, f.slots[0].facialWeights.mouth, 1.8f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, f.slots[1].facialWeights.overall, 1.0f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, f.slots[1].facialWeights.brow, 1.0f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, f.slots[1].facialWeights.eye, 1.0f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, f.slots[1].facialWeights.mouth, 1.0f, 1e-4f);
    });
    core.AbandonScene();
    reopened.AbandonScene();
}

void Test_T1_F10_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F10 legacy global facial_weights.json fallback")) return;
    TempWorkspace ws("t1_f10_02");
    fx::WriteTextFileUtf8(ws.Sub("config/facial_weights.json"),
        R"({"version":1,"overall":1.25,"brow":0.9,"eye":1.1,"mouth":1.4})");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    GKE2E_CHECK(ctx, core.LoadFacialWeights());
    GKE2E_CHECK_NEAR(ctx, core.player.controller.facialWeights.overall, 1.25f, 1e-4f);
}

void Test_T1_F10_03(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F10 bone save to current track slot bone_overrides.json")) return;
    TempWorkspace ws("t1_f10_03");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) actors[i].Init(1010 + i, "hski", "Actor", {0, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, TrySelectSlot(core, 1));
    core.SetLiveBoneOverride(13, "LeftUpperArm", 5.0f, 10.0f, -3.0f, false, true, false);
    GKE2E_CHECK(ctx, core.SaveLiveOverridesToMotion());
    GKE2E_CHECK(ctx, std::filesystem::exists(ws.Sub("motions/group_song/positions/slot_2/bone_overrides.json")));
    core.AbandonScene();
}

void Test_T1_F10_04(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F10 bone save to global bone_overrides.json")) return;
    TempWorkspace ws("t1_f10_04");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    core.SetLiveBoneOverride(13, "LeftUpperArm", 2.0f, 4.0f, 6.0f, false, true, false);
    GKE2E_CHECK(ctx, core.SaveLiveOverridesToGlobal());
    GKE2E_CHECK(ctx, std::filesystem::exists(ws.Sub("config/bone_overrides.json")));
}

void Test_T1_F10_05(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F10 placement save for slot 1 -> global initial_position.json")) return;
    TempWorkspace ws("t1_f10_05");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    bool infoOk = false;
    const uint64_t infoHashBefore = FileFnv1a64(ws.Sub("motions/group_song/info.json"), infoOk);
    GKE2E_REQUIRE(ctx, infoOk);

    std::vector<FakeActor> actors(2);
    actors[0].Init(1020, "hski", "Saki", {0, 0, 0});
    actors[1].Init(1021, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 0, {0.25f, 0.0f, -0.5f}, 0.0f));
    GKE2E_CHECK(ctx, TrySaveSlotOffset(core, 0));
    GKE2E_CHECK(ctx, std::filesystem::exists(ws.Sub("config/initial_position.json")));
    GKE2E_CHECK_EQ(ctx, FileFnv1a64(ws.Sub("motions/group_song/info.json"), infoOk), infoHashBefore);

    jsonlite::Value initPos;
    std::string err;
    GKE2E_REQUIRE(ctx, jsonlite::ParseFile(MotionPathToUtf8(ws.Sub("config/initial_position.json")), initPos, err));
    GKE2E_CHECK_EQ(ctx, initPos.IntPath("version", 0), 1);
    GKE2E_CHECK_EQ(ctx, initPos.StringPath("space", ""), std::string("world"));
    GKE2E_CHECK_NEAR(ctx, initPos.FloatPath("offset_m.0", 99.0f), 0.25f, 1e-4f);
    GKE2E_CHECK_NEAR(ctx, initPos.FloatPath("offset_m.1", 99.0f), 0.0f, 1e-4f);
    GKE2E_CHECK_NEAR(ctx, initPos.FloatPath("offset_m.2", 99.0f), -0.5f, 1e-4f);

    Core reopened;
    ReopenCoreWorkspace(reopened, ws);
    GKE2E_CHECK(ctx, reopened.LoadInitialPosition());
    GKE2E_CHECK(ctx, NearVec3(reopened.player.controller.initialPositionOffset, {0.25f, 0.0f, -0.5f}, 1e-4f));
    reopened.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, reopened.SelectMotionByIndex(0, false));
    InspectFormation(reopened, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[0]), {0.25f, 0.0f, -0.5f}, 1e-4f));
    });
    core.AbandonScene();
    reopened.AbandonScene();
}

void Test_T1_F10_06(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F10 placement save for slot 2+ -> group info.json (zero rebake)")) return;
    TempWorkspace ws("t1_f10_06");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    bool hOk1 = false, hOk2 = false;
    const auto clip1 = ws.Sub("motions/group_song/motion.gkclip");
    const auto clip2 = ws.Sub("motions/group_song/positions/slot_2/motion.gkclip");
    const uint64_t hashBefore1 = FileFnv1a64(clip1, hOk1);
    const uint64_t hashBefore2 = FileFnv1a64(clip2, hOk2);
    GKE2E_REQUIRE(ctx, hOk1 && hOk2);

    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) actors[i].Init(1030 + i, "hski", "Actor", {0, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {-0.8f, 0.1f, 0.6f}, 25.0f));
    GKE2E_CHECK(ctx, TrySaveSlotOffset(core, 1));

    GKE2E_CHECK_EQ(ctx, FileFnv1a64(clip1, hOk1), hashBefore1);
    GKE2E_CHECK_EQ(ctx, FileFnv1a64(clip2, hOk2), hashBefore2);
    jsonlite::Value info;
    std::string err;
    GKE2E_REQUIRE(ctx, jsonlite::ParseFile(MotionPathToUtf8(ws.Sub("motions/group_song/info.json")), info, err));
    const auto* pos1 = info.FindPath("positions.1");
    GKE2E_REQUIRE(ctx, pos1 != nullptr);
    GKE2E_CHECK_NEAR(ctx, pos1->FloatPath("x", 0.0f), -0.8f, 1e-4f);
    GKE2E_CHECK_NEAR(ctx, pos1->FloatPath("yaw_degrees", 0.0f), 25.0f, 1e-4f);
    core.AbandonScene();
}

void Test_T2_F10_01(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F10 facial weight boundaries (0, 2, <0, >2, NaN)")) return;
    TempWorkspace ws("t2_f10_01");
    Core core;
    InitCoreWorkspace(core, ws, 0);
    core.player.controller.facialWeights = {0.0f, 2.0f, 0.0f, 2.0f};
    GKE2E_CHECK(ctx, core.SaveFacialWeights());
    core.player.controller.facialWeights = {-0.1f, 1.0f, 1.0f, 1.0f};
    GKE2E_CHECK(ctx, !core.SaveFacialWeights());
    core.player.controller.facialWeights = {2.1f, 1.0f, 1.0f, 1.0f};
    GKE2E_CHECK(ctx, !core.SaveFacialWeights());
    core.player.controller.facialWeights = {std::numeric_limits<float>::quiet_NaN(), 1.0f, 1.0f, 1.0f};
    GKE2E_CHECK(ctx, !core.SaveFacialWeights());
}

void Test_T2_F10_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F10 corrupt facial_weights.json recovery")) return;
    TempWorkspace ws("t2_f10_02");
    fx::WriteTextFileUtf8(ws.Sub("config/facial_weights.json"), "{corrupt json");
    Core core;
    InitCoreWorkspace(core, ws, 0);
    GKE2E_CHECK(ctx, !core.LoadFacialWeights());
    GKE2E_CHECK_NEAR(ctx, core.player.controller.facialWeights.overall, 1.0f, 1e-4f);
}

void Test_T2_F10_03(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F10 overflow actor bone save writes to sourceSlotIndex")) return;
    TempWorkspace ws("t2_f10_03");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) actors[i].Init(1040 + i, "hski", "Actor", {0, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, TrySetSlotSource(core, 2, 1));
    GKE2E_CHECK(ctx, TrySelectSlot(core, 2));
    core.SetLiveBoneOverride(13, "LeftUpperArm", 7.0f, 0.0f, 0.0f, false, true, false);
    GKE2E_CHECK(ctx, core.SaveLiveOverridesToMotion());
    GKE2E_CHECK(ctx, std::filesystem::exists(ws.Sub("motions/group_song/positions/slot_2/bone_overrides.json")));
    core.AbandonScene();
}

void Test_T2_F10_04(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F10 solo-song 2+ placement save boundary")) return;
    TempWorkspace ws("t2_f10_04");
    Core core;
    InitCoreWorkspace(core, ws, 0);
    std::vector<FakeActor> actors(2);
    actors[0].Init(1050, "hski", "Saki", {0, 0, 0});
    actors[1].Init(1051, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, !TrySaveSlotOffset(core, 1));
    core.AbandonScene();
}

void Test_T2_F10_05(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F10 unrecognized/NPC actor (character == empty) fallback")) return;
    TempWorkspace ws("t2_f10_05");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    std::vector<FakeActor> actors(2);
    actors[0].Init(1060, "", "NPC", {0, 0, 0});
    actors[1].Init(1061, "hski", "Saki", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, TrySelectSlot(core, 0));
    core.player.controller.facialWeights = {1.3f, 0.9f, 1.1f, 1.4f};
    GKE2E_CHECK(ctx, core.SaveFacialWeights());
    GKE2E_CHECK(ctx, TrySelectSlot(core, 1));
    core.player.controller.facialWeights = {1.7f, 1.7f, 1.7f, 1.7f};
    GKE2E_CHECK(ctx, core.SaveFacialWeights());

    jsonlite::Value fw;
    std::string err;
    GKE2E_REQUIRE(ctx, jsonlite::ParseFile(MotionPathToUtf8(ws.Sub("config/facial_weights.json")), fw, err));
    GKE2E_CHECK_NEAR(ctx, fw.FloatPath("overall", -1.0f), 1.3f, 1e-4f);
    const auto* byChar = fw.Find("by_character");
    GKE2E_REQUIRE(ctx, byChar != nullptr && byChar->IsObject());
    GKE2E_CHECK(ctx, byChar->Find("") == nullptr);
    const auto* hskiNode = byChar->Find("hski");
    GKE2E_REQUIRE(ctx, hskiNode != nullptr);
    GKE2E_CHECK_NEAR(ctx, hskiNode->FloatPath("overall", -1.0f), 1.7f, 1e-4f);

    Core reopened;
    ReopenCoreWorkspace(reopened, ws);
    reopened.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, reopened.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, reopened.LoadFacialWeights());
    InspectFormation(reopened, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_NEAR(ctx, f.slots[0].facialWeights.overall, 1.3f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, f.slots[0].facialWeights.mouth, 1.4f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, f.slots[1].facialWeights.overall, 1.7f, 1e-4f);
    });
    core.AbandonScene();
    reopened.AbandonScene();
}

void Test_T2_F10_06(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F10 missing info.json on legacy track placement save")) return;
    TempWorkspace ws("t2_f10_06");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    std::error_code ec;
    std::filesystem::remove(ws.Sub("motions/group_song/info.json"), ec);
    std::vector<FakeActor> actors(2);
    actors[0].Init(1070, "hski", "Saki", {0, 0, 0});
    actors[1].Init(1071, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, !TrySaveSlotOffset(core, 1));
    core.AbandonScene();
}

// ============================================================================
// F11: Multi-Actor Camera Center & Post-Actor Tick (6 T1 + 5 T2)
// ============================================================================

void Test_T1_F11_01(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F11 followSlotIndex = -1 arithmetic mean (Hips)")) return;
    TempWorkspace ws("t1_f11_01");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    ConfigureDirectFollowCamera(core.camera);
    core.camera.settings.followTarget = CameraFollowTarget::Hips;
    if (!TrySetFollowSlotIndex(core.camera.settings, -1, ctx, "Missing followSlotIndex")) return;
    std::vector<FakeActor> actors(3);
    actors[0].Init(1101, "hski", "A", {0.0f, 0.0f, 0.0f});
    actors[1].Init(1102, "ttmr", "B", {3.0f, 0.0f, 0.0f});
    actors[2].Init(1103, "fktn", "C", {6.0f, 0.0f, 0.0f});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 0, {0.0f, 0.0f, 0.0f}, 0.0f));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {3.0f, 0.0f, 0.0f}, 0.0f));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 2, {6.0f, 0.0f, 0.0f}, 0.0f));
    RunFrameAll(core, actors, 1, 0.016f, {0, 1, 2});
    GKE2E_CHECK(ctx, camRig.writeCalls > 0);
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, {3.0f, 1.0f, 0.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearVec3(core.camera.desired.position, {3.0f, 1.0f, 0.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearVec3(core.camera.followOrigin, {3.0f, 1.0f, 0.0f}, 1e-3f));

    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 2, {9.0f, 0.0f, 0.0f}, 0.0f));
    RunFrameAll(core, actors, 2, 0.016f, {0, 1, 2});
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, {4.0f, 1.0f, 0.0f}, 1e-3f));
    core.AbandonScene();
}

void Test_T1_F11_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F11 followSlotIndex = -1 Chest & Head targets")) return;
    TempWorkspace ws("t1_f11_02");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    ConfigureDirectFollowCamera(core.camera);
    if (!TrySetFollowSlotIndex(core.camera.settings, -1, ctx, "Missing followSlotIndex")) return;
    std::vector<FakeActor> actors(2);
    actors[0].Init(1110, "hski", "A", {0, 0, 0});
    actors[1].Init(1111, "ttmr", "B", {2, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 0, {0.0f, 0.0f, 0.0f}, 0.0f));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {2.0f, 0.0f, 0.0f}, 0.0f));

    core.camera.settings.followTarget = CameraFollowTarget::Chest;
    RunFrameAll(core, actors, 1, 0.016f, {0, 1});
    GKE2E_CHECK(ctx, camRig.writeCalls > 0);
    GKE2E_CHECK(ctx, NearVec3(core.camera.followOrigin, {1.0f, 1.0f, 0.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, {1.0f, 1.50f, 0.0f}, 1e-3f));

    core.camera.settings.followTarget = CameraFollowTarget::Head;
    RunFrameAll(core, actors, 2, 0.016f, {0, 1});
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, {1.0f, 1.72f, 0.0f}, 1e-3f));
    core.AbandonScene();
}

void Test_T1_F11_03(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F11 followSlotIndex = 0..N-1 specific slot tracking")) return;
    TempWorkspace ws("t1_f11_03");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    ConfigureDirectFollowCamera(core.camera);
    if (!TrySetFollowSlotIndex(core.camera.settings, 1, ctx, "Missing followSlotIndex")) return;
    std::vector<FakeActor> actors(3);
    actors[0].Init(1120, "hski", "A", {0.0f, 0.0f, 0.0f});
    actors[1].Init(1121, "ttmr", "B", {1.0f, 0.0f, 0.0f});
    actors[2].Init(1122, "fktn", "C", {10.0f, 0.0f, 0.0f});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 0, {0.0f, 0.0f, 0.0f}, 0.0f));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {1.0f, 0.0f, 0.0f}, 0.0f));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 2, {10.0f, 0.0f, 0.0f}, 0.0f));
    RunFrameAll(core, actors, 1, 0.016f, {0, 1, 2});
    GKE2E_CHECK(ctx, camRig.writeCalls > 0);
    // Asymmetric {0, 1, 10} ensures slot 1 (1.0) != mean (3.667) != slot 0 (0.0).
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, {1.0f, 1.0f, 0.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearVec3(core.camera.followOrigin, {1.0f, 1.0f, 0.0f}, 1e-3f));

    if (!TrySetFollowSlotIndex(core.camera.settings, 2, ctx, "Missing followSlotIndex")) return;
    RunFrameAll(core, actors, 2, 0.016f, {0, 1, 2});
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, {10.0f, 1.0f, 0.0f}, 1e-3f));
    core.AbandonScene();
}

void Test_T1_F11_04(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F11 camera.Tick triggered after last active actor writes bones")) return;
    TempWorkspace ws("t1_f11_04");
    Core core;
    InitCoreWorkspace(core, ws, 4);
    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    core.camera.settings.enabled = true;
    core.camera.settings.mode = CameraMode::Follow;
    std::vector<FakeActor> actors(4);
    for (int i = 0; i < 4; ++i) actors[i].Init(1130 + i, "hski", "A", {static_cast<float>(i) * 2.0f, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    const int beforeWrites = camRig.writeCalls;
    const std::array<size_t, 4> order{3, 1, 0, 2};
    for (size_t i = 0; i < 3; ++i) {
        core.OnLateUpdate(&actors[order[i]], &actors[order[i]].animator, nullptr, 0.016f, i == 0, 0.016f, 1);
        GKE2E_CHECK_EQ(ctx, camRig.writeCalls, beforeWrites);
    }
    core.OnLateUpdate(&actors[order[3]], &actors[order[3]].animator, nullptr, 0.016f, false, 0.016f, 1);
    GKE2E_CHECK(ctx, actors[order[3]].writes > 0);
    for (size_t i = 0; i < 4; ++i) GKE2E_CHECK(ctx, actors[i].writes > 0);
    GKE2E_CHECK_EQ(ctx, camRig.writeCalls, beforeWrites + 1);
    core.AbandonScene();
}

void Test_T1_F11_05(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F11 empty + slots excluded from -1 arithmetic mean")) return;
    TempWorkspace ws("t1_f11_05");
    Core core;
    InitCoreWorkspace(core, ws, 4);
    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    ConfigureDirectFollowCamera(core.camera);
    if (!TrySetFollowSlotIndex(core.camera.settings, -1, ctx, "Missing followSlotIndex")) return;
    std::vector<FakeActor> actors(2);
    actors[0].Init(1140, "hski", "A", {0, 0, 0});
    actors[1].Init(1141, "ttmr", "B", {4, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 0, {0.0f, 0.0f, 0.0f}, 0.0f));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {4.0f, 0.0f, 0.0f}, 0.0f));
    RunFrameAll(core, actors, 1, 0.016f, {0, 1});
    GKE2E_CHECK(ctx, camRig.writeCalls > 0);
    // 2 occupied actors at {0, 4} in 4-slot song -> mean x = 2.0 (empty + slots do not dilute to 1.0).
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, {2.0f, 1.0f, 0.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearVec3(core.camera.followOrigin, {2.0f, 1.0f, 0.0f}, 1e-3f));
    core.AbandonScene();
}

void Test_T1_F11_06(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F11 camera.json persists followSlotIndex")) return;
    TempWorkspace ws("t1_f11_06");
    for (int slotVal : {-1, 0, 3}) {
        Core core;
        InitCoreWorkspace(core, ws, 0);
        if (!TrySetFollowSlotIndex(core.camera.settings, slotVal, ctx, "Missing followSlotIndex")) return;
        GKE2E_CHECK(ctx, core.SaveCameraSettings());
        Core reopened;
        ReopenCoreWorkspace(reopened, ws);
        GKE2E_CHECK(ctx, reopened.LoadCameraSettings());
        int loadedSlot = -999;
        if (!TryGetFollowSlotIndex(reopened.camera.settings, loadedSlot, ctx, "Missing followSlotIndex")) return;
        GKE2E_CHECK_EQ(ctx, loadedSlot, slotVal);
    }
}

void Test_T2_F11_01(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F11 CameraSettings::IsValid() on followSlotIndex")) return;
    CameraSettings s;
    if (!TrySetFollowSlotIndex(s, -1, ctx, "Missing followSlotIndex")) return;
    GKE2E_CHECK(ctx, s.IsValid());
    TrySetFollowSlotIndex(s, 0, ctx, "Missing followSlotIndex");
    GKE2E_CHECK(ctx, s.IsValid());
    TrySetFollowSlotIndex(s, -2, ctx, "Missing followSlotIndex");
    GKE2E_CHECK(ctx, !s.IsValid());
}

void Test_T2_F11_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F11 empty or out-of-range followSlotIndex target")) return;
    TempWorkspace ws("t2_f11_02");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    core.camera.settings.enabled = true;
    core.camera.settings.mode = CameraMode::Follow;
    if (!TrySetFollowSlotIndex(core.camera.settings, 9, ctx, "Missing followSlotIndex")) return;
    std::vector<FakeActor> actors(2);
    actors[0].Init(1150, "hski", "A", {0, 0, 0});
    actors[1].Init(1151, "ttmr", "B", {2, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    RunFrameAll(core, actors, 1, 0.016f, {0, 1});
    GKE2E_CHECK(ctx, std::isfinite(camRig.currentFrame.position.x));
    core.AbandonScene();
}

void Test_T2_F11_03(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F11 partial fnFollowTarget failure in -1 mode")) return;
    TempWorkspace ws("t2_f11_03");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    FakeCameraRig camRig; camRig.ResetDefault();
    camRig.allowHead = false;
    WireCameraController(core.camera, camRig);
    core.camera.settings.enabled = true;
    core.camera.settings.mode = CameraMode::Follow;
    core.camera.settings.followTarget = CameraFollowTarget::Head;
    if (!TrySetFollowSlotIndex(core.camera.settings, -1, ctx, "Missing followSlotIndex")) return;
    std::vector<FakeActor> actors(2);
    actors[0].Init(1160, "hski", "A", {0, 0, 0});
    actors[1].Init(1161, "ttmr", "B", {2, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    RunFrameAll(core, actors, 1, 0.016f, {0, 1});
    GKE2E_CHECK(ctx, std::isfinite(camRig.currentFrame.position.x));
    core.AbandonScene();
}

void Test_T2_F11_04(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F11 Stopped releases camera in multi-actor mode")) return;
    TempWorkspace ws("t2_f11_04");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    core.camera.settings.enabled = true;
    core.camera.settings.mode = CameraMode::Follow;
    std::vector<FakeActor> actors(2);
    actors[0].Init(1170, "hski", "A", {0, 0, 0});
    actors[1].Init(1171, "ttmr", "B", {2, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    RunFrameAll(core, actors, 1, 0.016f, {0, 1});
    core.StopPerformance();
    GKE2E_CHECK(ctx, !core.camera.owned);
    core.AbandonScene();
}

void Test_T2_F11_05(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F11 CameraMode::Track stability across actor callbacks")) return;
    TempWorkspace ws("t2_f11_05");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    core.camera.settings.enabled = true;
    core.camera.settings.mode = CameraMode::Track;
    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) actors[i].Init(1180 + i, "hski", "A", {static_cast<float>(i), 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    const int before = camRig.writeCalls;
    RunFrameAll(core, actors, 1, 0.016f, {0, 1, 2});
    GKE2E_CHECK_EQ(ctx, camRig.writeCalls, before + 1);
    core.AbandonScene();
}

// ============================================================================
// Tier 3 Core Pairwise Tests (T3-M3-01..06, T3-M3-08)
// ============================================================================

void Test_T3_M3_01(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "T3 F7 x F8 5-actor auto-driving under rotated 90 deg anchor")) return;
    TempWorkspace ws("t3_m3_01");
    Core core;
    InitCoreWorkspace(core, ws, 5);
    std::vector<FakeActor> actors(5);
    const Quaternion anchorRot = Quaternion::FromEulerDegreesXYZ(0.0f, 90.0f, 0.0f);
    for (int i = 0; i < 5; ++i) {
        actors[i].Init(1301 + i, "hski", "Idol", {0.0f, 0.0f, 0.0f},
                       i == 0 ? anchorRot : Quaternion::Identity());
    }
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {1.0f, 0.0f, 0.0f}, 0.0f));
    RunFrameAll(core, actors, 1, 0.02f, {0, 1, 2, 3, 4});
    for (int i = 0; i < 5; ++i) GKE2E_CHECK(ctx, actors[i].writes > 0);
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&actors[1].hips), {0.0f, 1.0f, -1.0f}, 1e-3f));
    GKE2E_CHECK(ctx, NearQuat(WorldRotation(&actors[1].hips), anchorRot, 1e-3f));
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&actors[2].hips), {0.0f, 1.0f, 0.0f}, 1e-3f));
    core.AbandonScene();
}

void Test_T3_M3_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "T3 F8 x F9 slot swap preserves local offsets under rotated anchor")) return;
    TempWorkspace ws("t3_m3_02");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    std::vector<FakeActor> actors(3);
    const Quaternion anchorRot = Quaternion::FromEulerDegreesXYZ(0.0f, 90.0f, 0.0f);
    actors[0].Init(1310, "hski", "Saki", {0, 0, 0}, anchorRot);
    actors[1].Init(1311, "ttmr", "Temari", {1, 0, 0});
    actors[2].Init(1312, "fktn", "Kotone", {2, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {1.0f, 0.0f, 0.5f}, 15.0f));
    GKE2E_CHECK(ctx, TrySwapSlots(core, 1, 2));
    RunFrameAll(core, actors, 1, 0.02f, {0, 1, 2});
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_EQ(ctx, f.slots[1].character, std::string("fktn"));
        GKE2E_CHECK_EQ(ctx, f.slots[2].character, std::string("ttmr"));
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[1]), {1.0f, 0.0f, 0.5f}, 1e-4f));
        GKE2E_CHECK_NEAR(ctx, SlotYawDeg(f.slots[1]), 15.0f, 1e-4f);
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[2]), {0.0f, 0.0f, 0.0f}, 1e-4f));
        GKE2E_CHECK_NEAR(ctx, SlotYawDeg(f.slots[2]), 0.0f, 1e-4f);
    });
    const Vector3 expectedSlot1Hips = anchorRot.Rotate(Vector3{1.0f, 1.0f, 0.5f});
    const Quaternion expectedSlot1Rot = (Quaternion::FromEulerDegreesXYZ(0.0f, 15.0f, 0.0f) * anchorRot).Normalized();
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&actors[2].hips), expectedSlot1Hips, 1e-3f));
    GKE2E_CHECK(ctx, NearQuat(WorldRotation(&actors[2].hips), expectedSlot1Rot, 1e-3f));
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&actors[1].hips), {0.0f, 1.0f, 0.0f}, 1e-3f));
    core.AbandonScene();
}

void Test_T3_M3_03(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "T3 F9 x F10 empty + swap + by_character facial + slot bone overrides")) return;
    TempWorkspace ws("t3_m3_03");
    Core core;
    InitCoreWorkspace(core, ws, 4);
    std::vector<FakeActor> actors(2);
    actors[0].Init(1320, "hski", "Saki", {0, 0, 0});
    actors[1].Init(1321, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {0.4f, 0.0f, -0.2f}, 0.0f));
    GKE2E_CHECK(ctx, TrySwapSlots(core, 1, 3));
    GKE2E_CHECK(ctx, TrySelectSlot(core, 3));
    core.player.controller.facialWeights = {1.6f, 1.0f, 1.0f, 1.0f};
    GKE2E_CHECK(ctx, core.SaveFacialWeights());
    core.SetLiveBoneOverride(13, "LeftUpperArm", 3.0f, 0.0f, 0.0f, false, true, false);
    GKE2E_CHECK(ctx, core.SaveLiveOverridesToMotion());
    GKE2E_CHECK(ctx, std::filesystem::exists(ws.Sub("motions/group_song/positions/slot_4/bone_overrides.json")));
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, !f.slots[1].occupied);
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[1]), {0.4f, 0.0f, -0.2f}, 1e-4f));
        GKE2E_CHECK(ctx, f.slots[3].occupied);
        GKE2E_CHECK_EQ(ctx, f.slots[3].character, std::string("ttmr"));
    });

    jsonlite::Value fw;
    std::string err;
    GKE2E_REQUIRE(ctx, jsonlite::ParseFile(MotionPathToUtf8(ws.Sub("config/facial_weights.json")), fw, err));
    GKE2E_CHECK_NEAR(ctx, fw.FloatPath("by_character.ttmr.overall", -1.0f), 1.6f, 1e-4f);

    Core reopened;
    ReopenCoreWorkspace(reopened, ws);
    reopened.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, reopened.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, reopened.LoadFacialWeights());
    InspectFormation(reopened, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_NEAR(ctx, f.slots[0].facialWeights.overall, 1.0f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, f.slots[1].facialWeights.overall, 1.6f, 1e-4f);
    });
    core.AbandonScene();
    reopened.AbandonScene();
}

void Test_T3_M3_04(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "T3 F7 x F11 shuffled 5-actor callback order + followSlotIndex=-1")) return;
    TempWorkspace ws("t3_m3_04");
    Core core;
    InitCoreWorkspace(core, ws, 5);
    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    core.camera.settings.enabled = true;
    core.camera.settings.mode = CameraMode::Follow;
    if (!TrySetFollowSlotIndex(core.camera.settings, -1, ctx, "Missing followSlotIndex")) return;
    std::vector<FakeActor> actors(5);
    for (int i = 0; i < 5; ++i) actors[i].Init(1330 + i, "hski", "A", {static_cast<float>(i), 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    const int before = camRig.writeCalls;
    RunFrameAll(core, actors, 1, 0.02f, {4, 1, 3, 0, 2});
    GKE2E_CHECK_EQ(ctx, camRig.writeCalls, before + 1);
    core.AbandonScene();
}

void Test_T3_M3_05(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "T3 F9 x F11 slot swap updates followSlotIndex=0 target")) return;
    TempWorkspace ws("t3_m3_05");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    ConfigureDirectFollowCamera(core.camera);
    if (!TrySetFollowSlotIndex(core.camera.settings, 0, ctx, "Missing followSlotIndex")) return;
    std::vector<FakeActor> actors(2);
    actors[0].Init(1340, "hski", "Saki", {0, 0, 0});
    actors[1].Init(1341, "ttmr", "Temari", {5, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 0, {0.0f, 0.0f, 0.0f}, 0.0f));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {5.0f, 0.0f, 0.0f}, 0.0f));
    RunFrameAll(core, actors, 1, 0.02f, {0, 1});
    GKE2E_CHECK(ctx, camRig.writeCalls > 0);
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, WorldPosition(&actors[0].hips), 1e-3f));

    GKE2E_CHECK(ctx, TrySwapSlots(core, 0, 1));
    RunFrameAll(core, actors, 2, 0.02f, {0, 1});
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_EQ(ctx, f.slots[0].character, std::string("ttmr"));
        GKE2E_CHECK_EQ(ctx, f.slots[1].character, std::string("hski"));
    });
    const Vector3 slot0Hips = WorldPosition(&actors[1].hips);
    const Vector3 slot1Hips = WorldPosition(&actors[0].hips);
    GKE2E_CHECK(ctx, !NearVec3(slot0Hips, slot1Hips, 1e-1f));
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, slot0Hips, 1e-3f));

    if (!TrySetFollowSlotIndex(core.camera.settings, -1, ctx, "Missing followSlotIndex")) return;
    RunFrameAll(core, actors, 3, 0.02f, {0, 1});
    const Vector3 meanHips{
        (slot0Hips.x + slot1Hips.x) * 0.5f,
        (slot0Hips.y + slot1Hips.y) * 0.5f,
        (slot0Hips.z + slot1Hips.z) * 0.5f
    };
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, meanHips, 1e-3f));
    core.AbandonScene();
}

void Test_T3_M3_06(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "T3 F8 x F10 save slot 2+ offset to info.json -> reload -> 180 deg anchor")) return;
    TempWorkspace ws("t3_m3_06");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    std::vector<FakeActor> actors(2);
    const Quaternion anchorRot = Quaternion::FromEulerDegreesXYZ(0.0f, 180.0f, 0.0f);
    actors[0].Init(1350, "hski", "Saki", {0, 0, 0}, anchorRot);
    actors[1].Init(1351, "ttmr", "Temari", {1, 0, 0});
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {0.5f, 0.0f, 0.5f}, 0.0f));
    GKE2E_CHECK(ctx, TrySaveSlotOffset(core, 1));
    Core reopened;
    ReopenCoreWorkspace(reopened, ws);
    reopened.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, reopened.SelectMotionByIndex(0, false));
    InspectFormation(reopened, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[1]), {0.5f, 0.0f, 0.5f}, 1e-4f));
    });
    RunFrameAll(reopened, actors, 1, 0.02f, {0, 1});
    const Vector3 expected = anchorRot.Rotate(Vector3{0.5f, 1.0f, 0.5f});
    GKE2E_CHECK(ctx, NearVec3(WorldPosition(&actors[1].hips), expected, 1e-3f));
    core.AbandonScene();
    reopened.AbandonScene();
}

void Test_T3_M3_08(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "T3 F10 x F11 persist followSlotIndex=-1 + reload + SuspendForCapture")) return;
    TempWorkspace ws("t3_m3_08");
    Core core;
    InitCoreWorkspace(core, ws, 2);
    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    core.camera.settings.enabled = true;
    core.camera.settings.mode = CameraMode::Follow;
    if (!TrySetFollowSlotIndex(core.camera.settings, -1, ctx, "Missing followSlotIndex")) return;
    GKE2E_CHECK(ctx, core.SaveCameraSettings());
    Core reopened;
    ReopenCoreWorkspace(reopened, ws);
    WireCameraController(reopened.camera, camRig);
    GKE2E_CHECK(ctx, reopened.LoadCameraSettings());
    std::vector<FakeActor> actors(2);
    actors[0].Init(1360, "hski", "Saki", {0, 0, 0});
    actors[1].Init(1361, "ttmr", "Temari", {2, 0, 0});
    reopened.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, reopened.SelectMotionByIndex(0, true));
    RunFrameAll(reopened, actors, 1, 0.02f, {0, 1});
    GKE2E_CHECK(ctx, reopened.SuspendForCapture());
    GKE2E_CHECK(ctx, reopened.ResumeAfterCapture());
    reopened.AbandonScene();
}

// ============================================================================
// Tier 4 Core Real-World Scenarios (T4-M3-01, T4-M3-02)
// ============================================================================

void Test_T4_M3_01(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "T4 5-idol group-song live stage session (F7+F8+F9+F10+F11)")) return;
    TempWorkspace ws("t4_m3_01");
    Core core;
    InitCoreWorkspace(core, ws, 5);
    bool hOk1 = false, hOk4 = false;
    const auto clip1Path = ws.Sub("motions/group_song/motion.gkclip");
    const auto clip4Path = ws.Sub("motions/group_song/positions/slot_4/motion.gkclip");
    const uint64_t clipHash1 = FileFnv1a64(clip1Path, hOk1);
    const uint64_t clipHash4 = FileFnv1a64(clip4Path, hOk4);
    GKE2E_REQUIRE(ctx, hOk1 && hOk4);

    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    ConfigureDirectFollowCamera(core.camera);
    if (!TrySetFollowSlotIndex(core.camera.settings, -1, ctx, "Missing followSlotIndex")) return;

    std::vector<FakeActor> actors(5);
    const char* codes[5] = {"hski", "ttmr", "fktn", "amao", "kllj"};
    const Quaternion anchorRot = Quaternion::FromEulerDegreesXYZ(0.0f, 35.0f, 0.0f);
    for (int i = 0; i < 5; ++i) {
        actors[i].Init(1400 + i, codes[i], codes[i], {static_cast<float>(i), 0.0f, 0.0f},
                       i == 0 ? anchorRot : Quaternion::Identity());
    }
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    GKE2E_CHECK(ctx, TrySwapSlots(core, 1, 3));
    GKE2E_CHECK(ctx, TrySelectSlot(core, 3));
    core.player.controller.facialWeights = {1.35f, 1.1f, 0.9f, 1.2f};
    GKE2E_CHECK(ctx, core.SaveFacialWeights());
    core.SetLiveBoneOverride(13, "LeftUpperArm", 6.0f, -4.0f, 2.0f, false, true, false);
    GKE2E_CHECK(ctx, core.SaveLiveOverridesToMotion());
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 3, {0.6f, 0.0f, -0.4f}, 10.0f));
    GKE2E_CHECK(ctx, TrySaveSlotOffset(core, 3));
    for (uint64_t f = 1; f <= 30; ++f) {
        RunFrameAll(core, actors, f, 1.0f / 60.0f, {2, 0, 4, 1, 3});
    }
    Vector3 meanSum{};
    for (int i = 0; i < 5; ++i) {
        const auto hp = WorldPosition(&actors[i].hips);
        meanSum.x += hp.x; meanSum.y += hp.y; meanSum.z += hp.z;
    }
    const Vector3 expectedMean{meanSum.x / 5.0f, meanSum.y / 5.0f, meanSum.z / 5.0f};
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, expectedMean, 1e-3f));

    if (!TrySetFollowSlotIndex(core.camera.settings, 3, ctx, "Missing followSlotIndex")) return;
    RunFrameAll(core, actors, 31, 1.0f / 60.0f, {0, 1, 2, 3, 4});
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, WorldPosition(&actors[1].hips), 1e-3f));

    core.StopPerformance();
    GKE2E_CHECK(ctx, !core.camera.owned);

    // Persistence & zero-rebake verification
    GKE2E_CHECK_EQ(ctx, FileFnv1a64(clip1Path, hOk1), clipHash1);
    GKE2E_CHECK_EQ(ctx, FileFnv1a64(clip4Path, hOk4), clipHash4);
    GKE2E_CHECK(ctx, std::filesystem::exists(ws.Sub("motions/group_song/positions/slot_4/bone_overrides.json")));

    jsonlite::Value fw, info;
    std::string err;
    GKE2E_REQUIRE(ctx, jsonlite::ParseFile(MotionPathToUtf8(ws.Sub("config/facial_weights.json")), fw, err));
    GKE2E_CHECK_NEAR(ctx, fw.FloatPath("by_character.ttmr.overall", -1.0f), 1.35f, 1e-4f);
    GKE2E_CHECK_NEAR(ctx, fw.FloatPath("by_character.ttmr.mouth", -1.0f), 1.2f, 1e-4f);
    GKE2E_REQUIRE(ctx, jsonlite::ParseFile(MotionPathToUtf8(ws.Sub("motions/group_song/info.json")), info, err));
    GKE2E_CHECK_NEAR(ctx, info.FloatPath("positions.3.x", 99.0f), 0.6f, 1e-4f);
    GKE2E_CHECK_NEAR(ctx, info.FloatPath("positions.3.z", 99.0f), -0.4f, 1e-4f);
    GKE2E_CHECK_NEAR(ctx, info.FloatPath("positions.3.yaw_degrees", 99.0f), 10.0f, 1e-4f);

    Core reopened;
    ReopenCoreWorkspace(reopened, ws);
    reopened.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, reopened.SelectMotionByIndex(0, false));
    GKE2E_CHECK(ctx, reopened.LoadFacialWeights());
    InspectFormation(reopened, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[3]), {0.6f, 0.0f, -0.4f}, 1e-4f));
        GKE2E_CHECK_NEAR(ctx, SlotYawDeg(f.slots[3]), 10.0f, 1e-4f);
        GKE2E_CHECK_NEAR(ctx, f.slots[1].facialWeights.overall, 1.35f, 1e-4f);
    });
    core.AbandonScene();
    reopened.AbandonScene();
}

void Test_T4_M3_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "T4 overflow + empty + swap + solo auto-placement lifecycle")) return;
    TempWorkspace ws("t4_m3_02");
    fx::TrackInfoSpec solo;
    solo.title = "Solo Extra"; solo.is_group = false; solo.position_count = 1;
    fx::write_synthetic_track_dir(ws.Sub("motions/solo_extra"), solo, true, true);
    Core core;
    InitCoreWorkspace(core, ws, 3);
    const int groupIdx = FindPlaylistIndex(core.motions, "group_song");
    const int soloIdx = FindPlaylistIndex(core.motions, "solo_extra");
    GKE2E_REQUIRE(ctx, groupIdx >= 0 && soloIdx >= 0);

    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(core.camera, camRig);
    ConfigureDirectFollowCamera(core.camera);
    if (!TrySetFollowSlotIndex(core.camera.settings, -1, ctx, "Missing followSlotIndex")) return;

    std::vector<FakeActor> actors(5);
    actors[0].Init(1450, "hski", "A", {0.0f, 0.0f, 0.0f});
    actors[1].Init(1451, "ttmr", "B", {2.0f, 0.0f, 0.0f});
    for (int i = 2; i < 5; ++i) actors[i].Init(1450 + i, "fktn", "C", {0.0f, 0.0f, 0.0f});

    core.PublishSceneActors({MakeRoster(actors)[0], MakeRoster(actors)[1]});
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(groupIdx, true));
    GKE2E_CHECK(ctx, TrySetSlotOffset(core, 1, {2.0f, 0.0f, 0.0f}, 0.0f));
    RunFrameAll(core, actors, 1, 0.016f, {0, 1});
    GKE2E_CHECK(ctx, NearVec3(camRig.currentFrame.position, {1.0f, 1.0f, 0.0f}, 1e-3f));

    GKE2E_CHECK(ctx, TrySwapSlots(core, 1, 2));
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK(ctx, !f.slots[1].occupied);
        GKE2E_CHECK(ctx, f.slots[2].occupied);
    });

    core.PublishSceneActors(MakeRoster(actors));
    RunFrameAll(core, actors, 2, 0.016f, {0, 1, 2, 3, 4});
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_EQ(ctx, static_cast<int>(f.slots.size()), 5);
        GKE2E_CHECK(ctx, f.slots[3].isOverflow);
        GKE2E_CHECK(ctx, f.slots[4].isOverflow);
        GKE2E_CHECK_NEAR(ctx, SlotOffsetVec3(f.slots[3]).x, SlotOffsetVec3(f.slots[2]).x + 0.5f, 1e-3f);
        GKE2E_CHECK_NEAR(ctx, SlotOffsetVec3(f.slots[4]).x, SlotOffsetVec3(f.slots[3]).x + 0.5f, 1e-3f);
    });

    GKE2E_CHECK(ctx, TrySetSlotSource(core, 3, 1));
    GKE2E_CHECK(ctx, TrySelectSlot(core, 3));
    core.SetLiveBoneOverride(13, "LeftUpperArm", 4.0f, 0.0f, 0.0f, false, true, false);
    GKE2E_CHECK(ctx, core.SaveLiveOverridesToMotion());
    GKE2E_CHECK(ctx, std::filesystem::exists(ws.Sub("motions/group_song/positions/slot_2/bone_overrides.json")));

    GKE2E_CHECK(ctx, core.SelectMotionByIndex(soloIdx, false));
    RunFrameAll(core, actors, 3, 0.016f, {0, 1, 2, 3, 4});
    InspectFormation(core, ctx, "Missing Core::formation", [&](const auto& f) {
        GKE2E_CHECK_EQ(ctx, static_cast<int>(f.slots.size()), 5);
        if (f.slots.size() != 5) return;
        std::vector<uint64_t> seenTokens;
        Vector3 slot0Hips{};
        for (int i = 0; i < 5; ++i) {
            GKE2E_CHECK(ctx, f.slots[i].occupied);
            GKE2E_CHECK(ctx, NearVec3(SlotOffsetVec3(f.slots[i]), {0.5f * static_cast<float>(i), 0.0f, 0.0f}, 1e-3f));
            const FakeActor* boundActor = FindActorByToken(core, f.slots[i].actorToken);
            GKE2E_CHECK(ctx, boundActor != nullptr);
            if (!boundActor) continue;
            GKE2E_CHECK(ctx, std::find(seenTokens.begin(), seenTokens.end(), f.slots[i].actorToken) == seenTokens.end());
            seenTokens.push_back(f.slots[i].actorToken);
            const Vector3 hipsPos = WorldPosition(&boundActor->hips);
            if (i == 0) {
                slot0Hips = hipsPos;
                GKE2E_CHECK(ctx, NearVec3(slot0Hips, {0.0f, 1.0f, 0.0f}, 1e-3f));
            } else {
                GKE2E_CHECK(ctx, NearVec3(hipsPos, {0.5f * static_cast<float>(i), 1.0f, 0.0f}, 1e-3f));
                GKE2E_CHECK(ctx, NearVec3(hipsPos, slot0Hips + Vector3{0.5f * static_cast<float>(i), 0.0f, 0.0f}, 1e-3f));
            }
        }
    });
    core.AbandonScene();
}

void Test_T2_F7_07(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3CoreOrPending(ctx, "F7 solo Preview/Paused retention & ensemble probe miss hysteresis")) return;
    TempWorkspace ws("t2_f7_07");
    Core core;
    InitCoreWorkspace(core, ws, 3);
    std::vector<FakeActor> actors(2);
    actors[0].Init(795, "hski", "Saki", {0.0f, 0.0f, 0.0f});
    actors[1].Init(796, "ttmr", "Temari", {1.5f, 0.0f, 0.0f});

    // Case 1: Solo actor in group-song Preview and solo Paused must NOT be evicted when fnEnsembleActorEligible returns false.
    core.fnEnsembleActorEligible = [](void*) -> bool { return false; };
    core.PublishSceneActors({MakeRoster(actors)[0]});
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, false));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(core.player.state), static_cast<int>(PlaybackState::Preview));
    RunFrameAll(core, actors, 1, 0.0f, {0});
    const auto sessionAfterBind = core.injectionSession;
    GKE2E_CHECK(ctx, !core.player.controller.bindings.empty());
    GKE2E_CHECK(ctx, core.player.controller.targetActor == &actors[0]);

    for (int scan = 0; scan < 5; ++scan) {
        core.PublishSceneActors({MakeRoster(actors)[0]});
    }
    GKE2E_CHECK_EQ(ctx, core.injectionSession, sessionAfterBind);
    GKE2E_CHECK(ctx, !core.player.controller.bindings.empty());
    GKE2E_CHECK(ctx, core.player.controller.targetActor == &actors[0]);
    GKE2E_CHECK(ctx, core.formation.slots[0].occupied);
    GKE2E_CHECK_EQ(ctx, core.formation.slots[0].actorInstanceId, 795);

    // Case 2: Multi-actor formation survives up to 3 transient fnEnsembleActorEligible misses while actors stay registered.
    core.fnEnsembleActorEligible = [](void*) -> bool { return true; };
    core.PublishSceneActors(MakeRoster(actors));
    GKE2E_CHECK(ctx, core.SelectMotionByIndex(0, true));
    RunFrameAll(core, actors, 1, 0.016f, {0, 1});
    GKE2E_CHECK(ctx, core.formation.active);
    const size_t trackCountBefore = core.formationTracks.size();
    GKE2E_CHECK_EQ(ctx, static_cast<int>(trackCountBefore), 2);

    core.fnEnsembleActorEligible = [](void*) -> bool { return false; };
    for (int miss = 0; miss < 3; ++miss) {
        core.PublishSceneActors(MakeRoster(actors));
        GKE2E_CHECK(ctx, core.formation.active);
        GKE2E_CHECK_EQ(ctx, core.formationTracks.size(), trackCountBefore);
    }

    // Immediate removal from sceneActorRegistry must still retire immediately even during hysteresis.
    core.PublishSceneActors({MakeRoster(actors)[0]});
    GKE2E_CHECK(ctx, !core.formation.active);
    GKE2E_CHECK_EQ(ctx, static_cast<int>(core.formationTracks.size()), 0);

    core.fnEnsembleActorEligible = nullptr;
    core.AbandonScene();
}

} // namespace

int main(int argc, char** argv) {
    gke2e::Runner runner;

    // F7: 6 T1 + 6 T2
    runner.Add("T1-F7-01", 1, "F7", "Auto-activation on >=2 actors", Test_T1_F7_01);
    runner.Add("T1-F7-02", 1, "F7", "5-idol simultaneous driving", Test_T1_F7_02);
    runner.Add("T1-F7-03", 1, "F7", "Preview frame-0 multi-actor pose", Test_T1_F7_03);
    runner.Add("T1-F7-04", 1, "F7", "Shared clock & single audio owner", Test_T1_F7_04);
    runner.Add("T1-F7-05", 1, "F7", "Automatic background read-only validation", Test_T1_F7_05);
    runner.Add("T1-F7-06", 1, "F7", "BurstBridgeMultiClient 5-slot context isolation", Test_T1_F7_06);
    runner.Add("T2-F7-01", 2, "F7", "Boundary 1-actor solo vs 2-actor auto-switch", Test_T2_F7_01);
    runner.Add("T2-F7-02", 2, "F7", "Legacy probe toggle & standalone panel removed", Test_T2_F7_02);
    runner.Add("T2-F7-03", 2, "F7", "Legacy group presets & cross-song mix-match removed", Test_T2_F7_03);
    runner.Add("T2-F7-04", 2, "F7", "Actor removal & pointer-reuse safety", Test_T2_F7_04);
    runner.Add("T2-F7-05", 2, "F7", "Capture suspend/resume across 3+ actors", Test_T2_F7_05);
    runner.Add("T2-F7-06", 2, "F7", "Un-drivable actor exclusion (fnCanDriveActor == false)", Test_T2_F7_06);
    runner.Add("T2-F7-07", 2, "F7", "Solo binding retention & ensemble probe miss hysteresis", Test_T2_F7_07);

    // F8: 6 T1 + 5 T2
    runner.Add("T1-F8-01", 1, "F8", "Anchor origin + identity rotation local +X", Test_T1_F8_01);
    runner.Add("T1-F8-02", 1, "F8", "Anchor rotated +90 deg Y local right", Test_T1_F8_02);
    runner.Add("T1-F8-03", 1, "F8", "Arbitrary anchor yaw (37 deg) + origin", Test_T1_F8_03);
    runner.Add("T1-F8-04", 1, "F8", "Yaw pivot around anchored slot position", Test_T1_F8_04);
    runner.Add("T1-F8-05", 1, "F8", "Dynamic slot-1 movement updates dependents without drift", Test_T1_F8_05);
    runner.Add("T1-F8-06", 1, "F8", "Stop / ResetToRestPose restores original pose without anchor residue", Test_T1_F8_06);
    runner.Add("T2-F8-01", 2, "F8", "Boundary yaw angles (+-180, 360, 450)", Test_T2_F8_01);
    runner.Add("T2-F8-02", 2, "F8", "Non-finite local offset / yaw rejected", Test_T2_F8_02);
    runner.Add("T2-F8-03", 2, "F8", "Zero offset (0,0,0,0) preserves native VMD path", Test_T2_F8_03);
    runner.Add("T2-F8-04", 2, "F8", "Null/cleared anchor fallback", Test_T2_F8_04);
    runner.Add("T2-F8-05", 2, "F8", "3D anchor orientation local-basis check", Test_T2_F8_05);

    // F9: 6 T1 + 6 T2
    runner.Add("T1-F9-01", 1, "F9", "Group song 2+ default (0,0,0,0)", Test_T1_F9_01);
    runner.Add("T1-F9-02", 1, "F9", "Group song explicit info.json placement", Test_T1_F9_02);
    runner.Add("T1-F9-03", 1, "F9", "Single-song 2+ auto-placement (x+0.5,y,z)", Test_T1_F9_03);
    runner.Add("T1-F9-04", 1, "F9", "Group-song overflow auto-place & sourceSlotIndex", Test_T1_F9_04);
    runner.Add("T1-F9-05", 1, "F9", "Drag-drop swap occupied <-> occupied", Test_T1_F9_05);
    runner.Add("T1-F9-06", 1, "F9", "Drag-drop swap occupied <-> empty + slot", Test_T1_F9_06);
    runner.Add("T2-F9-01", 2, "F9", "Slot count invariant across solo/group/overflow", Test_T2_F9_01);
    runner.Add("T2-F9-02", 2, "F9", "Out-of-range slot operations rejected", Test_T2_F9_02);
    runner.Add("T2-F9-03", 2, "F9", "Self-swap (from == to) is safe no-op", Test_T2_F9_03);
    runner.Add("T2-F9-04", 2, "F9", "Independent post-load slot offset & dirty flag", Test_T2_F9_04);
    runner.Add("T2-F9-05", 2, "F9", "Group track missing slot clip fallback", Test_T2_F9_05);
    runner.Add("T2-F9-06", 2, "F9", "Mid-session roster shrink/grow stability", Test_T2_F9_06);

    // F10: 6 T1 + 6 T2
    runner.Add("T1-F10-01", 1, "F10", "facial_weights.json by_character round-trip", Test_T1_F10_01);
    runner.Add("T1-F10-02", 1, "F10", "Legacy global facial_weights.json fallback", Test_T1_F10_02);
    runner.Add("T1-F10-03", 1, "F10", "Bone save to current track -> slot bone_overrides.json", Test_T1_F10_03);
    runner.Add("T1-F10-04", 1, "F10", "Bone save to global -> configRoot/bone_overrides.json", Test_T1_F10_04);
    runner.Add("T1-F10-05", 1, "F10", "Placement save for slot 1 -> global initial_position.json", Test_T1_F10_05);
    runner.Add("T1-F10-06", 1, "F10", "Placement save for slot 2+ -> group info.json (zero rebake)", Test_T1_F10_06);
    runner.Add("T2-F10-01", 2, "F10", "Facial weight boundaries (0.0, 2.0, <0, >2, NaN)", Test_T2_F10_01);
    runner.Add("T2-F10-02", 2, "F10", "Corrupt facial_weights.json recovery", Test_T2_F10_02);
    runner.Add("T2-F10-03", 2, "F10", "Overflow actor bone save writes to sourceSlotIndex", Test_T2_F10_03);
    runner.Add("T2-F10-04", 2, "F10", "Solo-song 2+ placement save boundary", Test_T2_F10_04);
    runner.Add("T2-F10-05", 2, "F10", "Unrecognized/NPC actor (character == empty) fallback", Test_T2_F10_05);
    runner.Add("T2-F10-06", 2, "F10", "Missing info.json on legacy track placement save", Test_T2_F10_06);

    // F11: 6 T1 + 5 T2
    runner.Add("T1-F11-01", 1, "F11", "followSlotIndex = -1 arithmetic mean (Hips)", Test_T1_F11_01);
    runner.Add("T1-F11-02", 1, "F11", "followSlotIndex = -1 Chest & Head targets", Test_T1_F11_02);
    runner.Add("T1-F11-03", 1, "F11", "followSlotIndex = 0..N-1 specific slot tracking", Test_T1_F11_03);
    runner.Add("T1-F11-04", 1, "F11", "camera.Tick triggered after last active actor writes bones", Test_T1_F11_04);
    runner.Add("T1-F11-05", 1, "F11", "Empty + slots excluded from -1 arithmetic mean", Test_T1_F11_05);
    runner.Add("T1-F11-06", 1, "F11", "camera.json persists followSlotIndex", Test_T1_F11_06);
    runner.Add("T2-F11-01", 2, "F11", "CameraSettings::IsValid() on followSlotIndex", Test_T2_F11_01);
    runner.Add("T2-F11-02", 2, "F11", "Empty or out-of-range followSlotIndex target", Test_T2_F11_02);
    runner.Add("T2-F11-03", 2, "F11", "Partial fnFollowTarget failure in -1 mode", Test_T2_F11_03);
    runner.Add("T2-F11-04", 2, "F11", "Stopped releases camera in multi-actor mode", Test_T2_F11_04);
    runner.Add("T2-F11-05", 2, "F11", "CameraMode::Track stability across actor callbacks", Test_T2_F11_05);

    // Tier 3 Core: 7 pairwise tests
    runner.Add("T3-M3-01", 3, "F7", "Pairwise F7 x F8: 5-actor auto-driving under rotated 90 deg anchor", Test_T3_M3_01);
    runner.Add("T3-M3-02", 3, "F8", "Pairwise F8 x F9: slot swap preserves local offsets under rotated anchor", Test_T3_M3_02);
    runner.Add("T3-M3-03", 3, "F9", "Pairwise F9 x F10: swap into + slot + by_character facial + slot bone overrides", Test_T3_M3_03);
    runner.Add("T3-M3-04", 3, "F7", "Pairwise F7 x F11: shuffled 5-actor callback order + followSlotIndex=-1 post-write tick", Test_T3_M3_04);
    runner.Add("T3-M3-05", 3, "F9", "Pairwise F9 x F11: slot swap updates followSlotIndex=0 target", Test_T3_M3_05);
    runner.Add("T3-M3-06", 3, "F8", "Pairwise F8 x F10: save slot 2+ offset -> reload -> 180 deg anchor", Test_T3_M3_06);
    runner.Add("T3-M3-08", 3, "F10", "Pairwise F10 x F11: persist followSlotIndex=-1 + reload + CaptureGuard", Test_T3_M3_08);

    // Tier 4 Core: 2 real-world scenarios
    runner.Add("T4-M3-01", 4, "F9", "5-Idol Group-Song Live Stage Session (F7+F8+F9+F10+F11)", Test_T4_M3_01);
    runner.Add("T4-M3-02", 4, "F9", "Overflow + Empty + Swap + Solo Auto-Placement Lifecycle (F7+F8+F9+F10+F11)", Test_T4_M3_02);

    return runner.Run(argc, argv);
}
