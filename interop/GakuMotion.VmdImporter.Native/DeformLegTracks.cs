namespace GakuMotion.VmdImporter.Native;

using System.Numerics;
using GakuMotion.VmdImporter.Vmd;

/// <summary>
/// Adapt unambiguous baked MMD leg tracks without changing the frozen evaluator.
/// In addition to D-leg tracks, a dense FK bake with fully neutral, per-frame
/// foot and toe IK tracks must not be re-solved as though its IK were active.
/// Ambiguous motions and explicit IK switches remain unchanged.
/// </summary>
internal static class DeformLegTracks
{
    private static readonly string[] Sides = ["左", "右"];
    private static readonly string[] Parts = ["足", "ひざ", "足首"];
    private const float MaxNeutralPositionSquared = 0.000001f; // 0.001 MMD units
    private const float MaxNeutralRotationSinSquared = 0.0000007615f; // 0.1 degrees
    private const float MinAnimatedRotationSinSquared = 0.0000190385f; // 0.5 degrees

    internal static VmdMotion Prepare(VmdMotion motion, out string[] adaptedSides)
    {
        var selected = new List<(string Side, bool UseDeform)>();
        foreach (string side in Sides)
        {
            if (motion.PropertyKeyframes.Any(p =>
                p.IkSwitches.ContainsKey(side + "足ＩＫ") || p.IkSwitches.ContainsKey(side + "つま先ＩＫ")))
                continue;

            bool completeDeform = Parts.All(part =>
                motion.BoneKeyframes.TryGetValue(side + part + "D", out var keys) && ValidTrack(keys));
            bool neutralOrdinary = Parts.All(part => NeutralTrack(motion, side + part));
            bool neutralIk = NeutralTrack(motion, side + "足ＩＫ") && NeutralTrack(motion, side + "つま先ＩＫ");
            if (completeDeform && neutralOrdinary && neutralIk &&
                Parts.Any(part => AnimatedRotation(motion.BoneKeyframes[side + part + "D"])))
            {
                selected.Add((side, true));
                continue;
            }

            // Fully sampled neutral IK tracks plus fully sampled moving FK legs
            // indicate a baked FK clip. Sparse stationary IK can instead be a
            // deliberate planted foot, so it must never use this fallback.
            bool hasAnyDeform = Parts.Any(part => motion.BoneKeyframes.ContainsKey(side + part + "D"));
            if (!hasAnyDeform && motion.MaxFrame > 0 &&
                Parts.All(part => DenseTrack(motion, side + part, false)) &&
                DenseTrack(motion, side + "足ＩＫ", true) && DenseTrack(motion, side + "つま先ＩＫ", true) &&
                AnimatedRotation(motion.BoneKeyframes[side + "ひざ"]) &&
                (AnimatedRotation(motion.BoneKeyframes[side + "足"]) || AnimatedRotation(motion.BoneKeyframes[side + "足首"])))
                selected.Add((side, false));
        }

        adaptedSides = selected.Select(item => item.Side + (item.UseDeform ? ":D" : ":FK")).ToArray();
        if (selected.Count == 0) return motion;

        var bones = new Dictionary<string, IReadOnlyList<BoneKeyframe>>(motion.BoneKeyframes, StringComparer.Ordinal);
        foreach (var (side, useDeform) in selected)
        foreach (string part in Parts)
        {
            if (!useDeform) continue;
            string name = side + part;
            bones[name] = motion.BoneKeyframes[name + "D"]
                .Select(k => k with { Name = name }).ToArray();
        }

        Dictionary<string, bool> DisableSelected(IReadOnlyDictionary<string, bool> original)
        {
            var switches = original.ToDictionary(kv => kv.Key, kv => kv.Value, StringComparer.Ordinal);
            foreach (var (side, _) in selected)
            {
                switches[side + "足ＩＫ"] = false;
                switches[side + "つま先ＩＫ"] = false;
            }
            return switches;
        }

        var properties = new List<PropertyKeyframe>(motion.PropertyKeyframes.Count + 1)
        {
            new(0, true, DisableSelected(new Dictionary<string, bool>()))
        };
        properties.AddRange(motion.PropertyKeyframes.Select(p =>
            new PropertyKeyframe(p.Frame, p.Visible, DisableSelected(p.IkSwitches))));
        return new VmdMotion
        {
            Version = motion.Version, ModelName = motion.ModelName, MaxFrame = motion.MaxFrame,
            BoneKeyframes = bones, MorphKeyframes = motion.MorphKeyframes,
            CameraKeyframes = motion.CameraKeyframes, PropertyKeyframes = properties
        };
    }

    private static bool NeutralTrack(VmdMotion motion, string name) =>
        !motion.BoneKeyframes.TryGetValue(name, out var keys) || keys.All(IsNeutral);

    private static bool ValidTrack(IReadOnlyList<BoneKeyframe> keys) =>
        keys.Count != 0 && keys.All(k => Finite(k.Position) && RotationSinSquared(k.Rotation) is not null);

    private static bool DenseTrack(VmdMotion motion, string name, bool requireNeutral)
    {
        if (!motion.BoneKeyframes.TryGetValue(name, out var keys) ||
            (ulong)keys.Count != (ulong)motion.MaxFrame + 1 || !ValidTrack(keys)) return false;
        for (int i = 0; i < keys.Count; i++)
            if (keys[i].Frame != (uint)i || (requireNeutral && !IsNeutral(keys[i]))) return false;
        return true;
    }

    private static bool IsNeutral(BoneKeyframe key) =>
        Finite(key.Position) && key.Position.LengthSquared() <= MaxNeutralPositionSquared &&
        RotationSinSquared(key.Rotation) is float value && value <= MaxNeutralRotationSinSquared;

    private static bool AnimatedRotation(IReadOnlyList<BoneKeyframe> keys) =>
        keys.Any(k => RotationSinSquared(k.Rotation) is float value && value > MinAnimatedRotationSinSquared);

    private static float? RotationSinSquared(Quaternion q)
    {
        float norm = q.LengthSquared();
        if (!float.IsFinite(norm) || norm < 1e-12f) return null;
        return (q.X * q.X + q.Y * q.Y + q.Z * q.Z) / norm;
    }

    private static bool Finite(Vector3 p) => float.IsFinite(p.X) && float.IsFinite(p.Y) && float.IsFinite(p.Z);
}
