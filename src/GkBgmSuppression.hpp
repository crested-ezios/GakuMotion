#pragma once

#include <cstdint>

namespace GakuMotion::bgm
{
	// This module is game independent. An empty Backend is unavailable, never a
	// global-volume fallback. Only an explicitly identified audio category may resolve.
	// In particular, finding a method named Mute or guessing "BGM" is insufficient.
	enum class Scope : uint8_t { Unverified, BgmOnly, CharacterVoiceOnly, SoundEffectsOnly };
	struct Target
	{
		uint64_t owner{0}; // Registered audio graph / ACF identity, not an Actor pointer.
		uint64_t generation{0}; // Changes before graph/category identity can be reused.
		uint64_t id{0}; // Category zero is valid; owner/generation establish identity.
		Scope scope{Scope::Unverified};
		bool Valid() const noexcept { return owner != 0 && generation != 0 &&
			(scope == Scope::BgmOnly || scope == Scope::CharacterVoiceOnly || scope == Scope::SoundEffectsOnly); }
		bool operator==(const Target&) const = default;
	};

	struct MuteState
	{
		bool muted{false};
		// Use a trustworthy writer revision when available. Zero explicitly means
		// value-only comparison: same-value external mute writes are not detectable.
		// Category mute is independent of the player's volume setting.
		uint64_t revision{0};
		bool operator==(const MuteState&) const = default;
	};

	enum class ReadStatus : uint8_t { Ok, Unavailable, Expired, Failed };
	enum class WriteStatus : uint8_t { Applied, Conflict, Expired, FailedNoMutation, Uncertain };
	struct WriteResult
	{
		WriteStatus status{WriteStatus::FailedNoMutation};
		MuteState state{};
		// If true, state is authoritative and attributable to this attempted write.
		// An uncertain write without this proof stays pending; it is never undone
		// merely because a subsequent read happens to find the desired value.
		bool hasState{false};
	};

	struct Backend
	{
		void* context{nullptr};
		bool (*isMainThread)(void*){nullptr}; // Must be safe to call from any thread.
		ReadStatus (*resolveBgm)(void*, Target&){nullptr};
		ReadStatus (*readMute)(void*, const Target&, MuteState&){nullptr};
		// Main-thread serialized conditional write. Conflict/Expired/FailedNoMutation
		// guarantee no mutation. Applied must return confirmed state. Partial setter
		// failures are Uncertain, not FailedNoMutation. No throwing across this ABI.
		WriteResult (*compareExchangeMute)(void*, const Target&, const MuteState&, bool){nullptr};

		bool Complete() const noexcept
		{
			return isMainThread && resolveBgm && readMute && compareExchangeMute;
		}
		bool operator==(const Backend&) const = default;
	};

	enum class Status : uint8_t
	{
		Idle, Unavailable, WrongThread, InvalidAudio, Acquired, AlreadyHeld, AlreadyMuted,
		Released, Expired, ExternalChange, ReadFailed, WriteFailed, RecoveryPending
	};
	enum class ReleaseReason : uint8_t
	{
		None, PolicyInactive, TargetChanged, BackendChanged, CapabilityLost, Recovery, Stop, SceneLost, Shutdown
	};

	inline const char* StatusName(Status value) noexcept
	{
		switch (value)
		{
		case Status::Idle: return "idle";
		case Status::Unavailable: return "unavailable";
		case Status::WrongThread: return "wrong-thread";
		case Status::InvalidAudio: return "invalid-audio";
		case Status::Acquired: return "acquired";
		case Status::AlreadyHeld: return "held";
		case Status::AlreadyMuted: return "already-muted";
		case Status::Released: return "released";
		case Status::Expired: return "expired";
		case Status::ExternalChange: return "external-change";
		case Status::ReadFailed: return "read-failed";
		case Status::WriteFailed: return "write-failed";
		case Status::RecoveryPending: return "recovery-pending";
		}
		return "unknown";
	}

