#pragma once

#include "GakuMotion.hpp"
#include <array>
#include <algorithm>
#include <fstream>

namespace EnsembleRuntimeTest
{
    using namespace GakuMotion;
    struct Actor;
    struct Transform
    {
        Actor* owner{};
        Transform* parent{};
        Vector3 localPosition{};
        Quaternion localRotation{Quaternion::Identity()};
    };
    struct Animator { Actor* owner{}; bool enabled{true}; void* controller{}; };
    struct Actor
    {
        Transform root, hips;
        Animator animator;
        bool alive{true};
        int reads{0}, writes{0}, invalidTouches{0};
    };
    inline int audioPlays{}, audioPauses{}, audioStops{}, audioResumes{}, audioSeeks{};
    inline std::string audioPath;
    inline void Touch(Actor* actor, bool write = false)
    {
        if (!actor) return;
        if (write) ++actor->writes; else ++actor->reads;
        if (!actor->alive) ++actor->invalidTouches;
    }
    inline Quaternion Rotation(Transform* transform)
    {
        return transform->parent ? Rotation(transform->parent) * transform->localRotation : transform->localRotation;
    }
    inline Vector3 Position(Transform* transform)
    {
        return transform->parent ? Position(transform->parent) + Rotation(transform->parent).Rotate(transform->localPosition) : transform->localPosition;
    }
    inline void GetPosition(void* pointer, float* output)
    {
        auto* transform = static_cast<Transform*>(pointer); Touch(transform->owner);
        const auto value = Position(transform); output[0] = value.x; output[1] = value.y; output[2] = value.z;
    }
    inline void GetRotation(void* pointer, float* output)
    {
        auto* transform = static_cast<Transform*>(pointer); Touch(transform->owner);
        const auto value = Rotation(transform); output[0] = value.x; output[1] = value.y; output[2] = value.z; output[3] = value.w;
    }
    inline void SetPosition(void* pointer, float x, float y, float z)
    {
        auto* transform = static_cast<Transform*>(pointer); Touch(transform->owner, true);
        const Vector3 value{x, y, z};
        transform->localPosition = transform->parent ? Rotation(transform->parent).Inverse().Rotate(value - Position(transform->parent)) : value;
    }
    inline void SetRotation(void* pointer, float x, float y, float z, float w)
    {
        auto* transform = static_cast<Transform*>(pointer); Touch(transform->owner, true);
        const Quaternion value{x, y, z, w};
        transform->localRotation = transform->parent ? Rotation(transform->parent).Inverse() * value : value;
    }
    inline bool EnsembleNear(float actual, float expected) { return std::fabs(actual - expected) < 1e-4f; }
    inline bool EnsembleNear(const Vector3& actual, const Vector3& expected)
    { return EnsembleNear(actual.x, expected.x) && EnsembleNear(actual.y, expected.y) && EnsembleNear(actual.z, expected.z); }
    inline void Wire(IdolController& controller)
    {
        controller.fnGetBoneTransform = [](void* pointer, int hid) -> void* {
            auto* actor = static_cast<Actor*>(pointer); Touch(actor); return hid == 0 ? &actor->hips : nullptr;
        };
        controller.fnGetComponentTransform = [](void* pointer) -> void* {
            auto* actor = static_cast<Actor*>(pointer); Touch(actor); return &actor->root;
        };
        controller.fnGetTransformParent = [](void* pointer) -> void* { return static_cast<Transform*>(pointer)->parent; };
        controller.fnGetWorldPosition = GetPosition; controller.fnSetWorldPosition = SetPosition;
        controller.fnGetWorldRotation = GetRotation; controller.fnSetWorldRotation = SetRotation;
        controller.fnGetLocalPosition = [](void* pointer, float* output) {
            auto* transform = static_cast<Transform*>(pointer); Touch(transform->owner);
            output[0] = transform->localPosition.x; output[1] = transform->localPosition.y; output[2] = transform->localPosition.z;
        };
        controller.fnGetLocalRotation = [](void* pointer, float* output) {
            auto* transform = static_cast<Transform*>(pointer); Touch(transform->owner);
            const auto value = transform->localRotation; output[0] = value.x; output[1] = value.y; output[2] = value.z; output[3] = value.w;
        };
        controller.fnSetLocalPosition = [](void* pointer, float x, float y, float z) {
            auto* transform = static_cast<Transform*>(pointer); Touch(transform->owner, true); transform->localPosition = {x, y, z};
        };
        controller.fnSetLocalRotation = [](void* pointer, float x, float y, float z, float w) {
            auto* transform = static_cast<Transform*>(pointer); Touch(transform->owner, true); transform->localRotation = {x, y, z, w};
        };
        controller.fnGetAnimatorController = [](void* pointer) -> void* {
            auto* animator = static_cast<Animator*>(pointer); Touch(animator->owner); return animator->controller;
        };
        controller.fnSetAnimatorController = [](void* pointer, void* value) {
            auto* animator = static_cast<Animator*>(pointer); Touch(animator->owner, true); animator->controller = value;
        };
        controller.fnGetAnimatorEnabled = [](void* pointer) {
            auto* animator = static_cast<Animator*>(pointer); Touch(animator->owner); return animator->enabled;
        };
        controller.fnSetAnimatorEnabled = [](void* pointer, bool value) {
            auto* animator = static_cast<Animator*>(pointer); Touch(animator->owner, true); animator->enabled = value;
        };
        controller.fnCanDriveActor = [](void* pointer) { return pointer && static_cast<Actor*>(pointer)->alive; };
        controller.useAnimatorRebind = false;
    }
    inline void WriteClip(const std::filesystem::path& path, float fps, uint32_t frames, float turn)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        const auto write = [&](const auto& value) { output.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
        output.write("GKCLIPCL", 8);
        const uint32_t version = 4, bones = 1;
        const float duration = frames / fps, scale = .083f;
        write(version); write(fps); write(frames); write(duration); write(bones); write(scale);
        const uint16_t hid = 0; const uint8_t nameLength = 4, flags = 0;
        write(hid); write(nameLength); write(flags); output.write("Hips", 4);
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            const Vector3 root{static_cast<float>(frame) / fps, 0, 0};
            const auto rotation = Quaternion::FromEulerDegreesXYZ(0, turn * frame / (frames > 1 ? frames - 1 : 1), 0);
            write(root); write(rotation);
        }
        const uint8_t camera = 0; const uint32_t morphs = 0; write(camera); write(morphs);
    }
    struct Fixture
    {
        std::filesystem::path root;
        Core core;
        std::array<Actor, 4> actors;
        std::array<Vector3, 4> initialHips;
        explicit Fixture(const std::string& repoRoot)
        {
            root = std::filesystem::path(repoRoot) / "build/tests" / ("ensemble-runtime-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            core.initialized = true; core.SetControlActive(true);
            core.configRoot = MotionPathToUtf8(root / "config");
            core.motionsRoot = MotionPathToUtf8(root / "motions");
            Wire(core.player.controller);
            std::vector<SceneActorObservation> roster;
            for (size_t i = 0; i < actors.size(); ++i)
            {
                auto& actor = actors[i];
                actor.root.owner = &actor; actor.root.localPosition = {static_cast<float>(i) * 3, 0, 0};
                actor.hips.owner = &actor; actor.hips.parent = &actor.root; actor.hips.localPosition = {0, 1, 0};
                actor.animator.owner = &actor;
                actor.animator.controller = reinterpret_cast<void*>(uintptr_t{0x100} + i);
                initialHips[i] = Position(&actor.hips);
                roster.push_back({&actor, static_cast<int>(700 + i), "hski"});
            }
            core.PublishSceneActors(roster); core.currentActor = &actors[0];
            const auto groupDir = root / "motions" / "group-0";
            WriteClip(groupDir / "motion.gkclip", 30.0f, 30, 30.0f);
            WriteClip(groupDir / "positions/slot_2/motion.gkclip", 60.0f, 240, 45.0f);
            WriteClip(groupDir / "positions/slot_3/motion.gkclip", 24.0f, 12, 60.0f);
            WriteClip(groupDir / "positions/slot_4/motion.gkclip", 30.0f, 30, 75.0f);
            {
                std::ofstream info(groupDir / "info.json", std::ios::binary | std::ios::trunc);
                info << R"({"version":1,"title":"Group Test","subtitle":"Tester","is_group":true,"position_count":4,"positions":[{"slot":1,"x":0,"y":0,"z":0,"yaw_degrees":0},{"slot":2,"x":-1,"y":0,"z":0.2,"yaw_degrees":15},{"slot":3,"x":1,"y":0,"z":0.2,"yaw_degrees":30},{"slot":4,"x":25,"y":0,"z":-13,"yaw_degrees":45}]})";
            }
            core.RefreshMotionLibrary();
            core.player.fnPlayAudio = [](const std::string& path) { ++audioPlays; audioPath = path; };
            core.player.fnPauseAudio = [] { ++audioPauses; };
            core.player.fnStopAudio = [] { ++audioStops; };
            core.player.fnResumeAudio = [](float) { ++audioResumes; };
            core.player.fnSeekAudio = [](float) { ++audioSeeks; };
            core.player.fnSeekPausedAudio = [](float) { ++audioSeeks; };
            audioPlays = audioPauses = audioStops = audioResumes = audioSeeks = 0; audioPath.clear();
        }
        ~Fixture()
        {
            core.AbandonScene();
            std::error_code ignored; std::filesystem::remove_all(root, ignored);
        }
        bool SelectGroup(bool autoPlay = false) { return core.SelectMotionByIndex(0, autoPlay, !autoPlay); }
        void Frame(uint64_t frame, float elapsed, const std::array<size_t, 4>& order = {3, 2, 0, 1})
        {
            for (size_t call = 0; call < order.size(); ++call)
            {
                auto& actor = actors[order[call]];
                core.OnLateUpdate(&actor, &actor.animator, nullptr, elapsed, call == 0, elapsed, frame);
            }
        }
        control::v0::Command Command(control::v0::CommandKind kind)
        {
            control::v0::State state;
            core.ReadControlState(sizeof(state), &state);
            control::v0::Command command;
            command.kind = kind; command.session = state.session; command.motionGeneration = state.generation;
            command.libraryRevision = state.libraryRevision;
            return command;
        }
    };
}

