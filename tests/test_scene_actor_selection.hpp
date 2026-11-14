static void TestSceneActorSelection()
{
	BeginTest("scene actor roster and target selection lifetime");
	Check(DetectIdolCharacterCode("MDL_CHR_KRNH-base-0000_face(Clone)") == "krnh" &&
		FormatIdolDisplayName("krnh").find("有栖川凛花") != std::string::npos,
		"replacement identity and Chinese naming do not depend on a facial baseline existing");
	Check(DetectIdolCharacterCode("xkrnhx").empty() && DetectIdolCharacterCode("chr_krnh_ttmr").empty(),
		"unknown substrings and ambiguous character codes are not assigned a character name");
	Core core;
	GakuMotionActorSnapshot snapshot{};
	snapshot.actorToken = 0xffff;
	Check(core.ReadSelectedActor(sizeof(snapshot), &snapshot) == GAKU_E_UNAVAILABLE && snapshot.actorToken == 0xffff,
		"an inactive actor service does not change the caller's output");
	core.SetControlActive(true);
	Check(core.ReadSelectedActor(sizeof(snapshot), &snapshot) == GAKU_OK && snapshot.actorToken == 0 &&
		snapshot.abiVersion == GAKU_MOTION_ACTOR_VERSION, "the selected actor service represents an empty scene");
	void* a = reinterpret_cast<void*>(0x7100);
	void* b = reinterpret_cast<void*>(0x7200);
	core.PublishSceneActors({{a, 101, "hski"}, {b, 102, "ttmr"}, {a, 101, "hski"}});
	auto context = core.BuildGuiContext();
	Check(context.sceneActors.size() == 2 && core.player.controller.targetActor == a,
		"fresh actual actors are deduplicated and the first actor is selected automatically");
	Check(context.sceneActors[0].label.find("花海咲季") != std::string::npos && context.sceneActors[1].label.find("月村手毬") != std::string::npos,
		"scene entries use the existing character-name table");
	const auto token = context.sceneActors[1].token;
	const auto epoch = context.actorRosterEpoch;
	core.PublishSceneActors({{b, 102, "ttmr"}, {a, 101, "hski"}});
	Check(core.sceneActorRegistry.Epoch() == epoch && core.sceneActorRegistry.Resolve(token, epoch) == b,
		"Unity search order changes preserve stable choices");
	core.player.state = PlaybackState::Paused;
	core.player.currentTime = .45f;
	core.player.controller.bindings[a].actor = a;
	core.player.controller.currentBinding.actor = a;
	core.fnBeforeTargetChange = [] { return false; };
	context.fnSelectTargetActor(context.self, token, epoch);
	core.ProcessPendingGuiCommands();
	Check(core.player.controller.targetActor == a && !core.player.controller.bindings.empty(),
		"unconfirmed secondary physics restoration rejects switching without discarding ownership");
	core.fnBeforeTargetChange = nullptr;
	context.fnSelectTargetActor(context.self, token, epoch);
	Check(core.player.controller.targetActor == a, "GUI selection waits for the main-thread queue");
	core.ProcessPendingGuiCommands();
	Check(core.player.controller.targetActor == b && core.player.controller.bindings.empty() &&
		!core.player.controller.currentBinding.actor && core.player.currentTime == .45f,
		"switching releases the previous body/face binding while keeping the playback position");
	context.fnSelectTargetActor(context.self, context.sceneActors[0].token, epoch);
	core.PublishSceneActors({{b, 102, "ttmr"}});
	core.ProcessPendingGuiCommands();
	Check(core.player.controller.targetActor == b && core.KnownActorCount() == 1,
		"a removed actor disappears and a queued old-roster choice is rejected");
	const auto old = core.BuildGuiContext();
	core.PublishSceneActors({{b, 202, "hmsz"}}); // Same native address, new Unity instance.
	Check(core.player.state == PlaybackState::Stopped && core.sceneActorRegistry.Resolve(old.sceneActors[0].token, old.actorRosterEpoch) == nullptr,
		"pointer reuse is a new actor and cannot inherit the old playback or token");
	core.PublishSceneActors({});
	Check(core.BuildGuiContext().sceneActors.empty() && !core.player.controller.targetActor && !core.IsTargetActor(a),
		"an empty scene offers no entries and never means drive all actors");
	core.PublishSceneActors({{a, 301, ""}, {b, 302, ""}});
	const auto priorScene = core.BuildGuiContext();
	core.AbandonScene();
	core.PublishSceneActors({{a, 301, "npc0", "NPC", "mdl_chr_npc0-base-0000_face"}});
	Check(!core.sceneActorRegistry.Resolve(priorScene.sceneActors[0].token, priorScene.actorRosterEpoch),
		"scene boundaries invalidate old commands even if addresses and instance IDs are reused");
	Check(core.ReadSelectedActor(sizeof(snapshot), &snapshot) == GAKU_OK &&
		snapshot.actorToken == core.sceneActorRegistry.Find(a)->token && snapshot.rosterEpoch != priorScene.actorRosterEpoch &&
		std::string_view(snapshot.character) == "npc0" && std::string_view(snapshot.displayName) == "NPC" &&
		std::string_view(snapshot.faceName) == "mdl_chr_npc0-base-0000_face",
		"the selected actor API exposes the current adapter identity and roster revision");
}
