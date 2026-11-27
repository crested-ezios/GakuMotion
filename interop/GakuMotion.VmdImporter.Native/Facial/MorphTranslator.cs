namespace GakuMotion.VmdImporter.Native.Facial;

using System.Globalization;
using System.Text.Json;
using GakuMotion.VmdImporter.Vmd;

/// <summary>One source track that resolved to target channels.</summary>
internal sealed class MappedMorph
{
    public required string Source { get; init; }
    public required IReadOnlyList<MorphTarget> Targets { get; init; }
    public required int Keyframes { get; init; }
    public required float Peak { get; init; }
}

/// <summary>Facial side effects the Python importer records on disk.</summary>
internal sealed class MorphReport
{
    public required string Mapping { get; init; }
    public required string MappingLabel { get; init; }
    public List<MappedMorph> Mapped { get; } = new();
    public List<string> UnmappedActive { get; } = new();
    public List<string> UnmappedZero { get; } = new();
    public string? FacialAdapterVersion { get; init; }
    public string? ToothStatus { get; set; }
    public string? ToothChannel { get; set; }
    public string? ToothLut { get; set; }
    public float ToothPeak { get; set; }
    public int ToothActiveFrames { get; set; }
    public bool MikuRawSourcePreserved { get; set; }
    public bool MikuRawNativeAuthored { get; set; }
    public int MikuRawInvalidFrames { get; set; }
    public List<string> MikuRawUnsupportedNames { get; set; } = new();
    public bool MikuRawEyeSourcePreserved { get; set; }
    public bool MikuRawEyeNativeAuthored { get; set; }
    public int MikuRawEyeKeys { get; set; }
    public int MikuRawEyeCollapsedKeys { get; set; }
    public List<string> MikuRawEyeUnsupportedNames { get; set; } = new();
    public List<string> BlendshapeNames { get; } = new();
}

/// <summary>
/// Port of morph_translation.translate_morphs. Behaviour is the golden
/// implementation's: alias resolution, sum-then-clamp channel merge, and the
/// offline tooth appearance LUT that the game's mouth_028/mouth_026 expects.
/// </summary>
internal static class MorphTranslator
{
    public const string DefaultToothChannel = "b_mouth.mouth_028";

    private static readonly string[] NativeToothChannels = { "b_mouth.mouth_026", "b_mouth.mouth_028" };

