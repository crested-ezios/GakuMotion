#pragma once
#include "GkMotionData.hpp"
#include "GkJsonLite.hpp"
#include "GkMouthSourceChannels.hpp"
#include <array>
#include <limits>

namespace GakuMotion::mouth_source
{
    inline constexpr std::array<const char*, 18> Names{
        "miku.raw.v1.a", "miku.raw.v1.i", "miku.raw.v1.u", "miku.raw.v1.e", "miku.raw.v1.o",
        "miku.raw.v1.present", "miku.raw.v1.unsupported", "miku.raw.v1.native_authored",
        "miku.rawmouth.v1.a", "miku.rawmouth.v1.i", "miku.rawmouth.v1.u",
        "miku.rawmouth.v1.e", "miku.rawmouth.v1.o", "miku.rawmouth.v1.∧",
        "miku.rawmouth.v1.口角上げ", "miku.rawmouth.v1.present",
        "miku.rawmouth.v1.unsupported", "miku.rawmouth.v1.native_authored"};
    using Reader = int32_t (*)(const char*, float, float, int32_t, float*, int32_t);
    inline Reader reader{};
    inline Reader extraReader{};
    inline void (*report)(const char*, const char*){};

    inline bool HasFamily(const MotionClip& clip, size_t first, size_t count)
    {
        return std::all_of(Names.begin() + first, Names.begin() + first + count,
            [&](const char* name) { return std::count(clip.blendshape_names.begin(), clip.blendshape_names.end(), name) == 1; });
    }

    inline bool Append(MotionClip& clip, const std::vector<float>& source)
    {
        const size_t frames = clip.total_frames;
        const size_t previousCount = clip.blendshape_names.size();
        if (!frames || previousCount > 1024 || previousCount * frames != clip.blendshape_weights.size() ||
            source.size() != frames * Names.size() ||
            std::any_of(source.begin(), source.end(), [](float value) { return !std::isfinite(value) || value < 0 || value > 1; })) return false;
        const bool legacyPresent = HasFamily(clip, 0, 8);
        const bool currentPresent = HasFamily(clip, 8, 10);
        if (legacyPresent && currentPresent) return true;
        bool restoreLegacy = !legacyPresent, restoreCurrent = !currentPresent;
        for (size_t frame = 0; frame < frames; ++frame)
        {
            restoreLegacy = restoreLegacy && source[frame * Names.size() + 5] == 1;
            restoreCurrent = restoreCurrent && source[frame * Names.size() + 15] == 1;
        }
        std::vector<std::string> names = clip.blendshape_names;
        std::vector<std::pair<size_t, size_t>> added;
        for (size_t column = 0; column < Names.size(); ++column)
        {
            if ((column < 8 && !restoreLegacy) || (column >= 8 && !restoreCurrent)) continue;
            if (std::find(names.begin(), names.end(), Names[column]) != names.end()) continue;
            added.emplace_back(names.size(), column);
            names.emplace_back(Names[column]);
        }
        if (names.size() > 1024) return false;
        if (added.empty()) return true;
        std::vector<float> weights(frames * names.size());
        const auto legacyAuthored = std::find(clip.blendshape_names.begin(), clip.blendshape_names.end(), Names[7]);
        const size_t legacyAuthoredSlot = static_cast<size_t>(legacyAuthored - clip.blendshape_names.begin());
        const size_t authoredSlot = static_cast<size_t>(std::find(names.begin(), names.end(), Names[17]) - names.begin());
        for (size_t frame = 0; frame < frames; ++frame)
        {
            std::copy_n(clip.blendshape_weights.begin() + frame * previousCount, previousCount,
                weights.begin() + frame * names.size());
            for (const auto& [output, input] : added)
                weights[frame * names.size() + output] = source[frame * Names.size() + input];
            if (restoreCurrent && legacyAuthoredSlot < previousCount)
                weights[frame * names.size() + authoredSlot] = (std::max)(weights[frame * names.size() + authoredSlot],
                    clip.blendshape_weights[frame * previousCount + legacyAuthoredSlot]);
        }
        clip.blendshape_names = std::move(names);
        clip.blendshape_weights = std::move(weights);
        return true;
    }

    inline bool HasExtras(const MotionClip& clip)
    {
        return std::all_of(ExtraNames.begin(), ExtraNames.end(), [&](const char* name) {
            return std::count(clip.blendshape_names.begin(), clip.blendshape_names.end(), name) == 1;
        });
    }