	struct Snapshot
	{
		Status status{Status::Idle};
		ReleaseReason releaseReason{ReleaseReason::None};
		bool held{false}, ownsMutation{false}, recoveryPending{false};
		// Once ownership was discarded without confirming the owned original,
		// later Idle/status updates or a different lease cannot prove it restored.
		bool captureUnconfirmed{false};
		bool versioned{false};
		Target target{};
		MuteState original{}, applied{};
		uint64_t sourceEpoch{0};
	};

	class Lease
	{
	public:
		Lease() = default;
		Lease(const Lease&) = delete;
		Lease& operator=(const Lease&) = delete;
		Lease(Lease&&) = delete;
		Lease& operator=(Lease&&) = delete;
		// No destructor callbacks: destruction may run off the Unity main thread.
		// The host explicitly releases while backend context/code is still alive.

		const Snapshot& GetSnapshot() const noexcept { return snapshot_; }

		Status Sync(const Backend& backend, bool effectivePlaying, uint64_t sourceEpoch)
		{
			snapshot_.sourceEpoch = sourceEpoch;
			if (!effectivePlaying || sourceEpoch == 0)
			{
				const Status result = Release(ReleaseReason::PolicyInactive);
				blocked_ = false; // The inactive interval ends any previous ownership dispute.
				if (sourceEpoch == 0 && effectivePlaying && !snapshot_.held) return Set(Status::InvalidAudio);
				return result;
			}
			if (snapshot_.recoveryPending)
			{
				const Status result = Release(ReleaseReason::Recovery);
				if (snapshot_.held) return result;
			}
			if (snapshot_.held && !(backend == backend_))
			{
				const Status result = Release(ReleaseReason::BackendChanged);
				if (snapshot_.held) return result;
			}
			if (!backend.Complete())
			{
				if (snapshot_.held) return Release(ReleaseReason::CapabilityLost);
				return Set(Status::Unavailable);
			}
			if (!backend.isMainThread(backend.context)) return Set(Status::WrongThread);
			Target next;
			const ReadStatus resolved = backend.resolveBgm(backend.context, next);
			if (resolved != ReadStatus::Ok || !next.Valid())
			{
				if (snapshot_.held) return Release(ReleaseReason::CapabilityLost);
				return Set(Status::Unavailable);
			}
			if (snapshot_.held && !(next == snapshot_.target))
			{
				const Status result = Release(ReleaseReason::TargetChanged);
				if (snapshot_.held) return result;
			}
			// An external owner or failed write wins for this source/target. Do not
			// fight it every LateUpdate; an inactive interval/new source permits retry.
			if (blocked_ && blockedEpoch_ == sourceEpoch && next == blockedTarget_) return Set(blockedStatus_);
			if (blocked_) blocked_ = false;

			MuteState observed;
			const ReadStatus read = backend.readMute(backend.context, next, observed);
			if (read != ReadStatus::Ok)
			{
				if (snapshot_.held)
				{
					if (read == ReadStatus::Expired) { Clear(); return Set(Status::Expired); }
					snapshot_.recoveryPending = true;
				}
				return Set(Status::ReadFailed);
			}
			if (snapshot_.held)
			{
				if (observed == snapshot_.applied) return Set(Status::AlreadyHeld);
				Block(next, sourceEpoch, Status::ExternalChange);
				Clear(); // The external setting is authoritative; never undo it.
				return Set(Status::ExternalChange);
			}
			backend_ = backend;
			snapshot_.target = next;
			snapshot_.original = snapshot_.applied = observed;
			snapshot_.held = true;
			snapshot_.releaseReason = ReleaseReason::None;
			snapshot_.versioned = observed.revision != 0;
			proof_ = true;
			if (observed.muted) return Set(Status::AlreadyMuted);

			const WriteResult write = backend.compareExchangeMute(backend.context, next, observed, true);
			if (write.status == WriteStatus::Applied && write.hasState && write.state.muted)
			{
				snapshot_.applied = write.state;
				snapshot_.ownsMutation = true;
				snapshot_.versioned = write.state.revision != 0;
				return Set(Status::Acquired);
			}
			Block(next, sourceEpoch, write.status == WriteStatus::Conflict ? Status::ExternalChange : Status::WriteFailed);
			if (write.status == WriteStatus::Uncertain || write.status == WriteStatus::Applied)
			{
				snapshot_.ownsMutation = snapshot_.recoveryPending = true;
				proof_ = write.hasState;
				if (proof_) snapshot_.applied = write.state;
				return Set(Status::RecoveryPending);
			}
			Clear();
			return Set(write.status == WriteStatus::Expired ? Status::Expired : blockedStatus_);
		}