    public static (List<string> Names, float[,] Weights, MorphReport Report) Translate(
        IReadOnlyDictionary<string, IReadOnlyList<MorphKeyframe>> tracks,
        int totalFrames,
        string? toothChannel,
        GakumasMorphMap? map = null,
        ToothAppearanceLut? lut = null)
    {
        map ??= GakumasMorphMap.Default;
        lut ??= ToothAppearanceLut.Default;

        var report = new MorphReport
        {
            Mapping = map.SourceLabel,
            MappingLabel = map.SourceLabel,
            FacialAdapterVersion = map.FacialAdapterVersion
        };

        var channels = new Dictionary<string, float[]>(StringComparer.Ordinal);
        var sourceSamples = new Dictionary<string, float[]>(StringComparer.Ordinal);
        int frames = Math.Max(0, totalFrames);

        foreach (KeyValuePair<string, IReadOnlyList<MorphKeyframe>> track in tracks)
        {
            string sourceName = track.Key;
            float[] sampled = MorphCurve.Sample(track.Value, frames);
            if (track.Value.Count != 0) sourceSamples[sourceName] = sampled;

            if (!map.TryResolve(sourceName, out IReadOnlyList<MorphTarget> targets))
            {
                bool anyActive = false;
                for (int f = 0; f < frames; f++)
                {
                    if (sampled[f] != 0.0f) { anyActive = true; break; }
                }
                (anyActive ? report.UnmappedActive : report.UnmappedZero).Add(sourceName);
                continue;
            }

            float peak = 0.0f;
            for (int f = 0; f < frames; f++)
                if (sampled[f] > peak) peak = sampled[f];

            foreach (MorphTarget target in targets)
            {
                if (!channels.TryGetValue(target.Name, out float[]? channel))
                {
                    channel = new float[frames];
                    channels[target.Name] = channel;
                }
                for (int f = 0; f < frames; f++)
                {
                    // float32 accumulation, matching the float32 channel dtype.
                    channel[f] += sampled[f] * target.Scale;
                }
            }

            report.Mapped.Add(new MappedMorph
            {
                Source = sourceName,
                Targets = targets,
                Keyframes = track.Value.Count,
                Peak = peak
            });
        }

        // --- Tooth appearance ------------------------------------------------
        string? nativeTooth = null;
        foreach (string candidate in NativeToothChannels)
        {
            if (channels.ContainsKey(candidate)) { nativeTooth = candidate; break; }
        }

        if (nativeTooth != null)
        {
            report.ToothStatus = "native_vmd_tooth_preserved";
            report.ToothChannel = nativeTooth;
        }
        else if (!string.IsNullOrEmpty(toothChannel) && !string.Equals(toothChannel, "none", StringComparison.Ordinal))
        {
            var normalized = new Dictionary<string, IReadOnlyList<MorphKeyframe>>(StringComparer.Ordinal);
            foreach (KeyValuePair<string, IReadOnlyList<MorphKeyframe>> track in tracks)
                normalized[GakumasMorphMap.Normalize(track.Key)] = track.Value;

            var vowels = new float[frames * 5];
            ReadOnlySpan<string> names = new[] { "あ", "い", "う", "え", "お" };
            ReadOnlySpan<string> ascii = new[] { "a", "i", "u", "e", "o" };
            for (int channel = 0; channel < 5; channel++)
            {
                IReadOnlyList<MorphKeyframe> keys = Array.Empty<MorphKeyframe>();
                if (normalized.TryGetValue(names[channel], out IReadOnlyList<MorphKeyframe>? japanese))
                    keys = japanese;
                else if (normalized.TryGetValue(ascii[channel], out IReadOnlyList<MorphKeyframe>? latin))
                    keys = latin;

                float[] sampled = MorphCurve.Sample(keys, frames);
                for (int f = 0; f < frames; f++)
                    vowels[f * 5 + channel] = sampled[f];
            }

            float[] tooth = lut.Evaluate(vowels, frames, toothChannel);
            bool any = false;
            int activeFrames = 0;
            float peak = 0.0f;
            for (int f = 0; f < frames; f++)
            {
                if (tooth[f] > 0.005f) any = true;
                if (tooth[f] > 0.01f) activeFrames++;
                if (tooth[f] > peak) peak = tooth[f];
            }

            if (any)
            {
                channels[toothChannel!] = tooth;
                report.ToothStatus = "applied_from_offline_lut";
                report.ToothChannel = toothChannel;
                report.ToothLut = "tooth_appearance_lut.json (embedded from tools/vmd2gkmotion)";
                report.ToothPeak = peak;
                report.ToothActiveFrames = activeFrames;
            }
        }

        report.MikuRawSourcePreserved = MikuRawSource.Append(channels, sourceSamples, map, frames,
            out bool rawNative, out int rawInvalid, out List<string> rawUnsupported);
        report.MikuRawNativeAuthored = rawNative;
        report.MikuRawInvalidFrames = rawInvalid;
        report.MikuRawUnsupportedNames = rawUnsupported;
        MikuRawMouthSource.Append(channels, sourceSamples, map, frames);
        MikuRawMouthSource.AppendV2(channels, sourceSamples, map, frames);
        // Eye/brow intent is captured the same way the vowels are: the fit
        // replaces the whole family, so a clip that names a fitted key only
        // through the native mapping would otherwise lose it.
        report.MikuRawEyeSourcePreserved = MikuRawEyeSource.Append(channels, sourceSamples, map, frames,
            out bool eyeNative, out List<string> eyeUnsupported, out int collapsed);
        report.MikuRawEyeNativeAuthored = eyeNative;
        report.MikuRawEyeUnsupportedNames = eyeUnsupported;
        report.MikuRawEyeCollapsedKeys = collapsed;
        report.MikuRawEyeKeys = MikuRawEyeKeys.Names.Count;
        // --- Merge -----------------------------------------------------------
        // Several VMD aliases may feed one shape: sum first, clamp once.
        var namesList = new List<string>(channels.Keys);
        namesList.Sort(StringComparer.Ordinal);

        var weights = new float[frames, namesList.Count];
        for (int c = 0; c < namesList.Count; c++)
        {
            float[] channel = channels[namesList[c]];
            for (int f = 0; f < frames; f++)
            {
                float value = channel[f];
                if (value < 0.0f) value = 0.0f;
                else if (value > 1.0f) value = 1.0f;
                weights[f, c] = value;
            }
        }

        report.BlendshapeNames.AddRange(namesList);
        return (namesList, weights, report);
    }

