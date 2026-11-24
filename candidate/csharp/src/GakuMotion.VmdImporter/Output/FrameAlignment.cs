namespace GakuMotion.VmdImporter.Output;

using System.Numerics;
using GakuMotion.VmdImporter.Core;
using GakuMotion.VmdImporter.Rig;

/// <summary>
/// Frame alignment re-basing (K matrix) calculation, coordinate system conversion, and offset drift assertions.
/// Strictly replicates vmd2gkclip.py:78-310 and motionweave.retarget.
/// </summary>
public static class FrameAlignment
{
    /// <summary>
    /// Target child offset directions in the target's local frame.
    /// Only bones whose source and target axes disagree need an entry; everything else stays identity.
    /// </summary>
    public static readonly IReadOnlyDictionary<int, Vector3> TargetOffsetDirections = new Dictionary<int, Vector3>
    {
        [1] = new Vector3(-1.0f, 0.0f, 0.0f),         // LeftUpperLeg -> LeftLowerLeg
        [2] = new Vector3(+1.0f, 0.0f, 0.0f),         // RightUpperLeg -> RightLowerLeg
        [3] = new Vector3(-1.0f, 0.0f, 0.0f),         // LeftLowerLeg -> LeftFoot
        [4] = new Vector3(+1.0f, 0.0f, 0.0f),         // RightLowerLeg -> RightFoot
        [5] = new Vector3(-0.101174f, -0.057729f, 0.0f), // LeftFoot -> LeftToes
        [6] = new Vector3(+0.101174f, +0.057729f, 0.0f), // RightFoot -> RightToes
        [13] = new Vector3(-1.0f, 0.0f, 0.0f),        // LeftUpperArm -> LeftLowerArm
        [14] = new Vector3(+1.0f, 0.0f, 0.0f),        // RightUpperArm -> RightLowerArm
        [15] = new Vector3(-1.0f, 0.0f, 0.0f),        // LeftLowerArm -> LeftHand
        [16] = new Vector3(+1.0f, 0.0f, 0.0f),        // RightLowerArm -> RightHand
    };

    /// <summary>
    /// Source-side child offset directions of the real MMD model class (unit vectors).
    /// Legs use a vertical neutral (0, -1, 0) reference rather than YYB's ~5° thigh /
    /// ~1° shin inward rest tilt so neutral/FlowHMR legs do not artificially adduct.
    /// </summary>
    public static readonly IReadOnlyDictionary<string, Vector3> SourceOffsetDirections = new Dictionary<string, Vector3>
    {
        ["左ひざ"] = new Vector3(0.0f, -1.0f, 0.0f),
        ["右ひざ"] = new Vector3(0.0f, -1.0f, 0.0f),
        ["左足首"] = new Vector3(0.0f, -1.0f, 0.0f),
        ["右足首"] = new Vector3(0.0f, -1.0f, 0.0f),
        ["左ひじ"] = new Vector3(-0.1680042f, -0.1301597f, +0.0035826f),
        ["右ひじ"] = new Vector3(+0.1680042f, -0.1301597f, +0.0035826f),
        ["左手首"] = new Vector3(-0.1619331f, -0.1181167f, +0.0039037f),
        ["右手首"] = new Vector3(+0.1619331f, -0.1181167f, +0.0039037f),
    };

    /// <summary>
    /// Source thumb segment directions measured in source thumb frame from Track A.
    /// </summary>
    public static readonly IReadOnlyDictionary<int, Vector3> SourceThumbSegmentDirections = new Dictionary<int, Vector3>
    {
        [17] = new Vector3(-0.0151710f, -0.0168640f, +0.0078898f),
        [18] = new Vector3(+0.0151710f, -0.0168640f, +0.0078898f),
    };

    /// <summary>
    /// Target humanoid ID -> MMD child bone name defining the direction.
    /// </summary>
    public static readonly IReadOnlyDictionary<int, string> AlignViaChild = new Dictionary<int, string>
    {
        [1] = "左ひざ",
        [2] = "右ひざ",
        [3] = "左足首",
        [4] = "右足首",
        [5] = "左つま先",
        [6] = "右つま先",
        [13] = "左ひじ",
        [14] = "右ひじ",
        [15] = "左手首",
        [16] = "右手首",
    };