		Status Release(ReleaseReason reason)
		{
			snapshot_.releaseReason = reason;
			if (!snapshot_.held) return Set(Status::Idle);
			if (!snapshot_.ownsMutation) { Clear(true); return Set(Status::Released); }
			if (!backend_.isMainThread(backend_.context)) return Set(Status::WrongThread);
			MuteState observed;
			const ReadStatus read = backend_.readMute(backend_.context, snapshot_.target, observed);
			if (read == ReadStatus::Expired) { Clear(); return Set(Status::Expired); }
			if (read != ReadStatus::Ok)
			{
				snapshot_.recoveryPending = true;
				return Set(Status::RecoveryPending);
			}
			if (observed == snapshot_.original)
			{
				Clear(true);
				return Set(Status::Released);
			}
			if (!proof_)
			{
				snapshot_.recoveryPending = true;
				return Set(Status::RecoveryPending);
			}
			if (!(observed == snapshot_.applied))
			{
				Block(snapshot_.target, snapshot_.sourceEpoch, Status::ExternalChange);
				Clear();
				return Set(Status::ExternalChange);
			}
			const WriteResult write = backend_.compareExchangeMute(
				backend_.context, snapshot_.target, observed, snapshot_.original.muted);
			if (write.hasState && write.state.muted == snapshot_.original.muted &&
				(write.status == WriteStatus::Applied || write.status == WriteStatus::Uncertain))
			{
				Clear(true);
				return Set(Status::Released);
			}
			if (write.status == WriteStatus::Expired) { Clear(); return Set(Status::Expired); }
			if (write.status == WriteStatus::Conflict)
			{
				Block(snapshot_.target, snapshot_.sourceEpoch, Status::ExternalChange);
				Clear();
				return Set(Status::ExternalChange);
			}
			snapshot_.recoveryPending = true;
			if (write.status != WriteStatus::FailedNoMutation)
			{
				proof_ = write.hasState;
				if (proof_) snapshot_.applied = write.state;
			}
			return Set(Status::RecoveryPending);
		}

	private:
		Snapshot snapshot_;
		Backend backend_;
		bool proof_{false}, blocked_{false};
		Target blockedTarget_{};
		uint64_t blockedEpoch_{0};
		Status blockedStatus_{Status::Idle};
		Status Set(Status status) noexcept { snapshot_.status = status; return status; }
		void Clear(bool originalConfirmed = false) noexcept
		{
			if (snapshot_.ownsMutation && !originalConfirmed) snapshot_.captureUnconfirmed = true;
			snapshot_.held = snapshot_.ownsMutation = snapshot_.recoveryPending = false;
			backend_ = {};
			proof_ = false;
		}
		void Block(const Target& target, uint64_t epoch, Status reason) noexcept
		{
			blocked_ = true;
			blockedTarget_ = target;
			blockedEpoch_ = epoch;
			blockedStatus_ = reason;
		}
	};

	inline bool ReleaseForCapture(Lease& lease)
	{
		const Snapshot before = lease.GetSnapshot();
		if (!before.held && !before.ownsMutation)
		{
			// Expiry or another owner winning the race is not confirmation that
			// GM's original value was restored. Preserve this diagnostic status.
			return !before.captureUnconfirmed && !before.recoveryPending && before.status != Status::Expired &&
				before.status != Status::ExternalChange;
		}
		const Status result = lease.Release(ReleaseReason::PolicyInactive);
		const Snapshot after = lease.GetSnapshot();
		return result == Status::Released && !after.captureUnconfirmed && !after.held &&
			!after.ownsMutation && !after.recoveryPending;
	}
}