    /// <summary>Writes motion.morph_report.json with the Python tool's shape.</summary>
    public static void WriteReport(string path, MorphReport report, string clipSha256)
    {
        using var stream = new FileStream(path, FileMode.Create, FileAccess.Write);
        using var writer = new Utf8JsonWriter(stream, new JsonWriterOptions
        {
            Indented = true,
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping
        });

        writer.WriteStartObject();
        writer.WriteString("mapping", report.Mapping);

        writer.WriteStartArray("mapped");
        foreach (MappedMorph mapped in report.Mapped)
        {
            writer.WriteStartObject();
            writer.WriteString("source", mapped.Source);
            writer.WriteStartArray("targets");
            foreach (MorphTarget target in mapped.Targets)
            {
                writer.WriteStartObject();
                writer.WriteString("name", target.Name);
                if (target.Scale != 1.0f) writer.WriteNumber("scale", target.Scale);
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.WriteNumber("keyframes", mapped.Keyframes);
            writer.WriteNumber("peak", mapped.Peak);
            writer.WriteEndObject();
        }
        writer.WriteEndArray();

        WriteStringArray(writer, "unmapped_active", report.UnmappedActive);
        WriteStringArray(writer, "unmapped_zero", report.UnmappedZero);
        writer.WriteString("weight_unit", "0..1");
        if (report.FacialAdapterVersion != null)
            writer.WriteString("facial_adapter_version", report.FacialAdapterVersion);
        else
            writer.WriteNull("facial_adapter_version");
        writer.WriteString("runtime_contract", "mmd2.* channels require the v2 character adapter");
        writer.WriteStartObject("miku_raw_source");
        writer.WriteString("contract", MikuRawSource.Prefix+"*");
        writer.WriteBoolean("preserved", report.MikuRawSourcePreserved);
        writer.WriteBoolean("native_mouth_authored", report.MikuRawNativeAuthored);
        writer.WriteNumber("serialization_clipped_invalid_frames", report.MikuRawInvalidFrames);
        WriteStringArray(writer, "unsupported_source_names", report.MikuRawUnsupportedNames);
        writer.WriteEndObject();
        writer.WriteStartObject("miku_raw_eye_source");
        writer.WriteString("contract", MikuRawEyeSource.Prefix + "*");
        writer.WriteBoolean("preserved", report.MikuRawEyeSourcePreserved);
        writer.WriteBoolean("native_eye_authored", report.MikuRawEyeNativeAuthored);
        writer.WriteNumber("table_keys", report.MikuRawEyeKeys);
        writer.WriteNumber("collapsed_alias_keys", report.MikuRawEyeCollapsedKeys);
        WriteStringArray(writer, "unsupported_source_names", report.MikuRawEyeUnsupportedNames);
        writer.WriteEndObject();

        if (report.ToothStatus != null)
        {
            writer.WriteStartObject("tooth_appearance_matching");
            writer.WriteString("status", report.ToothStatus);
            writer.WriteString("channel", report.ToothChannel ?? string.Empty);
            if (report.ToothLut != null) writer.WriteString("lut_path", report.ToothLut);
            if (report.ToothStatus == "applied_from_offline_lut")
            {
                writer.WriteNumber("peak", report.ToothPeak);
                writer.WriteNumber("active_frames", report.ToothActiveFrames);
            }
            writer.WriteEndObject();
        }

        WriteStringArray(writer, "blendshape_names", report.BlendshapeNames);
        writer.WriteNumber("channel_count", report.BlendshapeNames.Count);
        writer.WriteString("clip_sha256", clipSha256);
        writer.WriteEndObject();
        writer.Flush();
    }

    private static void WriteStringArray(Utf8JsonWriter writer, string name, List<string> values)
    {
        writer.WriteStartArray(name);
        foreach (string value in values) writer.WriteStringValue(value);
        writer.WriteEndArray();
    }

    internal static string FormatFloat(float value) => value.ToString("R", CultureInfo.InvariantCulture);
}
