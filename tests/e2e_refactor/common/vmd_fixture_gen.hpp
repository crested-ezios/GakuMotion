#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gkmotion::e2e::fixtures {

enum class VmdVersion {
    V2,
    V1
};

enum class NegativeVmdKind {
    BadMagic,
    TruncatedHeader,
    TruncatedModel,
    TruncatedBoneRecord,
    InflatedBoneCount,
    TruncatedMorphRecord,
    TruncatedCameraRecord,
    ZeroQuaternionBone,
    ZeroFovCamera,
    InvalidCameraCurve
};

enum class NegativeWavKind {
    TruncatedRiff,
    MissingDataChunk,
    UnsupportedCodec,
    ZeroDataBytes
};

inline constexpr std::array<std::uint8_t, 64> kLinearBoneBezier64 = {
    20, 0, 0, 0,  20, 0, 0, 0,  107, 0, 0, 0,  107, 0, 0, 0,
    20, 0, 0, 0,  20, 0, 0, 0,  107, 0, 0, 0,  107, 0, 0, 0,
    20, 0, 0, 0,  20, 0, 0, 0,  107, 0, 0, 0,  107, 0, 0, 0,
    20, 0, 0, 0,  20, 0, 0, 0,  107, 0, 0, 0,  107, 0, 0, 0
};

inline constexpr std::array<std::uint8_t, 24> kLinearCamBezier24 = {
    20, 107, 20, 107,
    20, 107, 20, 107,
    20, 107, 20, 107,
    20, 107, 20, 107,
    20, 107, 20, 107,
    20, 107, 20, 107
};

