#include "multi_actor_fixture.hpp"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

GK_E2E_DEFINE_LOG_SINK()

namespace {

struct RecordedItem {
    ImRect rect;
    std::string label;
    ImGuiItemFlags flags{0};
    ImGuiItemStatusFlags status{0};
};

std::map<ImGuiID, RecordedItem> g_items;
std::vector<ImGuiID> g_itemOrder;

} // namespace

// External-linkage ImGui Test Engine hooks required by IMGUI_ENABLE_TEST_ENGINE.
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* context, ImGuiID id, const ImRect& rect, const ImGuiLastItemData* data) {
    if (!id) return;
    if (g_items.find(id) == g_items.end()) {
        g_itemOrder.push_back(id);
    }
    g_items[id].rect = rect;
    g_items[id].flags = data ? data->InFlags : 0;
    if (context && context->CurrentWindow && !context->CurrentWindow->IDStack.empty()) {
        for (const char* label : {
                "##play_mode", "##playback_speed", "##camera_quick_mode",
                "##physics_engine_select", "##drive_actor_combo", "##camera_mode",
                "跟随部位", "##prev", "##next", "##play", "##pause", "##stop"}) {
            if (ImHashStr(label, 0, context->CurrentWindow->IDStack.back()) == id) {
                g_items[id].label = label;
            }
        }
    }
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags status) {
    if (!id) return;
    if (g_items.find(id) == g_items.end()) {
        g_itemOrder.push_back(id);
    }
    g_items[id].label = label ? label : "";
    g_items[id].status = status;
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id) {
    const auto found = g_items.find(id);
    return found == g_items.end() ? nullptr : found->second.label.c_str();
}

namespace {

using namespace GakuMotion;
using namespace gkmotion::e2e::multi_actor;
namespace fx = gkmotion::e2e::fixtures;

class ImGuiHarness {
public:
    TempWorkspace ws;
    Core core;
    float windowWidth{1120.0f};
    float windowHeight{900.0f};
    float fontScale{1.0f};
    bool captureText{false};
    std::string renderedText;
    std::function<void(GuiContext&)> contextPatch;

    explicit ImGuiHarness(std::string_view tag, int groupSlots = 0)
        : ws(tag) {
        auto& io = ImGui::GetIO();
        savedDisplaySize_ = io.DisplaySize;
        savedFontScale_ = io.FontGlobalScale;
        savedConfigFlags_ = io.ConfigFlags;

        core.initialized = true;
        core.SetControlActive(true);
        core.configRoot = MotionPathToUtf8(ws.Sub("config"));
        core.motionsRoot = MotionPathToUtf8(ws.Sub("motions"));
        core.gui.motionsRoot = core.motionsRoot;
        core.playlistPath = MotionPathToUtf8(ws.Sub("config/playlist.json"));
        core.initialPositionPath = MotionPathToUtf8(ws.Sub("config/initial_position.json"));
        core.globalOverridePath = MotionPathToUtf8(ws.Sub("config/bone_overrides.json"));
        std::error_code ec;
        std::filesystem::create_directories(ws.Sub("config"), ec);
        std::filesystem::create_directories(ws.Sub("motions"), ec);

        WireIdolController(core.player.controller);
        WireCoreAudioCounters(core);

        // Populate a solo song and optionally a group song.
        fx::TrackInfoSpec solo;
        solo.title = "Solo ImGui Song";
        solo.subtitle = "Tester";
        solo.total_frames = 30;
        solo.fps = 30.0f;
        solo.duration_seconds = 1.0f;
        solo.audio_file = "song.wav";
        solo.audio_duration = 1.0f;
        fx::write_synthetic_track_dir(ws.Sub("motions/solo_imgui"), solo, true, true);

        if (groupSlots >= 2) {
            fx::write_synthetic_group_track(ws.Sub("motions/group_imgui"), groupSlots, "Group ImGui Song", "Tester", true, true);
        }
        core.RefreshMotionLibrary();
        if (!core.motions.empty()) {
            core.SelectMotionByIndex(0, false);
        }
        core.gui.activePage = GuiPage::Motion;
    }

