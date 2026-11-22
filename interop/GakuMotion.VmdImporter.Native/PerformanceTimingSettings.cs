namespace GakuMotion.VmdImporter.Native;

using System.Text.Json;
using System.Text.Json.Nodes;

/// <summary>Playback metadata only; source animation and WAV bytes stay unchanged.</summary>
internal sealed class PerformanceTimingSettings
{
    internal sealed record Track(float Speed = 1.0f, float OffsetSeconds = 0.0f)
    {
        public JsonObject ToJson() => new()
        {
            ["speed"] = Speed == 0.0f ? 0.0f : Speed,
            ["offset_seconds"] = OffsetSeconds == 0.0f ? 0.0f : OffsetSeconds
        };
    }

    public Track Motion { get; init; } = new();
    public Track Facial { get; init; } = new();
    public Track Camera { get; init; } = new();
    public Track Audio { get; init; } = new();

    public static PerformanceTimingSettings? ParseOptional(JsonElement request)
    {
        // Old API callers keep existing metadata on edits. New imports default
        // to unit speed and zero offset when the field is absent.
        if (!TryGetTimingObject(request, out JsonElement root)) return null;
        return new PerformanceTimingSettings
        {
            Motion = ReadTrack(root, "motion"),
            Facial = ReadTrack(root, "facial"),
            Camera = ReadTrack(root, "camera"),
            Audio = ReadTrack(root, "audio")
        };
    }

    public static PerformanceTimingSettings ReadFromInfoObject(JsonObject? info)
    {
        if (info?["track_timing"] is not JsonObject timingObj)
            return new PerformanceTimingSettings();
        return new PerformanceTimingSettings
        {
            Motion = ReadTrackFromJsonObject(timingObj, "motion", new Track()),
            Facial = ReadTrackFromJsonObject(timingObj, "facial", new Track()),
            Camera = ReadTrackFromJsonObject(timingObj, "camera", new Track()),
            Audio = ReadTrackFromJsonObject(timingObj, "audio", new Track())
        };
    }

    public PerformanceTimingSettings WithSlot1Timing(SlotTimingSettings slot1) => new()
    {
        Motion = slot1.Motion,
        Facial = slot1.Facial,
        Camera = Camera,
        Audio = Audio
    };

    public JsonObject ToJson() => new()
    {
        ["motion"] = Motion.ToJson(),
        ["facial"] = Facial.ToJson(),
        ["camera"] = Camera.ToJson(),
        ["audio"] = Audio.ToJson()
    };

    internal static bool TryGetTimingObject(JsonElement container, out JsonElement root)
    {
        if (!container.TryGetProperty("track_timing", out root) ||
            root.ValueKind is JsonValueKind.Null or JsonValueKind.Undefined)
        {
            root = default;
            return false;
        }
        if (root.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("track_timing must be an object");
        return true;
    }

    internal static Track ReadTrack(JsonElement root, string key)
    {
        if (!root.TryGetProperty(key, out JsonElement track)) return new();
        if (track.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException($"track_timing.{key} must be an object");
        float Number(string name, float fallback)
        {
            if (!track.TryGetProperty(name, out JsonElement number)) return fallback;
            if (number.ValueKind != JsonValueKind.Number || !number.TryGetSingle(out float value) || !float.IsFinite(value))
                throw new InvalidDataException($"track_timing.{key}.{name} must be finite");
            return value == 0.0f ? 0.0f : value;
        }
        float speed = Number("speed", 1.0f);
        if (speed < 0.0f) throw new InvalidDataException($"track_timing.{key}.speed must be at least zero");
        return new Track(speed, Number("offset_seconds", 0.0f));
    }

    internal static Track ReadTrackFromJsonObject(JsonObject timingObj, string key, Track fallback)
    {
        if (timingObj[key] is not JsonObject trackObj)
            return fallback;
        float speed = TryReadFiniteFloat(trackObj["speed"], out float parsedSpeed) && parsedSpeed >= 0.0f
            ? (parsedSpeed == 0.0f ? 0.0f : parsedSpeed)
            : fallback.Speed;
        float offset = TryReadFiniteFloat(trackObj["offset_seconds"], out float parsedOffset)
            ? (parsedOffset == 0.0f ? 0.0f : parsedOffset)
            : fallback.OffsetSeconds;
        return new Track(speed, offset);
    }

    internal static bool TryReadFiniteFloat(JsonNode? node, out float result)
    {
        result = 0.0f;
        if (node is not JsonValue v) return false;
        if (v.TryGetValue<float>(out float f) && float.IsFinite(f)) { result = f; return true; }
        if (v.TryGetValue<double>(out double d) && double.IsFinite(d)) { result = (float)d; return true; }
        if (v.TryGetValue<int>(out int i)) { result = i; return true; }
        if (v.TryGetValue<long>(out long l)) { result = l; return true; }
        return false;
    }
}

/// <summary>Per-position motion and facial timing metadata (speed multiplier and offset).</summary>
internal sealed record SlotTimingSettings
{
    public PerformanceTimingSettings.Track Motion { get; init; } = new();
    public PerformanceTimingSettings.Track Facial { get; init; } = new();

    public static SlotTimingSettings? ParseOptional(JsonElement container)
    {
        if (!PerformanceTimingSettings.TryGetTimingObject(container, out JsonElement root))
            return null;
        return new SlotTimingSettings
        {
            Motion = PerformanceTimingSettings.ReadTrack(root, "motion"),
            Facial = PerformanceTimingSettings.ReadTrack(root, "facial")
        };
    }

    public static SlotTimingSettings FromTopLevel(PerformanceTimingSettings? topLevel) => new()
    {
        Motion = topLevel?.Motion ?? new(),
        Facial = topLevel?.Facial ?? new()
    };

    public static SlotTimingSettings? ReadOptionalFromInfoObject(JsonObject? obj, SlotTimingSettings? fallback = null)
    {
        if (obj?["track_timing"] is not JsonObject timingObj)
            return null;
        SlotTimingSettings defaults = fallback ?? new SlotTimingSettings();
        return new SlotTimingSettings
        {
            Motion = PerformanceTimingSettings.ReadTrackFromJsonObject(timingObj, "motion", defaults.Motion),
            Facial = PerformanceTimingSettings.ReadTrackFromJsonObject(timingObj, "facial", defaults.Facial)
        };
    }

    public static SlotTimingSettings ReadFromInfoObject(JsonObject? obj, SlotTimingSettings fallback) =>
        ReadOptionalFromInfoObject(obj, fallback) ?? fallback;

    public JsonObject ToJson() => new()
    {
        ["motion"] = Motion.ToJson(),
        ["facial"] = Facial.ToJson()
    };
}
