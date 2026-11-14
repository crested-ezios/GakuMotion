#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace GakuMotion
{
	struct SceneActorObservation
	{
		void* actor{nullptr}; // Main-thread identity only; GUI never receives it.
		int instanceId{0};
		std::string character;
		std::string displayName{};
		std::string faceName{};
	};
	struct SceneActorEntry : SceneActorObservation { uint64_t token{0}; };
	struct GuiSceneActor
	{
		uint64_t token{0};
		std::string label;
		bool selected{false};
	};

	class SceneActorRegistry
	{
		std::vector<SceneActorEntry> entries_;
		uint64_t epoch_{1}, nextToken_{1};
	public:
		uint64_t Epoch() const { return epoch_; }
		const auto& Entries() const { return entries_; }
		void Clear() { entries_.clear(); ++epoch_; }
		const SceneActorEntry* Find(void* actor) const
		{
			for (const auto& item : entries_) if (item.actor == actor) return &item;
			return nullptr;
		}
		void* Resolve(uint64_t token, uint64_t epoch) const
		{
			if (epoch != epoch_) return nullptr;
			for (const auto& item : entries_) if (item.token == token) return item.actor;
			return nullptr;
		}
		void Publish(const std::vector<SceneActorObservation>& observations)
		{
			std::vector<SceneActorEntry> next;
			for (const auto& observation : observations)
			{
				if (!observation.actor || !observation.instanceId ||
					std::any_of(next.begin(), next.end(), [&](const auto& item) { return item.actor == observation.actor; })) continue;
				const auto* old = Find(observation.actor);
				SceneActorEntry entry;
				static_cast<SceneActorObservation&>(entry) = observation;
				entry.token = old && old->instanceId == observation.instanceId ? old->token : nextToken_++;
				next.push_back(std::move(entry));
			}
			// Preserve stable order even when Unity's find result changes order.
			std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) { return a.token < b.token; });
			bool changed = next.size() != entries_.size();
			if (!changed) for (size_t i = 0; i < next.size(); ++i) changed |= next[i].token != entries_[i].token;
			if (changed) ++epoch_;
			entries_ = std::move(next);
		}
	};
}
