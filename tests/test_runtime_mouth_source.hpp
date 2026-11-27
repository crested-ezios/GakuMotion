#pragma once

namespace runtime_mouth_test
{
    inline const char* Inputs[]{"a", "i", "u", "e", "o", "∧", "口角上げ"};
    inline const char* Outputs[]{"b_mouth.mouth_019", "b_mouth.mouth_025", "b_mouth.mouth_010",
        "b_mouth.mouth_027", "b_mouth.mouth_023", "b_mouth.mouth_001", "b_mouth.mouth_004",
        "b_mouth.mouth_024", "b_mouth.mouth_026", "b_mouth.mouth_028"};
    inline const GakuMouthFitFace Face{"mdl_chr_krnh-base-0000_face", "krnh", 7, 10, 27, 0,
        Inputs, Outputs, "synthetic-table-sha"};
    inline int evaluations{};
    inline const GakuMouthFitFace* Find(void*, const char* name)
    {
        return std::strcmp(name, Face.faceName) == 0 ? &Face : nullptr;
    }
    inline GakuStatus Evaluate(void*, const GakuMouthFitFace* face, const float* input,
        uint32_t inputCount, float* output, uint32_t outputCount)
    {
        if (face != &Face || inputCount != 7 || outputCount != 10) return GAKU_E_INVALID_ARGUMENT;
        ++evaluations;
        std::fill_n(output, outputCount, 0.0f);
        output[0] = input[0];
        output[8] = input[5] * .1f + input[6] * .2f;
        output[9] = .15f;
        return GAKU_OK;
    }
}