struct BoneKey {
    std::string name;
    std::uint32_t frame{0};
    std::array<float, 3> pos{0.0f, 0.0f, 0.0f};
    std::array<float, 4> rot{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<std::uint8_t, 64> bezier{kLinearBoneBezier64};
};

struct MorphKey {
    std::string name;
    std::uint32_t frame{0};
    float weight{1.0f};
};

struct CameraKey {
    std::uint32_t frame{0};
    float distance{-4.0f};
    std::array<float, 3> target{0.0f, 1.0f, 0.0f};
    std::array<float, 3> rot_rad{0.0f, 0.0f, 0.0f};
    std::array<std::uint8_t, 24> curves{kLinearCamBezier24};
    std::uint32_t fov{45};
    std::uint8_t perspective{0};
};

struct IkKey {
    std::uint32_t frame{0};
    bool visible{true};
    std::vector<std::pair<std::string, bool>> ik_switches;
};

struct VmdSpec {
    VmdVersion version{VmdVersion::V2};
    std::string model_name{"SyntheticModel"};
    std::vector<BoneKey> bones;
    std::vector<MorphKey> morphs;
    std::vector<CameraKey> cameras;
    std::vector<IkKey> iks;
};

struct ClipBoneSpec {
    std::uint16_t hid{0};
    std::string name{"Hips"};
    std::uint8_t flags{1};
};

struct PositionSlotSpec {
    int slot{1};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float yaw_degrees{0.0f};
    bool has_motion{true};
    bool has_facial{false};
    std::string source_vmd{"source.vmd"};
    std::string source_morph_vmd;
    std::string source_vmd_sha256;
    float source_fps{30.0f};
    int frames{3};
    int bones{1};
};

struct TrackInfoSpec {
    std::string title{"Synthetic Track"};
    std::string subtitle{"Synthetic Author"};
    float duration_seconds{0.5f};
    int total_frames{15};
    int bone_count{1};
    float fps{30.0f};
    std::string source_vmd{"source.vmd"};
    float source_fps{30.0f};
    std::string source_vmd_sha256{"0000000000000000000000000000000000000000000000000000000000000000"};
    bool has_facial{false};
    bool has_camera{false};
    std::string source_morph_vmd;
    std::string source_camera_vmd;
    std::string audio_file;
    float audio_duration{0.0f};
    bool has_cover{false};
    std::string cover_file;
    bool is_group{false};
    int position_count{1};
    std::vector<PositionSlotSpec> positions;
    std::string import_backend{"csharp"};
    std::string import_candidate_b_tree{"0000000000000000000000000000000000000000000000000000000000000000"};
};

namespace detail {

inline void AppendU8(std::vector<std::uint8_t>& buf, std::uint8_t v) {
    buf.push_back(v);
}

inline void AppendU16LE(std::vector<std::uint8_t>& buf, std::uint16_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

inline void AppendU32LE(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

inline void AppendF32LE(std::vector<std::uint8_t>& buf, float v) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v));
    std::memcpy(&bits, &v, sizeof(bits));
    AppendU32LE(buf, bits);
}

inline void AppendBytes(std::vector<std::uint8_t>& buf, const void* data, std::size_t len) {
    const auto* ptr = static_cast<const std::uint8_t*>(data);
    buf.insert(buf.end(), ptr, ptr + len);
}

inline void AppendFixedBytes(std::vector<std::uint8_t>& buf, std::string_view bytes, std::size_t width) {
    const std::size_t copyLen = std::min(bytes.size(), width);
    AppendBytes(buf, bytes.data(), copyLen);
    for (std::size_t i = copyLen; i < width; ++i) {
        buf.push_back(0);
    }
}

struct SjisMapping {
    std::string_view utf8;
    std::string_view sjis;
};

inline std::string EncodeNameToSjisBytes(std::string_view name) {
    static constexpr SjisMapping kTable[] = {
        { "\xE5\x85\xA8\xE3\x81\xA6\xE3\x81\xAE\xE8\xA6\xAA", "\x91\x53\x82\xC4\x82\xCC\x90\x65" }, // 全ての親
        { "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC", "\x83\x5A\x83\x93\x83\x5E\x81\x5B" }, // センター
        { "\xE3\x82\xB0\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\x96", "\x83\x4F\x83\x8B\x81\x5B\x83\x75" }, // グルーブ
        { "\xE4\xB8\x8B\xE5\x8D\x8A\xE8\xBA\xAB",             "\x89\xBA\x94\xBC\x90\x67" },         // 下半身
        { "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB",             "\x8F\xE3\x94\xBC\x90\x67" },         // 上半身
        { "\xE9\xA6\x96",                                     "\x8E\xF1" },                         // 首
        { "\xE9\xA0\xAD",                                     "\x93\xAA" },                         // 頭
        { "\xE5\xB7\xA6\xE8\x85\x95",                         "\x8D\xB6\x98\x72" },                 // 左腕
        { "\xE5\x8F\xB3\xE8\x85\x95",                         "\x89\x45\x98\x72" },                 // 右腕
        { "\xE5\xB7\xA6\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB", "\x8D\xB6\x91\xAB\x82\x68\x82\x6A" }, // 左足ＩＫ
        { "\xE5\x8F\xB3\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB", "\x89\x45\x91\xAB\x82\x68\x82\x6A" }, // 右足ＩＫ
        { "\xE4\xB8\xA1\xE7\x9B\xAE",                         "\x97\xBC\x96\xDA" },                 // 両目
        { "\xE5\xB7\xA6\xE7\x9B\xAE",                         "\x8D\xB6\x96\xDA" },                 // 左目
        { "\xE5\x8F\xB3\xE7\x9B\xAE",                         "\x89\x45\x96\xDA" },                 // 右目
        { "\xE7\x9B\xAE\xE6\x88\xBB",                         "\x96\xDA\x96\xDF" },                 // 目戻
        { "\xE7\x9B\xAE",                                     "\x96\xDA" },                         // 目
        { "\xE3\x81\x82",                                     "\x82\xA0" },                         // あ
        { "\xE3\x81\x84",                                     "\x82\xA2" },                         // い
        { "\xE3\x81\x86",                                     "\x82\xA4" },                         // う
        { "\xE3\x81\x88",                                     "\x82\xA6" },                         // え
        { "\xE3\x81\x8A",                                     "\x82\xA8" },                         // お
        { "\xE3\x81\xBE\xE3\x81\xB0\xE3\x81\x9F\xE3\x81\x8D", "\x82\xDC\x82\xCE\x82\xBD\x82\xAB" }, // まばたき
        { "\xE7\xAC\x91\xE3\x81\x84",                         "\x8F\xCE\x82\xA2" },                 // 笑い
        { "\xE3\x81\xAB\xE3\x82\x84\xE3\x82\x8A",             "\x82\xC9\x82\xE2\x82\xE8" },         // にやり
        { "\xE5\x9B\xB0\xE3\x82\x8B",                         "\x8D\xA2\x82\xE9" },                 // 困る
    };
    for (const auto& entry : kTable) {
        if (name == entry.utf8) {
            return std::string(entry.sjis);
        }
    }
    return std::string(name);
}

inline std::filesystem::path PathFromUtf8(std::string_view utf8Path) {
    std::u8string u8;
    u8.reserve(utf8Path.size());
    for (unsigned char c : utf8Path) {
        u8.push_back(static_cast<char8_t>(c));
    }
    return std::filesystem::path(u8);
}

inline void AppendJsonString(std::string& out, std::string_view s) {
    out.push_back('"');
    for (unsigned char ch : s) {
        switch (ch) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (ch < 0x20u) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(ch));
                    out.append(buf);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    out.push_back('"');
}

inline std::string FormatFloatJson(double val) {
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::setprecision(6) << val;
    std::string s = oss.str();
    if (s == "-0") {
        s = "0";
    }
    return s;
}

} // namespace detail

inline bool WriteBinaryFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }
    if (!data.empty()) {
        ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return ofs.good();
}

