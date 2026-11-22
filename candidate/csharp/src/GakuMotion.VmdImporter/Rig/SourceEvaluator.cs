namespace GakuMotion.VmdImporter.Rig;

using System.Numerics;
using GakuMotion.VmdImporter.Core;
using GakuMotion.VmdImporter.Vmd;

/// <summary>
/// Evaluates dense frame-by-frame 82-bone source joint world transforms, root trajectory, and morph weights from VmdMotion.
/// Strictly replicates convert.py convert_vmd_to_gkmotion evaluation loop.
/// </summary>
public sealed class SourceEvaluator
{
    public const float DefaultRootDisplacementScale = 0.07795f; // meters / MMD unit

    public VirtualMMDRig Rig { get; }
    public float RootScale { get; }

    public SourceEvaluator(VirtualMMDRig? rig = null, float rootScale = DefaultRootDisplacementScale)
    {
        Rig = rig ?? new VirtualMMDRig(useSourceSkeleton: true);
        RootScale = rootScale;
    }

    /// <summary>
    /// Evaluates dense frame-by-frame joint world transforms from a VMD motion.
    /// </summary>
    public SourceEvaluationResult Evaluate(VmdMotion motion, int startFrame = 0, int endFrame = -1, float fps = 30.0f)
    {
        int totalFrames = endFrame >= 0 ? (endFrame + 1) : (int)(motion.MaxFrame + 1);
        if (totalFrames <= 0) totalFrames = 1;

        // 1. Prepare native animation if version >= 3
        Rig.PrepareNativeAnimation(motion, totalFrames);

        // 2. Pre-sample all bone keyframes present in VMD
        var sampledPos = new Dictionary<string, Vector3[]>(motion.BoneKeyframes.Count);
        var sampledRot = new Dictionary<string, Quaternion[]>(motion.BoneKeyframes.Count);
        foreach (var (name, kfs) in motion.BoneKeyframes)
        {
            var (pos, rot) = MotionSampling.SampleBoneKeyframes(kfs, totalFrames, RotationInterpolationMode.NormalizedComponent);
            sampledPos[name] = pos;
            sampledRot[name] = rot;
        }

        // 3. Resolve IK states from property keyframes
        var (leftFootIk, rightFootIk, leftToeIk, rightToeIk) = ResolveIkStates(motion, totalFrames);

        // 4. Evaluate frames
        var boneNames = Rig.Bones.Keys.ToList();
        int boneCount = boneNames.Count;

        var denseRotations = new Quaternion[totalFrames, boneCount];
        var densePositions = new Vector3[totalFrames, boneCount];
        var rootTrajectory = new Vector3[totalFrames];

        for (int f = startFrame; f < totalFrames; f++)
        {
            var rotMap = new Dictionary<string, Quaternion>(sampledRot.Count);
            foreach (var (name, rArr) in sampledRot)
            {
                if (f < rArr.Length)
                {
                    rotMap[name] = Math32.ToUnityRotation(rArr[f]);
                }
            }

            var posMap = new Dictionary<string, Vector3>(sampledPos.Count);
            foreach (var (name, pArr) in sampledPos)
            {
                if (f < pArr.Length)
                {
                    posMap[name] = pArr[f];
                }
            }

            var worldRots = Rig.EvaluateFrame(
                f, rotMap, posMap,
                leftLegIk: leftFootIk[f],
                rightLegIk: rightFootIk[f],
                leftToeIk: leftToeIk[f],
                rightToeIk: rightToeIk[f]);

            for (int bIdx = 0; bIdx < boneCount; bIdx++)
            {
                string bName = boneNames[bIdx];
                denseRotations[f, bIdx] = worldRots[bName];
                densePositions[f, bIdx] = Rig.Bones[bName].WorldPosition;
            }

            // Root trajectory: (lower_body.world_pos - hips_rest_pos) * root_scale
            Vector3 hipsDisp = Rig.LowerBody.WorldPosition - Rig.SourceHipsRestPosition;
            rootTrajectory[f] = hipsDisp * RootScale;
        }

        // Subtract frame 0 origin for root trajectory (convert.py / root_displacement.py)
        Vector3 rootOrigin = Vector3.Zero;
        if (totalFrames > 0)
        {
            Vector3 origin = rootTrajectory[0];
            rootOrigin = origin;
            for (int f = 0; f < totalFrames; f++)
            {
                rootTrajectory[f] -= origin;
            }
        }

        // 5. Morph weights
        var morphWeights = new Dictionary<string, float[]>(motion.MorphKeyframes.Count);
        foreach (var (name, morphKfs) in motion.MorphKeyframes)
        {
            float[] weights = new float[totalFrames];
            if (motion.MorphTracks.TryGetValue(name, out var morphTrack))
            {
                for (int f = 0; f < totalFrames; f++)
                {
                    weights[f] = morphTrack.Sample(f);
                }
            }
            morphWeights[name] = weights;
        }

        return new SourceEvaluationResult
        {
            TotalFrames = totalFrames,
            Fps = fps,
            BoneNames = boneNames,
            DenseRotations = denseRotations,
            DensePositions = densePositions,
            RootTrajectory = rootTrajectory,
            RootOrigin = rootOrigin,
            MorphWeights = morphWeights
        };
    }

    private static (bool[] LeftFoot, bool[] RightFoot, bool[] LeftToe, bool[] RightToe) ResolveIkStates(VmdMotion motion, int totalFrames)
    {
        bool[] leftFoot = new bool[totalFrames];
        bool[] rightFoot = new bool[totalFrames];
        bool[] leftToe = new bool[totalFrames];
        bool[] rightToe = new bool[totalFrames];

        Array.Fill(leftFoot, true);
        Array.Fill(rightFoot, true);
        Array.Fill(leftToe, true);
        Array.Fill(rightToe, true);

        if (motion.PropertyKeyframes.Count == 0)
            return (leftFoot, rightFoot, leftToe, rightToe);

        var sortedKeys = motion.PropertyKeyframes.OrderBy(k => k.Frame).ToList();

        bool curLeftFoot = true, curRightFoot = true, curLeftToe = true, curRightToe = true;
        int kIdx = 0;

        for (int f = 0; f < totalFrames; f++)
        {
            while (kIdx < sortedKeys.Count && sortedKeys[kIdx].Frame <= (uint)f)
            {
                var pk = sortedKeys[kIdx];
                if (pk.IkSwitches.TryGetValue("左足ＩＫ", out bool lf)) curLeftFoot = lf;
                if (pk.IkSwitches.TryGetValue("右足ＩＫ", out bool rf)) curRightFoot = rf;
                if (pk.IkSwitches.TryGetValue("左つま先ＩＫ", out bool lt)) curLeftToe = lt;
                if (pk.IkSwitches.TryGetValue("右つま先ＩＫ", out bool rt)) curRightToe = rt;
                kIdx++;
            }

            leftFoot[f] = curLeftFoot;
            rightFoot[f] = curRightFoot;
            leftToe[f] = curLeftToe;
            rightToe[f] = curRightToe;
        }

        return (leftFoot, rightFoot, leftToe, rightToe);
    }
}
