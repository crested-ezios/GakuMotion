#pragma once

static void TestFaceFamilyOwnership()
{
	g_current = "photography expression family replacement";
	struct Face
	{
		std::vector<std::string> names;
		std::vector<float> weights;
	};
	static Face a, b;
	static Face* live;
	a = {
		{
			"b_mouth.mouth_020", "b_mouth.mouth_002", "b_mouth.mouth_101",
			"b_brow.brow_003", "b_eye.eye_001", "b_iris.iris_001", "under045", "custom_Geo_Mouth_0"
		},
		{5, 65, 14, 12, 9, 22, 31, 44}
	};
	b = {{"b_mouth.mouth_002", "b_mouth.mouth_020"}, {17, 23}};
	live = &a;
	FaceApi api{
		[](void*) -> void* { return live; },
		[](void* p) { return static_cast<int>(static_cast<Face*>(p)->names.size()); },
		[](void* p, int i) { return static_cast<Face*>(p)->names[i]; },
		[](void* p, int i) { return static_cast<Face*>(p)->weights[i]; },
		[](void* p, int i, float w) { static_cast<Face*>(p)->weights[i] = w; }
	};
	MotionClip clip;
	clip.total_frames = 2;
	clip.fps = 30;
	clip.blendshape_names = {"b_mouth.mouth_020"};
	clip.blendshape_weights = {.4f, .8f};
	auto actor = reinterpret_cast<void*>(0xface);
	BlendshapeBinding binding;
	binding.Observe();
	binding.Apply(actor, clip, 0, api);
	Check(Near(a.weights[0], 40) && Near(a.weights[1], 0) && Near(a.weights[2], 0),
	      "mouth curves clear unlisted smile and asymmetric pose channels");
	Check(Near(a.weights[3], 12) && Near(a.weights[4], 9) && Near(a.weights[5], 22) &&
	      Near(a.weights[6], 31) && Near(a.weights[7], 44),
	      "mouth replacement preserves eyes, brows, iris and corrective mesh channels");
	Check(binding.diagnosticChannels.size() == 3 && binding.diagnosticChannels[1].target == 0 &&
	      Near(binding.diagnosticChannels[1].before, 65) && Near(binding.diagnosticChannels[1].after, 0),
	      "observer reports zeroed default channels with actual before and after values");
	binding.facialWeights.overall = 0.5f;
	binding.facialWeights.mouth = 0.5f;
	binding.Apply(actor, clip, 0, api);
	Check(Near(a.weights[0], 10) && Near(a.weights[3], 12) && Near(a.weights[4], 9),
	      "overall and mouth controls multiply only the owned mouth output");
	binding.facialWeights.overall = 2.0f;
	binding.facialWeights.mouth = 2.0f;
	binding.Apply(actor, clip, 0, api);
	Check(Near(a.weights[0], 100), "amplified facial weights clamp to 100 percent");
	binding.facialWeights = FacialWeights{};
	a.weights[1] = 97;
	a.weights[2] = 35; // A looping pose / talk updates every frame.
	binding.Apply(actor, clip, 1.f / 60, api);
	Check(Near(a.weights[0], 60) && Near(a.weights[1], 0) && Near(a.weights[2], 0),
	      "dynamic native smile is replaced each callback without additive accumulation");
	binding.Apply(actor, clip, 1.f / 60, api);
	Check(binding.diagnosticChannels.size() == 3 && binding.ownedIndices.size() == 3,
	      "repeated paused writes do not append ownership or diagnostic channels");
	binding.Restore(actor, api);
	Check(Near(a.weights[0], 5) && Near(a.weights[1], 65) && Near(a.weights[2], 14),
	      "Stop restores every captured native mouth weight including omitted siblings");
	MotionClip unknown = clip;
	unknown.blendshape_names = {"b_mouth.unknown"};
	binding.Apply(actor, unknown, 0, api);
	Check(binding.ownedIndices.empty() && Near(a.weights[1], 65),
	      "unmatched family-like names cannot clear native expression families");
	binding.Restore(actor, api);
	clip.blendshape_names.push_back("b_brow.brow_003");
	clip.blendshape_weights = {.4f, .1f, .8f, .3f};
	binding.Apply(actor, clip, 0, api);
	Check(Near(a.weights[3], 10) && Near(a.weights[4], 9),
	      "family ownership follows actual matched curves rather than claiming all facial controls");
	binding.facialWeights.brow = 0.5f;
	binding.Apply(actor, clip, 0, api);
	Check(Near(a.weights[3], 5) && Near(a.weights[0], 40) && Near(a.weights[4], 9),
	      "brow control scales brow without modifying mouth or unowned eyes");
	binding.facialWeights.eye = 0.5f;
	Check(Near(binding.facialWeights.Apply("b_eye.eye_001", 0.8f), 0.4f) &&
	      Near(binding.facialWeights.Apply("b_iris.iris_001", 0.8f), 0.8f),
	      "eye control scales eye morphs but leaves iris channels untouched");
	binding.facialWeights = FacialWeights{};
	live = &b;
	binding.Apply(actor, clip, 0, api);
	Check(Near(a.weights[1], 0) && Near(b.weights[0], 0) && Near(b.weights[1], 40),
	      "model replacement clears new indices and never writes the retired model");
	MotionClip legacy;
	binding.Apply(actor, legacy, 0, api);
	Check(Near(b.weights[0], 17) && Near(b.weights[1], 23) && binding.ownedIndices.empty(),
	      "legacy clip restores all default mouth siblings and releases ownership");
	live = &a;
	a.weights[4] = 82.f;
	binding.Apply(actor, legacy, 0, api, true);
	Check(Near(a.weights[4], 0) && Near(a.weights[5], 22),
		"Motion without eye tracks during Animator takeover opens the eyelids and preserves iris weights");
	Check(BlendshapeBinding::IdleBlinkWeight(3.57f) > .5f && BlendshapeBinding::IdleBlinkWeight(3.71f) == 0.f,
		"the idle blink closes and reopens on the wall-clock schedule");
	binding.Restore(actor, api);
	Check(Near(a.weights[4], 82), "exiting takeover restores the original eyelid weight");
	MotionClip authored = clip;
	authored.blendshape_names = {"b_eye.eye_001"};
	authored.blendshape_weights = {.3f, .8f};
	binding.Apply(actor, authored, 0, api, true);
	Check(Near(a.weights[4], 30) && binding.idleBlinkIndex < 0, "authored eyelids override standby blinking");
	binding.Restore(actor, api);
}

