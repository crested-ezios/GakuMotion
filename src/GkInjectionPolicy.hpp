#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace GakuMotion::injection
{
	// OutGame hosts both Home and Costume. Its scene name alone never grants
	// permission. Costume must be selected from the actual page lifecycle.
	enum class Context : uint8_t { Unknown, Photography, Costume, Live };
	enum class OwnerRole : uint8_t { Other, Photography, OutGame, Live };

	// Value-only presenter evidence. PreviewContext below also requires an
	// exact Active Scene observed in the human-operated DevReview samples.
	struct OwnerSnapshot
	{
		bool confirmed{false};
		uintptr_t presenterIdentity{0}; // Equality token only; never dereferenced.
		int sceneHandle{0};
		bool sceneLoaded{false};
		OwnerRole role{OwnerRole::Other};
		bool costumePage{false};

		bool Valid() const noexcept
		{
			return confirmed && presenterIdentity != 0 && sceneHandle != 0 && sceneLoaded;
		}
		Context AllowedContext() const noexcept
		{
			if (!Valid()) return Context::Unknown;
			if (role == OwnerRole::Photography) return Context::Photography;
			if (role == OwnerRole::OutGame && costumePage) return Context::Costume;
			if (role == OwnerRole::Live) return Context::Live;
			return Context::Unknown;
		}
		bool OwnsActor(int actorSceneHandle) const noexcept
		{
			return AllowedContext() != Context::Unknown && actorSceneHandle == sceneHandle;
		}
		bool SameOwner(const OwnerSnapshot& other) const noexcept
		{
			return Valid() && other.Valid() && presenterIdentity == other.presenterIdentity &&
				sceneHandle == other.sceneHandle && role == other.role;
		}
		bool ReplacedBy(const OwnerSnapshot& other) const noexcept
		{
			return Valid() && other.Valid() && !SameOwner(other);
		}
	};

	inline bool PreviewPhotographyScene(std::string_view name) noexcept
	{
		return name == "Photography" || name == "env_3d_photo_hotspring-00-00-night" ||
			name == "env_3d_adv_vocalroom-00-00-noon" || name == "env_3d_adv_danceroom-00-00-noon" ||
			name == "env_3d_adv_visualroom-00-00-noon" || name == "env_3d_adv_danceroom-00-00-night" ||
			name == "env_3d_adv_rooftop-00-00-noon" || name == "env_3d_adv_riverbed-00-00-noon" ||
			name == "env_3d_adv_riverbed-00-00-evening" || name == "env_3d_photo_studio-00-00-noon" ||
			name == "env_3d_adv_studio-01-00-noon";
	}

	inline Context PreviewContext(std::string_view activeScene, std::string_view presenterScene,
		const OwnerSnapshot& owner) noexcept
	{
		if (!owner.Valid()) return Context::Unknown;
		if (owner.role == OwnerRole::Photography && presenterScene == "Photography" &&
			PreviewPhotographyScene(activeScene)) return Context::Photography;
		if (activeScene == "env_3d_dress_dressroom-00-00-noon" && presenterScene == "OutGame" &&
			owner.role == OwnerRole::OutGame && owner.costumePage) return Context::Costume;
		return Context::Unknown;
	}

	// Explicit per-session Live admission. The ordinary preview policy above
	// never grants Live merely from a scene name or a discovered owner.
	inline Context LiveTakeoverContext(const OwnerSnapshot& owner, uintptr_t expectedOwner, int expectedScene,
		bool enabled, bool viewing, bool paused, bool liveGuardReady) noexcept
	{
		return enabled && viewing && paused && liveGuardReady && owner.Valid() &&
			owner.role == OwnerRole::Live && expectedOwner && expectedScene &&
			owner.presenterIdentity == expectedOwner && owner.sceneHandle == expectedScene
			? Context::Live : Context::Unknown;
	}

	struct Snapshot
	{
		bool sceneAllowed{false};
		bool captureHooksReady{false};
		uint64_t captureDepth{0};
		bool CanInject() const noexcept
		{
			return sceneAllowed && captureHooksReady && captureDepth == 0;
		}
		bool CanUseGui() const noexcept { return CanInject(); }
	};

	class Policy
	{
		static constexpr uint64_t SceneBit = 1, CaptureBit = 2, DepthUnit = 4;
		// One word publishes a coherent snapshot across the Unity and UI threads.
		std::atomic<uint64_t> state_{0};
		void SetBit(uint64_t bit, bool value) noexcept
		{
			if (value) state_.fetch_or(bit, std::memory_order_acq_rel);
			else state_.fetch_and(~bit, std::memory_order_acq_rel);
		}
	public:
		void SetContext(Context value) noexcept { SetSceneAllowed(value != Context::Unknown); }
		void SetSceneAllowed(bool value) noexcept { SetBit(SceneBit, value); }
		void SetSceneName(std::string_view name) noexcept
		{
			SetSceneAllowed(name == "Photography");
		}
		void SetCaptureHooksReady(bool value) noexcept { SetBit(CaptureBit, value); }
		void BeginCapture() noexcept { state_.fetch_add(DepthUnit, std::memory_order_acq_rel); }
		// A duplicate completion must not underflow into a permanent huge depth.
		bool EndCapture() noexcept
		{
			uint64_t prior = state_.load(std::memory_order_acquire);
			while (prior >= DepthUnit)
			{
				if (state_.compare_exchange_weak(prior, prior - DepthUnit, std::memory_order_acq_rel)) return true;
			}
			return false;
		}
		Snapshot Read() const noexcept
		{
			const uint64_t value = state_.load(std::memory_order_acquire);
			return { (value & SceneBit) != 0, (value & CaptureBit) != 0, value / DepthUnit };
		}
		bool CanInject() const noexcept { return Read().CanInject(); }
		bool CanUseGui() const noexcept { return Read().CanUseGui(); }
	};
}
