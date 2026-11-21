#pragma once
#include "GkCharacterNames.hpp"
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
		std::string presetName{};
		std::string shortName{};
	};
	inline std::string GakuActorsPresetName(const std::string& name)
	{
		const std::string marker=" | GakuActors[", suffix="] | preset:";
		const auto pos=name.find(marker);
		if(pos==std::string::npos||pos==0||pos>32)return {};
		for(size_t i=0;i<pos;++i)if(!((name[i]>='a'&&name[i]<='z')||(name[i]>='0'&&name[i]<='9')))return {};
		const auto end=name.find(suffix,pos+marker.size());
		if(end==std::string::npos||end==pos+marker.size())return {};
		for(size_t i=pos+marker.size();i<end;++i)if(!((name[i]>='0'&&name[i]<='9')||(i==pos+marker.size()&&name[i]=='-')))return {};
		const auto label=name.substr(end+suffix.size());
		if(label.empty()||label.size()>160)return {};
		for(unsigned char c:label)if(c<32)return {};
		return label;
	}
	struct SceneActorEntry : SceneActorObservation { uint64_t token{0}; };
	struct GuiSceneActor
	{
		uint64_t token{0};
		std::string label;
		bool selected{false};
		std::string character{};
		std::string displayName{};
		std::string shortName{};
	};

	/// Resolves display name for UI/bridge metadata without changing FormatSceneActorLabel.
	inline std::string ResolveSceneActorDisplayName(const SceneActorObservation& item)
	{
		if (!item.presetName.empty()) return item.presetName;
		if (!item.displayName.empty()) return item.displayName;
		if (!item.character.empty())
		{
			if (const char* cn = LookupIdolChineseName(item.character.c_str())) return cn;
			return item.character;
		}
		return {};
	}

	/// Resolves compact idol card name (short given name -> preset -> displayName -> fallback).
	inline std::string ResolveSceneActorShortName(const SceneActorObservation& item)
	{
		if (!item.shortName.empty()) return item.shortName;
		if (!item.presetName.empty()) return item.presetName;
		if (!item.character.empty())
		{
			if (const char* sn = LookupIdolShortName(item.character.c_str())) return sn;
		}
		if (!item.displayName.empty()) return item.displayName;
		if (!item.character.empty())
		{
			if (const char* cn = LookupIdolChineseName(item.character.c_str())) return cn;
			return item.character;
		}
		return "未识别角色";
	}

	/// Formats a scene actor's UI display label, preferring any user preset name.
	inline std::string FormatSceneActorLabel(const SceneActorObservation& item, size_t oneBasedIndex)
	{
		const std::string name = !item.presetName.empty() ? item.presetName
			: !item.displayName.empty() ? item.displayName + " (" + item.character + ")"
			: item.character.empty() ? "未识别角色"
			: FormatIdolDisplayName(item.character.c_str());
		return name + " · " + std::to_string(oneBasedIndex);
	}

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
				if (entry.shortName.empty()) entry.shortName = ResolveSceneActorShortName(entry);
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
