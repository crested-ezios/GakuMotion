#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace GakuMotion
{
	// Value-only configuration: Unity object pointers never cross to the GUI.
	// Actor tokens are valid only for the registry generation that published them.
	struct EnsemblePart
	{
		uint64_t actorToken{0};
		std::string motionKey;
		float offsetSeconds{0.0f};
	};

	class EnsembleAssignments
	{
	public:
		static constexpr std::size_t kMaxParts = 3;
		using Parts = std::array<EnsemblePart, kMaxParts>;

		bool Set(std::size_t index, uint64_t actorToken, uint64_t actorEpoch,
		         std::string motionKey, float offsetSeconds)
		{
			if (index >= kMaxParts || !actorToken || !actorEpoch || motionKey.empty() ||
			    !std::isfinite(offsetSeconds) || (epoch_ && epoch_ != actorEpoch)) return false;
			for (std::size_t i = 0; i < parts_.size(); ++i)
				if (i != index && parts_[i].actorToken == actorToken) return false;
			epoch_ = actorEpoch;
			parts_[index] = {actorToken, std::move(motionKey), offsetSeconds};
			return true;
		}

		void Clear(std::size_t index)
		{
			if (index >= kMaxParts) return;
			parts_[index] = {};
			bool any = false;
			for (const auto& part : parts_) any |= part.actorToken != 0;
			if (!any) epoch_ = 0;
		}

		void Reset() { parts_ = {}; epoch_ = 0; }
		bool IsCurrent(uint64_t epoch) const { return epoch_ && epoch_ == epoch; }
		uint64_t Epoch() const { return epoch_; }
		const Parts& Get() const { return parts_; }

		// Each action is sampled from the same transport time. A positive offset
		// delays its start; clamp before its start and after its end.
		static float SampleTime(float transport, float offset, float duration)
		{
			if (!std::isfinite(transport) || !std::isfinite(offset) ||
			    !std::isfinite(duration) || duration <= 0) return 0.0f;
			const float time = transport - offset;
			return time <= 0 ? 0.0f : time >= duration ? duration : time;
		}

	private:
		Parts parts_{};
		uint64_t epoch_{0};
	};
}
