namespace GakuMotion.VmdImporter.Native;

using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using GakuMotion.VmdImporter.Rig;
using GakuMotion.VmdImporter.Vmd;

// Recover the absolute source position which the clip encoder subtracts at frame 0.
internal static class SourceFormation
{
    internal static Vector3 ReadOrigin(string directory)
    {
        string source = Path.Combine(directory, "source.vmd");
        string info = Path.Combine(directory, "info.json");
        var size = new FileInfo(source).Length;
        if (size <= 0 || size > 256L * 1024 * 1024) throw new InvalidDataException("invalid VMD size");
        using var document = JsonDocument.Parse(File.ReadAllBytes(info));
        if (!document.RootElement.TryGetProperty("source_vmd_sha256", out var hashNode) ||
            hashNode.ValueKind != JsonValueKind.String) throw new InvalidDataException("source hash missing");
        string expected = hashNode.GetString() ?? "";
        byte[] bytes = File.ReadAllBytes(source);
        if (!string.Equals(Convert.ToHexString(SHA256.HashData(bytes)), expected, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("source VMD does not match imported metadata");
        var evaluator = new SourceEvaluator();
        evaluator.Evaluate(VmdReader.Parse(bytes), endFrame: 0);
        Vector3 result = (evaluator.Rig.LowerBody.WorldPosition - evaluator.Rig.SourceHipsRestPosition) * evaluator.RootScale;
        if (!float.IsFinite(result.X) || !float.IsFinite(result.Y) || !float.IsFinite(result.Z))
            throw new InvalidDataException("invalid source formation origin");
        return result;
    }
}