    ~ImGuiHarness() {
        g_libraryModal.Reset();
        core.AbandonScene();
        auto& io = ImGui::GetIO();
        io.DisplaySize = savedDisplaySize_;
        io.FontGlobalScale = savedFontScale_;
        io.ConfigFlags = savedConfigFlags_;
    }

    void Frame(bool hosted = true, bool customInjectionOverride = false, bool forcedInjectionAllowed = true) {
        g_items.clear();
        g_itemOrder.clear();
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(windowWidth, windowHeight);
        io.DeltaTime = 1.0f / 60.0f;
        io.FontGlobalScale = fontScale;

        ImGui::NewFrame();
        if (hosted) {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight));
            ImGui::Begin("E2E_ImGui_Host", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        }
        if (captureText) {
            ImGui::LogToBuffer();
        }
        auto ctx = core.BuildGuiContext();
        ctx.injectionAllowed = customInjectionOverride ? forcedInjectionAllowed : true;
        if (contextPatch) {
            contextPatch(ctx);
        }
        core.gui.Draw(ctx, hosted);
        if (captureText) {
            renderedText = GImGui->LogBuffer.c_str();
            ImGui::LogFinish();
            captureText = false;
        }
        if (hosted) {
            ImGui::End();
        }
        ImGui::Render();
        core.ProcessPendingGuiCommands();
    }

    std::string CaptureText(bool hosted = true) {
        ImGui::GetIO().AddMousePosEvent(-100.0f, -100.0f);
        captureText = true;
        Frame(hosted);
        return renderedText;
    }

    const RecordedItem* FindItem(std::string_view label) const {
        for (ImGuiID id : g_itemOrder) {
            auto it = g_items.find(id);
            if (it != g_items.end() && it->second.label == label) {
                return &it->second;
            }
        }
        return nullptr;
    }

