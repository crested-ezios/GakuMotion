#pragma once

#include "GkMotionData.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace GakuMotion::photography_ensemble_probe
{
    inline constexpr unsigned SlotLimit = 3, FrameBudget = 120;
    struct Pose
    {
        Vector3 position;
        Quaternion rotation;
        bool available{};
    };
    inline bool Finite(const Pose& p) noexcept
    {
        const auto& q = p.rotation;
        const float norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
        return p.available && std::isfinite(p.position.x) && std::isfinite(p.position.y) &&
            std::isfinite(p.position.z) && std::isfinite(norm) && norm > .5f && norm < 1.5f;
    }
    inline bool Near(const Pose& a, const Pose& b) noexcept
    {
        if (!Finite(a) || !Finite(b)) return false;
        const auto p = a.position - b.position;
        const auto qa = a.rotation.Normalized(), qb = b.rotation.Normalized();
        const float dot = std::fabs(qa.x*qb.x + qa.y*qb.y + qa.z*qb.z + qa.w*qb.w);
        return p.x*p.x + p.y*p.y + p.z*p.z <= 1e-8f && dot >= .999999f;
    }
    // Extra actors do not have an official PhotographyActor wrapper. Their
    // callback identity is evidence only within the same verified scene and
    // roster epoch; a changed rig, owner, or reused address starts over.
    struct ExtraActorSample
    {
        uintptr_t actor{}, root{}, hips{}, rootParent{}, hipsParent{}, owner{};
        uint64_t token{}, epoch{}, frame{};
        int scene{}, instance{};
        Pose rootPose;
    };
    class ExtraActorEvidence
    {
        struct Entry { ExtraActorSample sample; bool stable{}; };
        std::vector<Entry> entries_;
        static bool SameIdentity(const ExtraActorSample& a, const ExtraActorSample& b)
        {
            return a.actor && a.actor == b.actor && a.root && a.root == b.root && a.hips && a.hips == b.hips &&
                a.rootParent == b.rootParent && a.hipsParent == b.hipsParent && a.owner && a.owner == b.owner &&
                a.token && a.token == b.token && a.epoch && a.epoch == b.epoch && a.scene && a.scene == b.scene &&
                a.instance && a.instance == b.instance && Near(a.rootPose, b.rootPose);
        }
    public:
        void Reset() { entries_.clear(); }
        void Observe(const ExtraActorSample& sample)
        {
            if (!sample.actor || !sample.frame) return;
            for (auto& entry : entries_) if (entry.sample.actor == sample.actor)
            {
                if (sample.frame == entry.sample.frame)
                {
                    if (!SameIdentity(entry.sample, sample)) entry.stable = false;
                }
                else entry.stable = sample.frame == entry.sample.frame + 1 && SameIdentity(entry.sample, sample);
                entry.sample = sample;
                return;
            }
            entries_.push_back({sample, false});
        }
        bool Eligible(const ExtraActorSample& sample) const
        {
            for (const auto& entry : entries_) if (entry.sample.actor == sample.actor)
                return entry.stable && entry.sample.frame <= sample.frame && sample.frame - entry.sample.frame <= 1 &&
                    SameIdentity(entry.sample, sample);
            return false;
        }
    };
    struct Slot
    {
        uintptr_t actor{}, actorRoot{}, hips{}, actorParent{}, hipsParent{}; // Equality tokens only.
        int instanceId{}, controllerIndex{-1};
        bool built{}, identityAvailable{}, rosterMatched{};
        Pose actorPose, beforePose, appliedPose;
        unsigned callbacks{};
    };
    struct Frame
    {
        uint64_t number{};
        uintptr_t owner{}, layout{};
        int scene{}, capacity{};
        bool available{};
        std::array<Slot, SlotLimit> slots{};
    };
    struct Evidence
    {
        unsigned frames{}, populatedSlots{}, matchingSlots{}, stableActorRoots{}, changedHipsAfterApply{};
        bool snapshotAvailable{}, crossFrameAvailable{}, appliedComparisonAvailable{};
        bool readyForInjection{};
        bool visualVerified{false}; // Observation never certifies rendered choreography.
    };
    inline bool StableIdentity(const Frame& certified, const Frame& current)
    {
        if (!current.available || !certified.available || certified.owner != current.owner ||
            certified.scene != current.scene || certified.layout != current.layout || certified.capacity != current.capacity) return false;
        for (unsigned i = 0; i < SlotLimit; ++i)
        {
            const auto& a = certified.slots[i]; const auto& b = current.slots[i];
            if (a.actor != b.actor || a.instanceId != b.instanceId || a.actorRoot != b.actorRoot || a.hips != b.hips ||
                a.actorParent != b.actorParent || a.hipsParent != b.hipsParent ||
                a.identityAvailable != b.identityAvailable || (b.identityAvailable && (!b.rosterMatched || !Near(a.actorPose, b.actorPose)))) return false;
        }
        return true;
    }
    class ReadinessLatch
    {
        Frame certified_{};
        bool ready_{};
    public:
        void Reset() { certified_ = {}; ready_ = false; }
        bool Validate(const Frame& current)
        {
            if (ready_ && !StableIdentity(certified_, current)) ready_ = false;
            return ready_;
        }
        bool Observe(const Frame& current, const Evidence& evidence, bool allowAcquire = true)
        {
            Validate(current);
            if (!ready_ && allowAcquire && evidence.readyForInjection)
            { certified_ = current; ready_ = true; }
            return ready_;
        }
    };
    class Session
    {
        Frame previous_{};
        unsigned frames_{};
    public:
        void Reset() { previous_ = {}; frames_ = 0; }
        bool Exhausted() const noexcept { return frames_ >= FrameBudget; }
        static Evidence Compare(const Frame& previous_, const Frame& next)
        {
            Evidence e;
            e.snapshotAvailable = next.available && next.owner && next.scene && next.capacity > 0 &&
                next.capacity <= int(SlotLimit);
            const bool sameOwner = previous_.available && previous_.owner == next.owner &&
                previous_.scene == next.scene && previous_.layout == next.layout &&
                previous_.capacity == next.capacity && previous_.number < next.number;
            bool callbackEvidence = true, rosterEvidence = true;
            for (unsigned i = 0; i < SlotLimit; ++i)
            {
                const auto& current = next.slots[i]; const auto& before = previous_.slots[i];
                if (current.identityAvailable && current.actor && current.instanceId)
                {
                    ++e.populatedSlots;
                    rosterEvidence &= current.rosterMatched;
                    callbackEvidence &= current.callbacks > 0 && before.callbacks > 0;
                }
                if (!sameOwner || !current.identityAvailable || !before.identityAvailable ||
                    current.actor != before.actor || current.instanceId != before.instanceId ||
                    current.actorRoot != before.actorRoot || !current.actorRoot || current.hips != before.hips ||
                    !current.hips || current.actorParent != before.actorParent || current.hipsParent != before.hipsParent) continue;
                ++e.matchingSlots;
                if (Finite(current.actorPose) && Finite(before.actorPose))
                {
                    e.crossFrameAvailable = true;
                    if (Near(current.actorPose, before.actorPose)) ++e.stableActorRoots;
                }
                if (Finite(before.appliedPose) && Finite(current.beforePose))
                {
                    e.appliedComparisonAvailable = true;
                    if (!Near(before.appliedPose, current.beforePose)) ++e.changedHipsAfterApply;
                }
            }
            e.readyForInjection = e.snapshotAvailable && sameOwner && e.populatedSlots >= 1 &&
                e.matchingSlots == e.populatedSlots && e.stableActorRoots == e.populatedSlots &&
                callbackEvidence && rosterEvidence;
            return e;
        }
        Evidence Observe(const Frame& next)
        {
            if (Exhausted()) { Evidence e; e.frames = frames_; return e; }
            auto e = Compare(previous_, next);
            if (!next.available)
            {
                e.frames = frames_;
                return e;
            }
            e.frames = ++frames_;
            previous_ = next;
            return e;
        }
    };
}
