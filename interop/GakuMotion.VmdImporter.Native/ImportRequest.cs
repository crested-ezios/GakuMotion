namespace GakuMotion.VmdImporter.Native;

using System.Text.Json;

internal sealed class PositionRequest
{
    public int Slot { get; init; }
    public int SourceSlot { get; init; }
    public string MotionVmd { get; init; } = string.Empty;
    public string MorphVmd { get; init; } = string.Empty;
    public bool DisableMorph { get; init; }
    public bool ClearMorph { get; init; }
    public float SourceFps { get; init; }
    public bool RebakeOutput { get; init; }
    public bool PreserveExisting { get; init; }
    public float X { get; init; }
    public float Y { get; init; }
    public float Z { get; init; }
    public float YawDegrees { get; init; }
    public SlotTimingSettings? TrackTiming { get; init; }
    public bool HasExplicitPlacement { get; init; }
}

/// <summary>
/// One in-game import job, serialized as UTF-8 JSON across the native boundary.
/// Field names mirror the Python CLI (tools/import_motion.py) so both backends
/// receive the same information.
/// </summary>
internal sealed class ImportRequest
{
    public required string MotionVmd { get; init; }
    public string EditDir { get; init; } = string.Empty;
    public string MorphVmd { get; init; } = string.Empty;
    public bool DisableMorph { get; init; }
    public required string MotionsRoot { get; init; }
    public string Title { get; init; } = string.Empty;
    public string Subtitle { get; init; } = string.Empty;
    public bool HasSubtitle { get; init; }
    public float Fps { get; init; }
    public float SourceFps { get; init; }
    public bool RebakeOutput { get; init; }
    public string Audio { get; init; } = string.Empty;
    public bool ClearAudio { get; init; }
    public float AudioDurationHint { get; init; }
    public string CameraVmd { get; init; } = string.Empty;
    public bool ClearCamera { get; init; }
    public float CameraFps { get; init; }
    public float CameraScale { get; init; }
    public string CameraRootRegistration { get; init; } = string.Empty;
    public string ToothChannel { get; init; } = string.Empty;
    public string LogPath { get; init; } = string.Empty;
    public string TimingJsonl { get; init; } = string.Empty;
    public string TransactionRoot { get; init; } = string.Empty;
    public PerformanceTimingSettings? TrackTiming { get; init; }
    public IReadOnlyList<PositionRequest>? Positions { get; init; }

