#pragma once
#include "GkLiveTakeoverDriver.hpp"
#include <string>

inline void TestLiveTakeoverDriver()
{
    using namespace GakuMotion::live_takeover;
    struct FakeLive
    {
        Evidence evidence{42, true, true, false};
        std::string calls;
        bool restoreOk{true};
        bool pauseApplies{true};
        bool loseReadinessOnPause{};
    };
    auto check = [](bool ok, const char* what) { StageRuntimeHarness::Check(ok, what); };
    FakeLive fake;
    Adapter adapter{&fake,
        [](void* p) { return static_cast<FakeLive*>(p)->evidence; },
        [](void* p, uint64_t owner) {
            auto& f = *static_cast<FakeLive*>(p); f.calls += 'P';
            if (f.evidence.owner != owner) return false;
            if (f.pauseApplies) f.evidence.paused = true;
            if (f.loseReadinessOnPause) f.evidence.ready = false;
            return true;
        },
        [](void* p, uint64_t owner) {
            auto& f = *static_cast<FakeLive*>(p); f.calls += 'R';
            return f.evidence.owner == owner && f.restoreOk;
        },
        [](void* p, uint64_t owner) {
            auto& f = *static_cast<FakeLive*>(p); f.calls += 'U';
            if (f.evidence.owner != owner) return false;
            f.evidence.paused = false; f.evidence.ready = true; return true;
        }};
    Driver driver(adapter);
    driver.Enable();
    check(driver.MayInject() && fake.calls == "P" && fake.evidence.paused,
        "driver pauses and verifies Live before granting control");
    driver.Disable();
    check(driver.State() == Phase::Off && fake.calls == "PRU" && !fake.evidence.paused,
        "driver restores actors before resuming the Live it paused");

    fake.calls.clear(); fake.evidence.paused = true;
    driver.Enable(); driver.Disable();
    check(driver.State() == Phase::Off && fake.calls == "R" && fake.evidence.paused,
        "driver leaves an already paused Live paused");

    fake.calls.clear(); fake.evidence.paused = false; fake.restoreOk = false;
    driver.Enable(); driver.Disable();
    check(driver.State() == Phase::Failed && fake.calls == "PR" && fake.evidence.paused,
        "driver does not resume Live after failed actor restore");

    fake.calls.clear(); fake.evidence = {99, true, true, false}; fake.restoreOk = true;
    driver.Check();
    check(driver.State() == Phase::Off && fake.calls.empty(),
        "driver does not touch an old Live after owner replacement");
    fake.pauseApplies = false;
    driver.Enable();
    check(driver.State() == Phase::Failed && !driver.MayInject() && fake.calls == "P",
        "successful pause call without observed pause cannot grant access");

    fake.evidence = {100, true, true, false}; driver.Check();
    fake.calls.clear(); fake.pauseApplies = true; fake.loseReadinessOnPause = true;
    driver.Enable();
    check(driver.State() == Phase::Off && fake.calls == "PU" && !fake.evidence.paused,
        "failed admission after a confirmed pause resumes the previously playing Live");

    Driver unavailable;
    unavailable.Enable();
    check(unavailable.State() == Phase::Off && !unavailable.MayInject(),
        "missing runtime adapter leaves Live takeover disabled");
}
