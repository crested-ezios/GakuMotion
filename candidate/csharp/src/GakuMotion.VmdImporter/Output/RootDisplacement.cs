namespace GakuMotion.VmdImporter.Output;

using System.Numerics;
using GakuMotion.VmdImporter.Rig;

/// <summary>
/// Root displacement calculation and Hips trajectory extraction.
/// Strictly replicates root_displacement.py with standard scale 0.07795f meters/unit (77.95 mm / unit).
/// </summary>
public static class RootDisplacement
{
    /// <summary>
    /// Authoritative default conversion scale: 0.07795 meters per MMD unit.
    /// </summary>
    public const float DefaultScale = 0.07795f;

    /// <summary>
    /// Legacy fallback conversion scale: 0.083 meters per MMD unit.
    /// </summary>
    public const float LegacyScale = 0.083f;

    /// <summary>
    /// Validates that displacement scale is positive and finite.
    /// </summary>
    public static float ValidateScale(float scale)
    {
        if (!float.IsFinite(scale) || scale <= 0.0f)
            throw new ArgumentException("root_displacement_scale must be finite and positive", nameof(scale));
        return scale;
    }

    /// <summary>
    /// Calculates controller displacement in MMD units from the evaluated rig state.
    /// Replicates Python root_displacement.py root_displacement_position(rig).
    /// </summary>
    public static Vector3 CalculateDisplacement(VirtualMMDRig rig)
    {
        if (rig.SourceStructureVersion >= 2)
        {
            // For rig version >= 2 (source skeleton / explicit Hips):
            // position = lower_body.world_pos - rig.source_hips_rest_position
            return rig.LowerBody.WorldPosition - rig.SourceHipsRestPosition;
        }

        // Legacy controller displacement from Groove bone:
        // position = groove.world_pos - np.array([0.0, center_y, 0.0])
        return rig.Groove.WorldPosition - new Vector3(0.0f, rig.CenterY, 0.0f);
    }

    /// <summary>
    /// Converts a frame-0 displacement in MMD units into meters using the validated displacement scale.
    /// </summary>
    public static Vector3 ComputeOrigin(Vector3 displacementMmd, float scale = DefaultScale)
    {
        ValidateScale(scale);
        return displacementMmd * scale;
    }

    /// <summary>
    /// Computes dense root trajectory array in meters, relative to frame 0 origin.
    /// Replicates convert.py / vmd2gkclip.py root trajectory pipeline.
    /// </summary>
    public static Vector3[] ComputeTrajectory(IReadOnlyList<Vector3> displacementsMmd, float scale = DefaultScale)
    {
        ValidateScale(scale);

        int totalFrames = displacementsMmd.Count;
        var trajectory = new Vector3[totalFrames];
        if (totalFrames == 0)
            return trajectory;

        Vector3 originMmd = displacementsMmd[0];
        for (int f = 0; f < totalFrames; f++)
        {
            Vector3 relDisp = displacementsMmd[f] - originMmd;
            trajectory[f] = relDisp * scale;
        }

        return trajectory;
    }

    /// <summary>
    /// Computes Hips root trajectory from world positions and rest position.
    /// </summary>
    public static Vector3[] ComputeHipsTrajectory(
        IReadOnlyList<Vector3> lowerBodyWorldPositions,
        Vector3 hipsRestPosition,
        float scale = DefaultScale)
    {
        ValidateScale(scale);

        int totalFrames = lowerBodyWorldPositions.Count;
        var trajectory = new Vector3[totalFrames];
        if (totalFrames == 0)
            return trajectory;

        Vector3 originLowerBody = lowerBodyWorldPositions[0];
        for (int f = 0; f < totalFrames; f++)
        {
            Vector3 relHips = lowerBodyWorldPositions[f] - originLowerBody;
            trajectory[f] = relHips * scale;
        }

        return trajectory;
    }
}