    public static ImportRequest Parse(ReadOnlySpan<byte> utf8Json)
    {
        using JsonDocument doc = JsonDocument.Parse(utf8Json.ToArray());
        JsonElement root = doc.RootElement;
        if (root.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("import request must be a JSON object");

        static string RequireFrom(JsonElement obj, string name)
        {
            if (!obj.TryGetProperty(name, out JsonElement value) || value.ValueKind != JsonValueKind.String)
                throw new InvalidDataException($"import request is missing string field '{name}'");
            return value.GetString() ?? string.Empty;
        }

        static string OptionalFrom(JsonElement obj, string name)
        {
            if (!obj.TryGetProperty(name, out JsonElement value) ||
                value.ValueKind is JsonValueKind.Null or JsonValueKind.Undefined)
                return string.Empty;
            if (value.ValueKind != JsonValueKind.String)
                throw new InvalidDataException($"import request field '{name}' must be a string");
            return value.GetString() ?? string.Empty;
        }

        static bool OptionalBoolFrom(JsonElement obj, string name)
        {
            if (!obj.TryGetProperty(name, out JsonElement value) ||
                value.ValueKind is JsonValueKind.Null or JsonValueKind.Undefined)
                return false;
            if (value.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
                throw new InvalidDataException($"import request field '{name}' must be a boolean");
            return value.ValueKind == JsonValueKind.True;
        }

        static float OptionalFloatFrom(JsonElement obj, string name)
        {
            if (!obj.TryGetProperty(name, out JsonElement value) ||
                value.ValueKind is JsonValueKind.Null or JsonValueKind.Undefined)
                return 0.0f;
            if (value.ValueKind != JsonValueKind.Number)
                throw new InvalidDataException($"import request field '{name}' must be a number");
            float parsed = value.GetSingle();
            if (!float.IsFinite(parsed))
                throw new InvalidDataException($"import request field '{name}' must be finite");
            return parsed;
        }

        static int OptionalIntFrom(JsonElement obj, string name, int fallback)
        {
            if (!obj.TryGetProperty(name, out JsonElement value) ||
                value.ValueKind is JsonValueKind.Null or JsonValueKind.Undefined)
                return fallback;
            if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt32(out int parsed))
                throw new InvalidDataException($"import request field '{name}' must be an integer");
            return parsed;
        }

        List<PositionRequest>? positions = null;
        if (root.TryGetProperty("positions", out JsonElement positionsVal) &&
            positionsVal.ValueKind is not (JsonValueKind.Null or JsonValueKind.Undefined))
        {
            if (positionsVal.ValueKind != JsonValueKind.Array)
                throw new InvalidDataException("import request field 'positions' must be an array");
            int count = positionsVal.GetArrayLength();
            if (count == 0)
                throw new InvalidDataException("positions 不能为空");
            positions = new List<PositionRequest>(count);
            int index = 0;
            foreach (JsonElement item in positionsVal.EnumerateArray())
            {
                if (item.ValueKind != JsonValueKind.Object)
                    throw new InvalidDataException("positions 中的每一项都必须是 JSON 对象");
                int slot = OptionalIntFrom(item, "slot", index + 1);
                if (slot < 1)
                    throw new InvalidDataException("positions 槽位序号必须 >= 1");
                int sourceSlot = OptionalIntFrom(item, "source_slot", 0);
                if (sourceSlot < 0)
                    throw new InvalidDataException("positions source_slot 必须 >= 0");
                float slotSourceFps = OptionalFloatFrom(item, "source_fps");
                if (slotSourceFps < 0.0f)
                    throw new InvalidDataException("positions source_fps 必须 >= 0");
                positions.Add(new PositionRequest
                {
                    Slot = slot,
                    SourceSlot = sourceSlot,
                    MotionVmd = OptionalFrom(item, "motion_vmd"),
                    MorphVmd = OptionalFrom(item, "morph_vmd"),
                    DisableMorph = OptionalBoolFrom(item, "disable_morph"),
                    ClearMorph = OptionalBoolFrom(item, "clear_morph"),
                    SourceFps = slotSourceFps,
                    RebakeOutput = OptionalBoolFrom(item, "rebake_output"),
                    PreserveExisting = OptionalBoolFrom(item, "preserve_existing"),
                    X = OptionalFloatFrom(item, "x"),
                    Y = OptionalFloatFrom(item, "y"),
                    Z = OptionalFloatFrom(item, "z"),
                    YawDegrees = OptionalFloatFrom(item, "yaw_degrees"),
                    TrackTiming = SlotTimingSettings.ParseOptional(item),
                    HasExplicitPlacement = OptionalBoolFrom(item, "has_explicit_placement")
                });
                index++;
            }
        }

        bool hasSubtitle = root.TryGetProperty("subtitle", out JsonElement subtitleVal) &&
                           subtitleVal.ValueKind == JsonValueKind.String;

        string editDir = OptionalFrom(root, "edit_dir");
        string motionVmd;
        if (editDir.Length == 0)
        {
            if (positions is null)
            {
                motionVmd = RequireFrom(root, "motion_vmd");
            }
            else
            {
                motionVmd = OptionalFrom(root, "motion_vmd");
                bool hasSlot1 = positions.Any(p => p.Slot == 1 && !string.IsNullOrEmpty(p.MotionVmd));
                if (!hasSlot1 && string.IsNullOrEmpty(motionVmd))
                    throw new InvalidDataException("缺少 1 号位动作 VMD 文件");
            }
        }
        else
        {
            motionVmd = OptionalFrom(root, "motion_vmd");
        }

        return new ImportRequest
        {
            MotionVmd = motionVmd,
            EditDir = editDir,
            MorphVmd = OptionalFrom(root, "morph_vmd"),
            DisableMorph = OptionalBoolFrom(root, "disable_morph"),
            MotionsRoot = editDir.Length == 0 ? RequireFrom(root, "motions_root") : OptionalFrom(root, "motions_root"),
            Title = OptionalFrom(root, "title"),
            Subtitle = hasSubtitle ? (subtitleVal.GetString() ?? string.Empty) : string.Empty,
            HasSubtitle = hasSubtitle,
            Fps = OptionalFloatFrom(root, "fps"),
            SourceFps = OptionalFloatFrom(root, "source_fps"),
            RebakeOutput = OptionalBoolFrom(root, "rebake_output"),
            Audio = OptionalFrom(root, "audio"),
            ClearAudio = OptionalBoolFrom(root, "clear_audio"),
            AudioDurationHint = OptionalFloatFrom(root, "audio_duration_hint"),
            CameraVmd = OptionalFrom(root, "camera_vmd"),
            ClearCamera = OptionalBoolFrom(root, "clear_camera"),
            CameraFps = OptionalFloatFrom(root, "camera_fps"),
            CameraScale = OptionalFloatFrom(root, "camera_scale"),
            CameraRootRegistration = OptionalFrom(root, "camera_root_registration"),
            ToothChannel = OptionalFrom(root, "tooth_channel"),
            LogPath = OptionalFrom(root, "log_path"),
            TimingJsonl = OptionalFrom(root, "timing_jsonl"),
            TransactionRoot = OptionalFrom(root, "transaction_root"),
            TrackTiming = PerformanceTimingSettings.ParseOptional(root),
            Positions = positions
        };
    }
}
