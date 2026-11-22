#pragma once

static void TestTrackTimingMetadata()
{
	BeginTest("track_timing_metadata_and_time_mapping");
	PerformanceTiming timing;
	Check(timing.motion.speed == 1 && timing.facial.offsetSeconds == 0 && ValidPerformanceTiming(timing),
		"legacy and new tracks default to unit speed and zero offset");
	timing.motion = {0.5f, 2.0f};
	timing.facial = {1.5f, -1.0f};
	timing.camera = {0.0f, -3.0f};
	timing.audio = {2.0f, 0.25f};
	jsonlite::Value info;
	std::string error;
	Check(jsonlite::ParseText("{\"track_timing\":" + PerformanceTimingJson(timing) + "}", info, error),
		"all four track timing settings serialize as valid locale-independent JSON");
	const auto restored = ReadPerformanceTiming(info);
	Check(restored.motion.speed == 0.5f && restored.motion.offsetSeconds == 2.0f &&
		restored.facial.speed == 1.5f && restored.facial.offsetSeconds == -1.0f &&
		restored.camera.speed == 0.0f && restored.camera.offsetSeconds == -3.0f &&
		restored.audio.speed == 2.0f && restored.audio.offsetSeconds == 0.25f,
		"independent track values survive metadata round trip including zero speed and negative offsets");
	Check(TrackSourceTime(1, timing.motion) == 0 && TrackSourceTime(6, timing.motion) == 2 &&
		TrackSourceTime(1, timing.facial) == 3 && TrackSourceTime(20, timing.camera) == 0,
		"positive offsets delay, negative offsets advance, and zero speed holds the first frame");
	Check(TrackTimelineDuration(10, timing.motion) == 22 && TrackTimelineDuration(12, timing.facial) == 7 &&
		TrackTimelineDuration(10, timing.camera) == 10,
		"performance duration transforms inversely with track speed and remains finite at zero speed");
	Check(jsonlite::ParseText("{\"track_timing\":{\"motion\":{\"speed\":-2,\"offset_seconds\":\"bad\"}}}", info, error),
		"malformed-field metadata fixture parses");
	const auto invalid = ReadPerformanceTiming(info);
	Check(invalid.motion.speed == 1 && invalid.motion.offsetSeconds == 0 && invalid.audio.speed == 1,
		"malformed metadata fields and missing tracks use safe defaults");
	timing.audio.speed = -1;
	Check(!ValidPerformanceTiming(timing), "negative track speed is rejected before import");
	timing.audio.speed = (std::numeric_limits<float>::infinity)();
	Check(!ValidPerformanceTiming(timing), "non-finite track speed is rejected before import");
	timing.audio.speed = 1;
	timing.audio.offsetSeconds = (std::numeric_limits<float>::quiet_NaN)();
	Check(!ValidPerformanceTiming(timing), "non-finite offset is rejected before import");

	// Per-slot track timing helpers + library scanner + ResolveSlotPerformanceTiming
	{
		PerformanceTiming global;
		global.motion = {1.1f, 0.25f};
		global.facial = {0.9f, -0.5f};
		global.camera = {1.5f, 1.0f};
		global.audio = {0.8f, 2.0f};

		jsonlite::Value posWithTiming;
		Check(jsonlite::ParseText(
			"{\"slot\":2,\"track_timing\":" + SlotTrackTimingJson({0.5f, 3.0f}, {2.0f, -1.5f}) + "}",
			posWithTiming, error), "SlotTrackTimingJson parses as valid JSON");
		TrackTiming outMotion, outFacial;
		bool hasExplicit = false;
		Check(ReadSlotTrackTimings(posWithTiming, global, outMotion, outFacial, &hasExplicit) && hasExplicit &&
			outMotion.speed == 0.5f && outMotion.offsetSeconds == 3.0f &&
			outFacial.speed == 2.0f && outFacial.offsetSeconds == -1.5f,
			"ReadSlotTrackTimings reads explicit slot motion/facial timing");

		jsonlite::Value posPartial;
		Check(jsonlite::ParseText(R"({"slot":2,"track_timing":{"motion":{"speed":0.75,"offset_seconds":1.0}}})",
			posPartial, error), "partial slot track_timing parses");
		Check(ReadSlotTrackTimings(posPartial, global, outMotion, outFacial, &hasExplicit) && hasExplicit &&
			outMotion.speed == 0.75f && outMotion.offsetSeconds == 1.0f &&
			outFacial.speed == 0.9f && outFacial.offsetSeconds == -0.5f,
			"partial slot track_timing falls back per-subtrack to global timing");

		jsonlite::Value posLegacy;
		Check(jsonlite::ParseText(R"({"slot":2,"x":1.0})", posLegacy, error), "legacy position parses");
		Check(!ReadSlotTrackTimings(posLegacy, global, outMotion, outFacial, &hasExplicit) && !hasExplicit &&
			outMotion.speed == 1.1f && outFacial.speed == 0.9f,
			"legacy position without track_timing defaults to global timing with hasExplicit=false");

		MotionEntry entry;
		entry.timing = global;
		MotionPositionEntry p1; p1.slot = 1; p1.hasTiming = true; p1.motionTiming = {1.4f, 0.1f}; p1.facialTiming = {1.0f, 0.0f};
		MotionPositionEntry p2; p2.slot = 2; p2.hasTiming = true; p2.motionTiming = {0.5f, 2.0f}; p2.facialTiming = {1.8f, -0.4f};
		MotionPositionEntry p3; p3.slot = 3; p3.hasTiming = false; p3.motionTiming = {9.0f, 9.0f};
		entry.positions = {p1, p2, p3};
		const PerformanceTiming r1 = ResolveSlotPerformanceTiming(&entry, 1, PerformanceTiming{});
		const PerformanceTiming r2 = ResolveSlotPerformanceTiming(&entry, 2, PerformanceTiming{});
		const PerformanceTiming r3 = ResolveSlotPerformanceTiming(&entry, 3, PerformanceTiming{});
		Check(r1.motion.speed == 1.4f && r1.camera.speed == 1.5f && r1.audio.speed == 0.8f &&
			r2.motion.speed == 0.5f && r2.facial.speed == 1.8f && r2.camera.speed == 1.5f && r2.audio.offsetSeconds == 2.0f &&
			r3.motion.speed == 1.1f && r3.facial.speed == 0.9f,
			"ResolveSlotPerformanceTiming overrides motion/facial per slot while preserving camera/audio");

		// Scanner test: explicit slot timing, legacy slot fallback, and synthesized directory slot fallback
		namespace fs = std::filesystem;
		const fs::path libRoot = fs::temp_directory_path() /
			("gkmotion_slot_timing_scan_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		const fs::path trackDir = libRoot / "mix_timing";
		auto writeMinClip = [](const fs::path& p) {
			fs::create_directories(p.parent_path());
			std::ofstream out(p, std::ios::binary | std::ios::trunc);
			auto put = [&](auto v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
			out.write("GKCLIPCL", 8);
			put(uint32_t{4}); put(30.0f); put(uint32_t{2}); put(0.0667f); put(uint32_t{1}); put(0.083f);
			put(uint16_t{0}); put(uint8_t{4}); put(uint8_t{0}); out.write("Hips", 4);
			for (int f = 0; f < 2; ++f) { put(Vector3{}); put(Quaternion::Identity()); }
			put(uint8_t{0}); put(uint32_t{0});
		};
		writeMinClip(trackDir / "motion.gkclip");
		writeMinClip(trackDir / "positions" / "slot_2" / "motion.gkclip");
		writeMinClip(trackDir / "positions" / "slot_3" / "motion.gkclip");
		writeMinClip(trackDir / "positions" / "slot_4" / "motion.gkclip");
		WriteTextFile(trackDir / "info.json", R"({
  "title": "Mixed Slot Timing",
  "is_group": true,
  "position_count": 4,
  "track_timing": {"motion":{"speed":1.25,"offset_seconds":0.5},"facial":{"speed":0.75,"offset_seconds":-0.25},"camera":{"speed":1.0,"offset_seconds":0.0},"audio":{"speed":1.0,"offset_seconds":0.0}},
  "positions": [
    {"slot":1,"x":0,"y":0,"z":0,"yaw_degrees":0,"track_timing":{"motion":{"speed":1.5,"offset_seconds":0.2},"facial":{"speed":0.8,"offset_seconds":0.1}}},
    {"slot":2,"x":1,"y":0,"z":0,"yaw_degrees":15,"track_timing":{"motion":{"speed":0.5,"offset_seconds":2.0},"facial":{"speed":2.0,"offset_seconds":-0.5}}},
    {"slot":3,"x":-1,"y":0,"z":0,"yaw_degrees":-15}
  ]
})");
		const auto scan = ScanMotionLibraryDetailed(MotionPathToUtf8(libRoot));
		Check(scan.Succeeded() && scan.motions.size() == 1 && scan.motions[0].positions.size() == 4,
			"scanner collects all 4 slots with mixed info/synthesized timing");
		if (scan.motions.size() == 1 && scan.motions[0].positions.size() == 4)
		{
			const auto& pos = scan.motions[0].positions;
			Check(pos[0].hasTiming && pos[0].motionTiming.speed == 1.5f && pos[0].facialTiming.speed == 0.8f,
				"slot 1 parses explicit position track_timing");
			Check(pos[1].hasTiming && pos[1].motionTiming.speed == 0.5f && pos[1].motionTiming.offsetSeconds == 2.0f &&
				pos[1].facialTiming.speed == 2.0f && pos[1].facialTiming.offsetSeconds == -0.5f,
				"slot 2 parses explicit position track_timing");
			Check(!pos[2].hasTiming && pos[2].motionTiming.speed == 1.25f && pos[2].facialTiming.speed == 0.75f,
				"legacy slot 3 without track_timing defaults to track timing with hasTiming=false");
			Check(!pos[3].hasTiming && pos[3].motionTiming.speed == 1.25f && pos[3].facialTiming.speed == 0.75f,
				"synthesized slot 4 directory entry without info.json object defaults to track timing");
		}
		std::error_code ec;
		fs::remove_all(libRoot, ec);
	}
#ifdef _WIN32
	{
		MotionImportRequest req;
		req.editDir = "synthetic/dir";
		req.positionsExplicit = true;
		ImportPositionSlot s1; s1.slot = 1; s1.preserveExisting = true; s1.hasExplicitPlacement = true;
		s1.hasTiming = true; s1.motionTiming = {1.5f, 0.25f}; s1.facialTiming = {0.8f, -0.5f};
		ImportPositionSlot s2; s2.slot = 2; s2.preserveExisting = true; s2.hasExplicitPlacement = false;
		s2.hasTiming = false; // omitted on edit -> omit track_timing from JSON
		req.positions = {s1, s2};
		std::string reason;
		Check(csharp_import::CanHandle(req, reason), "valid per-slot timing passes CanHandle");
		const std::string jsonText = csharp_import::detail::SerializeImportRequestJson(req);
		jsonlite::Value parsed;
		std::string parseErr;
		Check(jsonlite::ParseText(jsonText, parsed, parseErr) && parsed.IsObject(),
			"SerializeImportRequestJson produces valid JSON");
		const auto* pArr = parsed.Find("positions");
		Check(pArr && pArr->IsArray() && pArr->Size() == 2, "2 serialized position objects");
		if (pArr && pArr->Size() == 2)
		{
			const auto* j1 = pArr->At(0);
			const auto* j2 = pArr->At(1);
			Check(j1->BoolPath("has_explicit_placement", false) && j1->Find("track_timing") != nullptr &&
				j1->Find("track_timing")->Find("motion")->FloatPath("speed", 0.0f) == 1.5f &&
				j1->Find("track_timing")->Find("motion")->FloatPath("offset_seconds", 0.0f) == 0.25f &&
				j1->Find("track_timing")->Find("facial")->FloatPath("speed", 0.0f) == 0.8f,
				"C# import JSON emits has_explicit_placement and slot track_timing with offset_seconds");
			Check(!j2->BoolPath("has_explicit_placement", true) && j2->Find("track_timing") == nullptr,
				"C# import JSON omits track_timing when slot hasTiming=false to preserve existing slot timing");
		}
		req.positions[1].motionTiming.speed = -0.1f;
		Check(!csharp_import::CanHandle(req, reason), "invalid negative slot motionTiming rejected by CanHandle");
	}
#endif
}