static void TestEnsembleRuntime(const std::string& repoRoot)
{
    using namespace EnsembleRuntimeTest;
    BeginTest("a fourth formation actor has an independent track and position");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        Check(fixture.SelectGroup(false) && core.formation.active, "four actors activate unified formation");
        Check(core.TrackForSlot(3) && core.TrackForSlot(3) != core.TrackForSlot(0) &&
            core.TrackForSlot(3)->controller.targetActor == &fixture.actors[3] &&
            EnsembleNear(core.formation.slots[3].offset, {25.0f, 0.0f, -13.0f}),
            "fourth slot owns its own target, track, and placement");
        fixture.Frame(1, 0.0f);
        Check(core.formation.slots.size() == 4 && fixture.actors[3].writes > 0, "four actor identities are driven simultaneously");
    }

    BeginTest("unified formation shares one frame clock, single audio owner, and restores on stop");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        Check(fixture.SelectGroup(false) && core.player.state == PlaybackState::Preview,
            "selecting group track enters silent preview");
        Check(EnsembleNear(core.player.TimelineDuration(), 4.0f) && core.TrackForSlot(1)->clip.fps == 60 &&
            core.TrackForSlot(2)->clip.fps == 24, "performance duration includes longer secondary slot");
        for (int i = 0; i < 4; ++i)
        {
            auto* track = core.TrackForSlot(i);
            Check(track && !track->fnPlayAudio && !track->fnPauseAudio && !track->fnStopAudio && !track->fnResumeAudio &&
                !track->fnSeekAudio && !track->fnGetAudioTime && track->audioSourceDuration == 0,
                "secondary formation sampler tracks own no audio callbacks or audio duration");
        }
        const int beforePlay = audioPlays;
        core.player.Play(); fixture.Frame(10, .5f);
        Check(EnsembleNear(core.player.currentTime, .5f) && EnsembleNear(core.TrackForSlot(1)->currentTime, .5f) &&
            EnsembleNear(core.TrackForSlot(2)->currentTime, .5f) && audioPlays == beforePlay + 1 &&
            audioPath == core.motions[0].dir + "/song.wav", "any first actor advances once and master player owns audio");
        std::array<size_t, 4> order{0, 1, 2, 3}; uint64_t frame = 11;
        do
        {
            const float previous = core.player.currentTime;
            fixture.Frame(frame++, .01f, order);
            Check(EnsembleNear(core.player.currentTime, previous + .01f) &&
                EnsembleNear(core.TrackForSlot(1)->currentTime, core.player.currentTime) &&
                EnsembleNear(core.TrackForSlot(2)->currentTime, core.player.currentTime),
                "all actor callback orders sample one shared frame time");
        } while (std::next_permutation(order.begin(), order.end()));
        const float beforeDuplicate = core.player.currentTime;
        auto& first = fixture.actors[0];
        core.OnLateUpdate(&first, &first.animator, nullptr, .3f, true, .3f, frame - 1);
        Check(EnsembleNear(core.player.currentTime, beforeDuplicate), "duplicate callbacks in same physical frame cannot advance twice");
        core.player.Pause(); core.player.Seek(2.0f); fixture.Frame(frame++, .4f);
        Check(EnsembleNear(core.player.currentTime, 2.0f) && EnsembleNear(core.TrackForSlot(2)->currentTime, 2.0f) &&
            core.player.state == PlaybackState::Paused, "paused seek updates every actor cleanly");
        auto* shortTrack = core.TrackForSlot(2);
        Vector3 rPos; std::vector<Quaternion> rotations;
        shortTrack->clip.Sample(shortTrack->MotionSourceTime(), rPos, rotations);
        Check(EnsembleNear(rPos.x, 11.0f / 24.0f), "shorter slot holds last frame past its clip duration");
        core.StopPerformance();
        bool restored = true;
        for (size_t i = 0; i < size_t{4}; ++i)
            restored &= fixture.actors[i].animator.enabled &&
                fixture.actors[i].animator.controller == reinterpret_cast<void*>(uintptr_t{0x100} + i) &&
                EnsembleNear(Position(&fixture.actors[i].hips), fixture.initialHips[i]);
        Check(restored && core.player.currentTime == 0 && core.player.state == PlaybackState::Stopped,
            "Stop restores all four saved poses and original Animator ownership");
    }

    BeginTest("capture suspend/resume and per-slot overrides stay isolated across slots");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        WriteTextFile(fixture.root / "motions/group-0/bone_overrides.json", "{\"overrides\":[{\"humanoid_id\":0,\"euler_deg\":[0,10,0]}]}");
        WriteTextFile(fixture.root / "motions/group-0/positions/slot_2/bone_overrides.json", "{\"overrides\":[{\"humanoid_id\":0,\"euler_deg\":[0,20,0]}]}");
        WriteTextFile(fixture.root / "motions/group-0/positions/slot_3/bone_overrides.json", "{\"overrides\":[{\"humanoid_id\":0,\"euler_deg\":[0,30,0]}]}");
        Check(fixture.SelectGroup(false), "group track with per-slot overrides loads");
        for (int i = 0; i < 3; ++i)
        {
            const auto* track = core.TrackForSlot(i);
            const auto* ov = track && track->controller.boneOverrides ? track->controller.boneOverrides->Find(0, "") : nullptr;
            Check(ov && EnsembleNear(ov->eulerDeg.y, static_cast<float>(10 + i * 10)), "each slot binds its own override collection");
        }
        core.ToggleOverrides();
        for (int i = 0; i < 3; ++i)
        {
            const auto* track = core.TrackForSlot(i);
            Check(track && track->controller.boneOverrides && !track->controller.boneOverrides->enabled,
                "master override toggle disables every slot cleanly");
        }
        core.player.Play(); fixture.Frame(2, .2f);
        core.player.Pause();
        core.player.fnObserveAudio = [] { return audio::Observation{audio::PlaybackState::Paused, true, true, 1, 0}; };
        Check(core.SuspendForCapture() && core.captureSuspended && core.player.state == PlaybackState::Stopped &&
            fixture.actors[1].animator.enabled && fixture.actors[2].animator.enabled,
            "capture suspend confirms restore across all active actors");
        Check(core.ResumeAfterCapture() && core.player.state == PlaybackState::Paused && EnsembleNear(core.player.currentTime, .2f),
            "capture resume restores shared paused position");
        core.TrackForSlot(2)->controller.captureRestoreUnconfirmed = true;
        Check(!core.SuspendForCapture() && core.captureRestoreReport.body == capture::RestoreState::Unconfirmed &&
            !core.ResumeAfterCapture(), "unconfirmed secondary restore blocks resuming capture session");
    }

    BeginTest("actor loss and pointer reuse never touch departed actors");
    for (bool reuseAddress : {false, true})
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        Check(fixture.SelectGroup(true), "actor-lifetime fixture starts performance"); fixture.Frame(1, 0);
        auto& missing = fixture.actors[1]; missing.alive = false;
        const auto missingTouches = missing.invalidTouches;
        std::vector<SceneActorObservation> roster{{&fixture.actors[0], 700, "hski"}, {&fixture.actors[2], 702, "hski"}};
        if (reuseAddress) roster.push_back({&missing, 900, "ttmr"});
        core.PublishSceneActors(roster);
        Check(missing.invalidTouches == missingTouches && !core.formation.slots[1].occupied &&
            core.formation.slots[0].occupied && core.formation.slots[2].occupied,
            "roster removal or pointer reuse clears slot cleanly without accessing dead actor");
        fixture.actors[2].alive = false;
        core.PublishSceneActors({{&fixture.actors[0], 700, "hski"}});
        const int departedTouches = fixture.actors[2].invalidTouches;
        core.StopPerformance();
        Check(!core.formation.active && fixture.actors[2].invalidTouches == departedTouches,
            "shrinking to 1 actor and calling Stop never touches departed secondary actors");
    }

    BeginTest("Control v0 compatibility during active multi-actor formation");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        Check(fixture.SelectGroup(false) && core.formation.active, "Control compatibility fixture active");
        namespace api = GakuMotion::control::v0;
        api::Capabilities caps;
        Check(core.ReadControlCapabilities(sizeof(caps), &caps) == api::Error::Ok &&
            !(caps.availableMask & api::Mask(api::CommandKind::Load)) && !(caps.availableMask & api::Mask(api::CommandKind::Offset)),
            "Control v0 withholds ambiguous Load and Offset while multi-actor formation is active");
        uint64_t ticket = 999;
        auto command = fixture.Command(api::CommandKind::Seek); command.value = .25f;
        Check(core.SubmitControl(&command, &ticket) == api::Error::Pending && ticket != 0,
            "global Seek remains available through Control v0");
        core.ProcessPendingGuiCommands(); api::Result result;
        Check(core.PollControl(ticket, sizeof(result), &result) == api::Error::Ok && result.status == api::Error::Ok &&
            EnsembleNear(core.player.currentTime, .25f), "global Seek advances shared performance clock");
        core.ReleaseControl(ticket);
    }

    BeginTest("per-slot motion and facial timing across formation playback, duration, swap and source change");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        const auto groupDir = fixture.root / "motions" / "group-0";
        // Slot 2 has 240 frames @ 60fps = 4.0s clip; with speed=0.5, offset=2.0 -> 10.0s timeline duration!
        // Slot 1 has explicit slot-level override (speed=2.0 motion, 0.5 facial) while top-level audio/camera stay global.
        // Slot 3 omits slot track_timing and falls back to top-level motion/facial (1.0, 0.0).
        // Remove slot 4's clip file so setting sourceSlotIndex=0/3 exercises no-reload / fallback clip timing!
        {
            std::ofstream info(groupDir / "info.json", std::ios::binary | std::ios::trunc);
            info << R"({
              "version": 1,
              "title": "Group Timing Test",
              "is_group": true,
              "position_count": 4,
              "track_timing": {
                "motion": {"speed": 1.0, "offset_seconds": 0.0},
                "facial": {"speed": 1.0, "offset_seconds": 0.0},
                "camera": {"speed": 1.25, "offset_seconds": 0.5},
                "audio": {"speed": 0.8, "offset_seconds": 1.5}
              },
              "positions": [
                {"slot": 1, "x": 0, "y": 0, "z": 0, "yaw_degrees": 0, "track_timing": {"motion": {"speed": 2.0, "offset_seconds": 0.2}, "facial": {"speed": 0.5, "offset_seconds": -0.3}}},
                {"slot": 2, "x": -1, "y": 0, "z": 0.2, "yaw_degrees": 15, "track_timing": {"motion": {"speed": 0.5, "offset_seconds": 2.0}, "facial": {"speed": 1.5, "offset_seconds": 0.4}}},
                {"slot": 3, "x": 1, "y": 0, "z": 0.2, "yaw_degrees": 30},
                {"slot": 4, "x": 2, "y": 0, "z": 0.2, "yaw_degrees": 45, "track_timing": {"motion": {"speed": 0.25, "offset_seconds": 1.0}, "facial": {"speed": 1.0, "offset_seconds": 0.0}}}
              ]
            })";
        }
        std::error_code ignoredEc;
        std::filesystem::remove(groupDir / "positions/slot_4/motion.gkclip", ignoredEc);
        core.RefreshMotionLibrary();
        Check(fixture.SelectGroup(false) && core.formation.active, "group song with per-slot timing loads");

        // Slot 1 AdoptLoadedMotionSlot + LoadMotionByIndex both resolve slot 1 timing while preserving global camera/audio
        Check(EnsembleNear(core.player.timing.motion.speed, 2.0f) &&
            EnsembleNear(core.player.timing.motion.offsetSeconds, 0.2f) &&
            EnsembleNear(core.player.timing.facial.speed, 0.5f) &&
            EnsembleNear(core.player.timing.facial.offsetSeconds, -0.3f) &&
            EnsembleNear(core.player.timing.camera.speed, 1.25f) &&
            EnsembleNear(core.player.timing.audio.speed, 0.8f) &&
            EnsembleNear(core.player.timing.audio.offsetSeconds, 1.5f),
            "AdoptLoadedMotionSlot applies slot 1 motion/facial overrides while keeping camera/audio global");

        {
            MotionPlayer idxPlayer;
            idxPlayer.controller = core.player.controller;
            Check(idxPlayer.LoadMotionByIndex(core.motions, 0) &&
                EnsembleNear(idxPlayer.timing.motion.speed, 2.0f) &&
                EnsembleNear(idxPlayer.timing.facial.speed, 0.5f) &&
                EnsembleNear(idxPlayer.timing.camera.speed, 1.25f) &&
                EnsembleNear(idxPlayer.timing.audio.offsetSeconds, 1.5f),
                "LoadMotionByIndex resolves Slot 1 timing cleanly");
        }

        // Check per-slot track timings and mixed duration (slot 4 has 1.0s clip @ 0.25x + 1.0s offset = 5.0s; slot 2 has 4.0s clip @ 0.5x + 2.0s offset = 10.0s)
        auto* t0 = core.TrackForSlot(0);
        auto* t1 = core.TrackForSlot(1);
        auto* t2 = core.TrackForSlot(2);
        auto* t3 = core.TrackForSlot(3);
        Check(t0 && EnsembleNear(t0->timing.motion.speed, 2.0f) && EnsembleNear(t0->timing.facial.speed, 0.5f),
            "slot 0 track uses slot 1 timing");
        Check(t1 && EnsembleNear(t1->timing.motion.speed, 0.5f) && EnsembleNear(t1->timing.motion.offsetSeconds, 2.0f) &&
            EnsembleNear(t1->timing.facial.speed, 1.5f) && EnsembleNear(t1->timing.facial.offsetSeconds, 0.4f),
            "slot 1 track uses slot 2 timing");
        Check(t2 && EnsembleNear(t2->timing.motion.speed, 1.0f) && EnsembleNear(t2->timing.facial.speed, 1.0f),
            "slot 2 track falls back to track-global timing");
        Check(t3 && EnsembleNear(t3->timing.motion.speed, 0.25f) && EnsembleNear(t3->timing.motion.offsetSeconds, 1.0f),
            "slot 3 track resolves slot 4 timing even when clip falls back to slot 1");
        Check(EnsembleNear(core.player.TimelineDuration(), 10.0f),
            "UpdateFormationDuration reflects mixed per-slot timing (4s / 0.5 + 2s = 10s)");

        // SetSlotSource on slot 3 from sourceSlotIndex=3 (slot 4 fallback->clip 1) to sourceSlotIndex=0 (slot 1->clip 1):
        // Clip path/slot=1 doesn't reload, yet timing MUST update from 0.25x to 2.0x!
        Check(core.SetSlotSource(3, 0), "SetSlotSource onto same underlying fallback clip succeeds");
        Check(EnsembleNear(core.TrackForSlot(3)->timing.motion.speed, 2.0f) &&
            EnsembleNear(core.TrackForSlot(3)->timing.facial.speed, 0.5f),
            "SetSlotSource updates track timing even when clip does not reload");

        // SwapSlots swaps actor ownership across positions; slot 0 stays on slot 1 timing and slot 1 stays on slot 2 timing
        auto* actor0 = &fixture.actors[0];
        auto* actor1 = &fixture.actors[1];
        Check(core.SwapSlots(0, 1), "SwapSlots(0, 1) succeeds");
        Check(core.TrackForSlot(0)->controller.targetActor == actor1 &&
            EnsembleNear(core.TrackForSlot(0)->timing.motion.speed, 2.0f) &&
            core.TrackForSlot(1)->controller.targetActor == actor0 &&
            EnsembleNear(core.TrackForSlot(1)->timing.motion.speed, 0.5f),
            "SwapSlots binds swapped actors to each slot position's timing");
    }
}