inline bool WriteTextFileUtf8(const std::filesystem::path& path, std::string_view text) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }
    if (!text.empty()) {
        ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    return ofs.good();
}

inline std::vector<std::uint8_t> build_vmd_bytes(const VmdSpec& spec) {
    std::vector<std::uint8_t> out;
    const std::size_t modelWidth = (spec.version == VmdVersion::V2) ? 20u : 10u;
    out.reserve(30 + modelWidth + 24 + spec.bones.size() * 111 + spec.morphs.size() * 23 + spec.cameras.size() * 61);

    if (spec.version == VmdVersion::V2) {
        detail::AppendFixedBytes(out, "Vocaloid Motion Data 0002", 30);
    } else {
        detail::AppendFixedBytes(out, "Vocaloid Motion Data file", 30);
    }

    const std::string modelSjis = detail::EncodeNameToSjisBytes(spec.model_name);
    detail::AppendFixedBytes(out, modelSjis, modelWidth);

    // Bones
    detail::AppendU32LE(out, static_cast<std::uint32_t>(spec.bones.size()));
    for (const auto& b : spec.bones) {
        const std::string bSjis = detail::EncodeNameToSjisBytes(b.name);
        detail::AppendFixedBytes(out, bSjis, 15);
        detail::AppendU32LE(out, b.frame);
        detail::AppendF32LE(out, b.pos[0]);
        detail::AppendF32LE(out, b.pos[1]);
        detail::AppendF32LE(out, b.pos[2]);
        detail::AppendF32LE(out, b.rot[0]);
        detail::AppendF32LE(out, b.rot[1]);
        detail::AppendF32LE(out, b.rot[2]);
        detail::AppendF32LE(out, b.rot[3]);
        detail::AppendBytes(out, b.bezier.data(), b.bezier.size());
    }

    // Morphs
    detail::AppendU32LE(out, static_cast<std::uint32_t>(spec.morphs.size()));
    for (const auto& m : spec.morphs) {
        const std::string mSjis = detail::EncodeNameToSjisBytes(m.name);
        detail::AppendFixedBytes(out, mSjis, 15);
        detail::AppendU32LE(out, m.frame);
        detail::AppendF32LE(out, m.weight);
    }

    // Cameras
    detail::AppendU32LE(out, static_cast<std::uint32_t>(spec.cameras.size()));
    for (const auto& c : spec.cameras) {
        detail::AppendU32LE(out, c.frame);
        detail::AppendF32LE(out, c.distance);
        detail::AppendF32LE(out, c.target[0]);
        detail::AppendF32LE(out, c.target[1]);
        detail::AppendF32LE(out, c.target[2]);
        detail::AppendF32LE(out, c.rot_rad[0]);
        detail::AppendF32LE(out, c.rot_rad[1]);
        detail::AppendF32LE(out, c.rot_rad[2]);
        detail::AppendBytes(out, c.curves.data(), c.curves.size());
        detail::AppendU32LE(out, c.fov);
        detail::AppendU8(out, c.perspective);
    }

    // Light (0), SelfShadow (0)
    detail::AppendU32LE(out, 0u);
    detail::AppendU32LE(out, 0u);

    // IK / Property
    detail::AppendU32LE(out, static_cast<std::uint32_t>(spec.iks.size()));
    for (const auto& ik : spec.iks) {
        detail::AppendU32LE(out, ik.frame);
        detail::AppendU8(out, ik.visible ? 1u : 0u);
        detail::AppendU32LE(out, static_cast<std::uint32_t>(ik.ik_switches.size()));
        for (const auto& sw : ik.ik_switches) {
            const std::string swSjis = detail::EncodeNameToSjisBytes(sw.first);
            detail::AppendFixedBytes(out, swSjis, 20);
            detail::AppendU8(out, sw.second ? 1u : 0u);
        }
    }

    return out;
}