    bool ContainsItemContaining(std::string_view substr) const {
        for (ImGuiID id : g_itemOrder) {
            auto it = g_items.find(id);
            if (it != g_items.end() && it->second.label.find(substr) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    bool Click(std::string_view label) {
        const auto* item = FindItem(label);
        if (!item) return false;
        ImVec2 center = item->rect.GetCenter();
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(center.x, center.y);
        Frame();
        const auto* settledItem = FindItem(label);
        if (!settledItem) return false;
        const ImVec2 settledCenter = settledItem->rect.GetCenter();
        if (std::fabs(settledCenter.x - center.x) > 0.5f || std::fabs(settledCenter.y - center.y) > 0.5f) {
            center = settledCenter;
            io.AddMousePosEvent(center.x, center.y);
            Frame();
        }
        io.AddMouseButtonEvent(0, true);
        Frame();
        io.AddMouseButtonEvent(0, false);
        Frame();
        Frame();
        return true;
    }

private:
    ImVec2 savedDisplaySize_{1120.0f, 900.0f};
    float savedFontScale_{1.0f};
    ImGuiConfigFlags savedConfigFlags_{0};
};

// ============================================================================
// F12 Tier 1 Tests (6 tests)
// ============================================================================

void Test_T1_F12_01(gke2e::CaseContext& ctx) {
    ImGuiHarness app("t1_f12_01", 2);
    app.core.gui.developerMode = true;
    const GuiPage pages[] = {
        GuiPage::Library, GuiPage::Motion, GuiPage::Camera,
        GuiPage::Stage, GuiPage::Settings, GuiPage::Developer
    };
    for (GuiPage page : pages) {
        app.core.gui.activePage = page;
        app.Frame(true);
        const ImDrawData* drawData = ImGui::GetDrawData();
        GKE2E_CHECK(ctx, drawData != nullptr);
        if (drawData) {
            GKE2E_CHECK(ctx, drawData->Valid);
            GKE2E_CHECK(ctx, drawData->CmdListsCount > 0);
        }
    }
    GKE2E_PASS(ctx, "Rendered all 6 GuiPage tabs headlessly with valid ImDrawData");
}

void Test_T1_F12_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3ImGuiOrPending(ctx, "F12 legacy MotionPage ensemble/probe removal")) return;
    ImGuiHarness app("t1_f12_02", 2);
    std::vector<FakeActor> actors(2);
    actors[0].Init(1201, "hski", "Saki", {0, 0, 0});
    actors[1].Init(1202, "ttmr", "Temari", {1, 0, 0});
    app.core.PublishSceneActors(MakeRoster(actors));
    app.core.gui.activePage = GuiPage::Motion;
    const std::string text = app.CaptureText(true);
    GKE2E_CHECK(ctx, text.find("多人舞台") == std::string::npos);
    GKE2E_CHECK(ctx, text.find("摄影槽位验证探针") == std::string::npos);
    GKE2E_CHECK(ctx, text.find("启用多人舞台") == std::string::npos);
    GKE2E_CHECK(ctx, text.find("保存舞台预设") == std::string::npos);
    GKE2E_CHECK(ctx, text.find("载入舞台预设") == std::string::npos);
    GKE2E_CHECK(ctx, !app.ContainsItemContaining("多人舞台"));
    GKE2E_CHECK(ctx, !app.ContainsItemContaining("摄影槽位验证探针"));
}

void Test_T1_F12_03(gke2e::CaseContext& ctx) {
    ImGuiHarness app("t1_f12_03", 3);
    std::vector<FakeActor> actors(3);
    actors[0].Init(1210, "hski", "Saki", {0, 0, 0});
    actors[1].Init(1211, "ttmr", "Temari", {1, 0, 0});
    actors[2].Init(1212, "fktn", "Kotone", {2, 0, 0});
    app.core.PublishSceneActors(MakeRoster(actors));

    const ImTextureID fontTexId = ImGui::GetIO().Fonts->TexID;
    for (GuiPage page : {GuiPage::Motion, GuiPage::Library}) {
        app.core.gui.activePage = page;
        const std::string text = app.CaptureText(true);
        GKE2E_CHECK(ctx, text.find("ActorSlotStrip") == std::string::npos);
        GKE2E_CHECK(ctx, text.find("RotaryKnob") == std::string::npos);
        GKE2E_CHECK(ctx, text.find("当前位置上无角色") == std::string::npos);
        GKE2E_CHECK(ctx, text.find("编辑初始位置") == std::string::npos);
        GKE2E_CHECK(ctx, !app.ContainsItemContaining("RotaryKnob"));
        GKE2E_CHECK(ctx, !app.ContainsItemContaining("ActorSlotStrip"));

        const ImDrawData* drawData = ImGui::GetDrawData();
        GKE2E_REQUIRE(ctx, drawData != nullptr);
        for (int i = 0; i < drawData->CmdListsCount; ++i) {
            const ImDrawList* cl = drawData->CmdLists[i];
            for (int c = 0; c < cl->CmdBuffer.Size; ++c) {
                GKE2E_CHECK(ctx, cl->CmdBuffer[c].GetTexID() == fontTexId);
            }
        }
    }
    GKE2E_PASS(ctx, "Confirmed absence of portrait cards, rotary knobs, and non-font ImTextureIDs");
}

void Test_T1_F12_04(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3ImGuiOrPending(ctx, "F12 GuiTransportBar multi-actor adaptation")) return;
    ImGuiHarness app("t1_f12_04", 2);
    std::vector<FakeActor> actors(2);
    actors[0].Init(1220, "hski", "Saki", {0, 0, 0});
    actors[1].Init(1221, "ttmr", "Temari", {1, 0, 0});
    app.core.PublishSceneActors(MakeRoster(actors));
    app.Frame(true);
    const auto* prevBtn = app.FindItem("##prev");
    const auto* nextBtn = app.FindItem("##next");
    GKE2E_REQUIRE(ctx, prevBtn != nullptr && nextBtn != nullptr);
    GKE2E_CHECK(ctx, (prevBtn->flags & ImGuiItemFlags_Disabled) == 0);
    GKE2E_CHECK(ctx, (nextBtn->flags & ImGuiItemFlags_Disabled) == 0);
    const std::string text = app.CaptureText(true);
    GKE2E_CHECK(ctx, text.find("演出一次") == std::string::npos);
    GKE2E_CHECK(ctx, text.find("整场循环") == std::string::npos);
    GKE2E_CHECK(ctx, app.Click("##play"));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(app.core.player.state), static_cast<int>(PlaybackState::Playing));
    GKE2E_CHECK(ctx, app.Click("##pause"));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(app.core.player.state), static_cast<int>(PlaybackState::Paused));
    GKE2E_CHECK(ctx, app.Click("##stop"));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(app.core.player.state), static_cast<int>(PlaybackState::Stopped));
}