static void TestCharacterDefaultPriorPlayback()
{
	g_current = "multi-character facial adapter default prior";
	struct Face
	{
		std::vector<std::string> names;
		std::vector<float> weights;
	};
	static Face saki, temari, kotone, tsubame;
	static Face* live = nullptr;
	static std::string liveActorName;

	FaceApi api{
		[](void*) -> void* { return live; },
		[](void* p) { return static_cast<int>(static_cast<Face*>(p)->names.size()); },
		[](void* p, int i) { return static_cast<Face*>(p)->names[i]; },
		[](void* p, int i) { return static_cast<Face*>(p)->weights[i]; },
		[](void* p, int i, float w) { static_cast<Face*>(p)->weights[i] = w; },
		nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
		[](void*) -> std::string { return liveActorName; }
	};

	// 1. Saki (hski): default mouth_002 = 1.0 (100%)
	saki = {{"b_mouth.mouth_020", "b_mouth.mouth_002", "b_mouth.mouth_101"}, {0, 0, 0}};
	live = &saki;
	liveActorName = "chr_hski_001";
	auto actor = reinterpret_cast<void*>(0x1111);

	MotionClip clipRest;
	clipRest.total_frames = 2;
	clipRest.fps = 30;
	clipRest.blendshape_names = {"b_mouth.mouth_020"};
	clipRest.blendshape_weights = {0.0f, 0.0f};

	BlendshapeBinding binding;
	binding.Observe();
	binding.Apply(actor, clipRest, 0, api);
	Check(Near(saki.weights[0], 0) && Near(saki.weights[1], 100) && Near(saki.weights[2], 0),
	      "Saki at rest applies authentic 100% mouth_002 baseline while clearing unlisted mouth_101");

	MotionClip clipSing;
	clipSing.total_frames = 2;
	clipSing.fps = 30;
	clipSing.blendshape_names = {"b_mouth.mouth_020"};
	clipSing.blendshape_weights = {0.4f, 1.0f}; // 40% at t=0, 100% at t=1/30

	binding.Apply(actor, clipSing, 0, api);
	Check(Near(saki.weights[0], 40) && Near(saki.weights[1], 0),
	      "Saki at 40% vowel completely yields mouth_002 instead of keeping a 60% smile");

	binding.Apply(actor, clipSing, 1.0f / 30, api);
	Check(Near(saki.weights[0], 100) && Near(saki.weights[1], 0),
	      "Saki at 100% vowel keeps mouth_002 at zero");

	// Simultaneous explicit vowels do not add a baseline.
	MotionClip clipSaturated = clipRest;
	clipSaturated.blendshape_names = {"b_mouth.mouth_019", "b_mouth.mouth_020"};
	clipSaturated.blendshape_weights = {0.8f, 0.5f, 0.8f, 0.5f}; // sum = 1.3
	binding.Apply(actor, clipSaturated, 0, api);
	Check(Near(saki.weights[1], 0), "Saki overlapping vowels do not add a character smile");

	// Config multiplier
	binding.baselineMultiplier = 0.5f;
	binding.Apply(actor, clipRest, 0, api);
	Check(Near(saki.weights[1], 50), "baselineMultiplier 0.5 scales resting mouth_002 to 50%");
	binding.baselineMultiplier = 1.0f;

	// Config disable
	binding.enableBaseline = false;
	binding.Apply(actor, clipRest, 0, api);
	Check(Near(saki.weights[1], 0), "enableBaseline false reverts to strict zeroing");
	binding.enableBaseline = true;

	// Authored curve takes precedence
	MotionClip clipAuthored = clipRest;
	clipAuthored.blendshape_names = {"b_mouth.mouth_002", "b_mouth.mouth_020"};
	clipAuthored.blendshape_weights = {0.85f, 0.4f, 0.85f, 0.4f};
	binding.Apply(actor, clipAuthored, 0, api);
	Check(Near(saki.weights[1], 85), "explicit native mouth_002 stays authored");
	binding.Restore(actor, api);

	// 2. Temari (ttmr): default mouth_014 = 0.7 (70%)
	temari = {{"b_mouth.mouth_020", "b_mouth.mouth_014"}, {0, 0}};
	live = &temari;
	liveActorName = "mdl_chr_ttmr";
	BlendshapeBinding temariBinding;
	temariBinding.Apply(actor, clipRest, 0, api);
	Check(Near(temari.weights[1], 70), "Temari at rest applies 70% mouth_014 baseline");

	temariBinding.Apply(actor, clipSing, 0, api);
	Check(Near(temari.weights[0], 40) && Near(temari.weights[1], 0),
	      "Temari explicit vowel fully yields the character mouth_014 prior");
	temariBinding.Restore(actor, api);

	// 3. Kotone (fktn): default mouth_016 = 1.0 (100%)
	kotone = {{"b_mouth.mouth_020", "b_mouth.mouth_016"}, {0, 0}};
	live = &kotone;
	liveActorName = "chr_fktn_stage";
	BlendshapeBinding kotoneBinding;
	kotoneBinding.Apply(actor, clipRest, 0, api);
	Check(Near(kotone.weights[1], 100), "Kotone at rest applies 100% mouth_016 baseline");
	kotoneBinding.Restore(actor, api);

	// 4. Tsubame (atbm): default brow_003 = 1.0, brow_014 = 0.7, mouth_014 = 0.7
	tsubame = {
		{"b_brow.brow_001", "b_brow.brow_003", "b_brow.brow_014", "b_mouth.mouth_020", "b_mouth.mouth_014"},
		{0, 0, 0, 0, 0}
	};
	live = &tsubame;
	liveActorName = "actor_atbm";
	MotionClip clipTsubame;
	clipTsubame.total_frames = 2;
	clipTsubame.fps = 30;
	clipTsubame.blendshape_names = {"b_brow.brow_001", "b_mouth.mouth_020"};
	clipTsubame.blendshape_weights = {0.3f, 0.0f, 0.3f, 0.0f}; // brow_001 = 0.3, mouth = 0

	BlendshapeBinding tsubameBinding;
	tsubameBinding.Apply(actor, clipTsubame, 0, api);
	Check(Near(tsubame.weights[0], 30) && Near(tsubame.weights[1], 0) && Near(tsubame.weights[2], 0),
	      "explicit brow expression yields all character brow priors without proportional leftovers");
	Check(Near(tsubame.weights[4], 70), "Tsubame mouth_014 stays at 70% when mouth is at rest");
	tsubameBinding.Restore(actor, api);

	// 5. Character override test
	live = &saki;
	liveActorName = "unknown_actor";
	BlendshapeBinding overrideBinding;
	overrideBinding.characterOverride = "hski";
	overrideBinding.Apply(actor, clipRest, 0, api);
	Check(Near(saki.weights[1], 100), "characterOverride 'hski' forces Saki baseline on unidentifiable actor");
	overrideBinding.Restore(actor, api);

	// 6. Clip containing zero-track for mouth_002 (like VMD MMD dummy tracks)
	live = &saki;
	liveActorName = "chr_hski_stage";
	MotionClip clipZeroTrack = clipRest;
	clipZeroTrack.blendshape_names = {"b_mouth.mouth_002", "b_mouth.mouth_020"};
	clipZeroTrack.blendshape_weights = {0.0f, 0.0f, 0.0f, 0.0f}; // mouth_002 is present in clip but 0
	BlendshapeBinding zeroTrackBinding;
	zeroTrackBinding.Apply(actor, clipZeroTrack, 0, api);
	Check(Near(saki.weights[1], 100),
	      "zero-track mouth_002 in clip still lifts to authentic 100% baseline when mouth is at rest");
	zeroTrackBinding.Restore(actor, api);
}