    /// <summary>
    /// Parent relations along the Gakumas humanoid skeleton.
    /// </summary>
    public static readonly IReadOnlyDictionary<string, string> GakumasParent = new Dictionary<string, string>
    {
        ["Spine"] = "Hips",
        ["Chest"] = "Spine",
        ["UpperChest"] = "Chest",
        ["Neck"] = "UpperChest",
        ["Head"] = "Neck",
        ["LeftEye"] = "Head",
        ["RightEye"] = "Head",
        ["LeftShoulder"] = "UpperChest",
        ["RightShoulder"] = "UpperChest",
        ["LeftUpperArm"] = "LeftShoulder",
        ["RightUpperArm"] = "RightShoulder",
        ["LeftLowerArm"] = "LeftUpperArm",
        ["RightLowerArm"] = "RightUpperArm",
        ["LeftHand"] = "LeftLowerArm",
        ["RightHand"] = "RightLowerArm",
        ["LeftUpperLeg"] = "Hips",
        ["RightUpperLeg"] = "Hips",
        ["LeftLowerLeg"] = "LeftUpperLeg",
        ["RightLowerLeg"] = "RightUpperLeg",
        ["LeftFoot"] = "LeftLowerLeg",
        ["RightFoot"] = "RightLowerLeg",
        ["LeftToes"] = "LeftFoot",
        ["RightToes"] = "RightFoot",
        ["LeftThumbProximal"] = "LeftHand",
        ["LeftThumbIntermediate"] = "LeftThumbProximal",
        ["LeftThumbDistal"] = "LeftThumbIntermediate",
        ["LeftIndexProximal"] = "LeftHand",
        ["LeftIndexIntermediate"] = "LeftIndexProximal",
        ["LeftIndexDistal"] = "LeftIndexIntermediate",
        ["LeftMiddleProximal"] = "LeftHand",
        ["LeftMiddleIntermediate"] = "LeftMiddleProximal",
        ["LeftMiddleDistal"] = "LeftMiddleIntermediate",
        ["LeftRingProximal"] = "LeftHand",
        ["LeftRingIntermediate"] = "LeftRingProximal",
        ["LeftRingDistal"] = "LeftRingIntermediate",
        ["LeftLittleProximal"] = "LeftHand",
        ["LeftLittleIntermediate"] = "LeftLittleProximal",
        ["LeftLittleDistal"] = "LeftLittleIntermediate",
        ["RightThumbProximal"] = "RightHand",
        ["RightThumbIntermediate"] = "RightThumbProximal",
        ["RightThumbDistal"] = "RightThumbIntermediate",
        ["RightIndexProximal"] = "RightHand",
        ["RightIndexIntermediate"] = "RightIndexProximal",
        ["RightIndexDistal"] = "RightIndexIntermediate",
        ["RightMiddleProximal"] = "RightHand",
        ["RightMiddleIntermediate"] = "RightMiddleProximal",
        ["RightMiddleDistal"] = "RightMiddleIntermediate",
        ["RightRingProximal"] = "RightHand",
        ["RightRingIntermediate"] = "RightRingProximal",
        ["RightRingDistal"] = "RightRingIntermediate",
        ["RightLittleProximal"] = "RightHand",
        ["RightLittleIntermediate"] = "RightLittleProximal",
        ["RightLittleDistal"] = "RightLittleIntermediate",
    };

    /// <summary>
    /// Unit quaternion taking direction u to direction v (minimal arc).
    /// Replicates Python vmd2gkclip.py _fromto(u, v).
    /// </summary>
    public static Quaternion FromTo(Vector3 u, Vector3 v)
    {
        double uLen = Math.Sqrt((double)u.X * u.X + (double)u.Y * u.Y + (double)u.Z * u.Z);
        double vLen = Math.Sqrt((double)v.X * v.X + (double)v.Y * v.Y + (double)v.Z * v.Z);
        if (uLen < 1e-12 || vLen < 1e-12)
            return Quaternion.Identity;

        Vector3 un = new((float)(u.X / uLen), (float)(u.Y / uLen), (float)(u.Z / uLen));
        Vector3 vn = new((float)(v.X / vLen), (float)(v.Y / vLen), (float)(v.Z / vLen));

        float d = Vector3.Dot(un, vn);
        if (d > 1.0f - 1e-9f)
            return Quaternion.Identity;

        if (d < -(1.0f - 1e-9f))
        {
            Vector3 ax = MathF.Abs(un.X) < 0.9f ? new Vector3(1.0f, 0.0f, 0.0f) : new Vector3(0.0f, 1.0f, 0.0f);
            Vector3 w = Vector3.Cross(un, ax);
            float wLen = w.Length();
            if (wLen > 0.0f) w /= wLen;
            return new Quaternion(w.X, w.Y, w.Z, 0.0f);
        }

        Vector3 cross = Vector3.Cross(un, vn);
        Quaternion q = new Quaternion(cross.X, cross.Y, cross.Z, 1.0f + d);
        return Math32.NormalizeQuaternionWxyz(q);
    }

