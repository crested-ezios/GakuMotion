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
            nativeAuthored |= normalized.StartsWith("b_mouth.", StringComparison.Ordinal) || normalized == "tooth";
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
            if (MikuRawEyeKeys.IndexOf(pair.Key) >= 0 || MikuRawEyeSource.IsNativeEyeChannel(pair.Key)) continue;
            bool resolved = map.TryResolve(pair.Key, out var targets);
            bool blocked = !resolved || targets.Count == 0 || targets.Any(target =>
                target.Name.StartsWith("b_mouth.", StringComparison.Ordinal) ||
                target.Name is "mmd2.a" or "mmd2.i" or "mmd2.u" or "mmd2.e" or "mmd2.o");
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
}