inline std::vector<std::uint8_t> make_body_vmd(std::uint32_t last_frame = 2, bool include_morphs = false,
                                               std::uint32_t seed = 0) {
    VmdSpec spec;
    std::vector<std::uint32_t> frames;
    frames.push_back(0u);
    if (last_frame > 0u) {
        frames.push_back(last_frame);
    }

    for (std::uint32_t f : frames) {
        {
            BoneKey root;
            root.name = "\xE5\x85\xA8\xE3\x81\xA6\xE3\x81\xAE\xE8\xA6\xAA"; // 全ての親
            root.frame = f;
            spec.bones.push_back(std::move(root));
        }
        {
            BoneKey center;
            center.name = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC"; // センター
            center.frame = f;
            if (f > 0u) {
                if (seed == 0u) {
                    center.pos = { 0.0f, 0.25f, 0.0f };
                    center.rot = { 0.0f, 0.0f, 0.0f, 1.0f };
                } else {
                    const float dx = static_cast<float>(static_cast<int>(seed % 7u) - 3) * 0.25f;
                    const float dz = static_cast<float>(static_cast<int>(seed % 5u) - 2) * 0.125f;
                    const float t = static_cast<float>((seed % 15u) + 1u) / 32.0f;
                    const float qw = static_cast<float>(std::sqrt(1.0 - static_cast<double>(t) * static_cast<double>(t)));
                    center.pos = { dx, 0.25f, dz };
                    center.rot = { t, 0.0f, 0.0f, qw };
                }
            }
            spec.bones.push_back(std::move(center));
        }
        {
            BoneKey upper;
            upper.name = "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB"; // 上半身
            upper.frame = f;
            spec.bones.push_back(std::move(upper));
        }
    }

    if (include_morphs) {
        for (std::uint32_t f : frames) {
            MorphKey m1;
            m1.name = "\xE3\x81\x82"; // あ
            m1.frame = f;
            m1.weight = (f == 0u) ? 0.0f : 1.0f;
            spec.morphs.push_back(std::move(m1));

            MorphKey m2;
            m2.name = "\xE3\x81\xBE\xE3\x81\xB0\xE3\x81\x9F\xE3\x81\x8D"; // まばたき
            m2.frame = f;
            m2.weight = (f == 0u) ? 0.0f : 0.75f;
            spec.morphs.push_back(std::move(m2));
        }
    }

    return build_vmd_bytes(spec);
}

inline std::vector<std::uint8_t> make_morph_vmd(std::uint32_t last_frame = 2) {
    VmdSpec spec;
    std::vector<std::uint32_t> frames;
    frames.push_back(0u);
    if (last_frame > 0u) {
        frames.push_back(last_frame);
    }
    for (std::uint32_t f : frames) {
        MorphKey m1;
        m1.name = "\xE3\x81\x82"; // あ
        m1.frame = f;
        m1.weight = (f == 0u) ? 0.0f : 1.0f;
        spec.morphs.push_back(std::move(m1));

        MorphKey m2;
        m2.name = "\xE3\x81\xBE\xE3\x81\xB0\xE3\x81\x9F\xE3\x81\x8D"; // まばたき
        m2.frame = f;
        m2.weight = (f == 0u) ? 0.0f : 1.0f;
        spec.morphs.push_back(std::move(m2));
    }
    return build_vmd_bytes(spec);
}

inline std::vector<std::uint8_t> make_eye_only_vmd(std::uint32_t last_frame = 2) {
    VmdSpec spec;
    std::vector<std::uint32_t> frames;
    frames.push_back(0u);
    if (last_frame > 0u) {
        frames.push_back(last_frame);
    }
    for (std::uint32_t f : frames) {
        BoneKey eye;
        eye.name = "\xE4\xB8\xA1\xE7\x9B\xAE"; // 両目
        eye.frame = f;
        spec.bones.push_back(std::move(eye));
    }
    return build_vmd_bytes(spec);
}

inline std::vector<std::uint8_t> make_camera_vmd(std::uint32_t last_frame = 2, std::uint32_t fov = 45) {
    VmdSpec spec;
    std::vector<std::uint32_t> frames;
    frames.push_back(0u);
    if (last_frame > 0u) {
        frames.push_back(last_frame);
    }
    for (std::uint32_t f : frames) {
        CameraKey ck;
        ck.frame = f;
        ck.fov = fov;
        spec.cameras.push_back(std::move(ck));
    }
    return build_vmd_bytes(spec);
}

inline std::vector<std::uint8_t> make_empty_vmd() {
    VmdSpec spec;
    return build_vmd_bytes(spec);
}

