#pragma once

#include "GkMotionData.hpp"
#include "GkBlendshapePlayer.hpp"
#include "GkBoneOverride.hpp"
#include "GkRuntimeConfig.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace GakuMotion
{
	// =============================================================================
	// StageFormation — Unified multi-actor slot assignment and customization model
	// =============================================================================
	// Replaces legacy optional ensemble drafts/presets with an always-on formation
	// that mirrors the active stage slots (1号位..N号位):
	//   - Solo tracks or empty selection: max(1, actorCount) slots
	//   - Group tracks: max(actorCount, trackPositionCount) slots
	// Slot properties (offset, yawDegrees, sourceSlotIndex) stay with the slot
	// across drag-drop swaps; actor properties (facialWeights, boneOverrides)
	// follow the actor entity.
	// =============================================================================

	struct StageSlotState
	{
		int slotIndex{0};
		uint64_t actorToken{0};
		int actorInstanceId{0};
		std::string character;
		std::string displayName;
		std::string shortName;
		bool occupied{false};
		bool isOverflow{false};
		int sourceSlotIndex{0}; // 0-based position index within current track
		Vector3 offset{0.0f, 0.0f, 0.0f};
		float yawDegrees{0.0f};
		bool placementDirty{false};
		FacialWeights facialWeights;
		bool hasActorBoneOverrides{false};
		std::string physicsBackend;
	};

	struct ActorCustomization
	{
		FacialWeights facialWeights;
		BoneOverrideSet actorMotionOverrides;
		BoneOverrideSet effectiveOverrides;
		bool facialCustomized{false};
		bool overridesLoaded{false};
		std::string loadedSlotDir;
	};

	inline Vector3 ComputeDefaultSlotOffset(const MotionEntry* motion, int slotIndex,
	                                        const Vector3& slot0Default,
	                                        const std::vector<StageSlotState>& currentSlots) noexcept
	{
		if (slotIndex <= 0) return slot0Default;
		const bool isGroup = motion && motion->isGroup && motion->positionCount >= 2;
		const int trackPosCount = isGroup ? motion->positionCount : 1;
		if (isGroup && slotIndex < trackPosCount)
		{
			const int targetSlot1Based = slotIndex + 1;
			for (const auto& pos : motion->positions)
			{
				if (pos.slot == targetSlot1Based)
				{
					return Vector3{pos.x, pos.y, pos.z};
				}
			}
			return Vector3{0.0f, 0.0f, 0.0f};
		}
		if (slotIndex - 1 >= 0 && slotIndex - 1 < static_cast<int>(currentSlots.size()))
		{
			const auto& prev = currentSlots[static_cast<size_t>(slotIndex - 1)].offset;
			return Vector3{prev.x + 0.5f, prev.y, prev.z};
		}
		return Vector3{0.5f * static_cast<float>(slotIndex), 0.0f, 0.0f};
	}

	inline float ComputeDefaultSlotYaw(const MotionEntry* motion, int slotIndex) noexcept
	{
		if (slotIndex <= 0 || !motion || !motion->isGroup || motion->positionCount < 2)
		{
			return 0.0f;
		}
		if (slotIndex >= motion->positionCount) return 0.0f;
		const int targetSlot1Based = slotIndex + 1;
		for (const auto& pos : motion->positions)
		{
			if (pos.slot == targetSlot1Based)
			{
				return std::isfinite(pos.yawDegrees) ? std::remainder(pos.yawDegrees, 360.0f) : 0.0f;
			}
		}
		return 0.0f;
	}

	struct StageFormation
	{
		bool active{false};
		bool available{false};
		bool verified{false};
		bool isGroupSong{false};
		int trackPositionCount{1};
		int slotCount{1};
		int selectedSlotIndex{0};
		uint64_t revision{1};
		uint64_t actorEpoch{0};
		std::string notice;
		std::vector<StageSlotState> slots;

		const StageSlotState* SelectedSlot() const noexcept
		{
			if (selectedSlotIndex < 0 || selectedSlotIndex >= static_cast<int>(slots.size()))
			{
				return nullptr;
			}
			return &slots[static_cast<size_t>(selectedSlotIndex)];
		}

		StageSlotState* SelectedSlot() noexcept
		{
			if (selectedSlotIndex < 0 || selectedSlotIndex >= static_cast<int>(slots.size()))
			{
				return nullptr;
			}
			return &slots[static_cast<size_t>(selectedSlotIndex)];
		}

		int FindSlotIndexByToken(uint64_t token) const noexcept
		{
			if (token == 0) return -1;
			for (size_t i = 0; i < slots.size(); ++i)
			{
				if (slots[i].occupied && slots[i].actorToken == token)
				{
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		int OccupiedCount() const noexcept
		{
			int count = 0;
			for (const auto& s : slots)
			{
				if (s.occupied && s.actorToken != 0) ++count;
			}
			return count;
		}
	};
} // namespace GakuMotion