    /// <summary>
    /// Computes orthonormal basis (x, y, z) from primary and hinge axes in double precision.
    /// Replicates MotionWeave retarget.py _basis(primary, hinge).
    /// </summary>
    private static void ComputeBasisDouble(
        Vector3 primary, Vector3 hinge,
        out double x0, out double x1, out double x2,
        out double y0, out double y1, out double y2,
        out double z0, out double z1, out double z2)
    {
        double px = primary.X, py = primary.Y, pz = primary.Z;
        double pLen = Math.Sqrt(px * px + py * py + pz * pz);
        if (pLen < 1e-12)
            throw new ArgumentException("primary axis must be nonzero", nameof(primary));
        x0 = px / pLen; x1 = py / pLen; x2 = pz / pLen;

        double hx = hinge.X, hy = hinge.Y, hz = hinge.Z;
        double dot = x0 * hx + x1 * hy + x2 * hz;
        double ypx = hx - x0 * dot;
        double ypy = hy - x1 * dot;
        double ypz = hz - x2 * dot;
        double yLen = Math.Sqrt(ypx * ypx + ypy * ypy + ypz * ypz);
        if (yLen < 1e-8)
            throw new ArgumentException("primary and hinge axes must not be parallel");
        y0 = ypx / yLen; y1 = ypy / yLen; y2 = ypz / yLen;

        z0 = x1 * y2 - x2 * y1;
        z1 = x2 * y0 - x0 * y2;
        z2 = x0 * y1 - x1 * y0;
    }

    /// <summary>
    /// Computes frame alignment unit quaternion K mapping target bone frame onto source bone frame.
    /// Replicates MotionWeave retarget.py align_bone_frames.
    /// </summary>
    public static Quaternion AlignBoneFrames(Vector3 sourcePrimary, Vector3 sourceHinge, Vector3 targetPrimary, Vector3 targetHinge)
    {
        ComputeBasisDouble(sourcePrimary, sourceHinge,
            out double sx0, out double sx1, out double sx2,
            out double sy0, out double sy1, out double sy2,
            out double sz0, out double sz1, out double sz2);

        ComputeBasisDouble(targetPrimary, targetHinge,
            out double tx0, out double tx1, out double tx2,
            out double ty0, out double ty1, out double ty2,
            out double tz0, out double tz1, out double tz2);

        // M = Source * Target^T in double precision
        // S columns: [sx, sy, sz], T columns: [tx, ty, tz]
        // M_rc = S_r0 * T_c0 + S_r1 * T_c1 + S_r2 * T_c2
        Matrix3x3 m = new(
            (float)(sx0 * tx0 + sy0 * ty0 + sz0 * tz0),
            (float)(sx0 * tx1 + sy0 * ty1 + sz0 * tz1),
            (float)(sx0 * tx2 + sy0 * ty2 + sz0 * tz2),

            (float)(sx1 * tx0 + sy1 * ty0 + sz1 * tz0),
            (float)(sx1 * tx1 + sy1 * ty1 + sz1 * tz1),
            (float)(sx1 * tx2 + sy1 * ty2 + sz1 * tz2),

            (float)(sx2 * tx0 + sy2 * ty0 + sz2 * tz0),
            (float)(sx2 * tx1 + sy2 * ty1 + sz2 * tz1),
            (float)(sx2 * tx2 + sy2 * ty2 + sz2 * tz2)
        );

        return MatrixToQuat(m);
    }

