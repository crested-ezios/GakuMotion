#pragma once
#include "GkLiveTakeoverSession.hpp"

namespace GakuMotion::live_takeover
{
    // Adapter calls are synchronous and must run on the Unity main thread.
    // Each Observe obtains fresh, value-only evidence; adapters must never
    // dereference an object retained from a previous scene or frame.
    struct Adapter
    {
        void* context{};
        Evidence (*observe)(void*){};
        bool (*pause)(void*, uint64_t owner){};
        bool (*restoreActors)(void*, uint64_t owner){};
        bool (*resume)(void*, uint64_t owner){};
    };

    class Driver
    {
        Session session_;
        Adapter adapter_{};
        Evidence Observe() const { return adapter_.observe ? adapter_.observe(adapter_.context) : Evidence{}; }
        bool Ready() const { return adapter_.observe && adapter_.pause && adapter_.restoreActors && adapter_.resume; }
    public:
        explicit Driver(Adapter adapter = {}) : adapter_(adapter) {}
        void SetAdapter(Adapter adapter) { if (session_.State() == Phase::Off) adapter_ = adapter; }
        Phase State() const { return session_.State(); }
        bool MayInject() const { return session_.MayInject(); }
        bool OwnsPause() const { return session_.OwnsPause(); }

        void Enable()
        {
            if (!Ready()) return;
            const Evidence before = Observe();
            if (session_.Enable(before) != Action::PauseLive) return;
            const bool called = adapter_.pause(adapter_.context, before.owner);
            const Evidence after = Observe();
            session_.PauseFinished(after, called);
            // If pause was confirmed but admission subsequently failed, put
            // back a previously playing Live before leaving the failed start.
            if (session_.State() == Phase::Failed && after.owner == before.owner &&
                after.pauseKnown && after.paused && adapter_.resume(adapter_.context, before.owner))
            {
                const Evidence resumed = Observe();
                if (resumed.owner == before.owner && resumed.pauseKnown && !resumed.paused)
                    session_.ResetFailedStart(before.owner);
            }
        }

        void Disable()
        {
            if (!Ready()) return;
            const Evidence before = Observe();
            if (session_.Disable(before) != Action::RestoreActors) return;
            const bool restored = adapter_.restoreActors(adapter_.context, before.owner);
            if (session_.RestoreFinished(Observe(), restored) != Action::ResumeLive) return;
            const bool called = adapter_.resume(adapter_.context, before.owner);
            session_.ResumeFinished(Observe(), called);
        }

        void Check()
        {
            if (session_.State() != Phase::Off) session_.ObserveOwner(Observe());
        }
		void Observe(const Evidence& evidence) { session_.ObserveOwner(evidence); }
    };
}