inline std::vector<std::uint8_t> make_negative_vmd(NegativeVmdKind kind) {
    std::vector<std::uint8_t> out;
    switch (kind) {
        case NegativeVmdKind::BadMagic: {
            detail::AppendFixedBytes(out, "Not A Valid VMD Header", 50);
            return out;
        }
        case NegativeVmdKind::TruncatedHeader: {
            detail::AppendBytes(out, "Vocaloid Motion ", 16);
            return out;
        }
        case NegativeVmdKind::TruncatedModel: {
            detail::AppendFixedBytes(out, "Vocaloid Motion Data 0002", 30);
            detail::AppendFixedBytes(out, "Synthetic", 10);
            return out;
        }
        case NegativeVmdKind::TruncatedBoneRecord: {
            detail::AppendFixedBytes(out, "Vocaloid Motion Data 0002", 30);
            detail::AppendFixedBytes(out, "SyntheticModel", 20);
            detail::AppendU32LE(out, 1u);
            out.resize(out.size() + 50u, 0u);
            return out;
        }
        case NegativeVmdKind::InflatedBoneCount: {
            detail::AppendFixedBytes(out, "Vocaloid Motion Data 0002", 30);
            detail::AppendFixedBytes(out, "SyntheticModel", 20);
            detail::AppendU32LE(out, 1000u);
            return out;
        }
        case NegativeVmdKind::TruncatedMorphRecord: {
            detail::AppendFixedBytes(out, "Vocaloid Motion Data 0002", 30);
            detail::AppendFixedBytes(out, "SyntheticModel", 20);
            detail::AppendU32LE(out, 0u); // boneCount
            detail::AppendU32LE(out, 1u); // morphCount
            out.resize(out.size() + 10u, 0u);
            return out;
        }
        case NegativeVmdKind::TruncatedCameraRecord: {
            detail::AppendFixedBytes(out, "Vocaloid Motion Data 0002", 30);
            detail::AppendFixedBytes(out, "SyntheticModel", 20);
            detail::AppendU32LE(out, 0u); // boneCount
            detail::AppendU32LE(out, 0u); // morphCount
            detail::AppendU32LE(out, 1u); // cameraCount
            out.resize(out.size() + 30u, 0u);
            return out;
        }
        case NegativeVmdKind::ZeroQuaternionBone: {
            VmdSpec spec;
            BoneKey b;
            b.name = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC"; // センター
            b.frame = 0u;
            b.rot = { 0.0f, 0.0f, 0.0f, 0.0f };
            spec.bones.push_back(std::move(b));
            return build_vmd_bytes(spec);
        }
        case NegativeVmdKind::ZeroFovCamera: {
            return make_camera_vmd(2u, 0u);
        }
        case NegativeVmdKind::InvalidCameraCurve: {
            VmdSpec spec;
            CameraKey ck;
            ck.frame = 0u;
            ck.curves[0] = 200u;
            spec.cameras.push_back(std::move(ck));
            return build_vmd_bytes(spec);
        }
    }
    return out;
}

inline std::vector<std::uint8_t> make_pcm_wav_bytes(double duration_seconds = 0.5,
                                                    std::uint32_t sample_rate = 8000u,
                                                    std::uint16_t channels = 1u,
                                                    std::uint16_t bits_per_sample = 16u,
                                                    bool data_first = false,
                                                    bool odd_junk_chunk = false) {
    const std::uint16_t bytes_per_sample = static_cast<std::uint16_t>(bits_per_sample / 8u);
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * bytes_per_sample);
    const std::uint32_t byte_rate = sample_rate * block_align;
    std::uint32_t sample_count = 0u;
    if (duration_seconds > 0.0) {
        const long long computed = std::llround(duration_seconds * static_cast<double>(sample_rate));
        sample_count = static_cast<std::uint32_t>(std::max<long long>(1, computed));
    }
    const std::uint32_t data_bytes = sample_count * block_align;

    std::vector<std::uint8_t> fmtChunk;
    detail::AppendBytes(fmtChunk, "fmt ", 4);
    detail::AppendU32LE(fmtChunk, 16u);
    detail::AppendU16LE(fmtChunk, 1u); // PCM
    detail::AppendU16LE(fmtChunk, channels);
    detail::AppendU32LE(fmtChunk, sample_rate);
    detail::AppendU32LE(fmtChunk, byte_rate);
    detail::AppendU16LE(fmtChunk, block_align);
    detail::AppendU16LE(fmtChunk, bits_per_sample);

    std::vector<std::uint8_t> junkChunk;
    if (odd_junk_chunk) {
        detail::AppendBytes(junkChunk, "JUNK", 4);
        detail::AppendU32LE(junkChunk, 3u);
        detail::AppendU8(junkChunk, static_cast<std::uint8_t>('A'));
        detail::AppendU8(junkChunk, static_cast<std::uint8_t>('B'));
        detail::AppendU8(junkChunk, static_cast<std::uint8_t>('C'));
        detail::AppendU8(junkChunk, 0u); // RIFF word alignment pad byte
    }

    std::vector<std::uint8_t> dataChunk;
    detail::AppendBytes(dataChunk, "data", 4);
    detail::AppendU32LE(dataChunk, data_bytes);
    dataChunk.resize(dataChunk.size() + data_bytes, 0u);

    const std::uint32_t riff_size = static_cast<std::uint32_t>(
        4u + fmtChunk.size() + junkChunk.size() + dataChunk.size());

    std::vector<std::uint8_t> out;
    out.reserve(8u + riff_size);
    detail::AppendBytes(out, "RIFF", 4);
    detail::AppendU32LE(out, riff_size);
    detail::AppendBytes(out, "WAVE", 4);
    if (data_first) {
        out.insert(out.end(), dataChunk.begin(), dataChunk.end());
        out.insert(out.end(), junkChunk.begin(), junkChunk.end());
        out.insert(out.end(), fmtChunk.begin(), fmtChunk.end());
    } else {
        out.insert(out.end(), fmtChunk.begin(), fmtChunk.end());
        out.insert(out.end(), junkChunk.begin(), junkChunk.end());
        out.insert(out.end(), dataChunk.begin(), dataChunk.end());
    }
    return out;
}

