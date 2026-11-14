#pragma once
#include "GkMikuFacialFitModel.hpp"
#include "GkRinhaFacialFit.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace GakuMotion
{
    inline constexpr const char* kMikuRawNames[] = {
        "miku.raw.v1.a", "miku.raw.v1.i", "miku.raw.v1.u", "miku.raw.v1.e", "miku.raw.v1.o",
        "miku.raw.v1.present", "miku.raw.v1.unsupported", "miku.raw.v1.native_authored"
    };
    inline constexpr const char* kMikuRawEyePrefix = "miku.raweye.v1.";
    inline constexpr const char* kMikuRawEyeMarkers[] = {
        "miku.raweye.v1.present", "miku.raweye.v1.unsupported", "miku.raweye.v1.native_authored"
    };
    enum class MikuFitDecision { Disabled, MissingRawSource, InvalidRawSource, UnsupportedSource,
                                 NativeAuthored, UnknownFace, CharacterMismatch, MissingOutput,
                                 OutsideDomain, InvalidModel, Applied,
                                 // Appended after the v1 decision names shipped: the GUI
                                 // and the packed snapshot mask are keyed by ordinal, so a
                                 // new value may only ever grow the tail.
                                 ExternalApplied, ExternalUnverified, MissingRawEyeSource,
                                 UnsupportedEyeSource };
    inline const char* MikuFitDecisionName(MikuFitDecision value)
    {
        switch (value) {
        case MikuFitDecision::Disabled: return "disabled";
        case MikuFitDecision::MissingRawSource: return "missing_raw_source";
        case MikuFitDecision::InvalidRawSource: return "invalid_raw_source";
        case MikuFitDecision::UnsupportedSource: return "unsupported_source_mouth";
        case MikuFitDecision::NativeAuthored: return "native_authored_mouth";
        case MikuFitDecision::UnknownFace: return "unverified_face_identity";
        case MikuFitDecision::CharacterMismatch: return "character_override_mismatch";
        case MikuFitDecision::MissingOutput: return "missing_native_output";
        case MikuFitDecision::OutsideDomain: return "outside_fitted_domain";
        case MikuFitDecision::InvalidModel: return "invalid_fitted_model";
        case MikuFitDecision::Applied: return "applied";
        case MikuFitDecision::ExternalApplied: return "applied_published_profile";
        case MikuFitDecision::ExternalUnverified: return "published_profile_unverified";
        case MikuFitDecision::MissingRawEyeSource: return "missing_raw_eye_source";
        case MikuFitDecision::UnsupportedEyeSource: return "unsupported_source_eye";
        }
        return "unknown";
    }
    inline bool HasMikuRawSource(const std::vector<std::string>& names)
    {
        for (const char* name : kMikuRawNames)
            if (std::count(names.begin(), names.end(), name) != 1) return false;
        return true;
    }
    inline bool HasMikuRawEyeSource(const std::vector<std::string>& names)
    {
        for (const char* name : kMikuRawEyeMarkers)
            if (std::count(names.begin(), names.end(), name) != 1) return false;
        return true;
    }
    inline bool IsEyeFamilyName(const std::string& name)
    {
        return name.rfind("b_eye.eye_", 0) == 0 || name.rfind("b_brow.brow_", 0) == 0;
    }
    inline const MikuMouthFitProfile* FindMikuMouthProfile(const std::string& code)
    {
        for (const auto& profile : kMikuMouthFits)
            if (code == profile.character) return &profile;
        return nullptr;
    }
    inline const MikuMouthFitProfile* FindMikuMouthFace(std::string name)
    {
        const std::string clone = "(Clone)";
        if (name.size() >= clone.size() && name.compare(name.size()-clone.size(), clone.size(), clone) == 0)
        {
            name.resize(name.size()-clone.size());
            if (!name.empty() && name.back() == ' ') name.pop_back();
        }
        for (const auto& profile : kMikuMouthFits)
            if (name == profile.faceName) return &profile;
        return nullptr;
    }
    inline bool EvaluateMikuMouthProfile(const MikuMouthFitProfile& profile,
                                        const std::array<double, 5>& source,
                                        std::array<double, 9>& output,
                                        double inputTolerance = 1e-10)
    {
        constexpr double tolerance = 1e-10;
        if (!std::isfinite(inputTolerance) || inputTolerance < tolerance || inputTolerance > 1e-7) return false;
        double total = 0;
        for (double value : source)
        {
            if (!std::isfinite(value) || value < 0 || value > 1) return false;
            total += value;
        }
        if (total > 1+inputTolerance) return false; // Do not normalize an authored out-of-domain mixture.
        const bool floatBoundaryRoundoff = total > 1 && inputTolerance > tolerance;
        output.fill(0);
        if (total == 0) return true; // Predictor zero, never a nonzero neutral oracle/default.
        const std::array<double, 6> augmented{source[0], source[1], source[2], source[3], source[4], 1.};
        for (size_t cell = 0; cell < profile.cells.size(); ++cell)
        {
            std::array<double, 6> bary{};
            bool inside = true;
            double sum = 0;
            for (size_t row = 0; row < 6; ++row)
            {
                double rowScale = 0;
                for (size_t column = 0; column < 6; ++column)
                {
                    bary[row] += profile.inverses[cell][row][column]*augmented[column];
                    if (column < 5) rowScale += std::fabs(profile.inverses[cell][row][column]);
                }
                // A float32 sum excess is expressed in input-coordinate units,
                // not local barycentric units (small cells amplify it). This
                // exception is confined to the outer sum=1 boundary; interior
                // cell selection keeps the exact portable 1e-10 rule.
                const double baryTolerance = floatBoundaryRoundoff
                    ? inputTolerance*(std::max)(1., rowScale) : tolerance;
                if (!std::isfinite(bary[row]) || bary[row] < -baryTolerance) { inside = false; break; }
                bary[row] = (std::max)(0., bary[row]);
                sum += bary[row];
            }
            if (!inside || !std::isfinite(sum) || sum <= 0) continue;
            std::array<double, 5> reconstructed{};
            output.fill(0);
            for (size_t vertex = 0; vertex < 6; ++vertex)
            {
                const unsigned index = profile.cells[cell][vertex];
                if (index >= profile.outputs.size()) return false;
                const double weight = bary[vertex]/sum;
                for (size_t axis = 0; axis < 5; ++axis) reconstructed[axis] += weight*profile.inputs[index][axis];
                for (size_t channel = 0; channel < 9; ++channel) output[channel] += weight*profile.outputs[index][channel];
            }
            for (size_t axis = 0; axis < 5; ++axis)
                if (std::fabs(reconstructed[axis]-source[axis]) > (std::max)(1e-9, inputTolerance)) return false;
            for (double value : output)
                if (!std::isfinite(value) || value < -tolerance || value > 1+tolerance) return false;
            if (output[7]+output[8] > 1+tolerance) return false;
            return true; // Exact frozen lexicographic first matching cell.
        }
        return false;
    }
    inline MikuFitDecision TryApplyMikuMouthFit(
        const std::vector<std::string>& names, const std::vector<float>& input,
        const std::vector<std::string>& available, const std::string& actualFaceName,
        const std::string& characterOverride, std::vector<float>& adapted)
    {
        if (!HasMikuRawSource(names)) return MikuFitDecision::MissingRawSource;
        std::array<double, 8> raw{};
        for (size_t i = 0; i < raw.size(); ++i)
        {
            const size_t slot = static_cast<size_t>(std::find(names.begin(), names.end(), kMikuRawNames[i])-names.begin());
            if (slot >= input.size() || !std::isfinite(input[slot])) return MikuFitDecision::InvalidRawSource;
            raw[i] = input[slot];
        }
        if (std::fabs(raw[5]-1.) > 1e-10 || raw[6] < 0 || raw[7] < 0) return MikuFitDecision::InvalidRawSource;
        if (raw[7] != 0) return MikuFitDecision::NativeAuthored;
        if (raw[6] != 0) return MikuFitDecision::UnsupportedSource;
        const auto* profile = FindMikuMouthFace(actualFaceName);
        if (!profile) return MikuFitDecision::UnknownFace;
        if (!characterOverride.empty() && characterOverride != "auto" && characterOverride != profile->character)
            return MikuFitDecision::CharacterMismatch;
        std::array<size_t, 9> slots{};
        for (size_t i = 0; i < slots.size(); ++i)
        {
            if (std::count(available.begin(), available.end(), profile->shapes[i]) != 1) return MikuFitDecision::MissingOutput;
            slots[i] = static_cast<size_t>(std::find(available.begin(), available.end(), profile->shapes[i])-available.begin());
        }
        std::array<double, 9> output{};
        // Only this float32 clip bridge permits 1e-7 boundary roundoff. Raw
        // values are unchanged; the portable double core keeps its 1e-10 default.
        if (!EvaluateMikuMouthProfile(*profile, {raw[0], raw[1], raw[2], raw[3], raw[4]}, output, 1e-7))
            return MikuFitDecision::OutsideDomain;
        if (adapted.size() != available.size()) return MikuFitDecision::InvalidModel;
        for (size_t i = 0; i < adapted.size(); ++i)
            if (available[i].rfind("b_mouth.mouth_", 0) == 0) adapted[i] = 0;
        for (size_t i = 0; i < output.size(); ++i)
            adapted[slots[i]] = static_cast<float>(std::clamp(output[i], 0., 1.));
        return MikuFitDecision::Applied;
    }

    // ---- Published profile path --------------------------------------------
    // Everything below only runs when a provider published a table for this
    // face. The compiled per-character tables above stay authoritative
    // otherwise, and their evaluator is untouched.
    struct ExternalFacialFit
    {
        const RinhaFacialFitFace* face{nullptr};
        const RinhaFacialFitService* provider{nullptr};
        std::string character;
        std::vector<int> mouthShapeSlots;  // ownedNames index per fitted mouth shape
        std::vector<int> eyeKeySlots;      // clip slot per fitted eye key
        std::vector<int> eyeChannelSlots;  // ownedNames index per fitted eye channel
        std::vector<float> eyeAmplitudes;  // scratch, never reallocated per frame
        std::vector<float> eyeWeights;     // scratch, never reallocated per frame
        int eyePresentSlot{-1}, eyeUnsupportedSlot{-1}, eyeNativeAuthoredSlot{-1};
        bool mouthPresent{false}, eyesPresent{false};
        bool mouthClaim{false}, eyeClaim{false};
        MikuFitDecision eyeClipDecision{MikuFitDecision::MissingRawEyeSource};
    };

    inline int FindClipSlot(const std::vector<std::string>& names, const std::string& wanted)
    {
        const auto found = std::find(names.begin(), names.end(), wanted);
        return found == names.end() ? -1 : static_cast<int>(found - names.begin());
    }

    inline MikuFitDecision CheckExternalEyeSource(const ExternalFacialFit& fit,
        const std::vector<std::string>& clipNames, std::span<const float> samples, bool allowUnverified)
    {
        if (!HasMikuRawEyeSource(clipNames)) return MikuFitDecision::MissingRawEyeSource;
        const size_t stride = clipNames.size();
        if (samples.empty() || samples.size() % stride != 0) return MikuFitDecision::InvalidRawSource;

        // Importers emit dense curves for unused profile keys. Evaluate actual
        // activity across the clip so a zero column does not block measured keys.
        const auto active = [&](size_t slot) {
            for (size_t i = slot; i < samples.size(); i += stride)
                if (samples[i] != 0.0f) return true;
            return false;
        };
        if (active(fit.eyeNativeAuthoredSlot)) return MikuFitDecision::NativeAuthored;
        if (active(fit.eyeUnsupportedSlot)) return MikuFitDecision::UnsupportedEyeSource;
        for (size_t i = fit.eyePresentSlot; i < samples.size(); i += stride)
            if (samples[i] != 1.0f) return MikuFitDecision::MissingRawEyeSource;

        for (size_t slot = 0; slot < stride; ++slot)
        {
            if (clipNames[slot].rfind(kMikuRawEyePrefix, 0) != 0 ||
                slot == static_cast<size_t>(fit.eyePresentSlot) ||
                slot == static_cast<size_t>(fit.eyeUnsupportedSlot) ||
                slot == static_cast<size_t>(fit.eyeNativeAuthoredSlot)) continue;
            const auto key = std::find(fit.eyeKeySlots.begin(), fit.eyeKeySlots.end(), static_cast<int>(slot));
            if (key == fit.eyeKeySlots.end() && active(slot)) return MikuFitDecision::UnsupportedEyeSource;
            if (key != fit.eyeKeySlots.end() && !allowUnverified &&
                (!fit.face->eyesGrade || !fit.face->eyeKeyGrades || !fit.face->eyeKeyGrades[key - fit.eyeKeySlots.begin()]) &&
                active(slot)) return MikuFitDecision::ExternalUnverified;
        }
        return MikuFitDecision::ExternalApplied;
    }

    // Bind-time resolution only: which clip channel feeds which fitted key, and
    // whether the model can host the fitted outputs at all. `modelNames` is the
    // live face's channel list, not the owned subset, because claiming a family
    // is what puts those channels into the owned subset afterwards.
    inline ExternalFacialFit BindExternalFacialFit(const std::string& faceName,
        const std::vector<std::string>& clipNames, const std::vector<std::string>& modelNames,
        std::span<const float> clipSamples, bool allowUnverified)
    {
        ExternalFacialFit fit;
        const auto match = facial_fit::FindFit(faceName);
        fit.face = match.face;
        fit.provider = match.provider;
        if (!fit.face) return fit;
        fit.character = fit.face->character ? fit.face->character : "";
        fit.mouthPresent = fit.face->mouthPresent != 0 && fit.face->mouthShapeNames != nullptr &&
            fit.face->mouthKnots > 0;
        fit.eyesPresent = fit.face->eyesPresent != 0 && fit.face->eyeKeyNames != nullptr &&
            fit.face->eyeChannelNames != nullptr &&
            fit.face->eyeKeyCount > 0 && fit.face->eyeKeyCount <= RINHA_FACIAL_FIT_MAX_KEYS &&
            fit.face->eyeChannelCount > 0 && fit.face->eyeChannelCount <= RINHA_FACIAL_FIT_MAX_CHANNELS;
        if (fit.mouthPresent)
        {
            bool complete = true;
            for (uint32_t shape = 0; shape < RINHA_FACIAL_FIT_MOUTH_SHAPES; ++shape)
                complete = complete && fit.face->mouthShapeNames[shape] &&
                    FindClipSlot(modelNames, fit.face->mouthShapeNames[shape]) >= 0;
            fit.mouthClaim = complete && HasMikuRawSource(clipNames) &&
                (fit.face->mouthGrade != 0 || allowUnverified);
        }
        if (fit.eyesPresent)
        {
            fit.eyePresentSlot = FindClipSlot(clipNames, kMikuRawEyeMarkers[0]);
            fit.eyeUnsupportedSlot = FindClipSlot(clipNames, kMikuRawEyeMarkers[1]);
            fit.eyeNativeAuthoredSlot = FindClipSlot(clipNames, kMikuRawEyeMarkers[2]);
            fit.eyeKeySlots.assign(fit.face->eyeKeyCount, -1);
            bool hostable = false;
            for (uint32_t key = 0; key < fit.face->eyeKeyCount; ++key)
            {
                if (!fit.face->eyeKeyNames[key]) { fit.eyesPresent = false; return fit; }
                fit.eyeKeySlots[key] = FindClipSlot(clipNames,
                    std::string(kMikuRawEyePrefix) + fit.face->eyeKeyNames[key]);
            }
            for (uint32_t channel = 0; channel < fit.face->eyeChannelCount; ++channel)
            {
                if (!fit.face->eyeChannelNames[channel]) { fit.eyesPresent = false; return fit; }
                hostable = hostable || FindClipSlot(modelNames, fit.face->eyeChannelNames[channel]) >= 0;
            }
            fit.eyeAmplitudes.assign(fit.face->eyeKeyCount, 0.0f);
            fit.eyeWeights.assign(fit.face->eyeChannelCount, 0.0f);
            fit.eyeClipDecision = CheckExternalEyeSource(fit, clipNames, clipSamples, allowUnverified);
            if (!hostable && fit.eyeClipDecision == MikuFitDecision::ExternalApplied)
                fit.eyeClipDecision = MikuFitDecision::MissingOutput;
            fit.eyeClaim = fit.eyeClipDecision == MikuFitDecision::ExternalApplied;
        }
        return fit;
    }

    // Second half of the bind: map fitted outputs onto the owned channel list.
    // Called once the family claim has rebuilt that list.
    inline void ResolveExternalFitSlots(ExternalFacialFit& fit, const std::vector<std::string>& ownedNames)
    {
        if (!fit.face) return;
        fit.mouthShapeSlots.assign(RINHA_FACIAL_FIT_MOUTH_SHAPES, -1);
        if (fit.mouthPresent)
            for (uint32_t shape = 0; shape < RINHA_FACIAL_FIT_MOUTH_SHAPES; ++shape)
                if (fit.face->mouthShapeNames[shape])
                    fit.mouthShapeSlots[shape] = FindClipSlot(ownedNames, fit.face->mouthShapeNames[shape]);
        if (fit.eyesPresent)
        {
            fit.eyeChannelSlots.assign(fit.face->eyeChannelCount, -1);
            for (uint32_t channel = 0; channel < fit.face->eyeChannelCount; ++channel)
                fit.eyeChannelSlots[channel] = FindClipSlot(ownedNames, fit.face->eyeChannelNames[channel]);
        }
    }

    inline MikuFitDecision TryApplyExternalMouthFit(const ExternalFacialFit& fit,
        const std::vector<std::string>& names, const std::vector<float>& input,
        const std::string& characterOverride, bool allowUnverified,
        const std::vector<std::string>& available, std::vector<float>& adapted)
    {
        if (!fit.mouthPresent) return MikuFitDecision::UnknownFace;
        if (!HasMikuRawSource(names)) return MikuFitDecision::MissingRawSource;
        std::array<float, 8> raw{};
        for (size_t i = 0; i < raw.size(); ++i)
        {
            const size_t slot = static_cast<size_t>(std::find(names.begin(), names.end(), kMikuRawNames[i]) - names.begin());
            if (slot >= input.size() || !std::isfinite(input[slot])) return MikuFitDecision::InvalidRawSource;
            raw[i] = input[slot];
        }
        if (std::fabs(raw[5] - 1.0f) > 1e-6f || raw[6] < 0 || raw[7] < 0) return MikuFitDecision::InvalidRawSource;
        if (raw[7] != 0) return MikuFitDecision::NativeAuthored;
        if (raw[6] != 0) return MikuFitDecision::UnsupportedSource;
        if (!characterOverride.empty() && characterOverride != "auto" && characterOverride != fit.character)
            return MikuFitDecision::CharacterMismatch;
        // A profile the bundle itself did not accept never applies by default,
        // even though the compiled path would have accepted a same-shaped one.
        if (fit.face->mouthGrade == 0 && !allowUnverified) return MikuFitDecision::ExternalUnverified;
        if (fit.mouthShapeSlots.size() != RINHA_FACIAL_FIT_MOUTH_SHAPES) return MikuFitDecision::InvalidModel;
        for (int slot : fit.mouthShapeSlots)
            if (slot < 0 || static_cast<size_t>(slot) >= available.size()) return MikuFitDecision::MissingOutput;
        if (adapted.size() != available.size()) return MikuFitDecision::InvalidModel;
        const float vowels[5]{raw[0], raw[1], raw[2], raw[3], raw[4]};
        std::array<float, RINHA_FACIAL_FIT_MOUTH_SHAPES> weights{};
        const auto status = fit.provider->evaluateMouth(fit.provider->context, fit.face,
            vowels, weights.data());
        if (status == GAKU_E_LIMIT) return MikuFitDecision::OutsideDomain;
        if (status != GAKU_OK) return MikuFitDecision::InvalidModel;
        for (size_t i = 0; i < adapted.size(); ++i)
            if (available[i].rfind("b_mouth.mouth_", 0) == 0) adapted[i] = 0;
        for (size_t shape = 0; shape < weights.size(); ++shape)
            adapted[static_cast<size_t>(fit.mouthShapeSlots[shape])] = std::clamp(weights[shape], 0.0f, 1.0f);
        return MikuFitDecision::Applied;
    }

    // Replaces the whole eye/brow family with the fitted weights, so omitted
    // siblings cannot keep the photography pose's eyes. Every refusal happens
    // before the first write, leaving the caller's adapted vector untouched.
    inline MikuFitDecision TryApplyExternalEyeFit(ExternalFacialFit& fit,
        const std::vector<std::string>& names, const std::vector<float>& input,
        const std::string& characterOverride,
        const std::vector<std::string>& available, std::vector<float>& adapted)
    {
        if (!fit.eyesPresent) return MikuFitDecision::UnknownFace;
        if (fit.eyePresentSlot < 0 || !HasMikuRawEyeSource(names)) return MikuFitDecision::MissingRawEyeSource;
        auto sample = [&](int slot)
        {
            return slot >= 0 && static_cast<size_t>(slot) < input.size() && std::isfinite(input[slot])
                ? input[slot] : 0.0f;
        };
        if (sample(fit.eyePresentSlot) < 0.5f) return MikuFitDecision::MissingRawEyeSource;
        if (sample(fit.eyeNativeAuthoredSlot) != 0.0f) return MikuFitDecision::NativeAuthored;
        if (sample(fit.eyeUnsupportedSlot) != 0.0f) return MikuFitDecision::UnsupportedEyeSource;
        if (!characterOverride.empty() && characterOverride != "auto" && characterOverride != fit.character)
            return MikuFitDecision::CharacterMismatch;
        if (fit.eyeClipDecision != MikuFitDecision::ExternalApplied) return fit.eyeClipDecision;
        for (int slot : fit.eyeChannelSlots)
            if (slot >= 0 && static_cast<size_t>(slot) >= available.size()) return MikuFitDecision::MissingOutput;
        if (adapted.size() != available.size()) return MikuFitDecision::InvalidModel;
        for (size_t key = 0; key < fit.eyeAmplitudes.size(); ++key)
            fit.eyeAmplitudes[key] = std::clamp(sample(fit.eyeKeySlots[key]), 0.0f, 1.0f);
        const auto status = fit.provider->evaluateEyes(fit.provider->context, fit.face,
            fit.eyeAmplitudes.data(), fit.eyeWeights.data());
        if (status != GAKU_OK) return MikuFitDecision::InvalidModel;
        for (size_t i = 0; i < adapted.size(); ++i)
            if (IsEyeFamilyName(available[i])) adapted[i] = 0;
        for (size_t channel = 0; channel < fit.eyeChannelSlots.size(); ++channel)
        {
            const int slot = fit.eyeChannelSlots[channel];
            if (slot >= 0) adapted[static_cast<size_t>(slot)] = std::clamp(fit.eyeWeights[channel], 0.0f, 1.0f);
        }
        return MikuFitDecision::ExternalApplied;
    }
}
