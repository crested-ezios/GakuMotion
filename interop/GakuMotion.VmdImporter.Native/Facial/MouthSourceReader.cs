namespace GakuMotion.VmdImporter.Native.Facial;

using GakuMotion.VmdImporter.Vmd;
using System.Text;

internal static class MouthSourceReader
{
    internal const int ChannelCount = 18;
    internal const int ExtraChannelCount = 4;
    internal const int V2ChannelCount = 14;

    internal enum Layout { Legacy, Extra, V2 }

    internal static int Count(Layout layout) => layout switch
    {
        Layout.Extra => ExtraChannelCount,
        Layout.V2 => V2ChannelCount,
        _ => ChannelCount
    };

    internal static bool ValidDimensions(float sourceFps, float clipFps, int frames) =>
        float.IsFinite(sourceFps) && sourceFps > 0 && float.IsFinite(clipFps) && clipFps > 0 &&
        frames > 0 && frames <= 1_000_000;

    internal static float[] Read(string sourcePath, float sourceFps, float clipFps, int frames, Layout layout = Layout.Legacy)
    {
        if (string.IsNullOrWhiteSpace(sourcePath) || !ValidDimensions(sourceFps, clipFps, frames))
            throw new ArgumentException("Invalid mouth source path or dimensions");
        using var stream = File.OpenRead(sourcePath);
        return Sample(ParseMorphs(stream), sourceFps, clipFps, frames, layout);
    }

    internal static IReadOnlyDictionary<string, IReadOnlyList<MorphKeyframe>> ParseMorphs(Stream stream)
    {
        if (!stream.CanRead || !stream.CanSeek) throw new ArgumentException("A seekable VMD source is required");
        using var reader = new BinaryReader(stream, Encoding.ASCII, leaveOpen: true);
        byte[] headerBytes = reader.ReadBytes(30);
        if (headerBytes.Length != 30) throw new InvalidDataException("Truncated VMD header");
        string header = Encoding.ASCII.GetString(headerBytes).TrimEnd('\0');
        if (!header.StartsWith("Vocaloid Motion Data", StringComparison.Ordinal))
            throw new InvalidDataException("Invalid VMD header");
        int modelBytes = header.StartsWith("Vocaloid Motion Data 0002", StringComparison.Ordinal) ? 20 : 10;
        if (stream.Length - stream.Position < modelBytes + 4) throw new InvalidDataException("Truncated VMD model");
        stream.Seek(modelBytes, SeekOrigin.Current);
        long boneBytes = (long)reader.ReadUInt32() * 111;
        if (stream.Length - stream.Position < boneBytes + 4) throw new InvalidDataException("Truncated VMD bones");
        stream.Seek(boneBytes, SeekOrigin.Current);
        uint morphCount = reader.ReadUInt32();
        if (stream.Length - stream.Position < (long)morphCount * 23)
            throw new InvalidDataException("Truncated VMD morphs");
        Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        Encoding shiftJis = Encoding.GetEncoding(932, EncoderFallback.ReplacementFallback, DecoderFallback.ReplacementFallback);
        var result = new Dictionary<string, IReadOnlyList<MorphKeyframe>>(StringComparer.Ordinal);
        for (uint index = 0; index < morphCount; index++)
        {
            byte[] nameBytes = reader.ReadBytes(15);
            int nameLength = Array.IndexOf(nameBytes, (byte)0);
            string name = shiftJis.GetString(nameBytes, 0, nameLength < 0 ? 15 : nameLength);
            uint frame = reader.ReadUInt32();
            float weight = reader.ReadSingle();
            if (!result.TryGetValue(name, out var keys))
            {
                keys = new List<MorphKeyframe>();
                result[name] = keys;
            }
            ((List<MorphKeyframe>)keys).Add(new MorphKeyframe(name, frame, weight));
        }
        return result;
    }

    internal static float[] Sample(IReadOnlyDictionary<string, IReadOnlyList<MorphKeyframe>> tracks,
        float sourceFps, float clipFps, int frames, Layout layout = Layout.Legacy)
    {
        if (!ValidDimensions(sourceFps, clipFps, frames)) throw new ArgumentOutOfRangeException(nameof(frames));
        var sourceSamples = new Dictionary<string, float[]>(StringComparer.Ordinal);
        foreach (var track in tracks)
            if (track.Value.Count != 0)
                sourceSamples[track.Key] = MorphCurve.Sample(track.Value, frames, (double)sourceFps / clipFps);
        var channels = new Dictionary<string, float[]>(StringComparer.Ordinal);
        var map = GakumasMorphMap.Default;
        if (layout == Layout.V2) MikuRawMouthSource.AppendV2(channels, sourceSamples, map, frames);
        else
        {
            if (layout == Layout.Legacy) MikuRawSource.Append(channels, sourceSamples, map, frames, out _, out _, out _);
            MikuRawMouthSource.Append(channels, sourceSamples, map, frames, includeExtras: layout == Layout.Extra);
        }
        int channelCount = Count(layout);
        var result = new float[checked(frames * channelCount)];
        string[] names = layout switch
        {
            Layout.Extra => MikuRawMouthSource.ExtraNames,
            Layout.V2 => MikuRawMouthSource.V2Names,
            _ => MikuRawSource.Names.Concat(MikuRawMouthSource.Names).ToArray()
        };
        for (int channel = 0; channel < names.Length; channel++)
        {
            if (!channels.TryGetValue(names[channel], out float[]? values)) continue;
            for (int frame = 0; frame < frames; frame++) result[frame * channelCount + channel] = values[frame];
        }
        return result;
    }
}