template <typename EntryT>
void VerifyGroupEntryInLibraryImpl(gke2e::CaseContext& ctx, const std::vector<EntryT>& entries) {
    if constexpr (gke2e::probes::HasIsGroup<EntryT> && gke2e::probes::HasPositionCount<EntryT>) {
        bool foundGroupEntry = false;
        for (const auto& le : entries) {
            if (le.id == "group_imgui") {
                foundGroupEntry = true;
                GKE2E_CHECK(ctx, le.isGroup);
                GKE2E_CHECK_EQ(ctx, le.positionCount, 3);
            }
        }
        GKE2E_CHECK(ctx, foundGroupEntry);
    } else {
        (void)ctx;
        (void)entries;
    }
}

template <typename CamSettingsT>
void SetFollowSlotIndexIfPresent(CamSettingsT& s, int slotIndex) {
    if constexpr (probes::HasFollowSlotIndex<CamSettingsT>) {
        s.followSlotIndex = slotIndex;
    } else {
        (void)s;
        (void)slotIndex;
    }
}

template <typename CamSettingsT>
void CheckFollowSlotIndexIfPresent(gke2e::CaseContext& ctx, const CamSettingsT& s, int expected) {
    if constexpr (probes::HasFollowSlotIndex<CamSettingsT>) {
        GKE2E_CHECK_EQ(ctx, s.followSlotIndex, expected);
    } else {
        (void)ctx;
        (void)s;
        (void)expected;
    }
}

void Test_T1_F12_05(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3ImGuiOrPending(ctx, "F12 GuiLibraryPage & GuiCameraPage group-song / multi-actor compatibility") ||
        !probes::RequireM3CoreOrPending(ctx, "F12 GuiCameraPage followSlotIndex multi-actor compatibility")) return;
    ImGuiHarness app("t1_f12_05", 3);
    std::vector<FakeActor> actors(3);
    actors[0].Init(1230, "hski", "Saki", {0, 0, 0});
    actors[1].Init(1231, "ttmr", "Temari", {1, 0, 0});
    actors[2].Init(1232, "fktn", "Kotone", {2, 0, 0});
    app.core.PublishSceneActors(MakeRoster(actors));

    app.core.gui.activePage = GuiPage::Library;
    app.Frame(true);
    GKE2E_CHECK(ctx, app.core.gui.libraryEntries.size() >= 2);
    bool foundGroupMotion = false;
    for (const auto& m : app.core.motions) {
        if (m.name == "group_imgui") {
            foundGroupMotion = true;
            GKE2E_CHECK(ctx, m.isGroup);
            GKE2E_CHECK_EQ(ctx, m.positionCount, 3);
        }
    }
    GKE2E_CHECK(ctx, foundGroupMotion);
    VerifyGroupEntryInLibraryImpl(ctx, app.core.gui.libraryEntries);

    app.core.gui.activePage = GuiPage::Camera;
    app.core.camera.settings.enabled = true;
    app.core.camera.settings.mode = CameraMode::Follow;
    SetFollowSlotIndexIfPresent(app.core.camera.settings, 1);
    app.core.camera.Publish();
    app.Frame(true);
    GKE2E_CHECK(ctx, app.FindItem("##camera_mode") != nullptr);
    GKE2E_CHECK(ctx, app.FindItem("##rate_x") != nullptr);
    GKE2E_CHECK(ctx, app.Click("保存镜头设置"));
    GKE2E_CHECK(ctx, std::filesystem::exists(app.ws.Sub("config/camera.json")));

    Core reopened;
    reopened.configRoot = MotionPathToUtf8(app.ws.Sub("config"));
    GKE2E_CHECK(ctx, reopened.LoadCameraSettings());
    CheckFollowSlotIndexIfPresent(ctx, reopened.camera.settings, 1);
    GKE2E_PASS(ctx, "Verified GuiLibraryPage group-song sync and GuiCameraPage multi-actor save round-trip");
}

