#pragma once

#include <cstdint>

// Value-only coordination for a future, separately verified Live adapter.
// This class never accesses a game object or changes the injection policy.
namespace GakuMotion::live_takeover
{
    enum class Phase { Off, WaitingPause, Active, WaitingRestore, WaitingResume, Failed };
    enum class Action { None, PauseLive, RestoreActors, ResumeLive };

    struct Evidence
    {
        uint64_t owner{}; // Identity token only; never dereferenced.
        bool ready{};     // Adapter has verified the Live, actors and pause APIs.
        bool pauseKnown{};
        bool paused{};
    };

    class Session
    {
        Phase phase_{Phase::Off};
        uint64_t owner_{};
        bool ownsPause_{};
        bool cancelPending_{};

        bool SameOwner(const Evidence& e) const { return e.owner != 0 && e.owner == owner_; }
        void LoseOwner() { phase_ = Phase::Off; owner_ = 0; ownsPause_ = false; cancelPending_ = false; }
		void OwnerUnavailable(const Evidence& e) { if (e.owner) LoseOwner(); else phase_ = Phase::Failed; }
    public:
        Phase State() const { return phase_; }
        bool MayInject() const { return phase_ == Phase::Active; }
        bool OwnsPause() const { return ownsPause_; }
		void ResetFailedStart(uint64_t owner)
		{
			if (phase_ == Phase::Failed && !ownsPause_ && owner && owner == owner_) LoseOwner();
		}

        Action Enable(const Evidence& e)
        {
            if (phase_ != Phase::Off || !e.owner || !e.ready || !e.pauseKnown) return Action::None;
            owner_ = e.owner;
            ownsPause_ = false;
            if (e.paused) { phase_ = Phase::Active; return Action::None; }
            phase_ = Phase::WaitingPause;
            return Action::PauseLive;
        }

        // Called only after the requested pause has completed and its result
        // has been independently observed. A failed call never grants access.
        Action PauseFinished(const Evidence& e, bool callSucceeded)
        {
            if (phase_ != Phase::WaitingPause) return Action::None;
            if (!SameOwner(e)) { OwnerUnavailable(e); return Action::None; }
            if (!callSucceeded || !e.ready || !e.pauseKnown || !e.paused)
            { phase_ = Phase::Failed; return Action::None; }
            ownsPause_ = true;
            if (cancelPending_) { phase_ = Phase::WaitingResume; return Action::ResumeLive; }
            phase_ = Phase::Active;
            return Action::None;
        }

        Action Disable(const Evidence& e)
        {
            if (phase_ == Phase::WaitingPause)
            {
                if (!SameOwner(e)) OwnerUnavailable(e);
                else cancelPending_ = true;
                return Action::None;
            }
            if (phase_ != Phase::Active) return Action::None;
            if (!SameOwner(e)) { OwnerUnavailable(e); return Action::None; }
            phase_ = Phase::WaitingRestore; // Immediately closes injection.
            return Action::RestoreActors;
        }

        Action RestoreFinished(const Evidence& e, bool restored)
        {
            if (phase_ != Phase::WaitingRestore) return Action::None;
            if (!SameOwner(e)) { OwnerUnavailable(e); return Action::None; }
            if (!restored) { phase_ = Phase::Failed; return Action::None; }
            if (!ownsPause_) { LoseOwner(); return Action::None; }
            if (!e.pauseKnown || !e.paused) { phase_ = Phase::Failed; return Action::None; }
            phase_ = Phase::WaitingResume;
            return Action::ResumeLive;
        }

        void ResumeFinished(const Evidence& e, bool callSucceeded)
        {
            if (phase_ != Phase::WaitingResume) return;
            if (!SameOwner(e)) { OwnerUnavailable(e); return; }
            if (!callSucceeded || !e.pauseKnown || e.paused) { phase_ = Phase::Failed; return; }
            LoseOwner();
        }

        // Scene loss must never cause a call on stale actor or presenter objects.
        void ObserveOwner(const Evidence& e)
        {
            if (phase_ != Phase::Off && !SameOwner(e)) OwnerUnavailable(e);
            else if (phase_ == Phase::Active && (!e.ready || !e.pauseKnown || !e.paused))
                phase_ = Phase::Failed;
        }
    };
}
