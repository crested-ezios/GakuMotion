#include "e2e_report.hpp"
#include "vmd_fixture_gen.hpp"

#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(__has_include)
#  if __has_include("e2e_report.hpp")
#    define GK_SELFTEST_HAS_REPORT 1
#  else
#    define GK_SELFTEST_HAS_REPORT 0
#  endif
#  if __has_include("__nonexistent_gk_header__.hpp")
#    define GK_SELFTEST_HAS_NONEXISTENT 1
#  else
#    define GK_SELFTEST_HAS_NONEXISTENT 0
#  endif
#else
#  define GK_SELFTEST_HAS_REPORT 0
#  define GK_SELFTEST_HAS_NONEXISTENT 0
#endif

namespace {

struct MockPreM3Camera {
    int followTarget{0};
};

struct MockPostM3Camera {
    int followTarget{0};
    int followSlotIndex{-1};
};

int EmitParityBundle(const std::filesystem::path& outDir) {
    namespace fx = gkmotion::e2e::fixtures;
    std::filesystem::create_directories(outDir);

    struct FileEntry {
        const char* name;
        std::vector<std::uint8_t> bytes;
    };

    const std::vector<FileEntry> entries = {
        { "body_default.vmd",       fx::make_body_vmd(2u, false, 0u) },
        { "body_morph_seed3.vmd",   fx::make_body_vmd(10u, true, 3u) },
        { "morph.vmd",              fx::make_morph_vmd(2u) },
        { "eye_only.vmd",           fx::make_eye_only_vmd(2u) },
        { "camera.vmd",             fx::make_camera_vmd(2u, 45u) },
        { "empty.vmd",              fx::make_empty_vmd() },
        { "neg_bad_magic.vmd",      fx::make_negative_vmd(fx::NegativeVmdKind::BadMagic) },
        { "neg_trunc_header.vmd",   fx::make_negative_vmd(fx::NegativeVmdKind::TruncatedHeader) },
        { "neg_trunc_model.vmd",    fx::make_negative_vmd(fx::NegativeVmdKind::TruncatedModel) },
        { "neg_trunc_bone.vmd",     fx::make_negative_vmd(fx::NegativeVmdKind::TruncatedBoneRecord) },
        { "neg_inflated_bone.vmd",  fx::make_negative_vmd(fx::NegativeVmdKind::InflatedBoneCount) },
        { "neg_trunc_morph.vmd",    fx::make_negative_vmd(fx::NegativeVmdKind::TruncatedMorphRecord) },
        { "neg_trunc_camera.vmd",   fx::make_negative_vmd(fx::NegativeVmdKind::TruncatedCameraRecord) },
        { "neg_zero_quat.vmd",      fx::make_negative_vmd(fx::NegativeVmdKind::ZeroQuaternionBone) },
        { "neg_zero_fov.vmd",       fx::make_negative_vmd(fx::NegativeVmdKind::ZeroFovCamera) },
        { "neg_bad_cam_curve.vmd",  fx::make_negative_vmd(fx::NegativeVmdKind::InvalidCameraCurve) },
        { "audio_default.wav",      fx::make_pcm_wav_bytes(0.5, 8000u, 1u, 16u, false, false) },
        { "audio_data_first_junk.wav", fx::make_pcm_wav_bytes(0.25, 44100u, 2u, 16u, true, true) },
        { "neg_trunc_riff.wav",     fx::make_negative_wav(fx::NegativeWavKind::TruncatedRiff) },
        { "neg_missing_data.wav",   fx::make_negative_wav(fx::NegativeWavKind::MissingDataChunk) },
        { "neg_bad_codec.wav",      fx::make_negative_wav(fx::NegativeWavKind::UnsupportedCodec) },
        { "neg_zero_data.wav",      fx::make_negative_wav(fx::NegativeWavKind::ZeroDataBytes) },
        { "clip_minimal.gkclip",    fx::make_minimal_gkclip_bytes(30.0f, 3u) },
        { "clip_blendshapes.gkclip", fx::make_minimal_gkclip_bytes(
            60.0f, 4u, {{0u, "Hips", 1u}, {7u, "Spine", 1u}}, {"a", "blink"}) },
        { "camera_minimal.gkcamera", fx::make_minimal_gkcamera_bytes(60.0f, 3u) },
    };

    for (const auto& e : entries) {
        if (!fx::WriteBinaryFile(outDir / e.name, e.bytes)) {
            std::cerr << "[selftest_common] Failed writing fixture: " << e.name << '\n';
            return 1;
        }
    }

    if (!fx::write_synthetic_group_track(outDir / "group_track_3", 3)) {
        std::cerr << "[selftest_common] Failed writing group_track_3\n";
        return 1;
    }
    return 0;
}

int EmitSampleJson(const std::string& outPath) {
    std::vector<gke2e::TestResult> sample = {
        {
            "SAMPLE-01",
            1,
            "F2",
            "\xE5\x9B\xA2\xE6\x9B\xB2 1\xE5\x8F\xB7\xE4\xBD\x8D", // 团曲 1号位
            gke2e::Status::Pass,
            "ok"
        },
        {
            "SAMPLE-02",
            2,
            "F5",
            "\xE8\x8A\xB1\xE6\xB5\xB7\xE5\x92\xB2\xE5\xAD\xA3", // 花海咲季
            gke2e::Status::PendingImpl,
            "pending header"
        },
        {
            "SAMPLE-03",
            3,
            "F13",
            "escape_check",
            gke2e::Status::Fail,
            std::string("ctrl:\b\f\n\r\t\x01\"\\")
        }
    };
    const std::string json = gke2e::SerializeResultsJson(sample);
    std::string err;
    if (!gke2e::WriteUtf8NoBomFile(outPath, json, &err)) {
        std::cerr << "[selftest_common] Failed writing sample JSON: " << err << '\n';
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i] ? std::string_view(argv[i]) : std::string_view{};
        if (arg == "--emit-fixtures" && i + 1 < argc) {
            return EmitParityBundle(gke2e::PathFromUtf8(argv[i + 1]));
        }
        if (arg == "--emit-sample-json" && i + 1 < argc) {
            return EmitSampleJson(argv[i + 1]);
        }
    }

