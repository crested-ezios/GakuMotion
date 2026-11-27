#pragma once

#include "plugin/GkAutoToolsAdapter.hpp"
#include "test_actorless_library.hpp"
#include "test_scene_nodes.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace AutoToolsAdapterTest
{
	struct MockRegistry
	{
		bool available{false};
		std::vector<GakuAutoToolsToolDescV1> tools;
		int unregisterOwnerCalls{0};
		std::string lastUnregisteredOwner;

		GakuAutoToolsRegistryV1 Table()
		{
			GakuAutoToolsRegistryV1 reg{};
			reg.abiVersion = GAKU_AUTOTOOLS_REGISTRY_VERSION;
			reg.structSize = sizeof(GakuAutoToolsRegistryV1);
			reg.context = this;
			reg.registerTool = [](void* ctx, const GakuAutoToolsToolDescV1* desc) -> GakuStatus
			{
				auto* self = static_cast<MockRegistry*>(ctx);
				if (!self || !desc || !desc->name || !desc->invoke) return GAKU_E_INVALID_ARGUMENT;
				for (auto& existing : self->tools)
				{
					if (std::strcmp(existing.name, desc->name) == 0)
					{
						existing = *desc;
						return GAKU_OK;
					}
				}
				self->tools.push_back(*desc);
				return GAKU_OK;
			};
			reg.unregisterTool = [](void* ctx, const char* name) -> GakuStatus
			{
				auto* self = static_cast<MockRegistry*>(ctx);
				if (!self || !name) return GAKU_E_INVALID_ARGUMENT;
				std::erase_if(self->tools, [&](const GakuAutoToolsToolDescV1& d) {
					return std::strcmp(d.name, name) == 0;
				});
				return GAKU_OK;
			};
			reg.unregisterOwner = [](void* ctx, const char* owner) -> GakuStatus
			{
				auto* self = static_cast<MockRegistry*>(ctx);
				if (!self || !owner) return GAKU_E_INVALID_ARGUMENT;
				++self->unregisterOwnerCalls;
				self->lastUnregisteredOwner = owner;
				std::erase_if(self->tools, [&](const GakuAutoToolsToolDescV1& d) {
					return d.ownerPluginId && std::strcmp(d.ownerPluginId, owner) == 0;
				});
				return GAKU_OK;
			};
			return reg;
		}

		const GakuAutoToolsToolDescV1* Find(const char* name) const
		{
			for (const auto& t : tools)
			{
				if (t.name && std::strcmp(t.name, name) == 0) return &t;
			}
			return nullptr;
		}

		jsonlite::Value Call(const char* name, const std::string& args = "{}") const
		{
			const auto* tool = Find(name);
			Check(tool != nullptr, std::string("tool registered: ") + name);
			if (!tool) return {};
			std::string buf(65536, '\0');
			uint32_t req = 0;
			GakuStatus st = tool->invoke(tool->userData, args.c_str(), static_cast<uint32_t>(buf.size()), buf.data(), &req);
			if (st == GAKU_E_SIZE && req > 0)
			{
				buf.assign(req, '\0');
				st = tool->invoke(tool->userData, args.c_str(), req, buf.data(), &req);
			}
			Check(st == GAKU_OK && req > 0, std::string(name) + " succeeds");
			buf.resize(req > 0 ? req - 1 : 0);
			jsonlite::Value out;
			std::string err;
			Check(jsonlite::ParseText(buf, out, err), std::string(name) + " returned valid JSON: " + err);
			return out;
		}

		jsonlite::Value CallWithSizeRetry(const char* name, const std::string& args = "{}") const
		{
			const auto* tool = Find(name);
			Check(tool != nullptr, std::string("tool registered: ") + name);
			if (!tool) return {};
			char smallBuf[4]{};
			uint32_t req = 0;
			const GakuStatus st1 = tool->invoke(tool->userData, args.c_str(), sizeof(smallBuf), smallBuf, &req);
			Check(st1 == GAKU_E_SIZE && req > sizeof(smallBuf),
			      std::string(name) + " returns GAKU_E_SIZE and requiredBytes for small buffer");
			std::string buf(req, '\0');
			uint32_t req2 = 0;
			const GakuStatus st2 = tool->invoke(tool->userData, args.c_str(), req, buf.data(), &req2);
			Check(st2 == GAKU_OK && req2 == req, std::string(name) + " succeeds on exact retry buffer");
			if (!buf.empty() && buf.back() == '\0') buf.pop_back();
			jsonlite::Value out;
			std::string err;
			Check(jsonlite::ParseText(buf, out, err), std::string(name) + " returned valid JSON: " + err);
			return out;
		}
	};

	inline void WriteGroupTrack(const std::filesystem::path& dir)
	{
		ActorlessLibraryFixture::WriteClip(dir / "motion.gkclip");
		ActorlessLibraryFixture::WriteClip(dir / "positions" / "slot_2" / "motion.gkclip");
		ActorlessLibraryFixture::WriteClip(dir / "positions" / "slot_3" / "motion.gkclip");
		WriteTextFile(dir / "song.wav", "RIFF");
		WriteTextFile(dir / "info.json",
			"{\"title\":\"Beyond the way\",\"subtitle\":\"Test Trio\",\"duration_seconds\":123.5,\"total_frames\":3705,\"fps\":30.0,\"has_audio\":true,\"positions\":[{\"slot\":1,\"x\":0,\"y\":0,\"z\":0,\"yaw_degrees\":0},{\"slot\":2,\"x\":-0.8,\"y\":0,\"z\":0.2,\"yaw_degrees\":10},{\"slot\":3,\"x\":0.8,\"y\":0,\"z\":0.2,\"yaw_degrees\":-10}]}");
	}
}

