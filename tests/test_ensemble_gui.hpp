#pragma once

// Included after the real ImGui Harness in test_gui_ux.cpp.
void TestEnsembleGui(const std::filesystem::path& root)
{
    const ImVec2 originalDisplay = ImGui::GetIO().DisplaySize;
    ImGui::GetIO().DisplaySize = ImVec2(1120, 1200);
    {
        Harness app(root);
        app.windowHeight = 1200;
        app.core.configRoot = MotionPathToUtf8(app.fixture / "config");
        app.core.gui.activePage = GuiPage::Motion;
        app.contextPatch = [&](GuiContext& context) {
            context.sceneActors = {{101, "摄影角色A", true}, {102, "摄影角色B", false}, {103, "摄影角色C", false}};
            context.fnSelectTargetActor = SelectSceneActor;
            context.formation.active = true;
            context.formation.slotCount = 3;
        };
        app.Settle();
        Check(!app.ItemWithLabel("多人舞台") && !app.ItemWithLabel("摄影槽位验证探针") &&
            !app.ItemWithLabel("启用多人舞台") && !app.ItemWithLabel("保存舞台预设") &&
            !app.ItemWithLabel("载入舞台预设"),
            "legacy ensemble panel and probe controls are removed from GuiMotionPage");
        Check(app.ItemWithLabel("##drive_actor_combo") && app.ItemWithLabel("##pos_x") &&
            app.ItemWithLabel("##prev") && app.ItemWithLabel("##next"),
            "unified minimal ImGui motion and transport controls render with multi-actor roster");
        auto* prevBtn = app.ItemWithLabel("##prev");
        auto* nextBtn = app.ItemWithLabel("##next");
        Check(prevBtn && !(prevBtn->flags & ImGuiItemFlags_Disabled) &&
            nextBtn && !(nextBtn->flags & ImGuiItemFlags_Disabled),
            "transport prev/next remain enabled in multi-actor mode");
    }
    ImGui::GetIO().DisplaySize = originalDisplay;
}