    gke2e::Runner runner;

    runner.Add("ST-01", 1, "SELFTEST", "Assertion macros and classic-locale float near checks",
        [](gke2e::CaseContext& ctx) {
            GKE2E_CHECK(ctx, 2 + 2 == 4);
            GKE2E_CHECK_MSG(ctx, true, "always true");
            GKE2E_CHECK_EQ(ctx, std::string("团曲"), "团曲");
            GKE2E_CHECK_EQ(ctx, 42u, 42);
            GKE2E_CHECK_NEAR(ctx, 1.0001, 1.0, 1e-3);

            // Verify NaN/Inf rejection on an isolated context
            gke2e::CaseContext nanCtx;
            GKE2E_CHECK_NEAR(nanCtx, std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0);
            GKE2E_REQUIRE(ctx, nanCtx.HasFailed());

            gke2e::CaseContext infCtx;
            GKE2E_CHECK_NEAR(infCtx, std::numeric_limits<double>::infinity(), 0.0, 1.0);
            GKE2E_REQUIRE(ctx, infCtx.HasFailed());

            // Verify GKE2E_REQUIRE early return
            gke2e::CaseContext reqCtx;
            bool reachedAfterRequire = false;
            [&](gke2e::CaseContext& inner) {
                GKE2E_REQUIRE(inner, false);
                reachedAfterRequire = true;
            }(reqCtx);
            GKE2E_REQUIRE(ctx, reqCtx.HasFailed() && !reachedAfterRequire);

            GKE2E_PASS(ctx, "macros verified");
        });

