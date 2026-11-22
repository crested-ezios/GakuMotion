namespace GakuMotion.VmdImporter.Native;

using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;
using GakuMotion.VmdImporter.Native.Facial;
using GakuMotion.VmdImporter.Native.Timebase;
using GakuMotion.VmdImporter.Output;
using GakuMotion.VmdImporter.Rig;
using GakuMotion.VmdImporter.Vmd;

internal static partial class ImportPipeline
{
    private static List<SlotSpec> NormalizeNewImportSlots(ImportRequest request)
    {
        if (request.Positions is not { Count: > 0 })
        {
            return
            [
                new SlotSpec(
                    Slot: 1,
                    SourceSlot: 0,
                    MotionVmd: request.MotionVmd,
                    MorphVmd: request.MorphVmd,
                    DisableMorph: request.DisableMorph,
                    ClearMorph: false,
                    SourceFps: request.SourceFps,
                    RebakeOutput: request.RebakeOutput,
                    PreserveExisting: false,
                    X: 0.0f,
                    Y: 0.0f,
                    Z: 0.0f,
                    YawDegrees: 0.0f)
            ];
        }

        var sorted = new List<SlotSpec>(request.Positions.Count + 1);
        var seenSlots = new HashSet<int>();
        foreach (PositionRequest p in request.Positions)
        {
            if (!seenSlots.Add(p.Slot))
                throw new InvalidDataException($"positions 中存在重复槽位: {p.Slot}");
            sorted.Add(new SlotSpec(
                Slot: p.Slot,
                SourceSlot: p.SourceSlot,
                MotionVmd: p.MotionVmd,
                MorphVmd: p.MorphVmd,
                DisableMorph: p.DisableMorph,
                ClearMorph: p.ClearMorph,
                SourceFps: p.SourceFps > 0 ? p.SourceFps : request.SourceFps,
                RebakeOutput: p.RebakeOutput || request.RebakeOutput,
                PreserveExisting: p.PreserveExisting,
                X: CleanZero(p.X),
                Y: CleanZero(p.Y),
                Z: CleanZero(p.Z),
                YawDegrees: CleanZero(p.YawDegrees)));
        }

        if (!seenSlots.Contains(1))
        {
            if (string.IsNullOrEmpty(request.MotionVmd))
                throw new InvalidDataException("缺少 1 号位动作 VMD 文件");
            sorted.Add(new SlotSpec(
                Slot: 1,
                SourceSlot: 0,
                MotionVmd: request.MotionVmd,
                MorphVmd: request.MorphVmd,
                DisableMorph: request.DisableMorph,
                ClearMorph: false,
                SourceFps: request.SourceFps,
                RebakeOutput: request.RebakeOutput,
                PreserveExisting: false,
                X: 0.0f,
                Y: 0.0f,
                Z: 0.0f,
                YawDegrees: 0.0f));
        }

        sorted.Sort((a, b) => a.Slot.CompareTo(b.Slot));
        if (sorted.Count > 0 && sorted[0].Slot == 1 && string.IsNullOrEmpty(sorted[0].MotionVmd) &&
            !string.IsNullOrEmpty(request.MotionVmd))
        {
            sorted[0] = sorted[0] with { MotionVmd = request.MotionVmd };
        }
        for (int i = 0; i < sorted.Count; i++)
        {
            if (sorted[i].Slot != i + 1)
                throw new InvalidDataException($"新建导入 positions 槽位必须从 1 开始连续编号（期望 {i + 1}，实际 {sorted[i].Slot}）");
            if (string.IsNullOrEmpty(sorted[i].MotionVmd))
                throw new InvalidDataException($"槽位 {sorted[i].Slot} 缺少动作 VMD 文件");
        }

        // Slot 1 is always normalized to origin (0, 0, 0, 0) and inherits top-level morph/disable if omitted.
        SlotSpec s1 = sorted[0];
        sorted[0] = s1 with
        {
            MorphVmd = !string.IsNullOrEmpty(s1.MorphVmd) ? s1.MorphVmd : (s1.DisableMorph ? string.Empty : request.MorphVmd),
            DisableMorph = s1.DisableMorph || request.DisableMorph,
            X = 0.0f,
            Y = 0.0f,
            Z = 0.0f,
            YawDegrees = 0.0f
        };

        return sorted;
    }

