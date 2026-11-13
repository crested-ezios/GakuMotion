#include "GkCaptureGuard.hpp"
#include <cstddef>
#include <cstdio>

using namespace GakuMotion::capture;
namespace Test
{
    struct State { int state{-1}; int padding{}; void* owner{}; char builder[16]{}; };
    int nativeCalls{}, beginCalls{}, endCalls{}, nextState{}, failures{}, exceptions{};
    bool active{}, live{}, beginOk{true}, continueOk{true}, unityThread{true}, clearOwnerOnComplete{};
    void Check(bool value, const char* message)
    { if (!value) { ++failures; std::printf("FAIL: %s\n", message); } }
    bool Active() { return active; }
    bool Live() { return live; }
    bool UnityThread() { return unityThread; }
    bool Begin(const char*) { ++beginCalls; active = false; return beginOk; }
    void End(const char*) { ++endCalls; if (endCalls >= beginCalls) active = true; }
    bool Continue(const char*) { return continueOk; }
    void Original(void* self, void*)
    {
        ++nativeCalls;
        auto* state = static_cast<State*>(self);
        state->state = nextState;
        if (clearOwnerOnComplete && nextState == -2) state->owner = nullptr;
    }
    void SetException(void*, void*, const void*) { ++exceptions; }
    void* Exception(void*, const char*, const char*, const char*) { static int token; return &token; }
    void Reset()
    {
        ledger = {}; liveAutoUnknown = false; liveReady = true;
        nativeCalls = beginCalls = endCalls = exceptions = 0; active = live = clearOwnerOnComplete = false;
        beginOk = continueOk = unityThread = true;
        callbacks = {}; callbacks.customActive = Active; callbacks.liveTakeoverActive = Live;
        callbacks.beginSuspend = Begin; callbacks.endSuspend = End;
        callbacks.continueCapture = Continue;
        callbacks.onUnityThread = UnityThread;
        autoPhoto.stateOffset = offsetof(State, state); autoPhoto.ownerOffset = offsetof(State, owner);
        autoPhoto.builderOffset = offsetof(State, builder); autoPhoto.original = Original;
        autoPhoto.setException = SetException; exceptionFromName = Exception; corlib = &nativeCalls;
    }
    void Run()
    {
        int owner = 1; State task; task.owner = &owner;
        Reset(); nextState = 0; GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        Check(nativeCalls == 1 && beginCalls == 0 && ledger.Count() == 1,
            "inactive automatic capture forwards and tracks its continuation");
        nextState = -2; GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        Check(nativeCalls == 2 && endCalls == 0 && ledger.Count() == 0,
            "inactive automatic continuation is never cancelled");

        Reset(); task.state = -1; task.owner = &owner; nextState = 0;
        GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        clearOwnerOnComplete = true; nextState = -2;
        GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        Check(ledger.Count() == 0 && !liveAutoUnknown,
            "terminal auto capture may clear its owner without losing completion");

        Reset(); task.state = -1; task.owner = &owner; active = live = true; nextState = 0;
        GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        Check(beginCalls == 1 && nativeCalls == 1 && !active && ledger.Count() == 1,
            "active takeover restores before automatic capture");
        nextState = -2; GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        Check(endCalls == 1 && active && ledger.Count() == 0,
            "automatic completion releases the matching suspension");

        Reset(); task.state = -1; active = live = true; nextState = 0;
        State second; second.owner = &owner;
        GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        GakuMotion::capture::Run(autoPhoto, &second, nullptr);
        Check(beginCalls == 2 && ledger.Count() == 2 && !active,
            "overlapping automatic captures each retain a suspension lease");
        nextState = -2;
        GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        Check(endCalls == 1 && ledger.Count() == 1 && !active,
            "first overlapping completion does not resume the takeover");
        GakuMotion::capture::Run(autoPhoto, &second, nullptr);
        Check(endCalls == 2 && ledger.Count() == 0 && active,
            "last overlapping completion releases the takeover");

        Reset(); task.state = -1; active = live = true; nextState = 0;
        GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        continueOk = false;
        GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        Check(nativeCalls == 1 && exceptions == 1 && ledger.Count() == 0,
            "unconfirmed automatic continuation is cancelled before native capture");

        Reset(); task.state = -1; active = live = true; beginOk = false;
        GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        Check(nativeCalls == 0 && exceptions == 1 && task.state == -2 && ledger.Count() == 0,
            "failed active restoration cancels rather than capture a custom pose");

        Reset(); task.state = -1; active = live = true; autoPhoto.stateOffset = -1; nextState = 0;
        GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        Check(beginCalls == 1 && nativeCalls == 1 && !active && liveAutoUnknown && endCalls == 0,
            "untrackable automatic capture keeps restored state suspended");
        autoPhoto.stateOffset = offsetof(State, state);

        Reset(); task.state = -1; nextState = -2;
        liveButton.stateOffset = offsetof(State, state); liveButton.ownerOffset = offsetof(State, owner);
        liveButton.builderOffset = offsetof(State, builder); liveButton.original = Original;
        liveButton.setException = SetException;
        GakuMotion::capture::Run(liveButton, &task, nullptr);
        Check(nativeCalls == 1 && exceptions == 0, "inactive Live manual photo remains native");
        task.state = -1; active = live = true;
        GakuMotion::capture::Run(liveButton, &task, nullptr);
        Check(nativeCalls == 1 && exceptions == 1 && task.state == -2,
            "manual Live photo is blocked while custom motion is active");

        Reset(); task.state = -1; unityThread = false; nextState = -2;
        GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        Check(nativeCalls == 1 && liveAutoUnknown && ledger.Count() == 0,
            "off-thread inactive auto capture forwards without touching the ledger");
        Reset(); task.state = -1; unityThread = false; live = active = true;
        GakuMotion::capture::Run(autoPhoto, &task, nullptr);
        Check(nativeCalls == 0 && exceptions == 1 && liveAutoUnknown && ledger.Count() == 0,
            "off-thread auto capture is rejected during Live takeover");
    }
}
int main()
{
    Test::Run();
    std::printf("Live capture guard: %d failures\n", Test::failures);
    return Test::failures ? 1 : 0;
}