    runner.Add("ST-02", 1, "SELFTEST", "Sticky fail and pending status transitions",
        [](gke2e::CaseContext& ctx) {
            gke2e::CaseContext c1;
            c1.Fail("initial failure");
            c1.Pass("cannot overwrite");
            c1.Pending("cannot overwrite");
            GKE2E_REQUIRE_EQ(ctx, gke2e::StatusString(c1.GetStatus()), "FAIL");
            GKE2E_REQUIRE_EQ(ctx, c1.GetDetail(), "initial failure");

            gke2e::CaseContext c2;
            [&](gke2e::CaseContext& inner) {
                GKE2E_PENDING(inner, "not implemented yet");
            }(c2);
            GKE2E_REQUIRE_EQ(ctx, gke2e::StatusString(c2.GetStatus()), "PENDING_IMPL");
            GKE2E_REQUIRE_EQ(ctx, c2.GetDetail(), "not implemented yet");

            GKE2E_PASS(ctx, "sticky transitions verified");
        });

    runner.Add("ST-03", 1, "SELFTEST", "RFC 8259 JSON escaping and UTF-8 CJK preservation",
        [](gke2e::CaseContext& ctx) {
            const std::string raw = std::string("a\"b\\c\b\f\n\r\t\x01") + "|\xE5\x9B\xA2\xE6\x9B\xB2|\xE8\x8A\xB1\xE6\xB5\xB7\xE5\x92\xB2\xE5\xAD\xA3|1\xE5\x8F\xB7\xE4\xBD\x8D";
            const std::string escaped = gke2e::EscapeJsonString(raw);
            const std::string expected =
                "\"a\\\"b\\\\c\\b\\f\\n\\r\\t\\u0001|\xE5\x9B\xA2\xE6\x9B\xB2|\xE8\x8A\xB1\xE6\xB5\xB7\xE5\x92\xB2\xE5\xAD\xA3|1\xE5\x8F\xB7\xE4\xBD\x8D\"";
            GKE2E_REQUIRE_EQ(ctx, escaped, expected);

            const std::string serialized = gke2e::SerializeResultsJson({
                { "T1-F2-01", 1, "F2", "\xE5\x9B\xA2\xE6\x9B\xB2", gke2e::Status::Pass, "" }
            });
            GKE2E_REQUIRE(ctx, serialized.size() >= 3);
            const bool hasBom = (static_cast<unsigned char>(serialized[0]) == 0xEF &&
                                 static_cast<unsigned char>(serialized[1]) == 0xBB &&
                                 static_cast<unsigned char>(serialized[2]) == 0xBF);
            GKE2E_REQUIRE(ctx, !hasBom);
            GKE2E_PASS(ctx, "JSON escaping verified");
        });

    runner.Add("ST-04", 1, "SELFTEST", "C++20 __has_include and concept member probes",
        [](gke2e::CaseContext& ctx) {
            GKE2E_REQUIRE_EQ(ctx, GK_SELFTEST_HAS_REPORT, 1);
            GKE2E_REQUIRE_EQ(ctx, GK_SELFTEST_HAS_NONEXISTENT, 0);

            // Missing header probe sets PENDING_IMPL
            gke2e::CaseContext hdrCtx;
            [&](gke2e::CaseContext& inner) {
                GKE2E_REQUIRE_HEADER(inner, GK_SELFTEST_HAS_NONEXISTENT, "__nonexistent_gk_header__.hpp");
            }(hdrCtx);
            GKE2E_REQUIRE_EQ(ctx, gke2e::StatusString(hdrCtx.GetStatus()), "PENDING_IMPL");

            // Pre-M3 mock camera lacks followSlotIndex -> takes else branch cleanly
            gke2e::CaseContext preCtx;
            MockPreM3Camera preCam;
            [&](gke2e::CaseContext& inner) {
                GKE2E_WITH_MEMBER_PROBE(inner, gke2e::probes::HasFollowSlotIdx, preCam, "Missing followSlotIndex", {
                    probed.followSlotIndex = 2;
                });
            }(preCtx);
            GKE2E_REQUIRE_EQ(ctx, gke2e::StatusString(preCtx.GetStatus()), "PENDING_IMPL");

            // Post-M3 mock camera has followSlotIndex -> executes probe body
            gke2e::CaseContext postCtx;
            MockPostM3Camera postCam;
            [&](gke2e::CaseContext& inner) {
                GKE2E_WITH_MEMBER_PROBE(inner, gke2e::probes::HasFollowSlotIdx, postCam, "Missing followSlotIndex", {
                    probed.followSlotIndex = 2;
                });
                GKE2E_PASS(inner, "probe invoked");
            }(postCtx);
            GKE2E_REQUIRE_EQ(ctx, gke2e::StatusString(postCtx.GetStatus()), "PASS");
            GKE2E_REQUIRE_EQ(ctx, postCam.followSlotIndex, 2);

            GKE2E_PASS(ctx, "capability probes verified");
        });