static void TestFacialSemanticBindingV2()
{
	g_current = "v2 MMD semantic binding, default fallback, seek and restoration";
	struct State
	{
		std::vector<std::string> names{
			"b_mouth.mouth_002", "b_mouth.mouth_024", "b_mouth.mouth_019", "b_mouth.mouth_016",
			"b_eye.eye_001", "b_eye.eye_002", "b_eye.eye_003", "b_brow.brow_003", "b_brow.brow_014",
			"b_iris.iris_001", "under045", "custom_Geo_Mouth_0"};
		std::vector<float> weights{23, 0, 0, 0, 13, 4, 5, 7, 8, 33, 44, 55};
		bool blink{true}, disabled{false};
	};
	static State state; state = State{};
	FaceApi api;
	api.getModel = [](void*) -> void* { return &state; };
	api.getCount = [](void*) { return static_cast<int>(state.names.size()); };
	api.getName = [](void*, int i) { return state.names[i]; };
	api.getWeight = [](void*, int i) { return state.weights[i]; };
	api.setWeight = [](void*, int i, float w) { state.weights[i] = w; };
	api.getActorName = [](void*) { return std::string("chr_hski"); };
	api.getFacial = [](void*) -> void* { return &state; };
	api.readBlink = [](void*, bool& blink, bool& disabled) { blink = state.blink; disabled = state.disabled; return true; };
	api.writeBlink = [](void*, bool blink, bool disabled) { state.blink = blink; state.disabled = disabled; return true; };
	MotionClip clip;
	clip.total_frames = 2; clip.fps = 30;
	clip.blendshape_names = {"mmd2.a", "mmd2.smile_eyes", "b_brow.brow_003"};
	clip.blendshape_weights = {.55f, .3f, .4f, 0, 0, 0};
	BlendshapeBinding binding;
	auto actor = reinterpret_cast<void*>(0xfac2);
	binding.Apply(actor, clip, 0, api);
	Check(Near(state.weights[1], 55) && Near(state.weights[0], 0) && Near(state.weights[2], 0),
	      "semantic A resolves to observed mouth024 and completely yields Saki default");
	Check(Near(state.weights[6], 30) && Near(state.weights[5], 0) && !state.blink && state.disabled,
	      "semantic smile uses eye003, owns eye family and suppresses procedural blink");
	Check(Near(state.weights[9], 33) && Near(state.weights[10], 44) && Near(state.weights[11], 55),
	      "semantic ownership preserves iris, correction and custom channels");
	binding.Apply(actor, clip, 1.f / 30, api);
	Check(Near(state.weights[0], 100) && Near(state.weights[1], 0), "quiet semantic state returns character default");
	binding.Apply(actor, clip, 0, api);
	const auto seek = state.weights;
	binding.Apply(actor, clip, 0, api);
	Check(seek == state.weights && Near(state.weights[1], 55), "backward seek and repeated paused samples are deterministic");
	binding.characterOverride = "fktn";
	binding.Apply(actor, clip, 1.f / 30, api);
	Check(Near(state.weights[3], 100) && Near(state.weights[0], 0) && binding.detectedCharacter == "fktn",
	      "same semantic clip rebinds the character default when override changes");
	binding.Restore(actor, api);
	Check(Near(state.weights[0], 23) && Near(state.weights[4], 13) && state.blink && !state.disabled,
	      "Stop restores original native values and procedural blink after semantic playback");
	binding.characterOverride = "auto";
	api.getModelName = [](void*) { return std::string("mdl_chr_fktn-base-0000_face"); };
	binding.Apply(actor, clip, 1.f / 30, api);
	Check(binding.detectedCharacter == "fktn" && Near(state.weights[3], 100),
	      "automatic facial identity follows the loaded face before the original actor name");
	binding.Restore(actor, api);
	api.getModelName = [](void*) { return std::string("mdl_chr_krnh-base-0000_face"); };
	binding.Apply(actor, clip, 1.f / 30, api);
	Check(binding.detectedCharacter == "krnh" && !binding.activeProfile,
	      "a recognized replacement without a baseline never borrows the original actor's baseline");
	binding.Restore(actor, api);
	const std::vector<std::string> available{"b_mouth.mouth_002", "b_mouth.mouth_024", "b_mouth.mouth_019"};
	auto native = EvaluateFacialAdapter({"mmd2.a", "b_mouth.mouth_019"}, {.55f, .2f}, available, FindCharacterBaseline("hski"));
	Check(Near(native[1], 0) && Near(native[2], .2f), "explicit native variant takes precedence over inferred semantic variant");
	const auto fallback = EvaluateFacialAdapter({"mmd2.a"}, {.55f}, {"b_mouth.mouth_019"}, nullptr);
	Check(Near(fallback[0], .55f), "missing preferred target uses another available member of its semantic group");
}