inline std::vector<std::uint8_t> make_negative_wav(NegativeWavKind kind) {
    std::vector<std::uint8_t> out;
    switch (kind) {
        case NegativeWavKind::TruncatedRiff: {
            detail::AppendBytes(out, "RIFF", 4);
            return out;
        }
        case NegativeWavKind::MissingDataChunk: {
            detail::AppendBytes(out, "RIFF", 4);
            detail::AppendU32LE(out, 28u);
            detail::AppendBytes(out, "WAVE", 4);
            detail::AppendBytes(out, "fmt ", 4);
            detail::AppendU32LE(out, 16u);
            detail::AppendU16LE(out, 1u);
            detail::AppendU16LE(out, 1u);
            detail::AppendU32LE(out, 8000u);
            detail::AppendU32LE(out, 16000u);
            detail::AppendU16LE(out, 2u);
            detail::AppendU16LE(out, 16u);
            return out;
        }
        case NegativeWavKind::UnsupportedCodec: {
            out = make_pcm_wav_bytes(0.1, 8000u, 1u, 16u, false, false);
            // Replace audioFormat (offset 20) from 1 (PCM) to 7 (mu-law)
            out[20] = 7u;
            out[21] = 0u;
            return out;
        }
        case NegativeWavKind::ZeroDataBytes: {
            return make_pcm_wav_bytes(0.0, 8000u, 1u, 16u, false, false);
        }
    }
    return out;
}

inline bool write_pcm_wav(const std::filesystem::path& path,
                          double duration_seconds = 0.5,
                          std::uint32_t sample_rate = 8000u,
                          std::uint16_t channels = 1u,
                          std::uint16_t bits_per_sample = 16u) {
    return WriteBinaryFile(path, make_pcm_wav_bytes(duration_seconds, sample_rate, channels, bits_per_sample));
}

inline std::vector<std::uint8_t> make_minimal_gkclip_bytes(float fps = 30.0f,
                                                         std::uint32_t frames = 3u,
                                                         const std::vector<ClipBoneSpec>& bones = {{0u, "Hips", 1u}},
                                                         const std::vector<std::string>& blendshapes = {}) {
    std::vector<std::uint8_t> out;
    const std::uint32_t boneCount = static_cast<std::uint32_t>(bones.size());
    const float durationSeconds = (frames > 1u)
        ? static_cast<float>(static_cast<double>(frames - 1u) / static_cast<double>(fps))
        : 0.0f;
    const float rootScale = 0.07795f;

    detail::AppendBytes(out, "GKCLIPCL", 8);
    detail::AppendU32LE(out, 4u); // version = 4
    detail::AppendF32LE(out, fps);
    detail::AppendU32LE(out, frames);
    detail::AppendF32LE(out, durationSeconds);
    detail::AppendU32LE(out, boneCount);
    detail::AppendF32LE(out, rootScale);

    for (const auto& b : bones) {
        const std::uint8_t nlen = static_cast<std::uint8_t>(std::min<std::size_t>(b.name.size(), 255u));
        detail::AppendU16LE(out, b.hid);
        detail::AppendU8(out, nlen);
        detail::AppendU8(out, b.flags);
        detail::AppendBytes(out, b.name.data(), nlen);
    }

    for (std::uint32_t f = 0; f < frames; ++f) {
        // rootPos (0, 0, 0)
        detail::AppendF32LE(out, 0.0f);
        detail::AppendF32LE(out, 0.0f);
        detail::AppendF32LE(out, 0.0f);
        for (std::uint32_t b = 0; b < boneCount; ++b) {
            detail::AppendF32LE(out, 0.0f);
            detail::AppendF32LE(out, 0.0f);
            detail::AppendF32LE(out, 0.0f);
            detail::AppendF32LE(out, 1.0f);
        }
    }

    // hasCamera = 0
    detail::AppendU8(out, 0u);

    // Blendshapes
    detail::AppendU32LE(out, static_cast<std::uint32_t>(blendshapes.size()));
    for (const auto& name : blendshapes) {
        const std::uint16_t len = static_cast<std::uint16_t>(std::min<std::size_t>(name.size(), 65535u));
        detail::AppendU16LE(out, len);
        detail::AppendBytes(out, name.data(), len);
    }
    const std::size_t totalWeights = static_cast<std::size_t>(frames) * blendshapes.size();
    for (std::size_t i = 0; i < totalWeights; ++i) {
        detail::AppendF32LE(out, 0.0f);
    }

    return out;
}

