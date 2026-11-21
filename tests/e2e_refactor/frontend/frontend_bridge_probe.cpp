#include "GkWebPanelBridge.hpp"
#include "e2e_report.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

GK_E2E_DEFINE_LOG_SINK()

namespace
{
	bool SupportsMethod(std::string_view target)
	{
		for (const char* m : GakuMotion::webpanel::kSupportedMethods)
		{
			if (m && std::string_view(m) == target) return true;
		}
		return false;
	}

	template <typename T>
	concept HasEntryIsGroup = requires(const T& e) { e.isGroup; e.positionCount; e.positions; };

	template <typename T>
	concept HasActorMeta = requires(const T& a) { a.character; a.shortName; a.displayName; };

	template <typename T>
	concept HasCamFollowSlot = requires(const T& c) { c.followSlotIndex; };
}

int main(int argc, char** argv)
{
	std::string outPath = "bridge_wire_fixtures.json";
	for (int i = 1; i < argc; ++i)
	{
		if (std::string_view(argv[i]) == "--out" && i + 1 < argc)
		{
			outPath = argv[++i];
		}
	}

	constexpr bool hasCompileTimeM4 =
		HasEntryIsGroup<GakuMotion::MotionEntry> &&
		HasActorMeta<GakuMotion::SceneActorObservation> &&
		HasCamFollowSlot<GakuMotion::CameraSettings>;

	const bool hasMethodsM4 =
		SupportsMethod("formation.selectSlot") &&
		SupportsMethod("formation.swapSlots") &&
		SupportsMethod("formation.setSlotOffset") &&
		SupportsMethod("formation.saveSlotOffset") &&
		SupportsMethod("formation.setSlotSource") &&
		SupportsMethod("actors.getPortrait");

	if (!hasCompileTimeM4 || !hasMethodsM4)
	{
		std::string reason = "C++ GkWebPanelBridge M4 contract not yet merged (";
		if constexpr (!HasEntryIsGroup<GakuMotion::MotionEntry>) reason += "MotionEntry.isGroup/positions ";
		if constexpr (!HasActorMeta<GakuMotion::SceneActorObservation>) reason += "SceneActorObservation.character/shortName ";
		if constexpr (!HasCamFollowSlot<GakuMotion::CameraSettings>) reason += "CameraSettings.followSlotIndex ";
		if (!hasMethodsM4) reason += "formation.*/actors.getPortrait RPCs";
		reason += ")";

		const std::string json =
			"{\n  \"m4Ready\": false,\n  \"reason\": " + gke2e::EscapeJsonString(reason) + "\n}\n";
		std::string err;
		if (!gke2e::WriteUtf8NoBomFile(outPath, json, &err))
		{
			return 2;
		}
		return 0;
	}

	// Post-M4 branch: drive a real headless GakuMotion::Core + webpanel::Bridge
	// and emit authentic C++ wire JSON responses/events for Tier 3/4 Vitest replay.
	std::vector<std::string> events;
	GakuMotion::Core core;
	core.enforceInjectionPolicy = true;
	core.injectionPolicy.SetSceneAllowed(true);
	core.injectionPolicy.SetCaptureHooksReady(true);
	core.motionsRoot = "synthetic/motions";

	// Populate 3 scene actors with idol character metadata
	std::vector<GakuMotion::SceneActorObservation> obs(3);
	obs[0].actor = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1001));
	obs[0].instanceId = 1001;
	obs[0].character = "hski";
	obs[0].displayName = "花海咲季";
	obs[1].actor = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1002));
	obs[1].instanceId = 1002;
	obs[1].character = "ttmr";
	obs[1].displayName = "月村手毬";
	obs[2].actor = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1003));
	obs[2].instanceId = 1003;
	obs[2].character = "fktn";
	obs[2].displayName = "藤田ことね";
	core.sceneActorRegistry.Publish(obs);

	// Populate a 3-slot group song entry in core.motions
	if constexpr (HasEntryIsGroup<GakuMotion::MotionEntry>)
	{
		GakuMotion::MotionEntry entry;
		entry.name = "group-live-3p";
		entry.title = "Campus Mode!!";
		entry.subtitle = "初星学園, 花海咲季";
		entry.dir = "synthetic/motions/group-live-3p";
		entry.durationSeconds = 180.0f;
		entry.totalFrames = 10800;
		entry.hasFacial = true;
		entry.hasCamera = true;
		entry.hasAudio = true;
		entry.isGroup = true;
		entry.positionCount = 3;
		core.motions.push_back(entry);
		core.libraryRevision = 2;
	}

	GakuMotion::webpanel::HostHooks hooks;
	hooks.releaseFileDialog = [] {};
	hooks.pluginVersion = "0.2.0-e2e";

	auto bridge = std::make_unique<GakuMotion::webpanel::Bridge>(core, hooks);
	bridge->SetEventSink(
		[](void* userData, const char* msg) {
			if (userData && msg) static_cast<std::vector<std::string>*>(userData)->emplace_back(msg);
		},
		&events);
	bridge->OnShow();
	bridge->OnUiTick();

	auto req = [&](const std::string& method, const std::string& params) -> std::string {
		return bridge->HandleMessageForTest(
			"{\"v\":1,\"id\":1,\"type\":\"request\",\"method\":\"" + method + "\",\"params\":" + params + "}");
	};

	const std::string helloResp = req("motion.hello", "{}");
	const std::string stateResp = req("state.get", "{}");
	const std::string portraitResp = req("actors.getPortrait", "{\"character\":\"hski\"}");
	const std::string selectSlotResp = req("formation.selectSlot", "{\"slotIndex\":1}");
	core.ProcessPendingGuiCommands();
	bridge->OnUiTick();
	const std::string camAllResp = req("camera.set", "{\"settings\":{\"mode\":2,\"followSlotIndex\":-1}}");
	core.ProcessPendingGuiCommands();
	bridge->OnUiTick();
	const std::string camSlot2Resp = req("camera.set", "{\"settings\":{\"mode\":2,\"followSlotIndex\":1}}");
	core.ProcessPendingGuiCommands();
	bridge->OnUiTick();
	const std::string startEditResp = req("library.startEdit", "{\"key\":\"group-live-3p\",\"revision\":2}");
	bridge->OnUiTick();

	std::string out = "{\n  \"m4Ready\": true,\n  \"responses\": {\n";
	out += "    \"motion.hello\": " + helloResp + ",\n";
	out += "    \"state.get\": " + stateResp + ",\n";
	out += "    \"actors.getPortrait\": " + portraitResp + ",\n";
	out += "    \"formation.selectSlot\": " + selectSlotResp + ",\n";
	out += "    \"camera.set.all\": " + camAllResp + ",\n";
	out += "    \"camera.set.slot2\": " + camSlot2Resp + ",\n";
	out += "    \"library.startEdit\": " + startEditResp + "\n";
	out += "  },\n  \"events\": [\n";
	for (size_t i = 0; i < events.size(); ++i)
	{
		out += "    " + events[i];
		if (i + 1 < events.size()) out.push_back(',');
		out.push_back('\n');
	}
	out += "  ]\n}\n";

	std::string err;
	if (!gke2e::WriteUtf8NoBomFile(outPath, out, &err))
	{
		return 2;
	}
	return 0;
}