void Test_T1_F12_06(gke2e::CaseContext& ctx) {
    ImGuiHarness app("t1_f12_06", 2);
    app.core.gui.RefreshMotions();
    app.core.gui.showWindow = true;
    app.Frame(false, true, true); // non-hosted standalone window
    GKE2E_CHECK(ctx, ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->Valid);

    app.core.gui.showWindow = false;
    app.Frame(false, true, true); // collapsed "打开 GakuMotion 控制面板" window
    GKE2E_CHECK(ctx, app.FindItem("打开 GakuMotion 控制面板") != nullptr);

    app.core.gui.showWindow = true;
    app.Frame(true, true, false); // hosted with injectionAllowed = false
    GKE2E_CHECK(ctx, ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->Valid);
    GKE2E_PASS(ctx, "Verified non-hosted, collapsed, and disabled draw paths");
}

// ============================================================================
// F12 Tier 2 Tests (6 tests)
// ============================================================================

void Test_T2_F12_01(gke2e::CaseContext& ctx) {
    GuiOverlay overlay;
    overlay.developerMode = true;
    const GuiPage pages[] = {
        GuiPage::Library, GuiPage::Motion, GuiPage::Camera,
        GuiPage::Stage, GuiPage::Settings, GuiPage::Developer
    };
    for (bool allowed : {false, true}) {
        for (GuiPage page : pages) {
            overlay.activePage = page;
            g_items.clear();
            g_itemOrder.clear();
            auto& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1120.0f, 900.0f);
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(1120.0f, 900.0f));
            ImGui::Begin("NullCtxHost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            ImGui::LogToBuffer();
            GuiContext emptyCtx{};
            emptyCtx.injectionAllowed = allowed;
            overlay.Draw(emptyCtx, true);
            const std::string txt = GImGui->LogBuffer.c_str();
            ImGui::LogFinish();
            ImGui::End();
            ImGui::Render();
            const ImDrawData* dd = ImGui::GetDrawData();
            GKE2E_CHECK(ctx, dd != nullptr && dd->Valid);
            if (allowed && page == GuiPage::Motion) {
                GKE2E_CHECK(ctx, txt.find("尚未绑定") != std::string::npos || txt.find("尚未就绪") != std::string::npos);
            }
        }
    }
    g_libraryModal.Reset();
    GKE2E_PASS(ctx, "Null/default-constructed GuiContext{} rendered safely across all pages");
}

template <typename GuiCtxT>
void VerifyFormationInGuiContextImpl(gke2e::CaseContext& ctx, const GuiCtxT& c, size_t minSlots = 1) {
    if constexpr (probes::HasFormationState<GuiCtxT>) {
        GKE2E_CHECK(ctx, c.formation.slots.size() >= minSlots);
    } else if constexpr (probes::HasStageFormationMemberGui<GuiCtxT>) {
        GKE2E_CHECK(ctx, c.stageFormation.slots.size() >= minSlots);
    }
}

