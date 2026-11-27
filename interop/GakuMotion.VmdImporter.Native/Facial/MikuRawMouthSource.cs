namespace GakuMotion.VmdImporter.Native.Facial;

internal static class MikuRawMouthSource
{
    internal const string Prefix = "miku.rawmouth.v1.";
    internal static readonly string[] InputNames = { "a", "i", "u", "e", "o", "∧", "口角上げ" };
    internal static readonly string[] Names = InputNames.Select(name => Prefix + name)
        .Concat(new[] { Prefix + "present", Prefix + "unsupported", Prefix + "native_authored" }).ToArray();
    private const string ExtraPrefix = "miku.rawmouth.extra.v1.";
    internal static readonly string[] ExtraNames =
        { ExtraPrefix + "present", ExtraPrefix + "にやり2", ExtraPrefix + "口角下げ", ExtraPrefix + "unsupported" };
    private const string V2Prefix = "miku.rawmouth.v2.";
    internal static readonly string[] V2Names = InputNames
        .Concat(new[] { "smirk", "corner_down", "smile", "wide_open", "present", "unsupported", "native_authored" })
        .Select(name => V2Prefix + name).ToArray();
    // Distinct authored gestures can feed the same output slot. They combine
    // by max, while multiple spellings of one gesture are rejected.
    private static readonly int[] V2OutputSlots = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 9, 5, 5, 9, 10, 10, 9, 1 };

    internal static int CanonicalInput(string name)
    {
        int vowel = MikuRawSource.CanonicalVowel(name);
        if (vowel >= 0) return vowel;
        return GakumasMorphMap.Normalize(name) switch { "∧" => 5, "口角上げ" => 6, _ => -1 };
    }

    private static int CanonicalExtra(string normalized) => normalized switch
    {
        // The NativeAOT build uses invariant globalization, so accept the VMD's
        // full-width digit explicitly even if FormKC leaves it unchanged.
        "にやり2" or "にやり２" => 0,
        "口角下げ" => 1,
        _ => -1
    };

    private static bool IsNativeAuthored(string normalized) =>
        normalized.StartsWith("b_mouth.", StringComparison.Ordinal) || normalized == "tooth";

    private static bool IsNonMouth(string name, string normalized) =>
        MikuRawEyeKeys.IndexOf(name) >= 0 || MikuRawEyeSource.IsNativeEyeChannel(name) ||
        normalized is "腰相対on" or "g警告off" or "c警告off";

    private static bool BlocksUnmappedInput(string name, GakumasMorphMap map) =>
        !map.TryResolve(name, out var targets) || targets.Count == 0 ||
        targets.Any(target => target.Name.StartsWith("b_mouth.", StringComparison.Ordinal) ||
            target.Name is "mmd2.a" or "mmd2.i" or "mmd2.u" or "mmd2.e" or "mmd2.o");

    internal static bool Append(Dictionary<string, float[]> channels,
        Dictionary<string, float[]> sourceSamples, GakumasMorphMap map, int frames, bool includeExtras = true)
    {
        var groups = Enumerable.Range(0, InputNames.Length).Select(_ => new List<string>()).ToArray();
        foreach (string name in sourceSamples.Keys)
        {
            int index = CanonicalInput(name);
            if (index >= 0) groups[index].Add(name);
        }
        if (!groups.Any(group => group.Count != 0)) return false;

        var raw = groups.Select(group => group.Count == 0 ? new float[frames]
            : (float[])sourceSamples[group[0]].Clone()).ToArray();
        var unsupported = new float[frames];
        var extraCounts = new int[2];
        var extras = includeExtras ? new[] { new float[frames], new float[frames] } : null;
        var residual = includeExtras ? new float[frames] : null;
        if (includeExtras)
        {
            foreach (string name in sourceSamples.Keys)
            {
                int extra = CanonicalExtra(GakumasMorphMap.Normalize(name));
                if (extra >= 0) extraCounts[extra]++;
            }
        }
        bool nativeAuthored = false;
        foreach (var pair in sourceSamples)
        {
            string normalized = GakumasMorphMap.Normalize(pair.Key).ToLowerInvariant();
            nativeAuthored |= IsNativeAuthored(normalized);
            int input = CanonicalInput(pair.Key);
            if (input >= 0)
            {
                for (int frame = 0; frame < frames; frame++)
                {
                    float value = pair.Value[frame];
                    float issue = !float.IsFinite(value) || value < 0 || value > 1 ? 1 :
                        groups[input].Count > 1 ? value : 0;
                    unsupported[frame] = Math.Max(unsupported[frame], issue);
                    if (residual != null) residual[frame] = Math.Max(residual[frame], issue);
                }
                continue;
            }
            if (IsNonMouth(pair.Key, normalized)) continue;
            bool blocked = BlocksUnmappedInput(pair.Key, map);
            int extraInput = includeExtras ? CanonicalExtra(normalized) : -1;
            if (!blocked && extraInput < 0) continue;
            for (int frame = 0; frame < frames; frame++)
            {
                float value = pair.Value[frame];
                float activity = float.IsFinite(value) ? Math.Clamp(Math.Abs(value), 0, 1) : 1;
                if (blocked) unsupported[frame] = Math.Max(unsupported[frame], activity);
                if (residual == null || extras == null) continue;
                if (extraInput < 0) residual[frame] = Math.Max(residual[frame], activity);
                else if (!float.IsFinite(value) || value < 0 || value > 1) residual[frame] = 1;
                else if (extraCounts[extraInput] > 1) residual[frame] = Math.Max(residual[frame], value);
                else extras[extraInput][frame] = value;
            }
        }
        for (int channel = 0; channel < InputNames.Length; channel++)
        {
            for (int frame = 0; frame < frames; frame++)
                raw[channel][frame] = float.IsFinite(raw[channel][frame]) ? Math.Clamp(raw[channel][frame], 0, 1) : 0;
            channels[Names[channel]] = raw[channel];
        }
        channels[Names[7]] = Enumerable.Repeat(1f, frames).ToArray();
        channels[Names[8]] = unsupported;
        channels[Names[9]] = Enumerable.Repeat(nativeAuthored ? 1f : 0f, frames).ToArray();
        if (extras != null && residual != null)
        {
            // Extra readers may distinguish these curves while the original
            // aggregate block marker stays intact for existing consumers.
            channels[ExtraNames[0]] = channels[Names[7]];
            channels[ExtraNames[1]] = extras[0];
            channels[ExtraNames[2]] = extras[1];
            channels[ExtraNames[3]] = residual;
        }
        return true;
    }

    internal static void AppendV2(Dictionary<string, float[]> channels,
        Dictionary<string, float[]> sourceSamples, GakumasMorphMap map, int frames)
    {
        const int present = 11, unsupported = 12, nativeAuthored = 13;
        // Stored VMD weights and interpolation can leave tiny errors at 0
        // and 1. Keep a narrow tolerance for supported actions only.
        const float endpointTolerance = 1e-6f;
        var groups = Enumerable.Range(0, V2OutputSlots.Length).Select(_ => new List<float[]>()).ToArray();
        var blocked = new List<float[]>();
        bool authored = false;
        foreach (var pair in sourceSamples)
        {
            string normalized = GakumasMorphMap.Normalize(pair.Key).ToLowerInvariant();
            authored |= IsNativeAuthored(normalized);
            int input = CanonicalInput(pair.Key);
            if (input < 0)
            {
                int extra = CanonicalExtra(normalized);
                input = extra >= 0 ? InputNames.Length + extra : normalized switch
                {
                    "にやり" => 9,
                    "ワ" or "ﾜ" => 10,
                    "にっこり" => 11,
                    "ん" => 12,
                    "▲" => 13,
                    "口横広げ" => 14,
                    "大口" => 15,
                    "叫び" => 16,
                    "ω" => 17,
                    "いー" => 18,
                    _ => -1
                };
            }
            if (input >= 0) groups[input].Add(pair.Value);
            else if (normalized is not ("照れ" or "眉太" or "キリッ" or "ｷﾘｯ" or "瞳小" or
                         "右ウィンク" or "しいたけ" or "ウィンク右2" or "ウィンク右２" or "見開き") &&
                     !IsNonMouth(pair.Key, normalized) && BlocksUnmappedInput(pair.Key, map))
                blocked.Add(pair.Value);
        }
        if (!groups.Any(group => group.Count != 0)) return;

        var residual = new float[frames];
        foreach (float[] track in blocked)
            for (int frame = 0; frame < frames; frame++)
                residual[frame] = Math.Max(residual[frame], float.IsFinite(track[frame])
                    ? Math.Clamp(Math.Abs(track[frame]), 0f, 1f) : 1f);
        var values = Enumerable.Range(0, present).Select(_ => new float[frames]).ToArray();
        for (int input = 0; input < groups.Length; input++)
        {
            var group = groups[input];
            float[] output = values[V2OutputSlots[input]];
            foreach (float[] track in group)
            {
                for (int frame = 0; frame < frames; frame++)
                {
                    float value = track[frame];
                    if (!float.IsFinite(value) || value < -endpointTolerance || value > 1f + endpointTolerance)
                    {
                        residual[frame] = 1f;
                        continue;
                    }
                    value = Math.Clamp(value, 0f, 1f);
                    if (group.Count > 1) residual[frame] = Math.Max(residual[frame], value);
                    else output[frame] = Math.Max(output[frame], value);
                }
            }
        }
        for (int input = 0; input < values.Length; input++) channels[V2Names[input]] = values[input];
        channels[V2Names[present]] = Enumerable.Repeat(1f, frames).ToArray();
        channels[V2Names[unsupported]] = residual;
        channels[V2Names[nativeAuthored]] = Enumerable.Repeat(authored ? 1f : 0f, frames).ToArray();
    }
}