    /// <summary>
    /// Shepperd's algorithm for converting a 3x3 orthonormal rotation matrix to a normalized Quaternion.
    /// </summary>
    private static Quaternion MatrixToQuat(in Matrix3x3 m)
    {
        double m00 = m.M00, m01 = m.M01, m02 = m.M02;
        double m10 = m.M10, m11 = m.M11, m12 = m.M12;
        double m20 = m.M20, m21 = m.M21, m22 = m.M22;

        double tr = m00 + m11 + m22;
        double x, y, z, w;
        if (tr > 0.0)
        {
            double s = Math.Sqrt(tr + 1.0) * 2.0;
            double invS = 1.0 / s;
            w = 0.25 * s;
            x = (m21 - m12) * invS;
            y = (m02 - m20) * invS;
            z = (m10 - m01) * invS;
        }
        else if (m00 > m11 && m00 > m22)
        {
            double s = Math.Sqrt(1.0 + m00 - m11 - m22) * 2.0;
            double invS = 1.0 / s;
            w = (m21 - m12) * invS;
            x = 0.25 * s;
            y = (m01 + m10) * invS;
            z = (m02 + m20) * invS;
        }
        else if (m11 > m22)
        {
            double s = Math.Sqrt(1.0 + m11 - m00 - m22) * 2.0;
            double invS = 1.0 / s;
            w = (m02 - m20) * invS;
            x = (m01 + m10) * invS;
            y = 0.25 * s;
            z = (m12 + m21) * invS;
        }
        else
        {
            double s = Math.Sqrt(1.0 + m22 - m00 - m11) * 2.0;
            double invS = 1.0 / s;
            w = (m10 - m01) * invS;
            x = (m02 + m20) * invS;
            y = (m12 + m21) * invS;
            z = 0.25 * s;
        }

        return Math32.NormalizeQuaternionWxyz(new Quaternion((float)x, (float)y, (float)z, (float)w));
    }

    /// <summary>
    /// Align the segment AND its bend plane.
    /// Replicates vmd2gkclip.py align_bone_frame.
    /// </summary>
    public static Quaternion AlignBoneFrame(Vector3 targetDirection, Vector3 sourceDirection)
    {
        return AlignBoneFrames(sourceDirection, new Vector3(1.0f, 0.0f, 0.0f),
                               targetDirection, new Vector3(0.0f, 0.0f, 1.0f));
    }

    /// <summary>
    /// Thumb frame alignment with extra X 90° turn and spread derived from geometry.
    /// Replicates vmd2gkclip.py thumb_frame_alignment.
    /// </summary>
    public static Quaternion ThumbFrameAlignment(Quaternion handAlignment, int handHid)
    {
        if (!SourceThumbSegmentDirections.TryGetValue(handHid, out var thumbDir))
            throw new ArgumentException($"Unknown hand humanoid id: {handHid}", nameof(handHid));

        Vector3 direction = Vector3.Transform(thumbDir, Quaternion.Inverse(handAlignment));
        float spread = MathF.Atan2(MathF.Abs(direction.Z), MathF.Abs(direction.X));
        Quaternion ry = Quaternion.CreateFromAxisAngle(Vector3.UnitY, spread);
        Quaternion rx90 = Quaternion.CreateFromAxisAngle(Vector3.UnitX, MathF.PI * 0.5f);
        return Math32.NormalizeQuaternionWxyz(handAlignment * ry * rx90);
    }

    /// <summary>
    /// Builds the K frame alignment quaternion map for all aligned humanoid bones.
    /// Replicates vmd2gkclip.py build_frame_alignment.
    /// </summary>
    public static Dictionary<int, Quaternion> BuildFrameAlignment(
        VirtualMMDRig rig,
        IReadOnlyDictionary<string, Quaternion> worldRotsRef,
        IReadOnlyDictionary<string, Vector3> posRef)
    {
        var K = new Dictionary<int, Quaternion>();

        foreach (var (hid, childName) in AlignViaChild)
        {
            if (!rig.Bones.TryGetValue(childName, out var childBone))
                continue;
            var par = childBone.Parent;
            if (par == null || !rig.Bones.ContainsKey(par.Name))
                continue;

            Vector3 o;
            if (SourceOffsetDirections.TryGetValue(childName, out var tblOffset))
            {
                o = tblOffset;
            }
            else
            {
                if (posRef.TryGetValue(childName, out var pChild) &&
                    posRef.TryGetValue(par.Name, out var pPar) &&
                    worldRotsRef.TryGetValue(par.Name, out var parWorld))
                {
                    Vector3 d = pChild - pPar;
                    o = Vector3.Transform(d, Quaternion.Inverse(parWorld));
                }
                else
                {
                    o = childBone.RestOffset;
                }
            }

            if (hid is 13 or 14 or 15 or 16)
            {
                Quaternion q = FromTo(TargetOffsetDirections[hid], o);
                if (hid is 14 or 16)
                {
                    // Mirror right arm with Rx180: (1, 0, 0, 0)
                    Quaternion rx180 = new(1.0f, 0.0f, 0.0f, 0.0f);
                    q = q * rx180;
                }
                K[hid] = Math32.NormalizeQuaternionWxyz(q);
            }
            else
            {
                K[hid] = AlignBoneFrame(TargetOffsetDirections[hid], o);
            }
        }

        // Leaf toes: 5 -> 19, 6 -> 20
        if (K.TryGetValue(5, out var k5)) K[19] = k5;
        if (K.TryGetValue(6, out var k6)) K[20] = k6;

        // RightShoulder (12): exactly Rx180
        K[12] = new Quaternion(1.0f, 0.0f, 0.0f, 0.0f);

        // Left arm & hand & fingers: arm=15, hand=17, fingers=27..38, thumb=24,25,26
        if (K.TryGetValue(15, out var k15))
        {
            K[17] = k15;
            for (int hid = 27; hid <= 38; hid++) K[hid] = k15;
            Quaternion leftThumb = ThumbFrameAlignment(k15, 17);
            K[24] = leftThumb;
            K[25] = leftThumb;
            K[26] = leftThumb;
        }

        // Right arm & hand & fingers: arm=16, hand=18, fingers=42..53, thumb=39,40,41
        if (K.TryGetValue(16, out var k16))
        {
            K[18] = k16;
            for (int hid = 42; hid <= 53; hid++) K[hid] = k16;
            Quaternion rightThumb = ThumbFrameAlignment(k16, 18);
            K[39] = rightThumb;
            K[40] = rightThumb;
            K[41] = rightThumb;
        }

        return K;
    }