void Test_T2_F12_02(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3ImGuiOrPending(ctx, "F12 GuiMotionPage actor-count boundaries (0, 1, 6) & epoch bumps")) return;
    ImGuiHarness app("t2_f12_02", 3);
    app.core.gui.activePage = GuiPage::Motion;
    const PlaybackState states[] = {
        PlaybackState::Stopped, PlaybackState::Preview, PlaybackState::Playing, PlaybackState::Paused
    };
    uint64_t prevEpoch = app.core.sceneActorRegistry.Epoch();
    for (int count : {1, 6, 0}) {
        std::vector<FakeActor> actors(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            const std::string code = (i == 0) ? "" : ("hski");
            const std::string name = (i == 0) ? "测试角色" : ("Idol" + std::to_string(i));
            actors[static_cast<size_t>(i)].Init(1250 + i, code, name, {static_cast<float>(i), 0, 0});
        }
        app.core.PublishSceneActors(MakeRoster(actors));
        const auto guiCtx = app.core.BuildGuiContext();
        GKE2E_CHECK(ctx, guiCtx.actorRosterEpoch > prevEpoch);
        prevEpoch = guiCtx.actorRosterEpoch;
        if (count == 6) {
            VerifyFormationInGuiContextImpl(ctx, guiCtx, 6);
        }
        for (PlaybackState st : states) {
            app.core.player.state = st;
            app.Frame(true);
            GKE2E_CHECK(ctx, ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->Valid);
        }
    }
    const auto finalCtx = app.core.BuildGuiContext();
    VerifyFormationInGuiContextImpl(ctx, finalCtx, 1);
    GKE2E_PASS(ctx, "Handled 0/1/6 actor boundaries, epoch bumps, and all PlaybackStates safely");
}

void Test_T2_F12_03(gke2e::CaseContext& ctx) {
    ImGuiHarness app("t2_f12_03", 2);
    const std::pair<float, float> viewports[] = {{320.0f, 200.0f}, {360.0f, 1800.0f}, {1920.0f, 1080.0f}};
    const float scales[] = {0.75f, 1.5f, 2.0f};
    const GuiPage pages[] = {GuiPage::Library, GuiPage::Motion, GuiPage::Camera, GuiPage::Stage};

    for (auto vp : viewports) {
        for (float sc : scales) {
            app.windowWidth = vp.first;
            app.windowHeight = vp.second;
            app.fontScale = sc;
            for (GuiPage page : pages) {
                app.core.gui.activePage = page;
                app.Frame(true);
                GKE2E_CHECK(ctx, app.core.gui.transportMeasuredHeight > 0.0f);
                GKE2E_CHECK(ctx, std::isfinite(app.core.gui.transportMeasuredWidth));
                GKE2E_CHECK(ctx, ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->Valid);
            }
        }
    }
    GKE2E_PASS(ctx, "Rendered cleanly across extreme viewports and FontGlobalScale boundaries");
}

void Test_T2_F12_04(gke2e::CaseContext& ctx) {
    ImGuiHarness app("t2_f12_04", 0);
    app.core.gui.scrub.active = true;
    app.core.gui.scrub.value = 0.5f;
    app.core.gui.transportNavigation.editingTime = true;
    app.core.gui.transportNavigation.editingFrame = true;
    g_libraryModal.isOpen = true;
    const float beforeTime = app.core.player.currentTime;

    app.Frame(true, true, false); // injectionAllowed = false
    GKE2E_CHECK(ctx, !app.core.gui.scrub.active);
    GKE2E_CHECK(ctx, !app.core.gui.transportNavigation.editingTime);
    GKE2E_CHECK(ctx, !app.core.gui.transportNavigation.editingFrame);
    GKE2E_CHECK_NEAR(ctx, app.core.player.currentTime, beforeTime, 1e-4f);
    g_libraryModal.Reset();
    GKE2E_PASS(ctx, "Permission loss cancelled active scrub without committing seek");
}

void Test_T2_F12_05(gke2e::CaseContext& ctx) {
    ImGuiHarness app("t2_f12_05", 0);
    MotionEntry emptyClip;
    emptyClip.name = "corrupt_empty";
    emptyClip.title = "";
    emptyClip.dir = MotionPathToUtf8(app.ws.Sub("motions/empty"));
    emptyClip.totalFrames = 0;
    emptyClip.durationSeconds = 0.0f;
    app.core.motions.push_back(emptyClip);
    app.core.currentMotionIndex = 999;
    app.core.player.clip.total_frames = 0;
    app.core.player.clip.duration_seconds = 0.0f;
    std::snprintf(app.core.gui.libraryFilter.InputBuf, sizeof(app.core.gui.libraryFilter.InputBuf), "zzzz_nomatch_zzz");
    app.core.gui.libraryFilter.Build();

    app.core.gui.activePage = GuiPage::Library;
    const std::string text = app.CaptureText(true);
    GKE2E_CHECK(ctx, ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->Valid);
    GKE2E_CHECK(ctx, text.find("00:00") != std::string::npos);

    app.core.currentMotionIndex = -1;
    app.Frame(true);
    GKE2E_CHECK(ctx, ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->Valid);
    GKE2E_PASS(ctx, "Handled empty/OOB library entries and zero-frame clips safely");
}

