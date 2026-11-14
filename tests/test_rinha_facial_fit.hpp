#pragma once

// Consumer-side checks for the published rinha.facial-fit profile: bind-time
// claims, clip-level gating, family replacement and refusal semantics. A fake
// provider is injected into facial_fit::service, so nothing here needs the real
// provider plugin to be loaded.

static void TestRinhaFacialFit()
{
    g_current = "Published facial fit profile (rinha.facial-fit)";

    struct FakeProvider
    {
        std::vector<std::string> shapes, keys, channels;
        std::vector<uint8_t> grades;
        std::array<float, RINHA_FACIAL_FIT_EYE_AMPLITUDES> amplitudes{0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
        std::vector<const char*> shapePointers, keyPointers, channelPointers;
        RinhaFacialFitFace face{};

        void Build(std::vector<std::string> mouthShapes, std::vector<std::string> eyeKeys,
                   std::vector<std::string> eyeChannels, std::vector<uint8_t> eyeGrades,
                   uint32_t mouthGrade, uint32_t eyesGrade)
        {
            shapes = std::move(mouthShapes);
            keys = std::move(eyeKeys);
            channels = std::move(eyeChannels);
            grades = std::move(eyeGrades);
            shapePointers.clear();
            for (const auto& name : shapes) shapePointers.push_back(name.c_str());
            keyPointers.clear();
            for (const auto& name : keys) keyPointers.push_back(name.c_str());
            channelPointers.clear();
            for (const auto& name : channels) channelPointers.push_back(name.c_str());
            face = {};
            face.faceName = "mdl_chr_krnh-base-0000_face";
            face.character = "krnh";
            face.mouthPresent = shapes.empty() ? 0u : 1u;
            face.eyesPresent = keys.empty() ? 0u : 1u;
            face.mouthGrade = mouthGrade;
            face.eyesGrade = eyesGrade;
            face.mouthKnots = 2;
            face.eyeKeyCount = static_cast<uint32_t>(keys.size());
            face.eyeChannelCount = static_cast<uint32_t>(channels.size());
            face.mouthShapeNames = shapePointers.empty() ? nullptr : shapePointers.data();
            face.eyeKeyNames = keyPointers.empty() ? nullptr : keyPointers.data();
            face.eyeChannelNames = channelPointers.empty() ? nullptr : channelPointers.data();
            face.eyeKeyGrades = grades.empty() ? nullptr : grades.data();
            face.eyeKeyAmplitudes = amplitudes.data();
        }
    };
    static FakeProvider fake;
    // Flip to simulate the provider plugin registering after this one activated.
    static bool providerUp = false;

    // The published krnh mouth shape order is part of the table, not a guess:
    // the provider names them and the consumer must host every one of them.
    const std::vector<std::string> mouthShapes{
        "b_mouth.mouth_019", "b_mouth.mouth_025", "b_mouth.mouth_010", "b_mouth.mouth_027",
        "b_mouth.mouth_023", "b_mouth.mouth_001", "b_mouth.mouth_004", "b_mouth.mouth_026",
        "b_mouth.mouth_028"};
    // Grade 1 for 真面目 (measured), grade 0 for 困る (unverified).
    fake.Build(mouthShapes, {"真面目", "困る"}, {"b_eye.eye_001", "b_brow.brow_003"}, {1, 0}, 0, 1);

    facial_fit::service = {};
    facial_fit::service.abiVersion = GAKU_SERVICE_VERSION;
    facial_fit::service.structSize = sizeof(RinhaFacialFitService);
    facial_fit::service.context = &fake;
    facial_fit::service.faceCount = [](void*) -> uint32_t { return 1; };
    facial_fit::service.faceAt = [](void*, uint32_t index) -> const RinhaFacialFitFace* {
        return index == 0 ? &fake.face : nullptr; };
    facial_fit::service.findFace = [](void*, const char* name) -> const RinhaFacialFitFace* {
        return name && std::string(name) == fake.face.faceName ? &fake.face : nullptr; };
    // Mouth: channel 0 tracks the first vowel; a vowel above 1 is out of domain.
    facial_fit::service.evaluateMouth = [](void*, const RinhaFacialFitFace* face,
        const float* vowels, float* out) -> GakuStatus {
        if (!face || !vowels || !out) return GAKU_E_INVALID_ARGUMENT;
        for (uint32_t index = 0; index < 5; ++index) if (vowels[index] > 1.0f) return GAKU_E_LIMIT;
        for (uint32_t channel = 0; channel < RINHA_FACIAL_FIT_MOUTH_SHAPES; ++channel) out[channel] = 0.0f;
        out[0] = vowels[0];
        return GAKU_OK;
    };
    // Eyes: key k feeds channel k one-to-one; a 0.75f amplitude is the
    // provider's own refusal case.
    facial_fit::service.evaluateEyes = [](void*, const RinhaFacialFitFace* face,
        const float* amplitudes, float* out) -> GakuStatus {
        if (!face || !amplitudes || !out) return GAKU_E_INVALID_ARGUMENT;
        for (uint32_t key = 0; key < face->eyeKeyCount; ++key)
            if (!(amplitudes[key] >= 0.0f && amplitudes[key] <= 1.0f)) return GAKU_E_INVALID_ARGUMENT;
        for (uint32_t channel = 0; channel < face->eyeChannelCount; ++channel) out[channel] = 0.0f;
        for (uint32_t key = 0; key < face->eyeKeyCount && key < face->eyeChannelCount; ++key)
        {
            if (amplitudes[key] == 0.75f) return GAKU_E_INTERNAL;
            out[key] = amplitudes[key];
        }
        return GAKU_OK;
    };
    static const RinhaFacialFitService published = facial_fit::service;
    static bool adapterUp = false;
    static const RinhaFacialFitFace adapterFace = [] {
        auto face = fake.face;
        face.faceName = "mdl_chr_npc0-base-0000_face";
        face.character = "npc0";
        return face;
    }();
    static const RinhaFacialFitService publishedAdapterFit = [] {
        auto fit = published;
        fit.findFace = [](void*, const char* name) -> const RinhaFacialFitFace* {
            return name && std::string_view(name) == adapterFace.faceName ? &adapterFace : nullptr;
        };
        return fit;
    }();
    facial_fit::available = true;
    facial_fit::generation = 1;

    Check(facial_fit::Find("mdl_chr_krnh-base-0000_face") == &fake.face &&
        facial_fit::Find("mdl_chr_krnh-base-0000_face(Clone)") == &fake.face,
        "the published face resolves by authored identity and its Unity clone suffix");
    Check(facial_fit::Find("mdl_chr_hski-base-0000_face") == nullptr,
        "a different face never resolves to the published profile");

    const std::string faceName = "mdl_chr_krnh-base-0000_face";
    const std::vector<std::string> clipNames{
        "b_eye.eye_001", "b_brow.brow_003",
        "miku.raweye.v1.真面目", "miku.raweye.v1.困る",
        "miku.raweye.v1.present", "miku.raweye.v1.unsupported", "miku.raweye.v1.native_authored"};
    const std::vector<std::string> acceptedOnly{
        "b_eye.eye_001", "b_brow.brow_003", "miku.raweye.v1.真面目",
        "miku.raweye.v1.present", "miku.raweye.v1.unsupported", "miku.raweye.v1.native_authored"};
    const std::vector<std::string> modelNames{
        "b_eye.eye_001", "b_brow.brow_003", "b_eye.eye_999", "b_mouth.mouth_019"};
    const std::vector<float> clipWeights{0.9f, 0.1f, 0.5f, 0.25f, 1.0f, 0.0f, 0.0f};
    const std::vector<float> acceptedWeights{0.9f, 0.1f, 0.5f, 1.0f, 0.0f, 0.0f};

    auto strict = BindExternalFacialFit(faceName, clipNames, modelNames, clipWeights, false);
    Check(strict.face == &fake.face && strict.eyesPresent && strict.eyePresentSlot == 4,
        "binding resolves the published profile and its raw-eye markers");
    Check(strict.eyeClipDecision == MikuFitDecision::ExternalUnverified && !strict.eyeClaim,
        "an unverified key driven by the clip keeps the eye family unclaimed by default");
    Check(!strict.mouthClaim, "an unaccepted mouth map is not claimed by default");

    auto accepted = BindExternalFacialFit(faceName, acceptedOnly, modelNames, acceptedWeights, false);
    Check(accepted.eyeClaim, "a clip that only drives measured keys claims the eye family");
    auto allowed = BindExternalFacialFit(faceName, clipNames, modelNames, clipWeights, true);
    Check(allowed.eyeClaim, "allowing unverified components claims the eye family with any key");
    auto bare = BindExternalFacialFit(faceName, {"b_eye.eye_001"}, modelNames, {}, true);
    Check(bare.face == &fake.face && !bare.eyeClaim && !bare.mouthClaim,
        "a legacy clip without raw eye source never claims a family");

    auto ownedNames = modelNames;
    ResolveExternalFitSlots(allowed, ownedNames);
    Check(allowed.eyeChannelSlots == std::vector<int>({0, 1}),
        "fitted channels resolve onto the owned list");

    std::vector<float> adapted(ownedNames.size(), 0.4f);
    Check(TryApplyExternalEyeFit(allowed, clipNames, clipWeights, "auto", ownedNames, adapted) ==
        MikuFitDecision::ExternalApplied, "a fully verified eye evaluation applies");
    Check(Near(adapted[0], 0.5f) && Near(adapted[1], 0.25f),
        "fitted eye weights land on their native channels");
    Check(adapted[2] == 0.0f,
        "an owned eye-family channel outside the profile is cleared, not left on the old pose");
    Check(adapted[3] == 0.4f, "the mouth family is untouched by the eye fit");

    auto refusal = [&](std::vector<float> weights, const std::string& override,
                       const ExternalFacialFit& fit, const std::vector<std::string>& clip,
                       MikuFitDecision expected, const char* reason)
    {
        std::vector<float> buffer(ownedNames.size(), 0.4f);
        const std::vector<float> before = buffer;
        const auto decision = TryApplyExternalEyeFit(const_cast<ExternalFacialFit&>(fit), clip, weights,
            override, ownedNames, buffer);
        Check(decision == expected && buffer == before, reason);
    };
    auto authored = clipWeights; authored[6] = 1.0f;
    refusal(authored, "auto", allowed, clipNames, MikuFitDecision::NativeAuthored,
        "a VMD that authors native eye channels keeps precedence over the fit");
    auto unsupported = clipWeights; unsupported[5] = 0.5f;
    refusal(unsupported, "auto", allowed, clipNames, MikuFitDecision::UnsupportedEyeSource,
        "an unsupported eye source refuses the whole clip instead of a partial face");
    auto absent = clipWeights; absent[4] = 0.0f;
    refusal(absent, "auto", allowed, clipNames, MikuFitDecision::MissingRawEyeSource,
        "a present flag of zero is treated as a legacy clip, not as a zero face");
    refusal(clipWeights, "auto", strict, clipNames, MikuFitDecision::ExternalUnverified,
        "an unverified key refuses the clip unless unverified components are allowed");
    refusal(clipWeights, "ttmr", allowed, clipNames, MikuFitDecision::CharacterMismatch,
        "a character override that disagrees with the profile refuses the fit");
    auto providerRefusal = clipWeights; providerRefusal[2] = 0.75f;
    refusal(providerRefusal, "auto", allowed, clipNames, MikuFitDecision::InvalidModel,
        "a provider refusal surfaces as an invalid model, never as a silent zero");
    refusal(clipWeights, "auto", ExternalFacialFit{}, clipNames, MikuFitDecision::UnknownFace,
        "an unresolved profile reports an unmatched face");

    // ---- published mouth map ------------------------------------------------
    std::vector<std::string> mouthClip(std::begin(kMikuRawNames), std::end(kMikuRawNames));
    std::vector<std::string> mouthModel = mouthShapes;
    mouthModel.push_back("b_eye.eye_001");
    auto mouthStrict = BindExternalFacialFit(faceName, mouthClip, mouthModel, {}, false);
    Check(!mouthStrict.mouthClaim, "the unaccepted krnh mouth map is not claimed by default");
    auto mouthAllowed = BindExternalFacialFit(faceName, mouthClip, mouthModel, {}, true);
    Check(mouthAllowed.mouthClaim, "allowing unverified components claims the mouth family");
    auto mouthOwned = mouthModel;
    ResolveExternalFitSlots(mouthAllowed, mouthOwned);
    std::vector<float> mouthWeights{0.25f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    std::vector<float> mouthAdapted(mouthOwned.size(), 0.3f);
    Check(TryApplyExternalMouthFit(mouthAllowed, mouthClip, mouthWeights, "auto", true, mouthOwned, mouthAdapted) ==
        MikuFitDecision::Applied, "an allowed published mouth map applies");
    Check(Near(mouthAdapted[0], 0.25f) && mouthAdapted[1] == 0.0f,
        "the published mouth map writes its own shapes and clears the rest of the family");
    Check(mouthAdapted.back() == 0.3f, "the eye channel is untouched by the mouth fit");
    auto mouthReject = [&](std::vector<float> weights, bool allowUnverified, MikuFitDecision expected,
                           const char* reason)
    {
        std::vector<float> buffer(mouthOwned.size(), 0.3f);
        const std::vector<float> before = buffer;
        const auto decision = TryApplyExternalMouthFit(mouthAllowed, mouthClip, weights, "auto",
            allowUnverified, mouthOwned, buffer);
        Check(decision == expected && buffer == before, reason);
    };
    mouthReject(mouthWeights, false, MikuFitDecision::ExternalUnverified,
        "the published mouth grade gate refuses the unverified map by default");
    auto mouthAuthored = mouthWeights; mouthAuthored[7] = 1.0f;
    mouthReject(mouthAuthored, true, MikuFitDecision::NativeAuthored,
        "authored native mouth keeps precedence over the published map");
    auto mouthUnsupported = mouthWeights; mouthUnsupported[6] = 0.1f;
    mouthReject(mouthUnsupported, true, MikuFitDecision::UnsupportedSource,
        "an unsupported source mouth refuses the published map");
    auto mouthOutside = mouthWeights; mouthOutside[1] = 2.0f;
    mouthReject(mouthOutside, true, MikuFitDecision::OutsideDomain,
        "a source outside the fitted domain is refused instead of extrapolated");

    // ---- end-to-end through the real binding --------------------------------
    struct FaceState
    {
        std::vector<std::string> names;
        std::vector<float> weights;
        std::string faceName;
    };
    static FaceState state;
    struct FitReport
    {
        std::vector<std::string> components, decisions;
        void Clear() { components.clear(); decisions.clear(); }
    };
    static FitReport report;
    state.names = {"b_eye.eye_001", "b_brow.brow_003", "b_eye.eye_999", "b_mouth.mouth_019"};
    state.weights.assign(state.names.size(), 17.0f);
    state.faceName = faceName;
    report.Clear();
    FaceApi api;
    api.getModel = [](void*) -> void* { return &state; };
    api.getCount = [](void*) { return static_cast<int>(state.names.size()); };
    api.getName = [](void*, int slot) { return state.names[slot]; };
    api.getWeight = [](void*, int slot) { return state.weights[slot]; };
    api.setWeight = [](void*, int slot, float value) { state.weights[slot] = value; };
    api.getActorName = [](void*) { return std::string("chr_ttmr"); };
    api.getModelName = [](void*) { return state.faceName; };
    api.reportFacialFit = [](const char* component, const char* decision, const char*)
    {
        report.components.push_back(component ? component : "");
        report.decisions.push_back(decision ? decision : "");
    };

    MotionClip clip;
    clip.fps = 1;
    clip.total_frames = 2;
    // The importer emits unverified key names even when their weights stay zero.
    // There is no mapped brow track, so fitting must claim that family itself.
    clip.blendshape_names = {"b_eye.eye_001", "miku.raweye.v1.真面目", "miku.raweye.v1.困る",
        "miku.raweye.v1.present", "miku.raweye.v1.unsupported", "miku.raweye.v1.native_authored"};
    clip.blendshape_weights = {0.9f, 0.5f, 0.0f, 1.0f, 0.0f, 0.0f,
                               0.9f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    BlendshapeBinding binding;
    binding.enableMikuEyeFit = true;
    void* actor = reinterpret_cast<void*>(0x52494e48);
    binding.Apply(actor, clip, 0, api);
    Check(binding.mikuEyeDecision == MikuFitDecision::ExternalApplied,
        "a dense zero track for an unverified key does not block the eye fit");
    Check(binding.fitRole == static_cast<unsigned>(kMikuMouthFits.size()) + 1,
        "the published profile reports its own role, outside the compiled table");
    Check(Near(state.weights[0], 50.0f) && state.weights[1] == 0.0f && state.weights[2] == 0.0f,
        "the live face receives fitted percentage weights and loses unlisted eye channels");
    Check(state.weights[3] == 17.0f,
        "a channel outside the claimed family is never written by the eye fit");
    Check(report.components.size() == 1 && report.components[0] == "eyes" &&
        report.decisions[0] == "applied_published_profile",
        "the host is told which component applied the published profile, once");
    binding.Apply(actor, clip, 0, api);
    Check(Near(state.weights[0], 50.0f) && report.components.size() == 1,
        "repeated evaluation is idempotent and does not repeat the notice");
    binding.enableMikuEyeFit = false;
    binding.Apply(actor, clip, 0, api);
    Check(binding.mikuEyeDecision == MikuFitDecision::Disabled && !binding.boundMikuEyeFit,
        "switching the eye profile off rebuilds the binding instead of keeping fitted weights");
    Check(Near(state.weights[0], 90.0f) && state.weights[1] == 17.0f &&
        state.weights[2] == 0.0f && state.weights[3] == 17.0f,
        "with the profile off, native eye mapping returns and unowned channels stay untouched");
    MotionClip lateUnverified = clip;
    lateUnverified.blendshape_weights[8] = 0.25f;
    binding.enableMikuEyeFit = true;
    binding.Apply(actor, clip, 0, api);
    binding.Apply(actor, lateUnverified, 0, api);
    Check(binding.mikuEyeDecision == MikuFitDecision::ExternalUnverified && Near(state.weights[0], 90.0f) &&
        state.weights[1] == 17.0f,
        "switching to the same channels with later unverified activity restores the brow and falls back for the entire clip");
    MotionClip lateUnsupported = clip;
    lateUnsupported.blendshape_weights[10] = 0.3f;
    binding.Apply(actor, lateUnsupported, 0, api);
    Check(binding.mikuEyeDecision == MikuFitDecision::UnsupportedEyeSource && Near(state.weights[0], 90.0f),
        "later unsupported source activity blocks the fit from the first frame");
    binding.Restore(actor, api);
    Check(std::all_of(state.weights.begin(), state.weights.end(), [](float value) { return value == 17.0f; }),
        "restoring returns the authored face to its captured weights");

    // ---- provider registering after this plugin activated --------------------
    // GakuLoader activates plugins in sorted filename order and
    // "GakuMotion.dll" sorts before "RinhaReplacement.dll", so a single
    // activation-time lookup would never see the provider on a real install.
    facial_fit::available = false;
    facial_fit::service = {};
    facial_fit::generation = 0;
    facial_fit::host = {};
    facial_fit::host.queryService = [](void*, const char* name, uint32_t version, uint32_t size, void* table) -> GakuStatus {
        if (adapterUp && name && std::string_view(name) == GAKU_CHARA_ADAPTER_SERVICE)
        {
            if (version != GAKU_CHARA_ADAPTER_VERSION) return GAKU_E_VERSION;
            if (size < sizeof(GakuCharaAdapterService)) return GAKU_E_SIZE;
            *static_cast<GakuCharaAdapterService*>(table) = {GAKU_CHARA_ADAPTER_VERSION,
                sizeof(GakuCharaAdapterService), nullptr,
                [](void*, const char* face, GakuCharaIdentity* identity) -> GakuStatus {
                    if (std::string_view(face) != adapterFace.faceName) return GAKU_E_UNAVAILABLE;
                    *identity = {"npc0", "NPC"};
                    return GAKU_OK;
                },
                [](void*, RinhaFacialFitService* fit) -> GakuStatus {
                    *fit = publishedAdapterFit;
                    return GAKU_OK;
                }};
            return GAKU_OK;
        }
        if (!providerUp || !name || std::string(name) != RINHA_FACIAL_FIT_SERVICE) return GAKU_E_UNAVAILABLE;
        if (version != GAKU_SERVICE_VERSION) return GAKU_E_VERSION;
        if (size < sizeof(RinhaFacialFitService)) return GAKU_E_SIZE;
        *static_cast<RinhaFacialFitService*>(table) = published;
        return GAKU_OK;
    };
    facial_fit::nextAttempt = {};
    Check(facial_fit::Find(faceName) == nullptr && !facial_fit::available &&
        facial_fit::generation == 0,
        "a provider that has not registered yet keeps the compiled path in charge");
    providerUp = true;
    facial_fit::nextAttempt = {}; // skip the retry interval, as a later frame would
    Check(facial_fit::Find(faceName) == &fake.face && facial_fit::available &&
        facial_fit::generation == 1,
        "the lookup is retried once the provider registers, and forces a rebind");
    adapterUp = true;
    facial_fit::nextAttempt = {};
    const auto identity = facial_fit::ResolveIdentity(std::string(adapterFace.faceName) + " (Clone)");
    Check(std::string_view(identity.character) == "npc0" && std::string_view(identity.displayName) == "NPC" &&
        facial_fit::generation == 2, "late CharaAdapter registration adds an identity outside the built-in table");
    const auto adapterMatch = facial_fit::FindFit(adapterFace.faceName);
    Check(adapterMatch.face == &adapterFace && adapterMatch.provider == &facial_fit::adapterFit,
        "a profile is paired with its CharaAdapter evaluator");
    Check(facial_fit::FindFit(faceName).provider == &facial_fit::service &&
        !facial_fit::ResolveIdentity(faceName).character[0], "legacy fit remains available for other faces");
    facial_fit::available = false;
    facial_fit::nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    Check(facial_fit::Find(faceName) == nullptr && facial_fit::generation == 2,
        "the retry is throttled instead of running a registry lookup every frame");

    facial_fit::available = false;
    facial_fit::service = {};
    facial_fit::adapter = {};
    facial_fit::adapterFit = {};
    facial_fit::host = {};
    facial_fit::nextAttempt = {};
}
