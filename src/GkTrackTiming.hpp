#pragma once

#include "GkJsonLite.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>

namespace GakuMotion
{
	struct TrackTiming
	{
		float speed{1.0f};
		float offsetSeconds{0.0f}; // Positive delays the track on the performance timeline.
	};

	struct PerformanceTiming
	{
		TrackTiming motion, facial, camera, audio;
	};

	inline bool ValidTrackTiming(const TrackTiming& timing) noexcept
	{
		return std::isfinite(timing.speed) && timing.speed >= 0.0f && std::isfinite(timing.offsetSeconds);
	}

	inline bool ValidPerformanceTiming(const PerformanceTiming& timing) noexcept
	{
		return ValidTrackTiming(timing.motion) && ValidTrackTiming(timing.facial) &&
			ValidTrackTiming(timing.camera) && ValidTrackTiming(timing.audio);
	}

	inline float TrackSourceTime(float timelineSeconds, const TrackTiming& timing) noexcept
	{
		if (!std::isfinite(timelineSeconds) || !ValidTrackTiming(timing)) return 0.0f;
		const double source = (static_cast<double>(timelineSeconds) - timing.offsetSeconds) * timing.speed;
		return static_cast<float>(std::clamp(source, 0.0, static_cast<double>((std::numeric_limits<float>::max)())));
	}

	inline float TrackTimelineDuration(float sourceDuration, const TrackTiming& timing) noexcept
	{
		if (!std::isfinite(sourceDuration) || sourceDuration < 0.0f || !ValidTrackTiming(timing)) return 0.0f;
		// A frozen track holds frame zero; retain a finite inspection/playback span.
		const double duration = timing.speed > 0.0f
			? static_cast<double>(sourceDuration) / timing.speed + timing.offsetSeconds
			: static_cast<double>(sourceDuration) + (std::max)(0.0f, timing.offsetSeconds);
		return static_cast<float>(std::clamp(duration, 0.0, static_cast<double>((std::numeric_limits<float>::max)())));
	}

	namespace detail
	{
		inline bool ReadTrackTimingObject(const jsonlite::Value& root, const char* key, TrackTiming& target)
		{
			const auto* value = root.Find(key);
			if (!value || !value->IsObject()) return false;
			if (const auto* speed = value->Find("speed"); speed && speed->IsNumber())
			{
				const float parsed = speed->AsFloat(target.speed);
				if (std::isfinite(parsed) && parsed >= 0.0f) target.speed = parsed;
			}
			if (const auto* offset = value->Find("offset_seconds"); offset && offset->IsNumber())
			{
				const float parsed = offset->AsFloat(target.offsetSeconds);
				if (std::isfinite(parsed)) target.offsetSeconds = parsed;
			}
			return true;
		}

		inline std::string TrackTimingJson(const TrackTiming& value)
		{
			const auto number = [](float v) {
				char buffer[64]{};
				const auto result = std::to_chars(buffer, buffer + sizeof(buffer), v);
				return std::string(buffer, result.ptr);
			};
			return "{\"speed\":" + number(value.speed) + ",\"offset_seconds\":" + number(value.offsetSeconds) + "}";
		}
	}

	inline PerformanceTiming ReadPerformanceTiming(const jsonlite::Value& info)
	{
		PerformanceTiming result;
		const auto* root = info.Find("track_timing");
		if (!root || !root->IsObject()) return result;
		detail::ReadTrackTimingObject(*root, "motion", result.motion);
		detail::ReadTrackTimingObject(*root, "facial", result.facial);
		detail::ReadTrackTimingObject(*root, "camera", result.camera);
		detail::ReadTrackTimingObject(*root, "audio", result.audio);
		return result;
	}

	inline bool ReadSlotTrackTimings(const jsonlite::Value& posObj, const PerformanceTiming& fallback,
		TrackTiming& outMotion, TrackTiming& outFacial, bool* outHasExplicit = nullptr)
	{
		outMotion = fallback.motion;
		outFacial = fallback.facial;
		const auto* root = posObj.Find("track_timing");
		if (!root || !root->IsObject())
		{
			if (outHasExplicit) *outHasExplicit = false;
			return false;
		}
		const bool hasMotion = detail::ReadTrackTimingObject(*root, "motion", outMotion);
		const bool hasFacial = detail::ReadTrackTimingObject(*root, "facial", outFacial);
		const bool explicitTiming = hasMotion || hasFacial;
		if (outHasExplicit) *outHasExplicit = explicitTiming;
		return explicitTiming;
	}

	inline std::string SlotTrackTimingJson(const TrackTiming& motion, const TrackTiming& facial)
	{
		return "{\"motion\":" + detail::TrackTimingJson(motion) +
			",\"facial\":" + detail::TrackTimingJson(facial) + "}";
	}

	inline std::string PerformanceTimingJson(const PerformanceTiming& timing)
	{
		return "{\"motion\":" + detail::TrackTimingJson(timing.motion) +
			",\"facial\":" + detail::TrackTimingJson(timing.facial) +
			",\"camera\":" + detail::TrackTimingJson(timing.camera) +
			",\"audio\":" + detail::TrackTimingJson(timing.audio) + "}";
	}
}