    inline bool AppendExtras(MotionClip& clip, const std::vector<float>& source)
    {
        const size_t frames = clip.total_frames;
        const size_t previousCount = clip.blendshape_names.size();
        constexpr size_t count = ExtraNames.size();
        if (!frames || !HasFamily(clip, 8, 10) || previousCount > 1024 - count ||
            previousCount * frames != clip.blendshape_weights.size() || source.size() != frames * count ||
            std::any_of(source.begin(), source.end(), [](float v) { return !std::isfinite(v) || v < 0 || v > 1; })) return false;
        if (HasExtras(clip)) return true;
        if (std::any_of(ExtraNames.begin(), ExtraNames.end(), [&](const char* name) {
            return std::find(clip.blendshape_names.begin(), clip.blendshape_names.end(), name) != clip.blendshape_names.end();
        })) return false;
        for (size_t frame = 0; frame < frames; ++frame)
            if (source[frame * count + ExtraPresent] != 1.f) return false;

        auto names = clip.blendshape_names;
        names.insert(names.end(), ExtraNames.begin(), ExtraNames.end());
        std::vector<float> weights(frames * names.size());
        for (size_t frame = 0; frame < frames; ++frame)
        {
            auto dest = weights.begin() + frame * names.size();
            std::copy_n(clip.blendshape_weights.begin() + frame * previousCount, previousCount, dest);
            std::copy_n(source.begin() + frame * count, count, dest + previousCount);
        }
        clip.blendshape_names = std::move(names);
        clip.blendshape_weights = std::move(weights);
        return true;
    }

    inline std::string Restore(MotionClip& clip, const std::string& directory,
        const jsonlite::Value* preloadedMetadata = nullptr)
    {
        const bool embedded = HasFamily(clip, 8, 10);
        if (embedded && (!extraReader || HasExtras(clip))) return "embedded";
        if (clip.blendshape_names.empty()) return "no_facial_tracks";
        std::error_code error;
        const auto root = MotionPathFromUtf8(directory);
        auto source = root / "source_morph.vmd";
        const bool separateMorph = std::filesystem::is_regular_file(source, error);
        if (!separateMorph) source = root / "source.vmd";
        if (!std::filesystem::is_regular_file(source, error)) return embedded ? "embedded" : "source_vmd_missing";
        if (!embedded && !reader) return "source_reader_unavailable";
        if (!std::isfinite(clip.fps) || clip.fps <= 0 || !clip.total_frames ||
            clip.total_frames > 1000000 || clip.total_frames > static_cast<uint32_t>(INT32_MAX / Names.size())) return "invalid_clip_dimensions";
        jsonlite::Value localMetadata;
        std::string metadataError;
        const bool known = preloadedMetadata != nullptr ||
            jsonlite::ParseFile(directory + "/info.json", localMetadata, metadataError);
        const jsonlite::Value& metadata = preloadedMetadata != nullptr ? *preloadedMetadata : localMetadata;
        const auto* morphFps = known && separateMorph ? metadata.Find("source_morph_fps") : nullptr;
        const auto* configuredFps = morphFps ? morphFps : known ? metadata.Find("source_fps") : nullptr;
        const float sourceFps = configuredFps && configuredFps->IsNumber()
            ? static_cast<float>(configuredFps->numberValue) : 30.0f;
        if (!std::isfinite(sourceFps) || sourceFps <= 0) return "invalid_source_fps";
        const auto sourcePath = MotionPathToUtf8(source);
        const auto frames = static_cast<int32_t>(clip.total_frames);
        if (!embedded)
        {
            std::vector<float> samples(static_cast<size_t>(frames) * Names.size());
            const int32_t status = reader(sourcePath.c_str(), sourceFps, clip.fps, frames,
                samples.data(), static_cast<int32_t>(samples.size()));
            if (status != 0) return "source_read_failed_" + std::to_string(status);
            if (!Append(clip, samples)) return "invalid_source_response";
            if (!HasFamily(clip, 8, 10)) return "source_has_no_mouth_intent";
        }
        if (extraReader && !HasExtras(clip))
        {
            std::vector<float> extras(static_cast<size_t>(frames) * ExtraNames.size());
            if (extraReader(sourcePath.c_str(), sourceFps, clip.fps, frames, extras.data(),
                static_cast<int32_t>(extras.size())) == 0) AppendExtras(clip, extras);
        }
        return (embedded ? "embedded source=" : "restored_from_vmd_in_memory source=") + sourcePath +
            " source_fps=" + std::to_string(sourceFps) + (HasExtras(clip) ? " extras=available" : "");
    }
}
