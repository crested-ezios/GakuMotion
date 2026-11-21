#include "GakuMotion.hpp"
#include <chrono>
#include <iostream>

using namespace GakuMotion;
namespace fs = std::filesystem;
static int passed = 0, failed = 0;
static void Check(bool ok, const char* label) {
    if (ok) ++passed;
    else { ++failed; std::cerr << "FAIL: " << label << '\n'; }
}
static void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
static std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
static void Clip(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    auto put = [&](auto value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
    out.write("GKCLIPCL", 8); put(uint32_t{4}); put(30.f); put(uint32_t{61}); put(2.f);
    put(uint32_t{1}); put(.07795f); put(uint16_t{0}); put(uint8_t{4}); put(uint8_t{0}); out.write("Hips", 4);
    for (int frame = 0; frame < 61; ++frame) { put(Vector3{}); put(Quaternion::Identity()); }
    put(uint8_t{0}); put(uint32_t{0});
}
static std::string Receipt(const DataLayout& layout, const std::string& state = "active") {
    auto quote = [](const std::string& text) { return "\"" + jsonlite::EscapeString(text) + "\""; };
    return "{\"schemaVersion\":1,\"pluginId\":\"gakumotion\",\"layout\":\"gakuloader-v1\",\"state\":" + quote(state) +
        ",\"sourceRoot\":" + quote(layout.localRoot) + ",\"paths\":{\"configs\":" + quote(layout.configs) +
        ",\"userData\":" + quote(layout.userData) + ",\"pluginData\":" + quote(layout.pluginData) +
        ",\"logs\":" + quote(layout.logs) + "}}";
}
static MotionImportRequest capturedImport;
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--validate-track") {
        MotionClip clip;
        std::string error;
        const auto path = fs::absolute(argv[2]) / "motion.gkclip";
        if (!clip.LoadFromFile(MotionPathToUtf8(path), &error)) { std::cerr << error; return 1; }
        std::cout << "synthetic track loaded: frames=" << clip.total_frames << " duration=" << clip.duration_seconds << '\n';
        return 0;
    }
    const auto base = argc > 1 ? fs::absolute(argv[1]) : fs::temp_directory_path();
    const auto root = base / (L"layout-中文-日本語-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto utf8 = MotionPathToUtf8(root);
    DataLayout offered{utf8, utf8 + "/Configs/gakumotion", utf8 + "/UserData/gakumotion",
        utf8 + "/PluginData/gakumotion", utf8 + "/Logs/gakumotion", utf8 + "/Temp/gakumotion", true};
    DataLayout selected;
    std::string error;
    Check(SelectDataLayout(utf8, nullptr, selected, error) && !selected.managed, "old host uses legacy without receipt");
    Check(SelectDataLayout(utf8, &offered, selected, error) && !selected.managed, "new host does not migrate before explicit activation");
    Check(!fs::exists(MotionPathFromUtf8(offered.configs)), "layout lookup creates no category");
    Write(root / "GakuMotion/control.json", "{\"selected_motion\":\"OLD\"}");
    Clip(root / "motions/OLD/motion.gkclip");
    Write(MotionPathFromUtf8(offered.configs + "/control.json"),
        "{\"selected_motion\":\"NEW\",\"overrides\":{\"global_file\":\"GakuMotion/bone_overrides.json\"}}");
    Clip(MotionPathFromUtf8(offered.Motions() + "/NEW/motion.gkclip"));
    Clip(MotionPathFromUtf8(offered.Motions() + "/NEW/positions/slot_2/motion.gkclip"));
    Write(MotionPathFromUtf8(offered.Motions() + "/NEW/custom/private-note.bin"), "synthetic custom bytes");
    const auto receipt = root / "PluginData/gakumotion/layout-v1.json";
    Write(receipt, Receipt(offered));
    Check(SelectDataLayout(utf8, &offered, selected, error) && selected.managed, "matching receipt selects managed layout");
    Core core; core.Initialize(utf8, &selected);
    Check(core.config.selectedMotion == "NEW" && core.motions.size() == 1 && core.motions[0].name == "NEW" &&
        core.motions[0].isGroup && core.motions[0].positionCount == 2,
        "managed root never mixes old config/library and scans group positions under Unicode paths");
    Check(core.globalOverridePath == offered.configs + "/bone_overrides.json", "known default override follows new configuration");
    Check(DataLayout::SamePath(selected.ResolveOverride(utf8 + "/GakuMotion/bone_overrides.json", ""), core.globalOverridePath), "absolute known old default follows new configuration");
    Check(DataLayout::SamePath(selected.ResolveOverride("motions/NEW/bone_overrides.json", ""), offered.Motions() + "/NEW/bone_overrides.json"), "old relative motion reference follows full library");
    Check(DataLayout::SamePath(selected.ResolveOverride("custom/outside.json", ""), utf8 + "/custom/outside.json"), "custom relative override keeps old localRoot anchor");
    Check(DataLayout::SamePath(selected.ResolveOverride(utf8 + "/external/user.json", ""), utf8 + "/external/user.json"), "explicit external override stays explicit");
    Check(core.SavePlaylist() && fs::exists(MotionPathFromUtf8(offered.Playlist())), "playlist saved only in PluginData");
    Check(core.SaveInitialPosition() && fs::exists(MotionPathFromUtf8(offered.configs + "/initial_position.json")), "Unicode config write succeeds");
    Check(core.SaveCameraSettings() && fs::exists(MotionPathFromUtf8(offered.configs + "/camera.json")), "Unicode camera save succeeds");
    core.fnStartImport = [](const MotionImportRequest& request, std::string&) { capturedImport = request; return false; };
    core.fnPollImport = [](std::string&) { return 0; };
    MotionImportRequest request;
    request.vmd = "synthetic.vmd";
    request.positions = {
        ImportPositionSlot{1, 0, "slot1.vmd"},
        ImportPositionSlot{2, 0, "slot2.vmd", "", false, false, 0.0f, false, false, -1.5f, 0.0f, 0.5f, 15.0f}
    };
    request.positionsExplicit = true;
    core.StartImport(request);
    Check(capturedImport.log == offered.logs + "/import.log" &&
        capturedImport.transactionRoot == offered.temp + "/transactions" &&
        capturedImport.positions.size() == 2 && capturedImport.positionsExplicit,
        "import uses Logs, managed transaction scratch, and forwards group positions");
	MotionClip loaded;
	Check(loaded.LoadFromFile(offered.Motions() + "/NEW/motion.gkclip") && loaded.total_frames == 61, "synthetic smoke track loads through production loader");
	MotionClip slot2Loaded;
	Check(slot2Loaded.LoadFromFile(offered.Motions() + "/NEW/positions/slot_2/motion.gkclip") && slot2Loaded.total_frames == 61,
		"group slot_2 clip loads through production loader");
	Check(core.DeleteMotionFiles("NEW", true), "managed track can be permanently deleted");
	Check(!fs::exists(MotionPathFromUtf8(offered.Motions() + "/NEW")) &&
		!fs::exists(MotionPathFromUtf8(offered.Motions() + "/.gakumotion-trash")),
		"managed delete removes full track tree without creating recovery storage");
    Check(Read(root / "GakuMotion/control.json") == "{\"selected_motion\":\"OLD\"}" && fs::exists(root / "motions/OLD/motion.gkclip"), "old originals remain unchanged");
    Check(!SelectDataLayout(utf8, nullptr, selected, error), "old host cannot fall back once managed receipt exists");
    Write(receipt, Receipt(offered, "pending"));
    Check(!SelectDataLayout(utf8, &offered, selected, error), "incomplete receipt refuses activation");
    auto wrong = offered; wrong.configs += "-other";
    Write(receipt, Receipt(wrong));
    Check(!SelectDataLayout(utf8, &offered, selected, error), "wrong category root refuses activation");
    wrong = offered; wrong.localRoot += "-other"; Write(receipt, Receipt(wrong));
    Check(!SelectDataLayout(utf8, &offered, selected, error), "cross-installation receipt refuses activation");
    Write(receipt, "{broken");
    Check(!SelectDataLayout(utf8, &offered, selected, error), "corrupt receipt refuses activation without fallback");
    Write(receipt, Receipt(offered));
    const auto freshRoot = root / "GakuLoader";
    const auto fresh = MotionPathToUtf8(freshRoot);
    DataLayout freshOffered{fresh, fresh + "/Configs/gakumotion", fresh + "/UserData/gakumotion",
        fresh + "/PluginData/gakumotion", fresh + "/Logs/gakumotion", fresh + "/Temp/gakumotion", true};
    Check(SelectDataLayout(fresh, &freshOffered, selected, error) && selected.managed,
        "independent host defaults to categorized paths without migration receipt");
    Check(selected.Motions() == fresh + "/PluginData/gakumotion/motions",
        "independent library belongs to PluginData");
    Core freshCore; freshCore.Initialize(fresh, &selected);
    Check(fs::is_directory(MotionPathFromUtf8(selected.Motions())) && freshCore.motions.empty() &&
        freshCore.RefreshMotionLibrary(), "first-run empty library is created and refresh succeeds");
    Check(fs::exists(MotionPathFromUtf8(selected.configs + "/control.json")),
        "first-run configuration uses Configs");
    Check(!fs::exists(freshRoot / "motions") && !fs::exists(freshRoot / "GakuMotion"),
        "fresh host does not create Localify-era Motion paths");
    Check(!SelectDataLayout(fresh, nullptr, selected, error),
        "independent host refuses missing categorized path service");
    std::cout << "layout tests: " << passed << " passed, " << failed << " failed\n";
    std::cout << "synthetic evidence: " << MotionPathToUtf8(root) << '\n';
    return failed ? 1 : 0;
}