    private static BakedSlotResult BakeSingleSlot(
        string vmdPath,
        string morphPath,
        bool disableMorph,
        float requestedSourceFps,
        float requestedOutputFps,
        string toothChannel,
        string? audioPath,
        ImportLog log,
        Action<int, string, string> progress,
        int progressBase,
        int progressSpan)
    {
        ValidateSourceFpsValue(requestedSourceFps);

        vmdPath = Path.GetFullPath(vmdPath);
        if (!File.Exists(vmdPath)) throw new InvalidDataException("找不到 VMD 文件: " + vmdPath);
        if (!string.Equals(Path.GetExtension(vmdPath), ".vmd", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("请选择 .vmd 动作文件");

        string geometryPath = Path.Combine(Path.GetDirectoryName(vmdPath)!, TargetGeometryName);
        if (File.Exists(geometryPath) && GeometryBindsVmd(geometryPath, vmdPath))
        {
            log.Line("该 VMD 旁存在已绑定的 target_leg_geometry.json：C# 后端未实现目标几何烘焙。");
            throw new ImportStepException("unsupported_target_geometry",
                "该 VMD 旁存在已绑定的 target_leg_geometry.json；原生目标几何腿部烘焙尚未实现；未修改曲目。");
        }

        long vmdBytes = new FileInfo(vmdPath).Length;
        var slotTiming = new ImportTiming { VmdBytes = vmdBytes };
        var sw = new Stopwatch();

        string resolvedMorph = string.Empty;
        VmdMotion? morphSource = null;
        if (!disableMorph && !string.IsNullOrEmpty(morphPath))
        {
            resolvedMorph = Path.GetFullPath(morphPath);
            if (!File.Exists(resolvedMorph)) throw new InvalidDataException("找不到表情 VMD 文件: " + resolvedMorph);
            if (!string.Equals(Path.GetExtension(resolvedMorph), ".vmd", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("表情文件必须是 .vmd");
            morphSource = VmdReader.Parse(resolvedMorph);
            if (morphSource.MorphKeyframes.Count == 0 && !EyeProfile.HasEyeTracks(morphSource))
                throw new InvalidDataException("所选表情文件不包含表情或眼神关键帧");
        }

        sw.Restart();
        VmdMotion motion = VmdReader.Parse(vmdPath);
        slotTiming.ParseMs = sw.Elapsed.TotalMilliseconds;
        progress(progressBase, "PARSE",
            $"VMD 已解析：{motion.BoneKeyframes.Count} 条骨骼轨道 / {motion.MorphKeyframes.Count} 条表情轨道，最大帧 {motion.MaxFrame}");

        if (motion.BoneKeyframes.Count == 0)
            throw new InvalidDataException("VMD 中没有骨骼动作（仅运镜/表情的 VMD 不支持）");
        bool hasBody = motion.BoneKeyframes.Keys.Any(name => !EyeOnlyBones.Contains(name));
        if (!hasBody)
            throw new InvalidDataException("VMD 中没有躯干骨骼动作（仅表情/眼动的 VMD 不支持作为动作）");

        float sourceFps = requestedSourceFps > 0 ? requestedSourceFps : DetectSourceFps(vmdPath, motion, audioPath);
        log.Line($"源动作帧率: {sourceFps:g} fps ({(requestedSourceFps > 0 ? "手动指定" : "自动识别")}); 身体末帧={BodyEndFrame(motion)}, 全部轨道末帧={motion.MaxFrame}");
        if (BodyEndFrame(motion) > 0 && motion.MaxFrame > BodyEndFrame(motion) * 1.25)
            log.Line("注意：表情或其他轨道明显长于身体动作；全部关键帧仍会保留，请检查同步。");
        float outputFps = requestedOutputFps > 0.0f ? requestedOutputFps : sourceFps;
        slotTiming.Fps = outputFps;

        bool overrideEyes = !disableMorph && morphSource != null &&
                            !string.Equals(resolvedMorph, vmdPath, StringComparison.OrdinalIgnoreCase);
        VmdMotion effective = overrideEyes ? WithEyeTracks(motion, morphSource!) : motion;
        effective = DeformLegTracks.Prepare(effective, out string[] deformLegSides);
        if (deformLegSides.Length != 0)
            log.Line("Detected baked leg tracks; adapted sides: " + string.Join(", ", deformLegSides));

        sw.Restart();
        var evaluator = new SourceEvaluator();
        SourceEvaluationResult result = evaluator.Evaluate(effective, fps: sourceFps);
        slotTiming.EvaluateMs = sw.Elapsed.TotalMilliseconds;
        progress(Math.Min(88, progressBase + (progressSpan * 65) / 100), "RETARGET", "正在转换全身骨骼局部姿态...");

        sw.Restart();
        bool hasEyeTracks = EyeProfile.HasEyeTracks(effective);
        GakumasBoneProfile profile = hasEyeTracks
            ? EyeProfile.Extend(GakumasBoneProfile.Default)
            : GakumasBoneProfile.Default;
        int version = hasEyeTracks ? 5 : GkClipFormat.DefaultVersion;

        GkClipData clip = GkClipEncoder.FromEvaluationResult(result, profile, evaluator.Rig, version);
        DisabledLegIkFk.Apply(effective, result, evaluator.Rig, clip);
        slotTiming.RetargetEncodeMs = sw.Elapsed.TotalMilliseconds;

        sw.Restart();
        progress(Math.Min(89, progressBase + (progressSpan * 85) / 100), "MORPH", "正在映射表情形态与眼神轨道...");
        IReadOnlyDictionary<string, IReadOnlyList<MorphKeyframe>> morphTracks =
            disableMorph
                ? new Dictionary<string, IReadOnlyList<MorphKeyframe>>()
                : (morphSource?.MorphKeyframes ?? motion.MorphKeyframes);
        string? resolvedTooth = disableMorph
            ? null
            : (string.IsNullOrEmpty(toothChannel) ? MorphTranslator.DefaultToothChannel : toothChannel);

        (List<string> names, float[,] weights, MorphReport report) =
            MorphTranslator.Translate(morphTracks, clip.TotalFrames, resolvedTooth);
        clip.BlendshapeNames = names;
        clip.BlendshapeWeights = names.Count > 0 ? weights : null;

        if (!ClipTimebase.IsIdentity(sourceFps, outputFps))
        {
            int sourceFrames = clip.TotalFrames;
            clip = ClipTimebase.Retime(clip, sourceFps, outputFps);
            log.Line($"timebase: source {sourceFps:g} fps x {sourceFrames} ticks -> " +
                     $"body {clip.Fps:g} fps x {clip.TotalFrames} frames, " +
                     $"duration {clip.DurationSeconds:F3}s unchanged");
        }

        byte[] bytes = GkClipEncoder.Encode(clip);
        slotTiming.FacialMs = sw.Elapsed.TotalMilliseconds;
        slotTiming.Frames = clip.TotalFrames;
        slotTiming.Bones = clip.Bones.Count;
        slotTiming.ClipBytes = bytes.Length;

        log.Line($"morphs: {morphTracks.Count} source tracks -> {names.Count} named blendshapes; " +
                 $"{report.UnmappedActive.Count} unmapped active tracks" +
                 (report.ToothStatus != null ? $" | tooth={report.ToothStatus}" : string.Empty));
        if (report.UnmappedActive.Count > 0)
            log.Line("  WARNING unmapped active morphs: " + string.Join(", ", report.UnmappedActive));

        using var sha = SHA256.Create();
        string sourceVmdSha256 = Convert.ToHexString(sha.ComputeHash(File.ReadAllBytes(vmdPath))).ToLowerInvariant();

        return new BakedSlotResult(clip, bytes, report, sourceFps, vmdPath, resolvedMorph, sourceVmdSha256, vmdBytes, slotTiming);
    }

    private static void WriteBakedSlotFiles(string slotDir, BakedSlotResult baked, bool clearStandaloneMorph)
    {
        Directory.CreateDirectory(slotDir);
        string clipPath = Path.Combine(slotDir, "motion.gkclip");
        using (var stream = new FileStream(clipPath, FileMode.Create, FileAccess.Write))
        {
            stream.Write(baked.ClipBytes, 0, baked.ClipBytes.Length);
        }

        using (var sha = SHA256.Create())
        {
            string clipDigest = Convert.ToHexString(sha.ComputeHash(baked.ClipBytes)).ToLowerInvariant();
            MorphTranslator.WriteReport(Path.Combine(slotDir, "motion.morph_report.json"), baked.Report, clipDigest);
        }

        string destSourceVmd = Path.Combine(slotDir, "source.vmd");
        if (!string.Equals(Path.GetFullPath(baked.VmdPath), Path.GetFullPath(destSourceVmd), StringComparison.OrdinalIgnoreCase))
            File.Copy(baked.VmdPath, destSourceVmd, overwrite: true);

        string destMorphVmd = Path.Combine(slotDir, "source_morph.vmd");
        bool distinctMorph = !clearStandaloneMorph &&
                             !string.IsNullOrEmpty(baked.MorphPath) &&
                             !string.Equals(Path.GetFullPath(baked.MorphPath), Path.GetFullPath(baked.VmdPath), StringComparison.OrdinalIgnoreCase);
        if (distinctMorph)
        {
            if (!string.Equals(Path.GetFullPath(baked.MorphPath), Path.GetFullPath(destMorphVmd), StringComparison.OrdinalIgnoreCase))
                File.Copy(baked.MorphPath, destMorphVmd, overwrite: true);
        }
        else if (File.Exists(destMorphVmd))
        {
            File.Delete(destMorphVmd);
        }
    }

    private static bool GeometryBindsVmd(string geometryPath, string vmdPath)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(File.ReadAllBytes(geometryPath));
            if (!doc.RootElement.TryGetProperty("source_vmd_sha256", out JsonElement bound) ||
                bound.ValueKind != JsonValueKind.String)
                return false;
            using var sha = SHA256.Create();
            string digest = Convert.ToHexString(sha.ComputeHash(File.ReadAllBytes(vmdPath))).ToLowerInvariant();
            return string.Equals(bound.GetString(), digest, StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Rate the VMD tick timeline advances at, i.e. the *source clock*.
    /// Mirrors import_motion.detect_fps (adjacent info.json, WAV duration,
    /// "60 fps" stem) and is deliberately independent of request.Fps: these
    /// signals say how many ticks the source carries per second, not how fast
    /// the clip should be stored. A re-baked 60-tick VMD (e.g. Motion-60FPS.vmd,
    /// 9443 ticks against a 157.35 s song) reports 60 here and needs no resample.
    /// </summary>
    internal static float DetectSourceFps(string vmdPath, VmdMotion motion, string? wavPath)
    {
        string infoPath = Path.Combine(Path.GetDirectoryName(vmdPath)!, "info.json");
        if (string.Equals(Path.GetFileName(vmdPath), "source.vmd", StringComparison.OrdinalIgnoreCase) &&
            File.Exists(infoPath))
        {
            try
            {
                using JsonDocument doc = JsonDocument.Parse(File.ReadAllBytes(infoPath));
                JsonElement root = doc.RootElement;
                if (root.TryGetProperty("source_fps", out JsonElement fpsElement) &&
                    fpsElement.ValueKind == JsonValueKind.Number &&
                    root.TryGetProperty("source_vmd_sha256", out JsonElement shaElement) &&
                    shaElement.ValueKind == JsonValueKind.String)
                {
                    float declared = fpsElement.GetSingle();
                    bool known = declared is 25.0f or 30.0f or 60.0f;
                    using var sha = SHA256.Create();
                    string digest = Convert.ToHexString(sha.ComputeHash(File.ReadAllBytes(vmdPath))).ToLowerInvariant();
                    if (known && string.Equals(shaElement.GetString(), digest, StringComparison.OrdinalIgnoreCase))
                        return declared;
                }
            }
            catch
            {
                // Fall through to the remaining heuristics, exactly like the Python tool.
            }
        }

        if (wavPath != null)
        {
            AudioProbeResult audioMeta = AudioProbe.Probe(wavPath);
            if (audioMeta.Valid && audioMeta.DurationSeconds > 0.0)
            {
                double ratio = (BodyEndFrame(motion) + 1) / audioMeta.DurationSeconds;
                if (ratio is >= 54.0 and <= 66.0) return 60.0f;
                if (ratio is >= 24.0 and <= 36.0) return 30.0f;
                if (ratio >= 45.0) return 60.0f;
                return 30.0f;
            }
        }

        if (Regex.IsMatch(Path.GetFileNameWithoutExtension(vmdPath), @"60\s*fps", RegexOptions.IgnoreCase))
            return 60.0f;
        return 30.0f;
    }

    // A few late facial, camera, IK or isolated bone keys must not set the
    // body clock. Use the upper quartile of animated body-track endpoints;
    // sparse files fall back to their latest body key instead of morphology.
    internal static uint BodyEndFrame(VmdMotion motion)
    {
        var tracks = motion.BoneKeyframes
            .Where(pair => !EyeOnlyBones.Contains(pair.Key) && pair.Value.Count > 0)
            .Select(pair => pair.Value)
            .ToArray();
        if (tracks.Length == 0) return 0;
        var animated = tracks.Where(keys => keys.Count > 1 && keys[^1].Frame > keys[0].Frame)
            .Select(keys => keys[^1].Frame).OrderBy(frame => frame).ToArray();
        return animated.Length >= 4 ? animated[(animated.Length - 1) * 3 / 4] : tracks.Max(keys => keys[^1].Frame);
    }

    /// <summary>
    /// Rebuilds the motion with the standalone facial VMD's eye tracks in place,
    /// matching vmd2gkclip.convert's `vmd.bone_keyframes[track] = morph_source[...]`.
    /// </summary>
    private static VmdMotion WithEyeTracks(VmdMotion motion, VmdMotion morphSource)
    {
        bool replaced = false;
        var bones = new Dictionary<string, IReadOnlyList<BoneKeyframe>>(motion.BoneKeyframes.Count);
        foreach (KeyValuePair<string, IReadOnlyList<BoneKeyframe>> track in motion.BoneKeyframes)
            bones[track.Key] = track.Value;
        foreach (string name in EyeProfile.SourceTracks)
        {
            if (!morphSource.BoneKeyframes.TryGetValue(name, out IReadOnlyList<BoneKeyframe>? keys)) continue;
            bones[name] = keys;
            replaced = true;
        }
        if (!replaced) return motion;

        return new VmdMotion
        {
            Version = motion.Version,
            ModelName = motion.ModelName,
            MaxFrame = motion.MaxFrame,
            BoneKeyframes = bones,
            MorphKeyframes = motion.MorphKeyframes,
            CameraKeyframes = motion.CameraKeyframes,
            PropertyKeyframes = motion.PropertyKeyframes
        };
    }
}
