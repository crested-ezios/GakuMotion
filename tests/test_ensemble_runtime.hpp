#pragma once

#include "GakuMotion.hpp"
#include <array>
#include <algorithm>

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
            const auto rotation = Quaternion::FromEulerDegreesXYZ(0, turn * frame / (frames - 1), 0);
            write(root); write(rotation);
        }
        const uint8_t camera = 0; const uint32_t morphs = 0; write(camera); write(morphs);
    }
    struct Fixture
    {
        std::filesystem::path root;
        Core core;
        std::array<Actor, 4> actors;
        ensemble::Settings settings;
        std::array<Vector3, 4> initialHips;
        Fixture(const std::string& repoRoot)
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
            settings.slots.resize(3);
            const std::array<float, 3> rates{30, 60, 24};
            const std::array<uint32_t, 3> frames{30, 240, 12};
            for (size_t i = 0; i < size_t{3}; ++i)
            {
                const std::string key = "motion-" + std::to_string(i);
                const auto directory = root / "motions" / key;
                WriteClip(directory / "motion.gkclip", rates[i], frames[i], 30 + i * 15.0f);
                MotionEntry entry;
                entry.name = key; entry.dir = MotionPathToUtf8(directory); entry.title = key;
                core.motions.push_back(entry);
                auto& slot = settings.slots[i]; slot.enabled = true; slot.motionKey = key;
                slot.actorToken = core.sceneActorRegistry.Find(&actors[i])->token; slot.actorHint = "actor " + std::to_string(i);
                slot.offset = {static_cast<float>(i) - 1, 0, .2f}; slot.yawDegrees = static_cast<float>(i) * 15;
            }
            settings.enabled = true; settings.masterSlot = 0; settings.cameraSlot = 1;
            core.ensembleSnapshot.available = core.ensembleSnapshot.verified = true;
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
        bool Apply() { return core.ApplyEnsemble(settings, core.ensembleSnapshot.revision, core.sceneActorRegistry.Epoch()); }
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
    BeginTest("a fourth ensemble actor has an independent track and position");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        fixture.settings.slots.emplace_back();
        auto& fourth = fixture.settings.slots.back();
        fourth.enabled = true; fourth.motionKey = fixture.settings.slots[0].motionKey;
        fourth.actorToken = core.sceneActorRegistry.Find(&fixture.actors[3])->token;
        fourth.offset = {25.0f, 0.0f, -13.0f};
        Check(fixture.Apply(), "four bound actors apply with independent slots");
        Check(core.EnsemblePlayer(3) && core.EnsemblePlayer(3) != core.EnsemblePlayer(0) &&
            core.EnsemblePlayer(3)->controller.targetActor == &fixture.actors[3],
            "fourth slot owns its own target and track");
        fixture.Frame(1, 0.0f);
        Check(core.EnsembleActorsValid(), "four actor identities stay valid after a frame");
    }
    BeginTest("ensemble looping belongs to the session, not the single-motion playlist");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        Check(fixture.Apply(), "looping fixture starts an ensemble");
        const auto originalMode = core.playlist.mode;
        core.QueueGui({Core::GuiAction::Mode, 0}); core.ProcessPendingGuiCommands();
        Check(!core.player.loop && core.playlist.mode == originalMode,
            "play-once changes the complete performance without changing the library playlist");
        core.QueueGui({Core::GuiAction::Mode, 1}); core.ProcessPendingGuiCommands();
        Check(core.player.loop && core.playlist.mode == originalMode,
            "whole-performance loop retains single-motion playlist preferences");
        Check(!core.DeleteMotionFiles("motion-1", true) && core.EnsembleActive() &&
            std::filesystem::exists(fixture.root / "motions" / "motion-1" / "motion.gkclip"),
            "deleting a referenced ensemble source is refused without altering the active performance");
    }
    BeginTest("ensemble preflight preserves an existing performance and validates actor identities");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        Check(fixture.Apply() && core.EnsembleActive() && core.player.state == PlaybackState::Preview,
            "three valid actor and motion slots commit as a silent preview");
        fixture.Frame(1, 0);
        const auto settings = core.ensembleActiveSettings;
        const auto generation = core.loadedMotionGeneration;
        auto* secondary = core.EnsemblePlayer(1);
        const auto binding = secondary->controller.currentBinding.actor;
        const auto reject = [&](const ensemble::Settings& candidate, const char* message, uint64_t epoch = 0) {
            Check(!core.ApplyEnsemble(candidate, core.ensembleSnapshot.revision,
                    epoch ? epoch : core.sceneActorRegistry.Epoch()) && core.EnsembleActive() &&
                core.ensembleActiveSettings == settings && core.loadedMotionGeneration == generation &&
                core.EnsemblePlayer(1) == secondary && secondary->controller.currentBinding.actor == binding, message);
        };
        auto invalid = settings; invalid.slots[1].actorToken = invalid.slots[0].actorToken;
        reject(invalid, "duplicate actors fail before replacing clips or ownership");
        invalid = settings; invalid.slots[1].actorToken = UINT64_MAX;
        reject(invalid, "an unknown actor token cannot replace the current ensemble");
        reject(settings, "an old roster epoch cannot rebind the ensemble", core.sceneActorRegistry.Epoch() + 1);
        Check(!core.ApplyEnsemble(settings, core.ensembleSnapshot.revision + 1, core.sceneActorRegistry.Epoch()) &&
            core.EnsembleActive() && core.ensembleActiveSettings == settings && core.EnsemblePlayer(1) == secondary,
            "an outdated GUI revision cannot replace the current ensemble");
        invalid = settings; invalid.slots[0].enabled = false;
        reject(invalid, "a disabled master slot is rejected before changing the ensemble");
        invalid = settings; invalid.cameraSlot = 2; invalid.slots[2].enabled = false;
        reject(invalid, "a disabled camera follow slot is rejected before changing the ensemble");
        invalid = settings; invalid.slots[1].motionKey = "missing-motion";
        reject(invalid, "a missing motion key preserves the running clip instances");
        const auto file = fixture.root / "motions/motion-1/motion.gkclip";
        std::ofstream(file, std::ios::binary | std::ios::trunc) << "incomplete";
        reject(settings, "an unreadable secondary clip preserves the original bound ensemble");
        core.fnBeforeTargetChange = [] { return false; };
        WriteClip(file, 60, 240, 45);
        reject(settings, "unconfirmed old physics restoration keeps the existing ensemble");
        core.fnBeforeTargetChange = nullptr;
    }

    BeginTest("ensemble uses one frame clock with independent source timing and silent secondary tracks");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        core.motions[1].timing.motion = {.5f, .25f};
        core.motions[1].timing.facial = {2, -.1f};
        Check(fixture.Apply(), "different FPS and duration sources apply together");
        Check(EnsembleNear(core.player.TimelineDuration(), 8.25f) && core.EnsemblePlayer(1)->clip.fps == 60 &&
            core.EnsemblePlayer(2)->clip.fps == 24, "performance duration includes the delayed slower secondary source");
        for (size_t i = 1; i < size_t{3}; ++i)
        {
            auto* track = core.EnsemblePlayer(i);
            Check(!track->fnPlayAudio && !track->fnPauseAudio && !track->fnStopAudio && !track->fnResumeAudio &&
                !track->fnSeekAudio && !track->fnGetAudioTime && track->audioSourceDuration == 0,
                "a secondary track owns no audio callbacks or audio duration");
        }
        const int beforePlay = audioPlays;
        core.player.Play(); fixture.Frame(10, .5f);
        Check(EnsembleNear(core.player.currentTime, .5f) && EnsembleNear(core.EnsemblePlayer(1)->currentTime, .5f) &&
            EnsembleNear(core.EnsemblePlayer(2)->currentTime, .5f) && audioPlays == beforePlay + 1 &&
            audioPath == core.motions[0].dir + "/song.wav", "an unselected first actor advances once and only the master starts audio");
        Check(EnsembleNear(core.EnsemblePlayer(1)->MotionSourceTime(), .125f) &&
            EnsembleNear(core.EnsemblePlayer(1)->FacialSourceTime(), 1.2f), "body and facial source times independently map the shared performance seconds");
        std::array<size_t, 4> order{0, 1, 2, 3}; uint64_t frame = 11;
        do
        {
            const float previous = core.player.currentTime;
            fixture.Frame(frame++, .01f, order);
            Check(EnsembleNear(core.player.currentTime, previous + .01f) && EnsembleNear(core.EnsemblePlayer(1)->currentTime, core.player.currentTime) &&
                EnsembleNear(core.EnsemblePlayer(2)->currentTime, core.player.currentTime), "all actor callback orders sample one shared frame time");
        } while (std::next_permutation(order.begin(), order.end()));
        const float beforeDuplicate = core.player.currentTime;
        auto& first = fixture.actors[0];
        core.OnLateUpdate(&first, &first.animator, nullptr, .3f, true, .3f, frame - 1);
        Check(EnsembleNear(core.player.currentTime, beforeDuplicate), "duplicate clock-eligible callbacks with the same physical frame cannot advance twice");
        core.player.Pause(); core.player.Seek(2.0f); fixture.Frame(frame++, .4f);
        Check(EnsembleNear(core.player.currentTime, 2.0f) && EnsembleNear(core.EnsemblePlayer(1)->currentTime, 2.0f) &&
            EnsembleNear(core.EnsemblePlayer(2)->currentTime, 2.0f) && core.player.state == PlaybackState::Paused,
            "paused seek updates every actor without advancing the performance clock");
        auto* shortTrack = core.EnsemblePlayer(2);
        Vector3 root; std::vector<Quaternion> rotations;
        shortTrack->clip.Sample(shortTrack->MotionSourceTime(), root, rotations);
        Check(EnsembleNear(root.x, 11.0f / 24.0f) && shortTrack->state == PlaybackState::Paused,
            "a shorter source holds its last frame while the longer performance continues");
        const auto heldPose = Rotation(&fixture.actors[2].hips);
        core.player.Seek(3.0f); fixture.Frame(frame++, 0);
        Check(std::fabs(Quaternion::Dot(heldPose, Rotation(&fixture.actors[2].hips))) > 1.0f - 1e-4f,
            "the shorter actor's actual written pose remains held across later performance times");
        core.player.Seek(.2f); fixture.Frame(frame++, 0);
        Check(EnsembleNear(core.EnsemblePlayer(1)->MotionSourceTime(), 0) && EnsembleNear(core.EnsemblePlayer(2)->currentTime, .2f),
            "a backwards paused seek reconstructs each source without retained short-track progress");
        core.StopPerformance();
        bool restored = true;
        for (size_t i = 0; i < size_t{3}; ++i)
            restored &= fixture.actors[i].animator.enabled && fixture.actors[i].animator.controller == reinterpret_cast<void*>(uintptr_t{0x100} + i) &&
                EnsembleNear(Position(&fixture.actors[i].hips), fixture.initialHips[i]) && core.EnsemblePlayer(i)->state == PlaybackState::Stopped;
        Check(restored && core.player.currentTime == 0, "Stop restores all three saved poses and original Animator ownership");
    }

    BeginTest("ensemble master selection and screenshot suspension preserve one transport owner");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        fixture.settings.masterSlot = 1; fixture.settings.cameraSlot = 2;
        fixture.settings.slots[0].enabled = false;
        Check(fixture.Apply() && core.EnsemblePlayer(1) == &core.player && core.EnsemblePlayer(0) == nullptr &&
            core.player.controller.targetActor == &fixture.actors[1] && core.EnsembleCameraActor() == &fixture.actors[2],
            "a nonzero master slot and independent follow slot bind correctly with a disabled first slot");
        fixture.Frame(1, 0); core.player.Play(); fixture.Frame(2, .2f);
        Check(audioPath == core.motions[1].dir + "/song.wav" && core.loadedMotionKey == "motion-1",
            "only the explicitly chosen nonzero master owns the loaded key and audio source");
        core.player.Pause();
        core.player.fnObserveAudio = [] { return audio::Observation{audio::PlaybackState::Paused, true, true, 1, 0}; };
        Check(core.SuspendForCapture() && core.captureSuspended && core.player.state == PlaybackState::Stopped &&
            core.EnsemblePlayer(2)->state == PlaybackState::Stopped && fixture.actors[1].animator.enabled && fixture.actors[2].animator.enabled,
            "screenshot suspension confirms restore for every active actor and stops secondary sampling");
        Check(core.ResumeAfterCapture() && core.player.state == PlaybackState::Paused && EnsembleNear(core.player.currentTime, .2f),
            "screenshot resume restores the paused shared position without starting another audio owner");
        fixture.Frame(3, 0);
        core.EnsemblePlayer(2)->controller.captureRestoreUnconfirmed = true;
        Check(!core.SuspendForCapture() && core.captureRestoreReport.body == capture::RestoreState::Unconfirmed &&
            !core.ResumeAfterCapture() && core.player.state == PlaybackState::Stopped,
            "one unconfirmed secondary restore blocks resuming the complete capture session");
    }

    BeginTest("ensemble override reload keeps independent per-motion collections");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        for (size_t i = 0; i < size_t{3}; ++i)
        {
            const auto path = fixture.root / "motions" / ("motion-" + std::to_string(i)) / "bone_overrides.json";
            WriteTextFile(path, "{\"overrides\":[{\"humanoid_id\":0,\"euler_deg\":[0," + std::to_string(10 + i * 10) + ",0]}]}");
        }
        Check(fixture.Apply(), "each actor loads its own motion override fixture");
        core.LoadOverrides(false);
        bool independent = true;
        for (size_t i = 0; i < size_t{3}; ++i)
        {
            const auto* track = core.EnsemblePlayer(i);
            const auto* override = track->controller.boneOverrides ? track->controller.boneOverrides->Find(0, "") : nullptr;
            independent &= track->controller.boneOverrides == &core.ensembleOverrides[i] && override &&
                EnsembleNear(override->eulerDeg.y, static_cast<float>(10 + i * 10));
        }
        Check(independent, "global reload never redirects all actors to the master's effective override set");
        core.ToggleOverrides();
        bool disabled = true;
        for (size_t i = 0; i < size_t{3}; ++i)
            disabled &= core.EnsemblePlayer(i)->controller.boneOverrides == &core.ensembleOverrides[i] && !core.ensembleOverrides[i].enabled;
        Check(disabled, "the global override toggle disables each actor while preserving its collection identity");
    }

    BeginTest("ensemble actor loss restores survivors without touching removed or reused identities");
    for (bool reuseAddress : {false, true})
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        Check(fixture.Apply(), "actor-lifetime fixture starts an ensemble"); fixture.Frame(1, 0);
        auto& missing = fixture.actors[1]; missing.alive = false;
        const auto missingTouches = missing.invalidTouches;
        std::vector<SceneActorObservation> roster{{&fixture.actors[0], 700, "hski"}, {&fixture.actors[2], 702, "hski"}};
        if (reuseAddress) roster.push_back({&missing, 900, "ttmr"});
        core.PublishSceneActors(roster);
        Check(!core.EnsembleActive() && !core.ensembleSnapshot.verified && core.player.state == PlaybackState::Stopped &&
            missing.invalidTouches == missingTouches, "roster removal or pointer reuse stops the ensemble without accessing the lost identity");
        Check(fixture.actors[0].animator.enabled && fixture.actors[0].animator.controller == reinterpret_cast<void*>(uintptr_t{0x100}) &&
            fixture.actors[2].animator.enabled && fixture.actors[2].animator.controller == reinterpret_cast<void*>(uintptr_t{0x102}) &&
            EnsembleNear(Position(&fixture.actors[0].hips), fixture.initialHips[0]) && EnsembleNear(Position(&fixture.actors[2].hips), fixture.initialHips[2]),
            "surviving actors restore their own pose and Animator after a different slot disappears");
    }

    BeginTest("ensemble blocks legacy single-actor placement and sole-check mutations");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        core.initialPositionPath = MotionPathToUtf8(fixture.root / "config/initial_position.json");
        const std::string initialBytes = "single actor preference sentinel\n";
        WriteTextFile(MotionPathFromUtf8(core.initialPositionPath), initialBytes);
        Check(fixture.Apply(), "legacy placement compatibility fixture starts an ensemble");
        const auto placement = core.player.controller.initialPositionOffset;
        const auto activeSettings = core.ensembleActiveSettings;
        core.QueueInitialPosition({9, 8, 7});
        core.QueueGui({Core::GuiAction::SaveInitialPosition});
        core.ProcessPendingGuiCommands();
        std::ifstream initialFile(MotionPathFromUtf8(core.initialPositionPath), std::ios::binary);
        const std::string afterBytes(std::istreambuf_iterator<char>(initialFile), {});
        Check(EnsembleNear(core.player.controller.initialPositionOffset, placement) && core.ensembleActiveSettings == activeSettings &&
            afterBytes == initialBytes && !std::filesystem::exists(MotionPathFromUtf8(core.initialPositionPath + ".tmp")),
            "old queued InitialPosition and SaveInitialPosition cannot change master placement or the single-actor preference file");
        const auto measurement = core.soleSettings;
        const auto experiment = core.experimentalSolePitchSettings;
        core.experimentalSolePitchSnapshot.actor = &fixture.actors[3];
        core.experimentalSolePitchSnapshot.poseSerial = 77;
        core.config.cmdSolePitchCheck = true;
        core.config.cmdSolePitchSettings = {3, .5f, 35, true, true, true};
        core.ConsumeConfigCommands();
        Check(EnsembleNear(core.player.controller.initialPositionOffset, placement) && core.soleSettings == measurement &&
            core.experimentalSolePitchSettings.experimentalSolePitchCorrection == experiment.experimentalSolePitchCorrection &&
            core.experimentalSolePitchSettings.maxPitchDegrees == experiment.maxPitchDegrees &&
            core.experimentalSolePitchSettings.useMeshEnvelope == experiment.useMeshEnvelope &&
            core.experimentalSolePitchSnapshot.actor == &fixture.actors[3] && core.experimentalSolePitchSnapshot.poseSerial == 77 &&
            !core.config.cmdSolePitchCheck,
            "a legacy sole pitch command cannot mutate ensemble placement, measurement settings or experiment state");
    }

    BeginTest("inactive ensemble tracks never retain actor restoration pointers");
    for (bool disableExplicitly : {true, false})
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        Check(fixture.Apply(), "inactive restoration lifecycle fixture starts an ensemble"); fixture.Frame(1, 0);
        if (disableExplicitly)
        {
            auto disabled = core.ensembleActiveSettings; disabled.enabled = false;
            Check(core.ApplyEnsemble(disabled, core.ensembleSnapshot.revision, core.sceneActorRegistry.Epoch()),
                "explicit disabling restores the ensemble before returning to single-actor mode");
        }
        else
        {
            fixture.actors[1].alive = false;
            core.PublishSceneActors({{&fixture.actors[0], 700, "hski"}, {&fixture.actors[2], 702, "hski"}});
        }
        auto& departed = fixture.actors[2]; departed.alive = false;
        core.PublishSceneActors({{&fixture.actors[0], 700, "hski"}});
        const int previousInvalidTouches = departed.invalidTouches;
        core.StopPerformance();
        Check(!core.EnsembleActive() && departed.invalidTouches == previousInvalidTouches,
            "a later Stop after disabling or actor loss never restores a secondary actor that subsequently departed");
    }

    BeginTest("legacy Control v0 cannot mutate ensemble motion selection or placement");
    {
        Fixture fixture(repoRoot); auto& core = fixture.core;
        Check(fixture.Apply(), "Control compatibility fixture starts an ensemble");
        namespace api = GakuMotion::control::v0;
        api::Capabilities caps;
        Check(core.ReadControlCapabilities(sizeof(caps), &caps) == api::Error::Ok &&
            !(caps.availableMask & api::Mask(api::CommandKind::Load)) && !(caps.availableMask & api::Mask(api::CommandKind::Offset)),
            "Control v0 capability snapshot withholds ambiguous ensemble Load and Offset commands");
        uint64_t ticket = 999;
        auto command = fixture.Command(api::CommandKind::Load);
        std::snprintf(command.motionKey, sizeof(command.motionKey), "motion-1");
        Check(core.SubmitControl(&command, &ticket) == api::Error::Unsupported && ticket == 0,
            "legacy Load reports Unsupported instead of silently replacing the ensemble master");
        command = fixture.Command(api::CommandKind::Offset); command.offset = {1, 2, 3}; ticket = 999;
        Check(core.SubmitControl(&command, &ticket) == api::Error::Unsupported && ticket == 0,
            "legacy spatial Offset reports Unsupported instead of changing one ensemble actor");
        command = fixture.Command(api::CommandKind::Seek); command.value = .25f;
        Check(core.SubmitControl(&command, &ticket) == api::Error::Pending && ticket != 0,
            "existing global Seek remains available through Control v0");
        core.ProcessPendingGuiCommands(); api::Result result;
        Check(core.PollControl(ticket, sizeof(result), &result) == api::Error::Ok && result.status == api::Error::Ok &&
            EnsembleNear(core.player.currentTime, .25f), "global Seek reaches the existing transport command drain");
        core.ReleaseControl(ticket);
    }
}
