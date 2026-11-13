#pragma once
#include "GkLiveTakeoverSession.hpp"

// These are synthetic state transitions, not a game or API compatibility test.
inline void TestLiveTakeoverSession()
{
    using namespace GakuMotion::live_takeover;
    auto check = [](bool ok, const char* what) { StageRuntimeHarness::Check(ok, what); };
    const Evidence running{1, true, true, false};
    const Evidence paused{1, true, true, true};
    const Evidence another{2, true, true, true};
    Session s;
    GakuMotion::injection::OwnerSnapshot liveOwner;
    liveOwner.confirmed = true; liveOwner.presenterIdentity = 1; liveOwner.sceneHandle = 3;
    liveOwner.sceneLoaded = true; liveOwner.role = GakuMotion::injection::OwnerRole::Live;
    check(liveOwner.OwnsActor(3) && !liveOwner.OwnsActor(4) &&
        GakuMotion::injection::PreviewContext("Live", "Live", liveOwner) == GakuMotion::injection::Context::Unknown,
        "Live owner alone never admits injection and cannot own another scene's actor");
    using GakuMotion::injection::LiveTakeoverContext;
    using GakuMotion::injection::Context;
    check(LiveTakeoverContext(liveOwner, 1, 3, false, true, true, true) == Context::Unknown &&
          LiveTakeoverContext(liveOwner, 1, 3, true, true, true, true) == Context::Live,
          "Live takeover needs explicit opt-in even with a paused confirmed owner");
    check(LiveTakeoverContext(liveOwner, 2, 3, true, true, true, true) == Context::Unknown &&
          LiveTakeoverContext(liveOwner, 1, 4, true, true, true, true) == Context::Unknown &&
          LiveTakeoverContext(liveOwner, 1, 3, true, false, true, true) == Context::Unknown &&
          LiveTakeoverContext(liveOwner, 1, 3, true, true, false, true) == Context::Unknown &&
          LiveTakeoverContext(liveOwner, 1, 3, true, true, true, false) == Context::Unknown,
          "Live injection rejects changed ownership, wrong mode, resumed playback and missing guard");
    check(!s.MayInject() && s.State() == Phase::Off, "Live takeover defaults off");
    check(s.Enable({1, false, true, false}) == Action::None && !s.MayInject(), "unverified Live remains blocked");
    check(s.Enable(running) == Action::PauseLive && !s.MayInject(), "pause is requested before injection");
    check(s.PauseFinished(paused, true) == Action::None && s.MayInject() && s.OwnsPause(), "observed pause permits injection");
    check(s.Disable(paused) == Action::RestoreActors && !s.MayInject(), "disable first revokes injection and restores actors");
    check(s.RestoreFinished(paused, true) == Action::ResumeLive, "owned pause resumes only after confirmed restoration");
    s.ResumeFinished(running, true);
    check(s.State() == Phase::Off, "confirmed resume returns to off");

    check(s.Enable(paused) == Action::None && s.MayInject() && !s.OwnsPause(), "already paused Live stays user-owned");
    check(s.Disable(paused) == Action::RestoreActors && s.RestoreFinished(paused, true) == Action::None &&
        s.State() == Phase::Off, "already paused Live is not resumed");

    s.Enable(running); s.PauseFinished(paused, true); s.Disable(paused);
    check(s.RestoreFinished(paused, false) == Action::None && s.State() == Phase::Failed && !s.MayInject(),
        "failed actor restoration cannot resume Live");
    s.ObserveOwner(another);
    check(s.State() == Phase::Off, "scene change drops stale ownership without a game action");

    s.Enable(running); s.Disable(running);
    check(s.PauseFinished(paused, true) == Action::ResumeLive && !s.MayInject(),
        "cancel during pending pause resumes without taking over actors");
    s.ResumeFinished(running, true);
    s.Enable(running); s.PauseFinished(paused, false);
    check(s.State() == Phase::Failed && !s.MayInject(), "unconfirmed pause cannot grant takeover");
    s.ObserveOwner(another);
    s.Enable(paused); s.ObserveOwner(running);
    check(!s.MayInject() && s.State() == Phase::Failed, "external Live resume revokes injection");
    s.ObserveOwner(another); s.Enable(paused); s.ObserveOwner({});
    check(s.State() == Phase::Failed && !s.MayInject(),
        "an unobserved owner fails closed without assuming scene destruction");
}
