#pragma once
#include <cstdint>
#include <cmath>

namespace gbb::experiment {
// Internal pairing state. No stream pointer is retained across callbacks.
enum class Reject : unsigned { Missing, Duplicate, Phase, Generation, Identity, Expired, Caller, Native, Count };
enum class Phase { Empty, InQuartz, Ready };
struct WarmState {
    uintptr_t owner{}; uint64_t generation{}, started{}; bool paired{};
    void Prepare(uintptr_t bridge, uint64_t epoch, uint64_t now) {
        if (owner != bridge || generation != epoch) { owner=bridge; generation=epoch; started=now; paired=false; }
    }
    bool MaySkipMissing(uint64_t now, uint64_t limit) const { return !paired && now >= started && now-started < limit; }
};
template<class Source> struct Pair {
    Source source{}; // Value copy, never an AnimationStream pointer.
    uintptr_t owner{}, job{};
    uint64_t started{};
    Phase phase{Phase::Empty};
    void Reset() { *this = {}; }
    bool Begin(uintptr_t bridge, uintptr_t campus, const Source& value, uint64_t now) {
        if (phase != Phase::Empty && owner == bridge && source.generation == value.generation) return false;
        Reset(); owner = bridge; job = campus; source = value; started = now; phase = Phase::InQuartz; return true;
    }
    bool Finish(uintptr_t bridge, uint64_t generation) {
        if (owner != bridge || phase != Phase::InQuartz || source.generation != generation) { Reset(); return false; }
        phase = Phase::Ready; return true;
    }
    bool Expired(uint64_t now, uint64_t ageLimit) const {
        return now < started || now < source.ticks || now - started >= ageLimit || now - source.ticks >= ageLimit;
    }
    Reject Check(uintptr_t bridge, uintptr_t campus, uint64_t generation, uintptr_t hair,
                 uintptr_t arm, uint64_t now, uint64_t ageLimit, bool& ok) const {
        ok = false;
        if (owner != bridge || phase == Phase::Empty) return Reject::Missing;
        if (source.generation != generation) return Reject::Generation;
        if (phase != Phase::Ready) return Reject::Phase;
        if (job != campus || source.hair != hair || source.arm != arm) return Reject::Identity;
        if (Expired(now, ageLimit)) return Reject::Expired;
        ok = true; return Reject::Count;
    }
};
inline bool TimelineBoundary(int oldFrame, float oldTime, int nextFrame, float nextTime) {
    return nextFrame < 0 || nextFrame < oldFrame || nextTime + .0001f < oldTime ||
        std::fabs(nextTime - oldTime) > .25f || (nextFrame == oldFrame && nextTime != oldTime);
}
} // namespace gbb::experiment