// Exercise the real player/controller path: native Animator operations may
// change facial state both before the first custom write and during Stop.
static void TestFaceAnimatorCaptureLifecycle()
{
	BeginTest("facial capture before Animator takeover and restore after Rebind");
	struct NativeState
	{
		bool useBlink{true}, autoDisabled{false}, enabled{true};
		void* controller{reinterpret_cast<void*>(0xface700)};
		std::vector<std::string> names{"b_eye.eye_001", "b_eye.eye_002", "b_mouth.mouth_019"};
		std::vector<float> weights{26, 41, 63};
		int animatorWrites{0}, rebinds{0}, blinkReads{0}, blinkWrites{0}, shapeWrites{0};
		bool firstReadBeforeAnimator{false};
	};
	static NativeState native;
	native = NativeState{};
	auto actor = reinterpret_cast<void*>(0xface710);
	auto animator = reinterpret_cast<void*>(0xface720);
	auto wire = [&](MotionPlayer& player)
	{
		WireControllerCallbacks(player.controller);
		player.clip = MakeSmallClip();
		player.clip.duration_seconds = 10;
		player.clip.blendshape_names = {"b_eye.eye_001"};
		player.clip.blendshape_weights = {.2f, .8f};
		player.controller.SetTargetActor(actor);
		auto& api = player.controller.faceApi;
		api.getModel = [](void*) -> void* { return &native; };
		api.getCount = [](void*) { return static_cast<int>(native.names.size()); };
		api.getName = [](void*, int i) { return native.names[i]; };
		api.getWeight = [](void*, int i) { return native.weights[i]; };
		api.setWeight = [](void*, int i, float weight)
		{
			native.weights[i] = weight;
			++native.shapeWrites;
		};
		api.getFacial = [](void*) -> void* { return &native; };
		api.readBlink = [](void*, bool& use, bool& disabled)
		{
			if (native.blinkReads++ == 0)
				native.firstReadBeforeAnimator = native.animatorWrites == 0 && native.rebinds == 0;
			use = native.useBlink;
			disabled = native.autoDisabled;
			return true;
		};
		api.writeBlink = [](void*, bool use, bool disabled)
		{
			native.useBlink = use;
			native.autoDisabled = disabled;
			++native.blinkWrites;
			return true;
		};
		player.controller.fnGetAnimatorController = [](void*) { return native.controller; };
		player.controller.fnGetAnimatorEnabled = [](void*) { return native.enabled; };
		player.controller.fnSetAnimatorController = [](void*, void* controller)
		{
			native.controller = controller;
			++native.animatorWrites;
			native.useBlink = false;
			native.autoDisabled = true;
			native.weights[0] = 97;
			native.weights[1] = 56;
		};
		player.controller.fnSetAnimatorEnabled = [](void*, bool enabled)
		{
			native.enabled = enabled;
			++native.animatorWrites;
			native.useBlink = false;
			native.autoDisabled = true;
		};
		player.controller.fnAnimatorRebind = [](void*)
		{
			++native.rebinds;
			native.useBlink = false;
			native.autoDisabled = true;
			native.weights[0] = 94;
			native.weights[1] = 57;
		};
	};

	MotionPlayer legacy;
	wire(legacy);
	legacy.controller.useAnimatorRebind = true;
	legacy.Play();
	legacy.Tick(1.f / 60, actor, animator);
	Check(native.firstReadBeforeAnimator,
	      "first real Tick captures blink before controller detach, disable and bind-time Rebind");
	Check(!native.enabled && native.controller == nullptr && native.rebinds == 1,
	      "legacy fixture performs actual controller takeover and bind-time Rebind");
	Check(!native.useBlink && native.autoDisabled && Near(native.weights[0], 50) && Near(native.weights[1], 0),
	      "first Tick applies sampled eyes after native Animator side effects");
	Check(legacy.controller.bindings.at(actor).face.savedUseBlink &&
	      !legacy.controller.bindings.at(actor).face.savedAutoDisabled,
	      "retained baseline is the original 1/0 pair rather than takeover's 0/1");
	legacy.Pause();
	int reads = native.blinkReads;
	legacy.Tick(1.f / 60, actor, animator);
	Check(legacy.state == PlaybackState::Paused && native.blinkReads == reads &&
	      !native.useBlink && native.autoDisabled,
	      "paused Tick keeps ownership without recapturing suppressed blink flags");
	legacy.Stop();
	Check(native.enabled && native.controller == reinterpret_cast<void*>(0xface700) && native.rebinds == 2,
	      "Stop restores Animator controller and enabled state with one restoration Rebind");
	Check(native.useBlink && !native.autoDisabled,
	      "Play Pause Stop restores original blink after Rebind and enabled setters overwrite it");
	Check(Near(native.weights[0], 26) && Near(native.weights[1], 41) && Near(native.weights[2], 63),
	      "Stop restores pre-takeover eye weights after native Rebind overwrites the face");
	int blinkWrites = native.blinkWrites, shapeWrites = native.shapeWrites;
	int animatorWrites = native.animatorWrites, rebinds = native.rebinds;
	legacy.Stop();
	Check(native.blinkWrites == blinkWrites && native.shapeWrites == shapeWrites &&
	      native.animatorWrites == animatorWrites && native.rebinds == rebinds,
	      "repeated Stop performs no extra face or Animator writes");

	// Stop retains the bone binding. A second takeover must prepare face state
	// on the fast path and capture the game's new configuration, not the first.
	native.useBlink = false;
	native.autoDisabled = false;
	native.weights[0] = 17;
	native.weights[1] = 39;
	legacy.Play();
	legacy.Tick(1.f / 60, actor, animator);
	Check(!legacy.controller.bindings.at(actor).face.savedUseBlink &&
	      !legacy.controller.bindings.at(actor).face.savedAutoDisabled && native.rebinds == rebinds,
	      "second Play prepares a fresh 0/0 blink baseline through retained bone binding");
	legacy.Pause();
	legacy.Tick(1.f / 60, actor, animator);
	legacy.Stop();
	Check(!native.useBlink && !native.autoDisabled && Near(native.weights[0], 17) && Near(native.weights[1], 39),
	      "second Play Pause Stop preserves new native flags and weights without stale first-play snapshot");
	Check(native.rebinds == rebinds + 1,
	      "retained-binding second playback rebinds only when returning the Animator on Stop");

	native = NativeState{};
	MotionPlayer preserve;
	wire(preserve);
	preserve.controller.preserveAnimatorForSecondaryPhysics = true;
	preserve.controller.useAnimatorRebind = true;
	preserve.Play();
	preserve.Tick(1.f / 60, actor, animator);
	Check(native.firstReadBeforeAnimator && native.animatorWrites == 0 && native.rebinds == 0 && native.enabled,
	      "preserve graph captures facial baseline without mutating or rebinding Animator");
	Check(!native.useBlink && native.autoDisabled && Near(native.weights[1], 0),
	      "preserve graph still replaces native eyes and suppresses automatic blink");
	preserve.Pause();
	preserve.Tick(1.f / 60, actor, animator);
	preserve.Stop();
	Check(native.useBlink && !native.autoDisabled && Near(native.weights[0], 26) && Near(native.weights[1], 41),
	      "preserve graph Stop restores face even without captured Animator state");
	Check(native.animatorWrites == 0 && native.rebinds == 0,
	      "face release does not rebind the untouched preserve graph");

	native = NativeState{};
	native.autoDisabled = true;
	native.enabled = false;
	MotionPlayer disabled;
	wire(disabled);
	disabled.Play();
	disabled.Tick(1.f / 60, actor, animator);
	disabled.Stop();
	Check(native.useBlink && native.autoDisabled,
	      "originally disabled automatic blink retains native 1/1 configuration after Animator restoration");
	Check(!native.enabled && native.controller == reinterpret_cast<void*>(0xface700),
	      "facial release does not force a game-disabled Animator on");

	native = NativeState{};
	MotionPlayer mouthOnly;
	wire(mouthOnly);
	mouthOnly.controller.preserveAnimatorForSecondaryPhysics = true;
	mouthOnly.clip.blendshape_names = {"b_mouth.mouth_019"};
	mouthOnly.Play();
	mouthOnly.Tick(1.f / 60, actor, animator);
	mouthOnly.Stop();
	Check(native.blinkReads == 0 && native.blinkWrites == 0 && native.useBlink && !native.autoDisabled,
	      "mouth-only prepare and Stop never acquire or rewrite native blink configuration");
	native = NativeState{};
	MotionPlayer unmatched;
	wire(unmatched);
	unmatched.controller.preserveAnimatorForSecondaryPhysics = true;
	unmatched.clip.blendshape_names = {"b_eye.eye_missing"};
	unmatched.Play();
	unmatched.Tick(1.f / 60, actor, animator);
	unmatched.Stop();
	Check(native.blinkReads == 0 && native.blinkWrites == 0 && native.shapeWrites == 0,
	      "unmatched eye curve cannot acquire blink merely because prepare runs before Animator takeover");
}