static void TestAutoToolsAdapter()
{
	using namespace AutoToolsAdapterTest;

	BeginTest("autotools_adapter_lazy_registration_and_buffer_retry");
	{
		GakuMotion::Core core;
		core.initialized = true;
		core.SetControlActive(true);
		MockRegistry reg;
		GakuMotion::autotools::PhysicsSlotView singlePhys;
		std::vector<GakuMotion::autotools::PhysicsSlotView> ensemblePhys;
		GakuMotion::autotools::BurstSummaryView burstSummary;

		GakuMotion::autotools::AdapterHooks hooks;
		hooks.queryService = [&](const char* name, uint32_t version, uint32_t size, void* out) -> GakuStatus
		{
			if (!reg.available) return GAKU_E_UNAVAILABLE;
			if (std::strcmp(name, GAKU_SERVICE_AUTOTOOLS_REGISTRY) != 0) return GAKU_E_UNAVAILABLE;
			if (version != GAKU_AUTOTOOLS_REGISTRY_VERSION) return GAKU_E_VERSION;
			if (size < sizeof(GakuAutoToolsRegistryV1)) return GAKU_E_SIZE;
			*static_cast<GakuAutoToolsRegistryV1*>(out) = reg.Table();
			return GAKU_OK;
		};
		hooks.readSinglePhysics = [&]() { return singlePhys; };
		hooks.readEnsemblePhysics = [&]() { return ensemblePhys; };
		hooks.readBurstSummary = [&]() { return burstSummary; };

		GakuMotion::autotools::Adapter adapter(core, hooks);
		adapter.OnRuntimeReady();
		Check(!adapter.IsConnected() && reg.tools.empty(),
		      "unavailable registry during onRuntimeReady leaves adapter cleanly disconnected");

		reg.available = true;
		adapter.OnTick();
		Check(adapter.IsConnected() && reg.tools.size() == 10,
		      "onTick lazily connects and registers all 10 tools when GakuAutoTools loads later");

		const char* expectedTools[] = {
			"motion.status", "motion.list", "motion.load", "motion.transport", "motion.scene_nodes",
			"motion.formation", "motion.experiment", "diag.physics", "diag.face", "diag.burst"
		};
		for (const char* name : expectedTools)
		{
			const auto* desc = reg.Find(name);
			Check(desc != nullptr && desc->ownerPluginId && std::strcmp(desc->ownerPluginId, "gakumotion") == 0,
			      std::string("registered under owner gakumotion: ") + name);
		}

		// Buffer retry check via motion.status & motion.list
		const auto statusRes = reg.CallWithSizeRetry("motion.status");
		Check(statusRes.BoolPath("ok", false) && statusRes.Find("actor") && statusRes.Find("clock"),
		      "motion.status returns actor and clock fields");
		const auto listRes = reg.CallWithSizeRetry("motion.list");
		Check(listRes.BoolPath("ok", false) && listRes.IntPath("motionCount", -1) == 0,
		      "motion.list succeeds with empty library");

		adapter.Shutdown();
		Check(!adapter.IsConnected() && reg.unregisterOwnerCalls == 1 &&
		      reg.lastUnregisteredOwner == "gakumotion" && reg.tools.empty(),
		      "shutdown unregisters owner gakumotion cleanly");
	}

	BeginTest("autotools_adapter_library_formation_load_transport_and_experiment");
	{
		namespace fs = std::filesystem;
		const fs::path fixture = fs::temp_directory_path() /
			("gkmotion_autotools_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		fs::remove_all(fixture);
		ActorlessLibraryFixture::WriteClip(fixture / "motions" / "solo_song" / "motion.gkclip");
		WriteGroupTrack(fixture / "motions" / "Beyond the way");

		GakuMotion::Core core;
		core.Initialize(fixture.string());
		core.SetControlActive(true);
		core.currentActor = reinterpret_cast<void*>(0x1001);

		const fs::path controlPath = fixture / "control.json";
		std::error_code ec;
		const auto controlBefore = fs::exists(controlPath, ec) ? StatFile(MotionPathToUtf8(controlPath)) : FileSignature{};

		MockRegistry reg;
		reg.available = true;
		GakuMotion::autotools::PhysicsSlotView singlePhys;
		std::vector<GakuMotion::autotools::PhysicsSlotView> ensemblePhys(3);
		GakuMotion::autotools::BurstSummaryView burstSummary;
		int dumpCalls = 0;

		GakuMotion::autotools::AdapterHooks hooks;
		hooks.queryService = [&](const char* name, uint32_t version, uint32_t size, void* out) -> GakuStatus
		{
			if (std::strcmp(name, GAKU_SERVICE_AUTOTOOLS_REGISTRY) != 0) return GAKU_E_UNAVAILABLE;
			if (version != GAKU_AUTOTOOLS_REGISTRY_VERSION || size < sizeof(GakuAutoToolsRegistryV1)) return GAKU_E_SIZE;
			*static_cast<GakuAutoToolsRegistryV1*>(out) = reg.Table();
			return GAKU_OK;
		};
		hooks.readSinglePhysics = [&]() { return singlePhys; };
		hooks.readEnsemblePhysics = [&]() { return ensemblePhys; };
		hooks.readBurstSummary = [&]() {
			auto copy = burstSummary;
			copy.dumpTraces = [&](const wchar_t*) -> int { ++dumpCalls; return GBB_OK; };
			return copy;
		};

		GakuMotion::autotools::Adapter adapter(core, hooks);
		adapter.OnRuntimeReady();
		Check(adapter.IsConnected(), "connected in functional test");

		// 1. motion.list includes details (duration/slotCount/hasAudio)
		const auto listRes = reg.CallWithSizeRetry("motion.list");
		Check(listRes.BoolPath("ok", false) && listRes.IntPath("motionCount", 0) == 2,
		      "motion.list sees 2 scanned tracks");
		const auto* details = listRes.Find("details");
		Check(details && details->IsArray() && details->Size() == 2, "motion.list returns details array");
		bool sawGroupDetails = false;
		if (details && details->IsArray())
		{
			for (const auto& d : details->arrayItems)
			{
				if (d.StringPath("key", "") == "Beyond the way")
				{
					sawGroupDetails = d.IntPath("slotCount", 0) == 3 && d.BoolPath("hasAudio", false) &&
						d.FloatPath("duration", 0.0f) > 0.0f;
				}
			}
		}
		Check(sawGroupDetails, "motion.list details include duration, slotCount, and hasAudio");

		// 2. Publish 3 actors -> formation.active == true
		void* actor1 = reinterpret_cast<void*>(0x2001);
		void* actor2 = reinterpret_cast<void*>(0x2002);
		void* actor3 = reinterpret_cast<void*>(0x2003);
		core.fnEnsembleActorEligible = [](void*) { return true; };
		core.PublishSceneActors({
			{actor1, 101, "hski", "Saki", "Saki", "face1"},
			{actor2, 102, "ttmr", "Temari", "Temari", "face2"},
			{actor3, 103, "fktn", "Kotone", "Kotone", "face3"}
		});
		Check(core.formation.active && core.formation.slotCount == 3,
		      "3 actors activate StageFormation with 3 slots");

		// 3. motion.load works even when formation.active == true!
		const auto loadRes = reg.Call("motion.load", R"({"motion_key":"Beyond the way","autoplay":false})");
		Check(loadRes.BoolPath("ok", false) &&
		      loadRes.StringPath("loadedMotionKey", "") == "Beyond the way" &&
		      loadRes.BoolPath("formationActive", false) &&
		      core.player.state == PlaybackState::Preview,
		      "motion.load switches ensemble track under formation.active=true in Preview mode");
		Check(core.formationTracks.size() == 3, "all 3 formation tracks created after motion.load");

		// 4. motion.formation status / select_slot / swap_slots
		const auto fmStatus = reg.CallWithSizeRetry("motion.formation");
		Check(fmStatus.BoolPath("ok", false) && fmStatus.BoolPath("active", false) &&
		      fmStatus.IntPath("slotCount", 0) == 3 && fmStatus.IntPath("selectedSlotIndex", -1) == 0,
		      "motion.formation returns active=true, slotCount=3, selectedSlotIndex=0");
		const auto* fmSlots = fmStatus.Find("slots");
		Check(fmSlots && fmSlots->IsArray() && fmSlots->Size() == 3 &&
		      fmSlots->At(0)->BoolPath("built", false) &&
		      fmSlots->At(1)->StringPath("character", "") == "ttmr" &&
		      fmSlots->At(1)->StringPath("sourceMode", "") == "slot_2",
		      "motion.formation slots report character, sourceMode, and built");

		const auto selSlot = reg.Call("motion.formation", R"({"action":"select_slot","slot_index":1})");
		Check(selSlot.BoolPath("ok", false) && selSlot.IntPath("selectedSlotIndex", -1) == 1,
		      "motion.formation select_slot selects slot 1");

		const auto swapSlot = reg.Call("motion.formation", R"({"action":"swap_slots","from_slot":0,"to_slot":2})");
		Check(swapSlot.BoolPath("ok", false) &&
		      swapSlot.Find("slots") && swapSlot.Find("slots")->Size() == 3 &&
		      swapSlot.Find("slots")->At(0)->StringPath("character", "") == "fktn" &&
		      swapSlot.Find("slots")->At(2)->StringPath("character", "") == "hski",
		      "motion.formation swap_slots swaps slot 0 and slot 2 actors");

		// 5. motion.transport under formation.active
		const auto playRes = reg.Call("motion.transport", R"({"action":"play"})");
		Check(playRes.BoolPath("ok", false) && core.player.state == PlaybackState::Playing,
		      "motion.transport play works under formation.active");
		const auto seekRes = reg.Call("motion.transport", R"({"action":"seek","value":0.015})");
		Check(seekRes.BoolPath("ok", false) && std::fabs(core.player.currentTime - 0.015f) < 0.005f,
		      "motion.transport seek updates timeline");
		const auto speedRes = reg.Call("motion.transport", R"({"action":"speed","value":1.5})");
		Check(speedRes.BoolPath("ok", false) && std::fabs(core.player.playbackSpeedMultiplier - 1.5f) < 0.01f,
		      "motion.transport speed updates playbackSpeedMultiplier");
		const auto offsetBlocked = reg.Call("motion.transport", R"({"action":"offset","offset":[0,0,0]})");
		Check(!offsetBlocked.BoolPath("ok", true) && offsetBlocked.StringPath("status", "") == "Unsupported",
		      "motion.transport offset retains GkControlCore Unsupported under formation.active");

		// 6. diag.physics across all 3 slots + top-level selected slot fields
		for (int i = 0; i < 3; ++i)
		{
			auto& snap = ensemblePhys[static_cast<size_t>(i)].snapshot;
			snap.version = GAKU_PHYSICS_DIAGNOSTICS_VERSION;
			snap.size = sizeof(snap);
			snap.requested_backend = 2;
			snap.actual_backend = 2;
			snap.bridge_state = GBB_STATE_INJECTING;
			snap.solved_bones = 12 + static_cast<uint32_t>(i);
			snap.bound_bones = 20;
			snap.actor = reinterpret_cast<uintptr_t>(
				core.sceneActorRegistry.Resolve(core.formation.slots[static_cast<size_t>(i)].actorToken,
				                                core.formation.actorEpoch));
			snap.sequence = 100 + static_cast<uint64_t>(i);
			snap.unity_frame = 500;
			ensemblePhys[static_cast<size_t>(i)].reason = "native";
		}
		const auto physRes = reg.CallWithSizeRetry("diag.physics");
		Check(physRes.BoolPath("ok", false) && physRes.BoolPath("available", false) &&
		      physRes.BoolPath("formationActive", false) &&
		      physRes.IntPath("primarySlotIndex", -1) == 1 &&
		      physRes.IntPath("solvedBones", 0) == 13 &&
		      physRes.Find("slots") && physRes.Find("slots")->Size() == 3,
		      "diag.physics returns all 3 ensemble slots and top-level selected slot fields");

		// 6b. If selected slot (1) has no snapshot yet, diag.physics falls back to first active slot (0)
		ensemblePhys[1].snapshot.actor = 0;
		const auto physFallback = reg.Call("diag.physics");
		Check(physFallback.BoolPath("ok", false) && physFallback.IntPath("primarySlotIndex", -1) == 0 &&
		      physFallback.IntPath("solvedBones", 0) == 12,
		      "diag.physics falls back to first populated slot when selected slot snapshot is missing");

		// 7. diag.face filtered diagnostics on selected actor
		auto* selTrack = core.TrackForSlot(1);
		Check(selTrack != nullptr, "selected slot has track");
		if (selTrack)
		{
			auto& b = selTrack->controller.bindings[actor2];
			b.actor = actor2;
			b.face.diagnosticStatus = GAKU_FACE_BOUND;
			b.face.diagnosticSequence = 42;
			b.face.diagnosticUnityFrame = 501;
			b.face.diagnosticTime = 0.5f;
			GakuFaceChannelDiagnostic ch0{}, ch1{}, ch2{};
			std::snprintf(ch0.name, sizeof(ch0.name), "mouth_a"); ch0.index = 1; ch0.target = 50.f; ch0.before = 0.f; ch0.after = 50.f;
			std::snprintf(ch1.name, sizeof(ch1.name), "eye_blink"); ch1.index = 2; ch1.target = 0.f; ch1.before = 0.f; ch1.after = 0.f;
			std::snprintf(ch2.name, sizeof(ch2.name), "mouth_i"); ch2.index = 3; ch2.target = 30.f; ch2.before = 0.f; ch2.after = 10.f;
			b.face.diagnosticChannels = {ch0, ch1, ch2};
		}
		const auto faceActive = reg.CallWithSizeRetry("diag.face", R"({"only_active":true,"only_diff":false})");
		Check(faceActive.BoolPath("ok", false) && faceActive.IntPath("matchedCount", 0) == 2 &&
		      faceActive.IntPath("slotIndex", -1) == 1,
		      "diag.face filters active channels on selected formation actor");
		const auto faceDiff = reg.Call("diag.face", R"({"only_active":true,"only_diff":true,"filter":"mouth"})");
		Check(faceDiff.BoolPath("ok", false) && faceDiff.IntPath("matchedCount", 0) == 1 &&
		      faceDiff.Find("channels")->At(0)->StringPath("name", "") == "mouth_i",
		      "diag.face only_diff + filter returns mismatched channel");

		// 8. motion.experiment updates memory without touching control.json
		const auto expRead = reg.Call("motion.experiment");
		Check(expRead.BoolPath("ok", false) && !expRead.BoolPath("updated", true) &&
		      expRead.IntPath("secondary_physics_backend", -1) == 2,
		      "motion.experiment read-only returns current settings");
		const auto expSet = reg.Call("motion.experiment",
			R"({"secondary_physics_backend":1,"before_quartz_experiment":false,"preserve_animator":true,"miku_mouth_fit":false,"waist_support":false})");
		Check(expSet.BoolPath("ok", false) && expSet.BoolPath("updated", false) &&
		      expSet.IntPath("secondary_physics_backend", -1) == 1 &&
		      !expSet.BoolPath("before_quartz_experiment", true) &&
		      expSet.BoolPath("preserve_animator", false) &&
		      !expSet.BoolPath("miku_mouth_fit", true) &&
		      !expSet.BoolPath("waist_support", true) &&
		      core.config.secondaryPhysicsBackend == 1 &&
		      !core.player.controller.IsExperimentalWaistSupportEnabled(),
		      "motion.experiment updates live settings in memory");
		core.SetSlotFacialWeights(1, {1.25f, 0.5f, 1.5f, 1.0f});
		const auto mouthSet = reg.Call("motion.experiment", R"({"mouth_weight":0.75})");
		const auto mouthRead = reg.Call("motion.experiment");
		const auto& selectedWeights = selTrack->controller.facialWeights;
		Check(mouthSet.BoolPath("ok", false) && mouthSet.BoolPath("updated", false) &&
		      mouthRead.FloatPath("mouth_weight", -1.0f) == 0.75f &&
		      selectedWeights.overall == 1.25f && selectedWeights.brow == 0.5f &&
		      selectedWeights.eye == 1.5f && selectedWeights.mouth == 0.75f &&
		      core.formation.slots[0].facialWeights.mouth == 1.0f,
		      "live mouth adjustment is local to the selected actor and preserves the other weights");
		core.captureSuspended = true;
		const auto mouthBlocked = reg.Call("motion.experiment", R"({"mouth_weight":1})");
		core.captureSuspended = false;
		const auto mouthOutOfRange = reg.Call("motion.experiment", R"({"mouth_weight":2.01})");
		Check(mouthBlocked.StringPath("status", "") == "Protected" &&
		      mouthOutOfRange.StringPath("status", "") == "InvalidArgument" && selectedWeights.mouth == 0.75f,
		      "live mouth adjustment rejects reserved contexts and invalid values");
		const FileSignature controlAfter = fs::exists(controlPath, ec) ? StatFile(MotionPathToUtf8(controlPath)) : FileSignature{};
		Check(controlBefore == controlAfter, "motion.experiment does not touch control.json on disk");

		// 9. diag.burst summary & optional dump_trace
		burstSummary.beforeQuartz = true;
		burstSummary.requestedBackend = 2;
		burstSummary.actualBackend = 2;
		burstSummary.multiReason = "native";
		burstSummary.multiSlots.resize(3);
		burstSummary.multiSlots[1].token = 77;
		burstSummary.multiSlots[1].actor = actor2;
		burstSummary.multiSlots[1].reason = "native";
		burstSummary.multiSlots[1].status.state = GBB_STATE_INJECTING;
		burstSummary.multiSlots[1].status.writes = 24;
		burstSummary.multiSlots[1].status.injected = 1;
		const auto burstRes = reg.Call("diag.burst", R"({"dump_trace":true})");
		Check(burstRes.BoolPath("ok", false) && burstRes.StringPath("mode", "") == "multi" &&
		      burstRes.IntPath("state", 0) == GBB_STATE_INJECTING &&
		      burstRes.StringPath("reason", "") == "native" &&
		      !burstRes.BoolPath("fault", true) &&
		      burstRes.IntPath("writes", 0) == 24 &&
		      burstRes.BoolPath("beforeQuartz", false) &&
		      burstRes.BoolPath("traceDumped", false) && dumpCalls == 1,
		      "diag.burst exposes multi-slot burst summary, beforeQuartz, and dump_trace");

		// 10. motion.scene_nodes list / apply / release
		SceneNodesTest::Fixture snFix;
		snFix.StandardTree();
		core.stageManager.api = snFix.manager.api;
		core.stageManager.candidates = snFix.manager.candidates;
		core.stageManager.currentSceneName = "Photography";
		core.stageManager.candidatesDiscovered = true;
		core.stageManager.SetInjectionEnabled(true);
		core.player.state = PlaybackState::Preview;
		core.RefreshSceneNodes();
		const auto snList = reg.CallWithSizeRetry("motion.scene_nodes", R"({"action":"list"})");
		Check(snList.BoolPath("ok", false) && snList.IntPath("count", 0) == 2,
		      "motion.scene_nodes list returns 2 eligible nodes");
		const auto snApply = reg.Call("motion.scene_nodes",
			R"({"action":"apply","updates":[{"name":"Banner","active":false}]})");
		Check(snApply.BoolPath("ok", false) && snApply.StringPath("status", "") == "Ok",
		      "motion.scene_nodes apply updates visibility");
		const auto snRelease = reg.Call("motion.scene_nodes", R"({"action":"release"})");
		Check(snRelease.BoolPath("ok", false) && snRelease.StringPath("status", "") == "Ok",
		      "motion.scene_nodes release restores baseline");

		// 11. Solo mode: transition back to 1 actor and verify solo diag.physics + solo motion.load + offset
		core.PublishSceneActors({{actor1, 101, "hski", "Saki", "Saki", "face1"}});
		core.currentActor = actor1;
		Check(!core.formation.active, "single actor returns formation.active to false");
		const auto soloPhysEmpty = reg.Call("diag.physics");
		Check(!soloPhysEmpty.BoolPath("ok", true) &&
		      soloPhysEmpty.StringPath("reason", "") == "no_physics_snapshot_for_actor",
		      "solo diag.physics reports no_physics_snapshot_for_actor when actor snapshot is empty");
		singlePhys.snapshot.version = GAKU_PHYSICS_DIAGNOSTICS_VERSION;
		singlePhys.snapshot.size = sizeof(singlePhys.snapshot);
		singlePhys.snapshot.actor = reinterpret_cast<uintptr_t>(actor1);
		singlePhys.snapshot.requested_backend = 2;
		singlePhys.snapshot.actual_backend = 2;
		singlePhys.snapshot.solved_bones = 15;
		singlePhys.reason = "native";
		const auto soloPhysOk = reg.Call("diag.physics");
		Check(soloPhysOk.BoolPath("ok", false) && !soloPhysOk.BoolPath("formationActive", true) &&
		      soloPhysOk.IntPath("solvedBones", 0) == 15,
		      "solo diag.physics returns single-actor snapshot");
		const auto soloLoad = reg.Call("motion.load", R"({"motion_key":"solo_song","autoplay":true})");
		Check(soloLoad.BoolPath("ok", false) && soloLoad.StringPath("loadedMotionKey", "") == "solo_song" &&
		      core.player.state == PlaybackState::Playing,
		      "solo motion.load supports autoplay=true");
		const auto soloOffset = reg.Call("motion.transport", R"({"action":"offset","offset":[0.1,0.2,0.3]})");
		Check(soloOffset.BoolPath("ok", false) &&
		      std::fabs(core.player.controller.initialPositionOffset.x - 0.1f) < 1e-4f,
		      "solo motion.transport offset applies in solo mode");

		adapter.Shutdown();
		fs::remove_all(fixture, ec);
	}
}