inline std::vector<std::uint8_t> make_minimal_gkcamera_bytes(float fps = 60.0f, std::uint32_t count = 3u) {
    std::vector<std::uint8_t> out;
    out.reserve(20u + static_cast<std::size_t>(count) * 40u);
    detail::AppendBytes(out, "GKCAM\0\0\0", 8);
    detail::AppendU32LE(out, 1u);
    detail::AppendF32LE(out, fps);
    detail::AppendU32LE(out, count);

    for (std::uint32_t i = 0; i < count; ++i) {
        detail::AppendF32LE(out, 0.0f);
        detail::AppendF32LE(out, 1.0f);
        detail::AppendF32LE(out, -4.0f);
        detail::AppendF32LE(out, 0.0f);
        detail::AppendF32LE(out, 0.0f);
        detail::AppendF32LE(out, 0.0f);
        detail::AppendF32LE(out, 1.0f);
        detail::AppendF32LE(out, 45.0f);
        detail::AppendF32LE(out, 2.0f);
        detail::AppendU32LE(out, 0u);
    }
    return out;
}

inline std::string make_info_json(const TrackInfoSpec& spec) {
    std::string out;
    out.reserve(1024);
    out.append("{\n");
    out.append("  \"title\": "); detail::AppendJsonString(out, spec.title); out.append(",\n");
    out.append("  \"subtitle\": "); detail::AppendJsonString(out, spec.subtitle); out.append(",\n");
    out.append("  \"duration_seconds\": "); out.append(detail::FormatFloatJson(spec.duration_seconds)); out.append(",\n");
    out.append("  \"total_frames\": "); out.append(std::to_string(spec.total_frames)); out.append(",\n");
    out.append("  \"bone_count\": "); out.append(std::to_string(spec.bone_count)); out.append(",\n");
    out.append("  \"fps\": "); out.append(detail::FormatFloatJson(spec.fps)); out.append(",\n");
    out.append("  \"source_vmd\": "); detail::AppendJsonString(out, spec.source_vmd); out.append(",\n");
    out.append("  \"source_fps\": "); out.append(detail::FormatFloatJson(spec.source_fps)); out.append(",\n");
    out.append("  \"source_vmd_sha256\": "); detail::AppendJsonString(out, spec.source_vmd_sha256); out.append(",\n");
    out.append("  \"has_facial\": "); out.append(spec.has_facial ? "true" : "false"); out.append(",\n");
    out.append("  \"has_camera\": "); out.append(spec.has_camera ? "true" : "false"); out.append(",\n");
    out.append("  \"source_morph_vmd\": "); detail::AppendJsonString(out, spec.source_morph_vmd); out.append(",\n");
    out.append("  \"source_camera_vmd\": "); detail::AppendJsonString(out, spec.source_camera_vmd); out.append(",\n");
    if (!spec.audio_file.empty()) {
        out.append("  \"audio_file\": "); detail::AppendJsonString(out, spec.audio_file); out.append(",\n");
        out.append("  \"audio_duration\": "); out.append(detail::FormatFloatJson(spec.audio_duration)); out.append(",\n");
    }
    if (spec.has_cover) {
        out.append("  \"has_cover\": true,\n");
        out.append("  \"cover_file\": "); detail::AppendJsonString(out, spec.cover_file); out.append(",\n");
    }
    out.append("  \"is_group\": "); out.append(spec.is_group ? "true" : "false"); out.append(",\n");
    out.append("  \"position_count\": "); out.append(std::to_string(spec.position_count)); out.append(",\n");
    out.append("  \"positions\": [");
    if (!spec.positions.empty()) {
        out.append("\n");
        for (std::size_t i = 0; i < spec.positions.size(); ++i) {
            const auto& p = spec.positions[i];
            out.append("    {\n");
            out.append("      \"slot\": "); out.append(std::to_string(p.slot)); out.append(",\n");
            out.append("      \"x\": "); out.append(detail::FormatFloatJson(p.x)); out.append(",\n");
            out.append("      \"y\": "); out.append(detail::FormatFloatJson(p.y)); out.append(",\n");
            out.append("      \"z\": "); out.append(detail::FormatFloatJson(p.z)); out.append(",\n");
            out.append("      \"yaw_degrees\": "); out.append(detail::FormatFloatJson(p.yaw_degrees)); out.append(",\n");
            out.append("      \"has_motion\": "); out.append(p.has_motion ? "true" : "false"); out.append(",\n");
            out.append("      \"has_facial\": "); out.append(p.has_facial ? "true" : "false"); out.append(",\n");
            out.append("      \"source_vmd\": "); detail::AppendJsonString(out, p.source_vmd); out.append(",\n");
            out.append("      \"source_morph_vmd\": "); detail::AppendJsonString(out, p.source_morph_vmd); out.append(",\n");
            out.append("      \"source_vmd_sha256\": "); detail::AppendJsonString(out, p.source_vmd_sha256); out.append(",\n");
            out.append("      \"source_fps\": "); out.append(detail::FormatFloatJson(p.source_fps)); out.append(",\n");
            out.append("      \"frames\": "); out.append(std::to_string(p.frames)); out.append(",\n");
            out.append("      \"bones\": "); out.append(std::to_string(p.bones)); out.append("\n");
            out.append("    }");
            if (i + 1 < spec.positions.size()) {
                out.append(",");
            }
            out.append("\n");
        }
        out.append("  ");
    }
    out.append("],\n");
    out.append("  \"track_timing\": {\n");
    out.append("    \"motion\": {\"speed\": 1.0, \"offset_seconds\": 0.0},\n");
    out.append("    \"facial\": {\"speed\": 1.0, \"offset_seconds\": 0.0},\n");
    out.append("    \"camera\": {\"speed\": 1.0, \"offset_seconds\": 0.0},\n");
    out.append("    \"audio\": {\"speed\": 1.0, \"offset_seconds\": 0.0}\n");
    out.append("  },\n");
    out.append("  \"import_backend\": "); detail::AppendJsonString(out, spec.import_backend); out.append(",\n");
    out.append("  \"import_candidate_b_tree\": "); detail::AppendJsonString(out, spec.import_candidate_b_tree); out.append("\n");
    out.append("}\n");
    return out;
}