void Test_T2_F12_06(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3ImGuiOrPending(ctx, "F12 legacy ensemble symbol retirement & invalid camera bounds")) return;
    GKE2E_CHECK(ctx, !probes::HasLegacyEnsembleSnapshot<GuiContext>);
    GKE2E_CHECK(ctx, !probes::HasLegacyEnsembleDraft<GuiOverlay>);
    GKE2E_CHECK(ctx, !probes::HasLegacyEnsembleActive<Core>);

    ImGuiHarness app("t2_f12_06", 2);
    app.core.gui.activePage = GuiPage::Camera;
    app.core.camera.settings.enabled = true;
    app.core.camera.settings.limitToScene = true;
    app.core.camera.settings.boundsMin = {5.0f, 0.0f, 0.0f};
    app.core.camera.settings.boundsMax = {1.0f, 2.0f, 2.0f};
    GKE2E_CHECK(ctx, !app.core.camera.settings.IsValid());
    app.Frame(true);
    GKE2E_CHECK(ctx, ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->Valid);
}

// ============================================================================
// Tier 3 ImGui Pairwise Test (T3-M3-07: F7 x F12)
// ============================================================================

void Test_T3_M3_07(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3ImGuiOrPending(ctx, "T3 F7 x F12 multi-actor Core + headless ImGui transport") ||
        !probes::RequireM3CoreOrPending(ctx, "T3 F7 x F12 multi-actor Core")) return;
    ImGuiHarness app("t3_m3_07", 3);
    std::vector<FakeActor> actors(3);
    for (int i = 0; i < 3; ++i) actors[i].Init(1370 + i, "hski", "Idol", {static_cast<float>(i), 0, 0});
    app.core.PublishSceneActors(MakeRoster(actors));
    app.core.gui.activePage = GuiPage::Motion;
    app.Frame(true);
    GKE2E_CHECK(ctx, app.Click("##play"));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(app.core.player.state), static_cast<int>(PlaybackState::Playing));
    for (size_t i = 0; i < actors.size(); ++i) {
        app.core.OnLateUpdate(&actors[i], &actors[i].animator, nullptr, 0.016f, i == 0, 0.016f, 1);
    }
    GKE2E_CHECK(ctx, app.Click("##pause"));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(app.core.player.state), static_cast<int>(PlaybackState::Paused));
    GKE2E_CHECK(ctx, app.Click("##stop"));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(app.core.player.state), static_cast<int>(PlaybackState::Stopped));
}

// ============================================================================
// Tier 4 ImGui Real-World Scenario (T4-M3-03: F7+F9+F10+F11+F12)
// ============================================================================