    /// <summary>
    /// Measures maximum angular drift of aligned source offsets between two frames in degrees.
    /// Replicates vmd2gkclip.py measure_offset_drift.
    /// </summary>
    public static float MeasureOffsetDrift(
        VirtualMMDRig rig,
        IReadOnlyDictionary<string, Quaternion> worldRef,
        IReadOnlyDictionary<string, Vector3> posRef,
        IReadOnlyDictionary<string, Quaternion> worldLate,
        IReadOnlyDictionary<string, Vector3> posLate)
    {
        float worst = 0.0f;
        foreach (var (hid, childName) in AlignViaChild)
        {
            if (!rig.Bones.TryGetValue(childName, out var childBone))
                continue;
            var par = childBone.Parent;
            if (par == null) continue;
            string pn = par.Name;

            Vector3 dRef = posRef[childName] - posRef[pn];
            Vector3 dLate = posLate[childName] - posLate[pn];

            Vector3 a = Vector3.Transform(dRef, Quaternion.Inverse(worldRef[pn]));
            Vector3 b = Vector3.Transform(dLate, Quaternion.Inverse(worldLate[pn]));

            Quaternion diffQ = FromTo(a, b);
            float dot = Math.Clamp(MathF.Abs(diffQ.W), -1.0f, 1.0f);
            float angDeg = 2.0f * MathF.Acos(dot) * (180.0f / MathF.PI);
            if (angDeg > worst) worst = angDeg;
        }
        return worst;
    }

    /// <summary>
    /// Asserts offset drift <= maxAllowedDegrees (default: 1.0°).
    /// </summary>
    public static void AssertOffsetDrift(float drift, float maxAllowedDegrees = 1.0f)
    {
        if (drift > maxAllowedDegrees)
        {
            throw new InvalidOperationException(
                $"frame alignment: source child offsets are NOT frame-constant (drift {drift:F2}° > {maxAllowedDegrees:F2}°) -- K alignment refused.");
        }
    }

    /// <summary>
    /// Decomposes world rotations into canonical local deltas along the Gakumas parent chain.
    /// Replicates MotionWeave retarget.py world_to_local_rotations.
    /// </summary>
    public static Quaternion[] WorldToCanonicalDeltas(
        IReadOnlyList<string> order,
        Quaternion[] worldQuats,
        IReadOnlyDictionary<string, string?> gkParents)
    {
        int count = order.Count;
        var deltas = new Quaternion[count];
        if (count == 0) return deltas;

        var nameToIndex = new Dictionary<string, int>(count);
        for (int i = 0; i < count; i++)
            nameToIndex[order[i]] = i;

        int[] parentIndices = new int[count];
        for (int i = 0; i < count; i++)
        {
            string name = order[i];
            if (gkParents.TryGetValue(name, out var parentName) && parentName != null && nameToIndex.TryGetValue(parentName, out int pIdx))
                parentIndices[i] = pIdx;
            else
                parentIndices[i] = -1;
        }

        for (int i = 0; i < count; i++)
        {
            int pIdx = parentIndices[i];
            Quaternion local = pIdx == -1
                ? worldQuats[i]
                : Quaternion.Inverse(worldQuats[pIdx]) * worldQuats[i];

            deltas[i] = Math32.NormalizeQuaternionWxyz(local);
        }

        return deltas;
    }
}