inline bool write_synthetic_track_dir(const std::filesystem::path& dir,
                                      const TrackInfoSpec& spec,
                                      bool write_camera = false,
                                      bool write_audio = false) {
    std::filesystem::create_directories(dir);
    const std::uint32_t frames = static_cast<std::uint32_t>(std::max(1, spec.total_frames));
    const auto clipBytes = make_minimal_gkclip_bytes(spec.fps, frames);
    if (!WriteBinaryFile(dir / "motion.gkclip", clipBytes)) {
        return false;
    }
    if (!WriteBinaryFile(dir / "source.vmd", make_body_vmd(2u, spec.has_facial, 0u))) {
        return false;
    }
    if (write_camera || spec.has_camera) {
        if (!WriteBinaryFile(dir / "camera.gkcamera", make_minimal_gkcamera_bytes(60.0f, frames))) {
            return false;
        }
        if (!WriteBinaryFile(dir / "source_camera.vmd", make_camera_vmd(2u, 45u))) {
            return false;
        }
    }
    if (write_audio || !spec.audio_file.empty()) {
        const std::string audioName = spec.audio_file.empty() ? "song.wav" : spec.audio_file;
        const double dur = (spec.audio_duration > 0.0f) ? static_cast<double>(spec.audio_duration) : 0.5;
        if (!write_pcm_wav(dir / audioName, dur, 8000u, 1u, 16u)) {
            return false;
        }
    }
    if (spec.is_group && spec.position_count >= 2) {
        for (int k = 2; k <= spec.position_count; ++k) {
            const std::filesystem::path slotDir = dir / "positions" / ("slot_" + std::to_string(k));
            std::filesystem::create_directories(slotDir);
            if (!WriteBinaryFile(slotDir / "motion.gkclip", clipBytes)) {
                return false;
            }
            if (!WriteBinaryFile(slotDir / "source.vmd", make_body_vmd(2u, spec.has_facial, static_cast<std::uint32_t>(k)))) {
                return false;
            }
        }
    }
    return WriteTextFileUtf8(dir / "info.json", make_info_json(spec));
}

inline bool write_synthetic_group_track(const std::filesystem::path& dir,
                                        int slot_count = 3,
                                        std::string_view title = "Group Track",
                                        std::string_view subtitle = "Synthetic Author",
                                        bool write_camera = true,
                                        bool write_audio = true) {
    const int count = std::max(2, slot_count);
    TrackInfoSpec spec;
    spec.title = std::string(title);
    spec.subtitle = std::string(subtitle);
    spec.total_frames = 3;
    spec.fps = 30.0f;
    spec.duration_seconds = 2.0f / 30.0f;
    spec.bone_count = 1;
    spec.has_facial = false;
    spec.has_camera = write_camera;
    if (write_camera) {
        spec.source_camera_vmd = "source_camera.vmd";
    }
    if (write_audio) {
        spec.audio_file = "song.wav";
        spec.audio_duration = 0.5f;
    }
    spec.is_group = true;
    spec.position_count = count;
    for (int k = 1; k <= count; ++k) {
        PositionSlotSpec pos;
        pos.slot = k;
        pos.x = 0.0f;
        pos.y = 0.0f;
        pos.z = 0.0f;
        pos.yaw_degrees = 0.0f;
        pos.has_motion = true;
        pos.has_facial = false;
        pos.source_vmd = "slot_" + std::to_string(k) + ".vmd";
        pos.source_fps = 30.0f;
        pos.frames = 3;
        pos.bones = 1;
        spec.positions.push_back(std::move(pos));
    }
    return write_synthetic_track_dir(dir, spec, write_camera, write_audio);
}

} // namespace gkmotion::e2e::fixtures
