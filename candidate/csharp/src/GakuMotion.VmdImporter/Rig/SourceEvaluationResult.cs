namespace GakuMotion.VmdImporter.Rig;

using System.Numerics;

/// <summary>
/// Output data structure from SourceEvaluator containing dense 82-bone world transforms, root trajectory, and morph weights.
/// </summary>
public sealed class SourceEvaluationResult
{
    public int TotalFrames { get; init; }
    public float Fps { get; init; } = 30.0f;
    public float DurationSeconds => TotalFrames / Fps;

    public IReadOnlyList<string> BoneNames { get; init; } = Array.Empty<string>();

    /// <summary>
    /// Dense joint world rotations: [frame, boneIndex]
    /// </summary>
    public Quaternion[,] DenseRotations { get; init; } = new Quaternion[0, 0];

    /// <summary>
    /// Dense joint world positions: [frame, boneIndex]
    /// </summary>
    public Vector3[,] DensePositions { get; init; } = new Vector3[0, 0];

    /// <summary>
    /// Root trajectory in meters (scale 0.07795 m/unit) relative to frame 0: [frame]
    /// </summary>
    public Vector3[] RootTrajectory { get; init; } = Array.Empty<Vector3>();

    /// <summary>
    /// Frame 0 world hips displacement in meters (scale 0.07795 m/unit) prior to frame-0 origin subtraction.
    /// </summary>
    public Vector3 RootOrigin { get; init; }

    /// <summary>
    /// Evaluated morph curve weights per frame: [morphName] -> float[frame]
    /// </summary>
    public IReadOnlyDictionary<string, float[]> MorphWeights { get; init; } = new Dictionary<string, float[]>();
}