static void TestRuntimeMouthSource()
{
    g_current = "runtime VMD mouth source and published seven-input fit";
    MotionClip clip;
    clip.fps = 30;
    clip.total_frames = 2;
    clip.blendshape_names = {"mmd2.a", "b_mouth.mouth_028", "b_eye.eye_005"};
    clip.blendshape_weights = {.6f, .7f, .4f, .2f, .9f, .1f};
    clip.root_positions = {{1, 2, 3}, {4, 5, 6}};
    const auto before = clip;
    std::vector<float> source(2 * mouth_source::Names.size());
    for (size_t frame = 0; frame < 2; ++frame)
    {
        source[frame * mouth_source::Names.size()] = .3f;
        source[frame * mouth_source::Names.size() + 5] = 1;
        source[frame * mouth_source::Names.size() + 8] = .8f;
        source[frame * mouth_source::Names.size() + 9] = .8f;
        source[frame * mouth_source::Names.size() + 13] = .04f;
        source[frame * mouth_source::Names.size() + 14] = .5f;
        source[frame * mouth_source::Names.size() + 15] = 1;
    }
    Check(mouth_source::Append(clip, source), "source curves append to a legacy clip in memory");
    bool originalsPreserved = clip.root_positions.size() == before.root_positions.size() &&
        std::memcmp(clip.root_positions.data(), before.root_positions.data(), clip.root_positions.size() * sizeof(Vector3)) == 0;
    for (size_t frame = 0; frame < 2; ++frame)
        for (size_t channel = 0; channel < before.blendshape_names.size(); ++channel)
            originalsPreserved = originalsPreserved && clip.blendshape_weights[frame * clip.blendshape_names.size() + channel] ==
                before.blendshape_weights[frame * before.blendshape_names.size() + channel];
    Check(originalsPreserved, "original body and every native/semantic curve are preserved byte for byte");
    Check(mouth_source::HasFamily(clip, 0, 8) && mouth_source::HasFamily(clip, 8, 10),
        "legacy raw8 and seven-input source contracts are both supplied");
    const auto once = clip.blendshape_weights;
    Check(mouth_source::Append(clip, source) && clip.blendshape_weights == once,
        "repeated source restore is idempotent");
    source[0] = std::numeric_limits<float>::quiet_NaN();
    Check(!mouth_source::Append(clip, source) && clip.blendshape_weights == once,
        "invalid source response never partially mutates the loaded clip");
    auto authoredClip = before;
    authoredClip.blendshape_names.emplace_back("miku.raw.v1.native_authored");
    authoredClip.blendshape_weights = {.6f, .7f, .4f, 1, .2f, .9f, .1f, 1};
    source[0] = .3f;
    Check(mouth_source::Append(authoredClip, source) &&
        authoredClip.blendshape_weights[FindClipSlot(authoredClip.blendshape_names, "miku.rawmouth.v1.native_authored")] == 1,
        "an existing native-authored marker carries into the newly restored mouth contract");
    MotionClip eyesOnly;
    eyesOnly.total_frames = 2;
    eyesOnly.blendshape_names = {"b_eye.eye_005"};
    eyesOnly.blendshape_weights = {.4f, .1f};
    const auto eyesBefore = eyesOnly;
    Check(mouth_source::Append(eyesOnly, std::vector<float>(2 * mouth_source::Names.size())) &&
        eyesOnly.blendshape_names == eyesBefore.blendshape_names && eyesOnly.blendshape_weights == eyesBefore.blendshape_weights,
        "eyes-only source never acquires raw mouth columns or mouth-family ownership");

    const auto previousService = facial_fit::mouthService;
    const auto previousHost = facial_fit::host;
    facial_fit::host = {};
    facial_fit::mouthService = {GAKU_MOUTH_FIT_VERSION, sizeof(GakuMouthFitService), nullptr,
        runtime_mouth_test::Find, runtime_mouth_test::Evaluate};
    std::vector<std::string> available(std::begin(runtime_mouth_test::Outputs), std::end(runtime_mouth_test::Outputs));
    available.emplace_back("b_mouth.mouth_031");
    available.emplace_back("b_eye.eye_005");
    auto fit = BindPublishedMouthFit("mdl_chr_krnh-base-0000_face(Clone)", clip.blendshape_names, available);
    ResolvePublishedMouthSlots(fit, available);
    Check(fit.claim && fit.face->qualityGrade == 0, "published experimental mouth table owns the family without a quality veto");
    std::vector<float> samples;
    clip.SampleBlendshapes(0, samples);
    std::vector<float> adapted(available.size(), .9f);
    auto decision = TryApplyPublishedMouthFit(fit, samples, "auto", available, adapted);
    Check(decision == MikuFitDecision::ExternalApplied && runtime_mouth_test::evaluations > 0,
        "valid seven-channel mixtures evaluate even when vowel sum exceeds one");
    Check(Near(adapted[0], .8f) && Near(adapted[8], .104f) && Near(adapted[9], .15f) &&
        adapted[10] == 0 && adapted[11] == .9f,
        "table output replaces the entire mouth family and preserves eye curves");
    const auto applied = adapted;
    samples[static_cast<size_t>(fit.unsupportedSlot)] = .1f;
    Check(TryApplyPublishedMouthFit(fit, samples, "auto", available, adapted) == MikuFitDecision::UnsupportedSource && adapted == applied,
        "unsupported authored mouth intent has an explicit decision before any write");
    {
        const auto priorNames = clip.blendshape_names;
        const auto priorWeights = clip.blendshape_weights;
        auto softClip = clip;
        Check(mouth_source::AppendExtras(softClip, {1, 0, .2f, 0, 1, .18f, .015f, 0}) &&
            mouth_source::HasExtras(softClip) &&
            std::equal(priorNames.begin(), priorNames.end(), softClip.blendshape_names.begin()) &&
            std::equal(priorWeights.begin(), priorWeights.begin() + priorNames.size(), softClip.blendshape_weights.begin()),
            "independent corner gesture samples append without changing earlier columns");
        auto softFit = BindPublishedMouthFit(runtime_mouth_test::Face.faceName, softClip.blendshape_names, available);
        ResolvePublishedMouthSlots(softFit, available);
        for (int frame = 0; frame < 2; ++frame)
        {
            softClip.SampleBlendshapes(frame / 30.f, samples);
            samples[softFit.inputSlots[0]] = 1.f;
            samples[softFit.inputSlots[1]] = 0.f;
            samples[softFit.unsupportedSlot] = frame == 0 ? .2f : .18f;
            adapted.assign(available.size(), .92f);
            Check(TryApplyPublishedMouthFit(softFit, samples, "auto", available, adapted) == MikuFitDecision::ExternalSoftA &&
                adapted[0] > 0 && adapted[8] == 0 && adapted[9] == 0 && adapted[11] == .92f,
                "both recognized A-like gestures produce a lightly open mouth with dental channels cleared");
            const auto reject = [&](int slot, float replacement, MikuFitDecision expected, const char* message) {
                const float prior = samples[slot];
                samples[slot] = replacement;
                adapted.assign(available.size(), .92f);
                Check(TryApplyPublishedMouthFit(softFit, samples, "auto", available, adapted) == expected &&
                    std::all_of(adapted.begin(), adapted.end(), [](float value) { return value == .92f; }), message);
                samples[slot] = prior;
            };
            reject(softFit.extraSlots[mouth_source::ExtraUnsupported], .05f, MikuFitDecision::UnsupportedSource,
                "an independent unknown input still prevents the reference correction");
            reject(softFit.unsupportedSlot, .9f, MikuFitDecision::UnsupportedSource,
                "independent inputs must account for the legacy unsupported marker");
            reject(softFit.inputSlots[1], .30f, MikuFitDecision::UnsupportedSource,
                "correction leaves dental vowel mixtures alone");
            reject(softFit.nativeAuthoredSlot, 1.f, MikuFitDecision::NativeAuthored,
                "native-authored mouth controls still win with the independent gesture source");
        }
        clip.SampleBlendshapes(0, samples);
        adapted = applied;
    }
    {
        using namespace mouth_source;
        auto v2Clip = clip;
        std::vector<float> semantics(2 * V2Names.size());
        semantics[V2Present] = semantics[V2Names.size() + V2Present] = 1.f;
        semantics[V2I] = .48f;
        semantics[V2Smile] = .5f;
        Check(AppendNamedFamily(v2Clip, semantics, V2Names, V2Present), "complete v2 family appends without requiring v1 changes");
        const auto kept = v2Clip.blendshape_weights;
        Check(AppendNamedFamily(v2Clip, semantics, V2Names, V2Present) && v2Clip.blendshape_weights == kept,
            "embedded v2 family survives a repeated restore");
        auto withSmile = available;
        withSmile.emplace_back("b_mouth.mouth_002");
        auto v2 = BindPublishedMouthFit(runtime_mouth_test::Face.faceName, v2Clip.blendshape_names, withSmile);
        ResolvePublishedMouthSlots(v2, withSmile);
        v2Clip.SampleBlendshapes(0, samples);
        samples[fit.unsupportedSlot] = 1.f;
        adapted.assign(withSmile.size(), .92f);
        Check(v2.useV2 && TryApplyPublishedMouthFit(v2, samples, "auto", withSmile, adapted) == MikuFitDecision::ExternalMouthV2 &&
            Near(adapted[9], .15f) && Near(adapted.back(), .075f) && Near(adapted[11], .92f),
            "known smile and I use their independently fitted outputs despite the old aggregate marker");
        samples[v2.v2Slots[V2Unsupported]] = .15f;
        const auto last = adapted;
        Check(TryApplyPublishedMouthFit(v2, samples, "auto", withSmile, adapted) == MikuFitDecision::UnsupportedSource && adapted == last,
            "an unknown v2 action still preserves fallback");
        samples[v2.v2Slots[V2Unsupported]] = 0.f;
        samples[v2.oldAuthoredSlot] = 1.f;
        Check(TryApplyPublishedMouthFit(v2, samples, "auto", withSmile, adapted) == MikuFitDecision::NativeAuthored && adapted == last,
            "an embedded author's native intent prevails over recovered v2 input");
        samples[v2.oldAuthoredSlot] = 0.f;
        samples[v2.v2Slots[V2I]] = 0.f;
        samples[v2.v2Slots[V2Smile]] = 0.f;
        samples[v2.v2Slots[V2A]] = 1.f;
        samples[v2.v2Slots[V2CornerDown]] = .2f;
        Check(TryApplyPublishedMouthFit(v2, samples, "auto", withSmile, adapted) == MikuFitDecision::ExternalSoftA &&
            Near(adapted[0], .28f) && adapted[8] == 0 && adapted[9] == 0 && adapted.back() == 0,
            "the reference no-teeth A result survives switching to v2");
        samples[v2.v2Slots[V2CornerDown]] = 0.f;
        samples[v2.v2Slots[V2A]] = 0.f;
        samples[v2.v2Slots[V2WideOpen]] = .8f;
        Check(TryApplyPublishedMouthFit(v2, samples, "auto", withSmile, adapted) == MikuFitDecision::ExternalMouthV2 &&
            Near(adapted[0], .60f), "a wide-open action uses the fitted A family in the absence of a vowel");
        clip.SampleBlendshapes(0, samples);
        adapted = applied;
    }
    samples[static_cast<size_t>(fit.unsupportedSlot)] = 0;
    samples[static_cast<size_t>(fit.nativeAuthoredSlot)] = 1;
    Check(TryApplyPublishedMouthFit(fit, samples, "auto", available, adapted) == MikuFitDecision::NativeAuthored && adapted == applied,
        "explicit native mouth authoring retains its existing precedence");
    Check(!BindPublishedMouthFit("mdl_chr_amao-base-0000_face", clip.blendshape_names, available).face,
        "the krnh provider cannot replace a compiled built-in role");
    auto incomplete = available;
    incomplete.erase(incomplete.begin() + 3);
    Check(!BindPublishedMouthFit(runtime_mouth_test::Face.faceName, clip.blendshape_names, incomplete).claim,
        "a missing live native output is diagnosed before family ownership");
    facial_fit::mouthService = previousService;
    facial_fit::host = previousHost;
}