    runner.Add("ST-05", 1, "SELFTEST", "Runner exception isolation across test cases",
        [](gke2e::CaseContext& ctx) {
            gke2e::Runner sub;
            sub.Add("SUB-THROW", 1, "SELFTEST", "Throws std::runtime_error",
                [](gke2e::CaseContext&) {
                    throw std::runtime_error("intentional test boom");
                });
            sub.Add("SUB-AFTER", 1, "SELFTEST", "Runs after throw",
                [](gke2e::CaseContext& inner) {
                    GKE2E_PASS(inner, "survived");
                });
            const auto results = sub.ExecuteAll();
            GKE2E_REQUIRE_EQ(ctx, results.size(), static_cast<std::size_t>(2));
            GKE2E_REQUIRE_EQ(ctx, gke2e::StatusString(results[0].status), "FAIL");
            GKE2E_REQUIRE(ctx, results[0].detail.find("intentional test boom") != std::string::npos);
            GKE2E_REQUIRE_EQ(ctx, gke2e::StatusString(results[1].status), "PASS");
            GKE2E_PASS(ctx, "exception isolation verified");
        });

    runner.Add("ST-06", 1, "SELFTEST", "VMD, WAV, gkclip, gkcamera, and info.json fixture sanity",
        [](gke2e::CaseContext& ctx) {
            namespace fx = gkmotion::e2e::fixtures;
            const auto body = fx::make_body_vmd(2u, false, 0u);
            GKE2E_REQUIRE(ctx, body.size() > 74u);
            GKE2E_REQUIRE(ctx, std::memcmp(body.data(), "Vocaloid Motion Data 0002", 25) == 0);

            const auto empty = fx::make_empty_vmd();
            GKE2E_REQUIRE_EQ(ctx, empty.size(), static_cast<std::size_t>(74));

            const auto wav = fx::make_pcm_wav_bytes(0.5, 8000u, 1u, 16u);
            GKE2E_REQUIRE_EQ(ctx, wav.size(), static_cast<std::size_t>(44 + 8000));
            GKE2E_REQUIRE(ctx, std::memcmp(wav.data(), "RIFF", 4) == 0);
            GKE2E_REQUIRE(ctx, std::memcmp(wav.data() + 8, "WAVE", 4) == 0);

            const auto clip = fx::make_minimal_gkclip_bytes(30.0f, 3u);
            GKE2E_REQUIRE(ctx, clip.size() > 32u);
            GKE2E_REQUIRE(ctx, std::memcmp(clip.data(), "GKCLIPCL", 8) == 0);

            const auto cam = fx::make_minimal_gkcamera_bytes(60.0f, 3u);
            GKE2E_REQUIRE_EQ(ctx, cam.size(), static_cast<std::size_t>(20 + 3 * 40));
            GKE2E_REQUIRE(ctx, std::memcmp(cam.data(), "GKCAM\0\0\0", 8) == 0);

            fx::TrackInfoSpec infoSpec;
            infoSpec.is_group = true;
            infoSpec.position_count = 3;
            infoSpec.positions = { {1}, {2}, {3} };
            const std::string infoJson = fx::make_info_json(infoSpec);
            GKE2E_REQUIRE(ctx, infoJson.find("\"is_group\": true") != std::string::npos);
            GKE2E_REQUIRE(ctx, infoJson.find("\"position_count\": 3") != std::string::npos);
            GKE2E_PASS(ctx, "fixture sanity verified");
        });

    return runner.Run(argc, argv);
}
