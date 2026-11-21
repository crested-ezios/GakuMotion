#pragma once

// Included after the real ImGui Harness in test_gui_ux.cpp.
inline int ensembleGuiApplyRequests{0};
inline ensemble::Settings ensembleGuiAppliedDraft;
inline uint64_t ensembleGuiRevision{0}, ensembleGuiEpoch{0};
inline void RecordEnsembleGuiApply(void*, const ensemble::Settings& draft, uint64_t revision, uint64_t epoch)
{
    ++ensembleGuiApplyRequests; ensembleGuiAppliedDraft = draft; ensembleGuiRevision = revision; ensembleGuiEpoch = epoch;
}

void TestEnsembleGui(const std::filesystem::path& root)
{
    const ImVec2 originalDisplay = ImGui::GetIO().DisplaySize;
    const int originalAudioPlays = audioPlays, originalAudioResumes = audioResumes, originalAudioPauses = audioPauses;
    ImGui::GetIO().DisplaySize = ImVec2(1120, 1800);
    {
        Harness app(root);
        app.windowHeight = 1800;
        app.core.configRoot = MotionPathToUtf8(app.fixture / "config");
        app.core.gui.activePage = GuiPage::Motion;
        app.contextPatch = [&](GuiContext& context) {
            context.sceneActors = {{101, "摄影角色A", true}, {102, "摄影角色B", false}, {103, "摄影角色C", false}};
            context.fnSelectTargetActor = SelectSceneActor;
            context.ensembleSnapshot.actorEpoch = 77;
        };
        actorSelections = 0;
        app.Settle();
        Check(app.ItemWithLabel("##drive_actor_combo") && app.ItemWithLabel("##pos_x"),
            "disabled ensemble leaves the established single actor and placement controls visible");
        app.Click("多人舞台");
        app.Click("新增角色");
        Check(app.core.gui.ensembleDraft.slots.size() == 2 && !app.core.gui.ensembleDraft.slots.back().enabled,
            "adding the first role creates an unbound second slot without enabling it");
        app.Click("新增角色"); app.Click("新增角色");
        Check(app.core.gui.ensembleDraft.slots.size() == 4,
            "repeated additions keep creating new logical roles");
        app.Click("启用多人舞台");
        auto* apply = app.ItemWithLabel("应用多人舞台配置");
        Check(app.core.gui.ensembleDraft.enabled && apply && (apply->flags & ImGuiItemFlags_Disabled) &&
            !app.core.EnsembleActive(), "enabling a draft with no bound slots cannot apply an invalid ensemble");
        Check(!app.ItemWithLabel("保存舞台预设") && !app.ItemWithLabel("载入舞台预设"),
            "the layout preset panel is hidden");
        const auto* probe = app.ItemWithLabel("摄影槽位验证探针");
        Check(probe && !(probe->flags & ImGuiItemFlags_Disabled),
            "the observation probe stays available before a scene has passed verification");
        app.Click("摄影槽位验证探针");
        Check(app.core.ensembleSnapshot.probeEnabled && !app.core.ensembleSnapshot.verified,
            "the real probe checkbox queues observation without granting verification");
        app.Click("使用此槽", 0);
        Check(app.core.gui.ensembleDraft.slots[0].enabled && actorSelections == 0,
            "editing a logical slot never calls the legacy single-target selector");
        auto& draft = app.core.gui.ensembleDraft;
        draft.slots[0].motionKey = "A"; draft.slots[0].actorHint = "仅提示A";
        draft.slots[0].offset = {1.25f, -.04f, .3f};
        const auto path = app.fixture / "config/ensembles/offline-draft.json";
        ensemble::Settings loaded; std::string error;
        Check(ensemble::Save(MotionPathToUtf8(path), draft, error) && ensemble::Load(MotionPathToUtf8(path), loaded, error) && loaded.enabled && loaded.slots[0].motionKey == "A" &&
            loaded.slots[0].actorHint == "仅提示A" && loaded.slots[0].actorToken == 0 && loaded.slots[0].offset.x == 1.25f &&
            !app.core.EnsembleActive() && !app.core.ensembleSnapshot.settings.enabled,
            "hidden preset storage still preserves an unbound draft without applying it");
        app.Click("复制主槽动作到全部槽");
        Check(draft.slots[1].motionKey == "A" && draft.slots[2].motionKey == "A" && !draft.slots[1].enabled,
            "copying the master motion fills other slots without silently enabling or binding them");
        app.Click("重置各槽站位");
        Check(draft.slots[0].offset.x == 0 && draft.slots[0].offset.y == 0 && draft.slots[0].offset.z == 0 &&
            draft.slots[0].yawDegrees == 0, "the layout reset clears position and yaw together");
        Check(loaded.slots[0].offset.x == 1.25f && loaded.slots[0].actorToken == 0 &&
            !app.core.EnsembleActive(), "preset storage never activates the performance");

        app.core.ensembleSnapshot.available = app.core.ensembleSnapshot.verified = true;
        app.contextPatch = [&](GuiContext& context) {
            context.sceneActors = {{101, "摄影角色A", true}, {102, "摄影角色B", false}, {103, "摄影角色C", false}};
            context.ensembleSnapshot.actorEpoch = 77;
            context.fnSelectTargetActor = SelectSceneActor;
            context.fnSetEnsemble = RecordEnsembleGuiApply;
        };
        app.Frame();
        draft = loaded; draft.slots[0].actorToken = 101;
        draft.masterSlot = 1; draft.cameraSlot = 0;
        app.Frame(); apply = app.ItemWithLabel("应用多人舞台配置");
        Check(apply && (apply->flags & ImGuiItemFlags_Disabled), "a disabled master slot blocks Apply even when scene verification passes");
        draft.masterSlot = 0; draft.cameraSlot = 2;
        app.Frame(); apply = app.ItemWithLabel("应用多人舞台配置");
        Check(apply && (apply->flags & ImGuiItemFlags_Disabled), "a disabled camera follow slot blocks Apply even when actors are bound");
        draft.cameraSlot = 0;
        ensembleGuiApplyRequests = 0;
        app.Frame(); app.Click("应用多人舞台配置");
        Check(ensembleGuiApplyRequests == 1 && ensembleGuiAppliedDraft == draft && ensembleGuiEpoch == 77 &&
            ensembleGuiRevision == app.core.ensembleSnapshot.revision && actorSelections == 0,
            "a valid actor-bound draft dispatches the ensemble revision and roster epoch without legacy actor selection");
        draft.slots[3].enabled = true;
        draft.slots[3].actorToken = 0;
        ensembleGuiApplyRequests = 0;
        app.Frame(); app.Click("应用多人舞台配置");
        Check(ensembleGuiApplyRequests == 1 && !ensembleGuiAppliedDraft.slots[3].enabled &&
            ensembleGuiAppliedDraft.slots[0].enabled,
            "an unbound extra slot is skipped while bound actors remain enabled");
        app.core.ensembleSnapshot.active = true; app.core.player.state = PlaybackState::Playing;
        app.Frame();
        Check(!app.ItemWithLabel("##drive_actor_combo") && !app.ItemWithLabel("##pos_x") &&
            app.ItemWithLabel("暂停并配置舞台"), "an active ensemble hides single actor placement and offers pause before configuration");
        const auto oldPauses = audioPauses;
        app.Click("暂停并配置舞台");
        Check(app.core.player.state == PlaybackState::Paused && audioPauses == oldPauses + 1 && actorSelections == 0,
            "pause-before-configure reaches the real queued Pause command without selecting a single actor");
        app.core.ensembleSnapshot.active = false;
        app.Frame(); app.Click("多人舞台");
    }
    ImGui::GetIO().DisplaySize = originalDisplay;
    audioPlays = originalAudioPlays; audioResumes = originalAudioResumes; audioPauses = originalAudioPauses;
}