void Test_T4_M3_03(gke2e::CaseContext& ctx) {
    if (!probes::RequireM3ImGuiOrPending(ctx, "T4 Capture Guard + Roster Churn + ImGui Green End-to-End") ||
        !probes::RequireM3CoreOrPending(ctx, "T4 Capture Guard + Roster Churn + ImGui Green End-to-End")) return;
    ImGuiHarness app("t4_m3_03", 4);
    FakeCameraRig camRig; camRig.ResetDefault();
    WireCameraController(app.core.camera, camRig);
    app.core.camera.settings.enabled = true;
    app.core.camera.settings.mode = CameraMode::Follow;

    std::vector<FakeActor> actors(4);
    for (int i = 0; i < 4; ++i) actors[i].Init(1480 + i, "hski", "Idol", {static_cast<float>(i), 0, 0});
    app.core.PublishSceneActors(MakeRoster(actors));
    app.core.gui.activePage = GuiPage::Motion;
    const std::string text = app.CaptureText(true);
    GKE2E_CHECK(ctx, text.find("多人舞台") == std::string::npos);
    GKE2E_CHECK(ctx, text.find("摄影槽位验证探针") == std::string::npos);
    GKE2E_CHECK(ctx, app.Click("##play"));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(app.core.player.state), static_cast<int>(PlaybackState::Playing));
    for (size_t i = 0; i < actors.size(); ++i) {
        app.core.OnLateUpdate(&actors[i], &actors[i].animator, nullptr, 0.016f, i == 0, 0.016f, 1);
    }
    GKE2E_CHECK(ctx, app.core.SuspendForCapture());
    GKE2E_CHECK_EQ(ctx, static_cast<int>(app.core.player.state), static_cast<int>(PlaybackState::Stopped));
    GKE2E_CHECK(ctx, app.core.ResumeAfterCapture());
    GKE2E_CHECK_EQ(ctx, static_cast<int>(app.core.player.state), static_cast<int>(PlaybackState::Playing));
    actors[2].alive = false;
    app.core.PublishSceneActors({MakeRoster(actors)[0], MakeRoster(actors)[1], MakeRoster(actors)[3]});
    app.Frame(true);
    GKE2E_CHECK(ctx, app.Click("##pause"));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(app.core.player.state), static_cast<int>(PlaybackState::Paused));
    GKE2E_CHECK(ctx, app.Click("##stop"));
    GKE2E_CHECK_EQ(ctx, static_cast<int>(app.core.player.state), static_cast<int>(PlaybackState::Stopped));
    GKE2E_CHECK(ctx, ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->Valid);
}

} // namespace

int main(int argc, char** argv) {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1120.0f, 900.0f);
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(1)));
    GImGui->TestEngineHookItems = true;

    gke2e::Runner runner;

    // F12: 6 T1 + 6 T2
    runner.Add("T1-F12-01", 1, "F12", "Headless GkGuiOverlay ImGui context init & all-page frame render", Test_T1_F12_01);
    runner.Add("T1-F12-02", 1, "F12", "GuiMotionPage minimal green render & legacy probe/panel removal", Test_T1_F12_02);
    runner.Add("T1-F12-03", 1, "F12", "Absence of WebView2-only portrait cards & rotary knobs in ImGui", Test_T1_F12_03);
    runner.Add("T1-F12-04", 1, "F12", "GuiTransportBar multi-actor transport & play-mode adaptation", Test_T1_F12_04);
    runner.Add("T1-F12-05", 1, "F12", "GuiLibraryPage & GuiCameraPage group-song / multi-actor compatibility", Test_T1_F12_05);
    runner.Add("T1-F12-06", 1, "F12", "GkGuiOverlay syntax/instantiation parity (non-hosted & disabled modes)", Test_T1_F12_06);

    runner.Add("T2-F12-01", 2, "F12", "Null & default-constructed GuiContext{} defensive rendering", Test_T2_F12_01);
    runner.Add("T2-F12-02", 2, "F12", "GuiMotionPage actor-count boundaries (0, 1, 6) & epoch bumps", Test_T2_F12_02);
    runner.Add("T2-F12-03", 2, "F12", "Extreme viewport dimensions & FontGlobalScale boundaries", Test_T2_F12_03);
    runner.Add("T2-F12-04", 2, "F12", "Permission loss cancels active scrub without committing", Test_T2_F12_04);
    runner.Add("T2-F12-05", 2, "F12", "Corrupt/partial library entries, OOB selectedMotionIndex, & empty clip", Test_T2_F12_05);
    runner.Add("T2-F12-06", 2, "F12", "Legacy ensemble symbol retirement & GuiCameraPage invalid-bounds rejection", Test_T2_F12_06);

    // Tier 3 ImGui pairwise
    runner.Add("T3-M3-07", 3, "F12", "Pairwise F7 x F12: multi-actor Core + headless ImGui transport lifecycle", Test_T3_M3_07);

    // Tier 4 ImGui real-world scenario
    runner.Add("T4-M3-03", 4, "F12", "Capture Guard + Roster Churn + ImGui Green End-to-End (F7+F9+F10+F11+F12)", Test_T4_M3_03);

    const int rc = runner.Run(argc, argv);
    g_libraryModal.Reset();
    ImGui::DestroyContext();
    return rc;
}
