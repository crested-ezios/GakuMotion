namespace GakuMotion.VmdImporter.Native;

using System.Diagnostics;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using GakuMotion.VmdImporter.Native.Camera;
using GakuMotion.VmdImporter.Native.Facial;
using GakuMotion.VmdImporter.Native.Timebase;
using GakuMotion.VmdImporter.Output;
using GakuMotion.VmdImporter.Rig;
using GakuMotion.VmdImporter.Vmd;

internal sealed class ImportOutcome
{
    public bool Ok { get; init; }
    public string Directory { get; init; } = string.Empty;
    public string Error { get; init; } = string.Empty;
    public string ErrorCode { get; init; } = string.Empty;
    public ImportTiming Timing { get; init; } = new();
}

/// <summary>
/// Product wiring around the frozen Candidate B evaluator. Nothing here changes
/// evaluation numerics: it parses, evaluates, retargets/encodes, then commits a
/// track directory that matches the Python importer's on-disk contract —
/// including facial channels, eye channels, camera.gkcamera and info.json.
/// </summary>
internal static class ImportPipeline
{
    /// <summary>Frozen Candidate B production tree (recorded in every timing line).</summary>
    public const string CandidateBTreeSha256 = "76f3f8d1f669dbe15de8a14cc545a516188558ba6868b7f9d91d65af8d8bcf1d";

    /// <summary>Camera root registration outcome recorded in info.json.</summary>
    private sealed record CameraRegistrationMetadata(
        string Result, string? Mode, string Reason, double? ResidualMeters, string Detail);

    private sealed class ImportStepException(string errorCode, string message) : Exception(message)
    {
        public string ErrorCode { get; } = errorCode;
    }

    private sealed record SlotSpec(
        int Slot,
        int SourceSlot,
        string MotionVmd,
        string MorphVmd,
        bool DisableMorph,
        bool ClearMorph,
        float SourceFps,
        bool RebakeOutput,
        bool PreserveExisting,
        float X,
        float Y,
        float Z,
        float YawDegrees);

    private sealed record BakedSlotResult(
        GkClipData Clip,
        byte[] ClipBytes,
        MorphReport Report,
        float SourceFps,
        string VmdPath,
        string MorphPath,
        string SourceVmdSha256,
        long VmdBytes,
        ImportTiming Timing);

    private sealed record SlotMetadataEntry(
        int Slot,
        float X,
        float Y,
        float Z,
        float YawDegrees,
        bool HasMotion,
        bool HasFacial,
        string SourceVmd,
        string SourceMorphVmd,
        string SourceVmdSha256,
        float SourceFps,
        int Frames,
        int Bones,
        double DurationSeconds,
        float OutputFps);

    private static readonly string[] PerSlotFiles =
    {
        "motion.gkclip",
        "motion.gk_world_dump",
        "motion.gkmotion",
        "motion.morph_report.json",
        "source.vmd",
        "source_morph.vmd",
        "bone_overrides.json",
        "root_height_calibration.json",
        "aux_replay.json",
        TargetGeometryName
    };

    private static readonly string[] MotionClipNames =
    {
        "motion.gkclip",
        "motion.gk_world_dump",
        "motion.gkmotion"
    };

    private const string TargetGeometryName = "target_leg_geometry.json";

    private static readonly string[] EyeOnlyBones = { "両目", "左目", "右目" };
    private const double DefaultCameraFps = 60.0;
    private const double DefaultCameraScale = 0.083;

    private static string SlotDirectory(string trackRoot, int slot1Based) =>
        slot1Based <= 1 ? trackRoot : Path.Combine(trackRoot, "positions", $"slot_{slot1Based}");

    private static bool HasMotionInDir(string dir) =>
        Directory.Exists(dir) && MotionClipNames.Any(name => File.Exists(Path.Combine(dir, name)));

    private static string? FindMotionFileInDir(string dir)
    {
        if (!Directory.Exists(dir)) return null;
        foreach (string name in MotionClipNames)
        {
            string candidate = Path.Combine(dir, name);
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }

    private static float CleanZero(float v) => v == 0.0f ? 0.0f : v;

    private static void ValidateSourceFpsValue(float fps)
    {
        if (fps != 0 && fps is not (25.0f or 30.0f or 60.0f))
            throw new ImportStepException("invalid_source_fps", "源动作帧率只支持自动、25、30 或 60 fps");
    }

    public static ImportOutcome Run(ImportRequest request, ImportLog log)
    {
        if (request.SourceFps != 0 && request.SourceFps is not (25.0f or 30.0f or 60.0f))
            return new ImportOutcome { Ok = false, Error = "源动作帧率只支持自动、25、30 或 60 fps", ErrorCode = "invalid_source_fps" };
        if (!string.IsNullOrEmpty(request.EditDir)) return RunEdit(request, log);
        var timing = new ImportTiming();
        var total = Stopwatch.StartNew();

        void Progress(int percent, string stage, string message) => log.Progress(percent, stage, message);

        try
        {
            log.Line($"GakuMotion C# VMD importer (Candidate B tree {CandidateBTreeSha256})");
            Progress(3, "INIT", "正在校验输入文件...");

            List<SlotSpec> slots = NormalizeNewImportSlots(request);
            foreach (SlotSpec s in slots)
                ValidateSourceFpsValue(s.SourceFps);

            string slot1VmdPath = Path.GetFullPath(slots[0].MotionVmd);
            if (!File.Exists(slot1VmdPath)) return Fail(log, timing, total, "找不到 VMD 文件: " + slot1VmdPath);
            if (!string.Equals(Path.GetExtension(slot1VmdPath), ".vmd", StringComparison.OrdinalIgnoreCase))
                return Fail(log, timing, total, "请选择 .vmd 动作文件");

            // Adding is never an implicit edit, including names that collide
            // only after Windows-safe folder-name normalization.
            EnsureNewTrackAvailable(NewTrackDestination(request, slot1VmdPath, out _));

            // Pre-validate all slot inputs and shared camera/audio before baking.
            var resolvedVmds = new string[slots.Count];
            var resolvedMorphs = new string[slots.Count];
            for (int i = 0; i < slots.Count; i++)
            {
                SlotSpec slot = slots[i];
                string vmdPath = Path.GetFullPath(slot.MotionVmd);
                if (!File.Exists(vmdPath)) return Fail(log, timing, total, "找不到 VMD 文件: " + vmdPath);
                if (!string.Equals(Path.GetExtension(vmdPath), ".vmd", StringComparison.OrdinalIgnoreCase))
                    return Fail(log, timing, total, "请选择 .vmd 动作文件");

                string geometryPath = Path.Combine(Path.GetDirectoryName(vmdPath)!, TargetGeometryName);
                if (File.Exists(geometryPath) && GeometryBindsVmd(geometryPath, vmdPath))
                {
                    log.Line("该 VMD 旁存在已绑定的 target_leg_geometry.json：C# 后端未实现目标几何烘焙。");
                    return new ImportOutcome
                    {
                        Ok = false,
                        ErrorCode = "unsupported_target_geometry",
                        Error = "该 VMD 旁存在已绑定的 target_leg_geometry.json；原生目标几何腿部烘焙尚未实现；未修改曲目。",
                        Timing = timing
                    };
                }

                string morphPath = string.Empty;
                if (!slot.DisableMorph && !string.IsNullOrEmpty(slot.MorphVmd))
                {
                    morphPath = Path.GetFullPath(slot.MorphVmd);
                    if (!File.Exists(morphPath)) return Fail(log, timing, total, "找不到表情 VMD 文件: " + morphPath);
                    if (!string.Equals(Path.GetExtension(morphPath), ".vmd", StringComparison.OrdinalIgnoreCase))
                        return Fail(log, timing, total, "表情文件必须是 .vmd");
                    VmdMotion morphSource = VmdReader.Parse(morphPath);
                    if (morphSource.MorphKeyframes.Count == 0 && !EyeProfile.HasEyeTracks(morphSource))
                        return Fail(log, timing, total, "所选表情文件不包含表情或眼神关键帧");
                }
                else if (slot.DisableMorph)
                {
                    log.Line($"槽位 {slot.Slot} 表情轨道：用户显式关闭（不生成 blendshape 通道）。");
                }

                resolvedVmds[i] = vmdPath;
                resolvedMorphs[i] = morphPath;
            }

            // --- Camera source (validated here, baked during staging) ----------
            string cameraPath = string.Empty;
            bool cameraIsBaked = false;
            if (!string.IsNullOrEmpty(request.CameraVmd))
            {
                cameraPath = Path.GetFullPath(request.CameraVmd);
                if (!File.Exists(cameraPath)) return Fail(log, timing, total, "找不到镜头文件: " + cameraPath);
                string extension = Path.GetExtension(cameraPath);
                cameraIsBaked = string.Equals(extension, ".gkcamera", StringComparison.OrdinalIgnoreCase);
                if (!cameraIsBaked && !string.Equals(extension, ".vmd", StringComparison.OrdinalIgnoreCase))
                    return Fail(log, timing, total, "镜头文件必须是 .vmd 或 .gkcamera");
            }

            // --- Audio (.wav / .mp3 / .flac / .ogg / .m4a / .aac / .opus) -----
            string? audioPath = AudioProbe.ResolveInputAudio(slot1VmdPath, request.Audio, request.ClearAudio);
            if (audioPath != null)
            {
                AudioProbeResult audioProbe = AudioProbe.Probe(audioPath, extractCover: true);
                if (!audioProbe.Valid)
                    return Fail(log, timing, total, audioProbe.Error);
            }

            var bakedSlots = new BakedSlotResult[slots.Count];
            for (int i = 0; i < slots.Count; i++)
            {
                SlotSpec slot = slots[i];
                int progressBase = 10 + (75 * i) / slots.Count;
                int progressSpan = Math.Max(5, 75 / slots.Count);
                BakedSlotResult baked = BakeSingleSlot(
                    resolvedVmds[i],
                    resolvedMorphs[i],
                    slot.DisableMorph,
                    slot.SourceFps,
                    request.Fps,
                    request.ToothChannel,
                    audioPath,
                    log,
                    Progress,
                    progressBase,
                    progressSpan);
                bakedSlots[i] = baked;

                timing.ParseMs += baked.Timing.ParseMs;
                timing.EvaluateMs += baked.Timing.EvaluateMs;
                timing.RetargetEncodeMs += baked.Timing.RetargetEncodeMs;
                timing.FacialMs += baked.Timing.FacialMs;
                timing.VmdBytes += baked.VmdBytes;
                timing.ClipBytes += baked.ClipBytes.Length;
                if (baked.Clip.TotalFrames > timing.Frames) timing.Frames = baked.Clip.TotalFrames;
                if (baked.Clip.Bones.Count > timing.Bones) timing.Bones = baked.Clip.Bones.Count;
            }
            timing.Fps = bakedSlots[0].Clip.Fps;

            Progress(90, "PACK", "正在写入 .gkclip 动作包...");

            var packSw = Stopwatch.StartNew();
            string committed = CommitTrack(request, slots, bakedSlots, audioPath,
                cameraPath, cameraIsBaked, timing, log, Progress);
            timing.PackMs = packSw.Elapsed.TotalMilliseconds;

            total.Stop();
            timing.TotalMs = total.Elapsed.TotalMilliseconds;
            log.Line(timing.ToUserLine());
            log.Line(timing.ToLogLine(CandidateBTreeSha256));
            AppendTimingRecord(request, timing, slot1VmdPath);
            Progress(100, "DONE", "曲目导入完成");

            return new ImportOutcome { Ok = true, Directory = committed, Timing = timing };
        }
        catch (ImportStepException ex)
        {
            return new ImportOutcome { Ok = false, ErrorCode = ex.ErrorCode, Error = ex.Message, Timing = timing };
        }
        catch (Exception ex)
        {
            return Fail(log, timing, total, ex is InvalidDataException ? ex.Message : ex.GetType().Name + ": " + ex.Message);
        }
    }

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

    /// <summary>
    /// Edits a private copy of the complete existing track. Only an explicitly
    /// requested body/facial change invokes the evaluator; metadata/audio/camera
    /// edits retain the original body bytes. Publish uses same-volume directory
    /// moves, with the original retained until the new directory is in place.
    /// </summary>
    private static ImportOutcome RunEdit(ImportRequest request, ImportLog log)
    {
        var timing = new ImportTiming();
        var total = Stopwatch.StartNew();
        string? work = null;
        string? backup = null;
        try
        {
            string target = Path.GetFullPath(request.EditDir).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            if (!Directory.Exists(target) || !HasMotionInDir(target))
                throw new InvalidDataException("目标必须是包含动作文件的已有曲目目录");
            string library = Path.GetDirectoryName(target) ?? throw new InvalidDataException("不能编辑文件系统根目录");
            string stagingParent = TransactionParent(request.TransactionRoot, library);
            work = Path.Combine(stagingParent, "gkmotion-cs-edit-" + Guid.NewGuid().ToString("N"));
            string stage = Path.Combine(work, "track");
            backup = Path.Combine(work, "original");
            WriteTransaction(work, "edit", target, "staging");
            log.Progress(5, "INIT", "正在准备曲目修改...");
            IReadOnlyList<EditTreeEntry> original = CaptureEditSignature(target);
            CopyEditTree(target, stage);
            EnsureEditUnchanged(target, original);
            EnsureEditUnchanged(stage, original);
            JsonObject info = ReadEditInfo(Path.Combine(stage, "info.json"));

            if (request.Positions is { Count: > 0 })
            {
                EditGroupSlotsInStage(request, stage, work, info, ref timing, log);
            }
            else
            {
                bool replaceBody = !string.IsNullOrEmpty(request.MotionVmd);
                bool replaceMorph = !string.IsNullOrEmpty(request.MorphVmd);
                bool rebake = replaceBody || replaceMorph || request.DisableMorph || request.SourceFps > 0 || request.RebakeOutput;
                if (rebake)
                {
                    // Existing target geometry is part of the authored track's
                    // body contract. Never discard its solve during a facial edit.
                    if (File.Exists(Path.Combine(stage, TargetGeometryName)))
                        throw new InvalidDataException("该曲目含目标腿部几何；原生后端尚不支持保留该几何的重新烘焙");
                    string body = replaceBody ? Path.GetFullPath(request.MotionVmd) : Path.Combine(stage, "source.vmd");
                    if (!File.Exists(body))
                        throw new InvalidDataException("该曲目缺少原始动作备份 source.vmd，请在动作栏重新指定动作 .vmd 文件");
                    string morph = request.DisableMorph ? string.Empty
                        : replaceMorph ? Path.GetFullPath(request.MorphVmd)
                        : File.Exists(Path.Combine(stage, "source_morph.vmd")) ? Path.Combine(stage, "source_morph.vmd") : string.Empty;
                    float fps = request.Fps > 0 ? request.Fps : (info["fps"]?.GetValue<float>() ?? 0);
                    string? existingAudio = AudioProbe.FindExistingAudioFile(stage, info);
                    // Audio changes are applied after conversion, matching edit mode:
                    // no discovery or replacement of the track's audio is implied.
                    var bakeRequest = new ImportRequest
                    {
                        MotionVmd = body,
                        MorphVmd = morph,
                        DisableMorph = request.DisableMorph,
                        MotionsRoot = Path.Combine(work, "converted"),
                        Title = "edit-body",
                        Fps = fps,
                        SourceFps = request.SourceFps > 0 ? request.SourceFps : (!replaceBody ? EditFps(info) : 0),
                        Audio = existingAudio ?? string.Empty,
                        ClearAudio = existingAudio is null,
                        ToothChannel = request.ToothChannel
                    };
                    ImportOutcome baked = Run(bakeRequest, log);
                    if (!baked.Ok)
                        return new ImportOutcome { Ok = false, Error = baked.Error, ErrorCode = baked.ErrorCode, Timing = baked.Timing };
                    timing = baked.Timing;
                    foreach (string name in new[] { "motion.gkclip", "motion.morph_report.json", "source.vmd" })
                        File.Copy(Path.Combine(baked.Directory, name), Path.Combine(stage, name), overwrite: true);
                    string bakedMorph = Path.Combine(baked.Directory, "source_morph.vmd");
                    if (!request.DisableMorph && File.Exists(bakedMorph))
                        File.Copy(bakedMorph, Path.Combine(stage, "source_morph.vmd"), overwrite: true);
                    else File.Delete(Path.Combine(stage, "source_morph.vmd"));

                    JsonObject bodyInfo = ReadEditInfo(Path.Combine(baked.Directory, "info.json"));
                    foreach (string key in new[] { "duration_seconds", "total_frames", "bone_count", "fps", "source_fps", "has_facial",
                                 "import_backend", "import_candidate_b_tree" })
                        info[key] = bodyInfo[key]?.DeepClone();
                    if (replaceBody)
                    {
                        info["source_vmd"] = body;
                        info["source_vmd_sha256"] = bodyInfo["source_vmd_sha256"]?.DeepClone();
                    }
                    bool hasBakedFacial = bodyInfo["has_facial"]?.GetValue<bool>() ?? false;
                    if (request.DisableMorph) info["source_morph_vmd"] = string.Empty;
                    else if (replaceMorph) info["source_morph_vmd"] = Path.GetFullPath(request.MorphVmd);
                    else if (!File.Exists(Path.Combine(stage, "source_morph.vmd")))
                    {
                        string resolvedSourceVmd = replaceBody ? body : ReadJsonString(info, "source_vmd");
                        info["source_morph_vmd"] = hasBakedFacial ? resolvedSourceVmd : string.Empty;
                    }

                    if (info["positions"] is JsonArray existingPositions && existingPositions.Count > 0 &&
                        existingPositions[0] is JsonObject slot1Obj)
                    {
                        slot1Obj["frames"] = bodyInfo["total_frames"]?.DeepClone();
                        slot1Obj["bones"] = bodyInfo["bone_count"]?.DeepClone();
                        slot1Obj["has_facial"] = bodyInfo["has_facial"]?.DeepClone();
                        slot1Obj["source_fps"] = bodyInfo["source_fps"]?.DeepClone();
                        slot1Obj["source_vmd"] = info["source_vmd"]?.DeepClone();
                        slot1Obj["source_vmd_sha256"] = info["source_vmd_sha256"]?.DeepClone();
                        slot1Obj["source_morph_vmd"] = info["source_morph_vmd"]?.DeepClone();
                        RecomputeTopLevelSummaryFromPositions(stage, info, existingPositions);
                    }
                }
            }

            // Camera registration reads this body/source metadata before its bake.
            WriteEditInfo(stage, info);
            if (request.ClearCamera)
            {
                File.Delete(Path.Combine(stage, "camera.gkcamera"));
                File.Delete(Path.Combine(stage, "source_camera.vmd"));
                info["source_camera_vmd"] = string.Empty;
                info.Remove("camera_root_registration");
            }
            else if (!string.IsNullOrEmpty(request.CameraVmd) ||
                     (request.SourceFps > 0 && File.Exists(Path.Combine(stage, "source_camera.vmd"))))
            {
                string camera = !string.IsNullOrEmpty(request.CameraVmd) ? Path.GetFullPath(request.CameraVmd)
                    : Path.Combine(stage, "source_camera.vmd");
                ImportOutcome imported = CameraImportPipeline.Run(new CameraImportRequest
                {
                    Source = camera, MotionDirectory = stage,
                    Fps = request.CameraFps > 0 ? request.CameraFps : DefaultCameraFps,
                    Scale = request.CameraScale > 0 ? request.CameraScale : DefaultCameraScale,
                    RootRegistration = string.IsNullOrEmpty(request.CameraRootRegistration)
                        ? CameraRegistration.ModeAuto : request.CameraRootRegistration
                }, log);
                if (!imported.Ok)
                    return new ImportOutcome { Ok = false, Error = imported.Error, ErrorCode = imported.ErrorCode, Timing = timing };
                timing.CameraMs = imported.Timing.CameraMs;
                if (Path.GetExtension(camera).Equals(".vmd", StringComparison.OrdinalIgnoreCase))
                {
                    string cameraBackup = Path.Combine(stage, "source_camera.vmd");
                    if (!string.Equals(Path.GetFullPath(camera), Path.GetFullPath(cameraBackup), StringComparison.OrdinalIgnoreCase))
                        File.Copy(camera, cameraBackup, overwrite: true);
                }
                else File.Delete(Path.Combine(stage, "source_camera.vmd"));
                if (!string.IsNullOrEmpty(request.CameraVmd)) info["source_camera_vmd"] = camera;
                // A previous camera's registration receipt cannot describe this one.
                info.Remove("camera_root_registration");
            }
            info["has_camera"] = File.Exists(Path.Combine(stage, "camera.gkcamera"));

            if (request.ClearAudio)
            {
                AudioProbe.DeleteExistingAudioFiles(stage);
                AudioProbe.DeleteExistingCoverFiles(stage);
                info.Remove("audio_file");
                info.Remove("audio_duration");
                info.Remove("audio_artist");
                info.Remove("audio_title");
                info["has_cover"] = false;
                info.Remove("cover_file");
            }
            else if (!string.IsNullOrEmpty(request.Audio))
            {
                if (AudioProbe.IsNetworkPath(request.Audio))
                    throw new InvalidDataException("不支持网络或流媒体路径，请选择本地音频文件。");
                string audio = Path.GetFullPath(request.Audio);
                if (!File.Exists(audio)) throw new InvalidDataException("找不到音频文件: " + audio);
                AudioProbeResult probed = AudioProbe.Probe(audio, extractCover: true);
                if (!probed.Valid) throw new InvalidDataException(probed.Error);
                AudioProbe.DeleteExistingAudioFiles(stage);
                string ext = Path.GetExtension(audio).ToLowerInvariant();
                string audioFile = "song" + ext;
                File.Copy(audio, Path.Combine(stage, audioFile), overwrite: true);
                info["audio_file"] = audioFile;
                double dur = probed.DurationSeconds > 0 ? probed.DurationSeconds : request.AudioDurationHint;
                if (dur > 0) info["audio_duration"] = dur;
                else info.Remove("audio_duration");
                if (!string.IsNullOrWhiteSpace(probed.Artist)) info["audio_artist"] = probed.Artist;
                else info.Remove("audio_artist");
                if (!string.IsNullOrWhiteSpace(probed.Title)) info["audio_title"] = probed.Title;
                else info.Remove("audio_title");
                string? coverFile = AudioProbe.SaveCoverToDirectory(probed.CoverBytes, probed.CoverMime, stage);
                if (coverFile != null)
                {
                    info["has_cover"] = true;
                    info["cover_file"] = coverFile;
                }
                else
                {
                    AudioProbe.DeleteExistingCoverFiles(stage);
                    info["has_cover"] = false;
                    info.Remove("cover_file");
                }
            }
            if (!string.IsNullOrWhiteSpace(request.Title)) info["title"] = request.Title.Trim();
            else if (info["title"] is null || info["title"]!.ToString().Length == 0) info["title"] = Path.GetFileName(target);
            if (request.HasSubtitle) info["subtitle"] = request.Subtitle.Trim();
            if (request.TrackTiming != null) info["track_timing"] = request.TrackTiming.ToJson();
            WriteEditInfo(stage, info);

            log.Progress(96, "PACK", "正在保存曲目修改...");
            WriteTransaction(work, "edit", target, "publish_pending");
            PublishEditedTrack(stage, target, backup, original);
            try { WriteTransaction(work, "edit", target, "committed"); }
            catch (Exception ex) { log.Line("曲目已保存；事务状态记录失败: " + ex.Message); }
            TryDelete(backup);
            total.Stop();
            timing.TotalMs = total.Elapsed.TotalMilliseconds;
            log.Line($"曲目修改已保存: {target}");
            log.Progress(100, "DONE", "曲目修改已保存");
            return new ImportOutcome { Ok = true, Directory = target, Timing = timing };
        }
        catch (ImportStepException ex)
        {
            return new ImportOutcome { Ok = false, ErrorCode = ex.ErrorCode, Error = ex.Message, Timing = timing };
        }
        catch (Exception ex)
        {
            return Fail(log, timing, total, ex is InvalidDataException ? ex.Message : ex.GetType().Name + ": " + ex.Message);
        }
        finally
        {
            // If restoring a failed commit also failed, preserve the original
            // directory and print its exact recovery location instead of deleting it.
            if (backup != null && Directory.Exists(backup)) log.Line("原曲目恢复备份保留于: " + backup);
            else if (work != null && Directory.Exists(work)) TryDelete(work);
        }
    }

    private static string ReadJsonString(JsonObject? obj, string key, string fallback = "") =>
        obj?[key] is JsonValue v && v.TryGetValue<string>(out string? s) ? (s ?? fallback) : fallback;

    private static bool ReadJsonBool(JsonObject? obj, string key, bool fallback = false) =>
        obj?[key] is JsonValue v && v.TryGetValue<bool>(out bool b) ? b : fallback;

    private static int ReadJsonInt(JsonObject? obj, string key, int fallback = 0)
    {
        if (obj?[key] is not JsonValue v) return fallback;
        if (v.TryGetValue<int>(out int i)) return i;
        if (v.TryGetValue<long>(out long l) && l is >= int.MinValue and <= int.MaxValue) return (int)l;
        if (v.TryGetValue<double>(out double d) && double.IsFinite(d)) return (int)Math.Round(d);
        return fallback;
    }

    private static float ReadJsonFloat(JsonObject? obj, string key, float fallback = 0.0f)
    {
        if (obj?[key] is not JsonValue v) return fallback;
        if (v.TryGetValue<float>(out float f) && float.IsFinite(f)) return f;
        if (v.TryGetValue<double>(out double d) && double.IsFinite(d)) return (float)d;
        return fallback;
    }

    private static double ReadJsonDouble(JsonObject? obj, string key, double fallback = 0.0)
    {
        if (obj?[key] is not JsonValue v) return fallback;
        if (v.TryGetValue<double>(out double d) && double.IsFinite(d)) return d;
        return fallback;
    }

    private static JsonObject BuildPositionJsonObject(SlotMetadataEntry entry) => new()
    {
        ["slot"] = entry.Slot,
        ["x"] = CleanZero(entry.X),
        ["y"] = CleanZero(entry.Y),
        ["z"] = CleanZero(entry.Z),
        ["yaw_degrees"] = CleanZero(entry.YawDegrees),
        ["has_motion"] = entry.HasMotion,
        ["has_facial"] = entry.HasFacial,
        ["source_vmd"] = entry.SourceVmd,
        ["source_morph_vmd"] = entry.SourceMorphVmd,
        ["source_vmd_sha256"] = entry.SourceVmdSha256,
        ["source_fps"] = entry.SourceFps,
        ["frames"] = entry.Frames,
        ["bones"] = entry.Bones
    };

    private static SlotMetadataEntry ReadSlotMetadataFromClip(string slotDir, int slot, JsonObject? metaObj,
        float x, float y, float z, float yaw, float defaultFps)
    {
        int frames = ReadJsonInt(metaObj, "frames", 0);
        int bones = ReadJsonInt(metaObj, "bones", 0);
        bool hasFacial = ReadJsonBool(metaObj, "has_facial", false);
        float srcFps = ReadJsonFloat(metaObj, "source_fps", 0.0f);
        if (srcFps <= 0.0f) srcFps = defaultFps > 0.0f ? defaultFps : 30.0f;
        double duration = 0.0;
        float outFps = defaultFps > 0.0f ? defaultFps : srcFps;

        string? clipFile = FindMotionFileInDir(slotDir);
        bool hasMotion = clipFile != null;
        if (clipFile != null && Path.GetExtension(clipFile).Equals(".gkclip", StringComparison.OrdinalIgnoreCase))
        {
            try
            {
                GkClipData clip = GkClipDecoder.Decode(File.ReadAllBytes(clipFile));
                if (frames <= 0) frames = clip.TotalFrames;
                if (bones <= 0) bones = clip.Bones.Count;
                if (metaObj == null || metaObj["has_facial"] is null)
                    hasFacial = clip.BlendshapeNames.Count > 0;
                duration = clip.DurationSeconds;
                if (clip.Fps > 0.0f) outFps = clip.Fps;
            }
            catch
            {
                if (frames > 0 && outFps > 0.0f) duration = frames / (double)outFps;
            }
        }
        else if (frames > 0 && outFps > 0.0f)
        {
            duration = frames / (double)outFps;
        }

        string sourceVmd = ReadJsonString(metaObj, "source_vmd");
        string sourceMorphVmd = ReadJsonString(metaObj, "source_morph_vmd");
        string sha = ReadJsonString(metaObj, "source_vmd_sha256");
        if (string.IsNullOrEmpty(sha))
        {
            string sourceVmdFile = Path.Combine(slotDir, "source.vmd");
            if (File.Exists(sourceVmdFile))
            {
                using var hash = SHA256.Create();
                sha = Convert.ToHexString(hash.ComputeHash(File.ReadAllBytes(sourceVmdFile))).ToLowerInvariant();
            }
        }

        return new SlotMetadataEntry(
            Slot: slot,
            X: x,
            Y: y,
            Z: z,
            YawDegrees: yaw,
            HasMotion: hasMotion,
            HasFacial: hasFacial,
            SourceVmd: sourceVmd,
            SourceMorphVmd: sourceMorphVmd,
            SourceVmdSha256: sha,
            SourceFps: srcFps,
            Frames: frames,
            Bones: bones,
            DurationSeconds: duration,
            OutputFps: outFps);
    }

    private static void RecomputeTopLevelSummaryFromPositions(string stage, JsonObject info, JsonArray positions)
    {
        int maxFrames = 0;
        int maxBones = 0;
        double maxDuration = 0.0;
        bool anyFacial = false;
        float fallbackFps = ReadJsonFloat(info, "fps", 30.0f);

        for (int i = 0; i < positions.Count; i++)
        {
            if (positions[i] is not JsonObject posObj) continue;
            int slot = ReadJsonInt(posObj, "slot", i + 1);
            int frames = ReadJsonInt(posObj, "frames", 0);
            int bones = ReadJsonInt(posObj, "bones", 0);
            bool hasFacial = ReadJsonBool(posObj, "has_facial", false);
            if (frames > maxFrames) maxFrames = frames;
            if (bones > maxBones) maxBones = bones;
            if (hasFacial) anyFacial = true;

            string slotDir = SlotDirectory(stage, slot);
            string? clipFile = FindMotionFileInDir(slotDir);
            double dur = 0.0;
            if (clipFile != null && Path.GetExtension(clipFile).Equals(".gkclip", StringComparison.OrdinalIgnoreCase))
            {
                try
                {
                    GkClipData clip = GkClipDecoder.Decode(File.ReadAllBytes(clipFile));
                    dur = clip.DurationSeconds;
                }
                catch { }
            }
            if (dur <= 0.0 && frames > 0 && fallbackFps > 0.0f)
                dur = frames / (double)fallbackFps;
            if (dur > maxDuration) maxDuration = dur;
        }

        if (maxDuration <= 0.0)
            maxDuration = ReadJsonDouble(info, "duration_seconds", 0.0);

        info["is_group"] = positions.Count >= 2;
        info["position_count"] = positions.Count;
        info["total_frames"] = maxFrames;
        info["bone_count"] = maxBones;
        info["duration_seconds"] = maxDuration;
        info["has_facial"] = anyFacial;
    }

    private static void CopySlotDirectoryContents(string sourceDir, string destDir)
    {
        if (!Directory.Exists(sourceDir)) return;
        Directory.CreateDirectory(destDir);
        foreach (string entry in Directory.EnumerateFileSystemEntries(sourceDir))
        {
            string name = Path.GetFileName(entry);
            string dst = Path.Combine(destDir, name);
            if ((File.GetAttributes(entry) & FileAttributes.Directory) != 0)
                CopyEditTree(entry, dst);
            else
                File.Copy(entry, dst, overwrite: true);
        }
    }

    private static void EditGroupSlotsInStage(ImportRequest request, string stage, string work, JsonObject info,
        ref ImportTiming timing, ImportLog log)
    {
        IReadOnlyList<PositionRequest> requested = request.Positions!;
        if (requested.Count == 0)
            throw new InvalidDataException("positions 不能为空");

        ValidateSourceFpsValue(request.SourceFps);
        var seenRequestedSlots = new HashSet<int>();
        var ordered = new List<PositionRequest>(requested.Count);
        foreach (PositionRequest p in requested)
        {
            ValidateSourceFpsValue(p.SourceFps);
            if (!seenRequestedSlots.Add(p.Slot))
                throw new InvalidDataException($"positions 中存在重复槽位: {p.Slot}");
            ordered.Add(p);
        }
        ordered.Sort((a, b) => a.Slot.CompareTo(b.Slot));

        // Build existing per-slot metadata map from info.json (fallback to top-level for slot 1).
        var existingMetaBySlot = new Dictionary<int, JsonObject>();
        if (info["positions"] is JsonArray existingArr)
        {
            for (int i = 0; i < existingArr.Count; i++)
            {
                if (existingArr[i] is JsonObject posObj)
                {
                    int slotNum = ReadJsonInt(posObj, "slot", i + 1);
                    if (slotNum >= 1 && !existingMetaBySlot.ContainsKey(slotNum))
                        existingMetaBySlot[slotNum] = posObj;
                }
            }
        }
        if (!existingMetaBySlot.ContainsKey(1))
        {
            existingMetaBySlot[1] = new JsonObject
            {
                ["slot"] = 1,
                ["x"] = 0.0f,
                ["y"] = 0.0f,
                ["z"] = 0.0f,
                ["yaw_degrees"] = 0.0f,
                ["has_motion"] = HasMotionInDir(stage),
                ["has_facial"] = ReadJsonBool(info, "has_facial", false),
                ["source_vmd"] = ReadJsonString(info, "source_vmd"),
                ["source_morph_vmd"] = ReadJsonString(info, "source_morph_vmd"),
                ["source_vmd_sha256"] = ReadJsonString(info, "source_vmd_sha256"),
                ["source_fps"] = ReadJsonFloat(info, "source_fps", ReadJsonFloat(info, "fps", 30.0f)),
                ["frames"] = ReadJsonInt(info, "total_frames", 0),
                ["bones"] = ReadJsonInt(info, "bone_count", 0)
            };
        }

        float topOutputFps = ReadJsonFloat(info, "fps", 0.0f);
        float topSourceFps = ReadJsonFloat(info, "source_fps", topOutputFps > 0.0f ? topOutputFps : 30.0f);

        // Plan mapping: targetSlot (1..M) <- sourceSlot (0 for newly appended, >=1 for existing)
        var plan = new List<(int TargetSlot, int SourceSlot, PositionRequest Pos, JsonObject? ExistingMeta, bool NeedsRebake, float ExistingSourceFps, float ExistingOutputFps)>(ordered.Count);
        var explicitSourceSlots = new HashSet<int>();
        foreach (PositionRequest p in ordered)
        {
            if (p.SourceSlot > 0)
                explicitSourceSlots.Add(p.SourceSlot);
        }
        var claimedSources = new HashSet<int>();

        for (int i = 0; i < ordered.Count; i++)
        {
            int targetSlot = i + 1;
            PositionRequest pos = ordered[i];
            int sourceSlot;
            if (pos.SourceSlot > 0)
            {
                string srcDir = SlotDirectory(stage, pos.SourceSlot);
                if (!HasMotionInDir(srcDir))
                    throw new InvalidDataException($"找不到源槽位 {pos.SourceSlot} 的动作文件");
                sourceSlot = pos.SourceSlot;
            }
            else if (pos.PreserveExisting || string.IsNullOrEmpty(pos.MotionVmd))
            {
                string srcDir = SlotDirectory(stage, pos.Slot);
                if (!HasMotionInDir(srcDir))
                    throw new InvalidDataException($"找不到槽位 {pos.Slot} 的已有动作文件");
                sourceSlot = pos.Slot;
            }
            else
            {
                string srcDir = SlotDirectory(stage, pos.Slot);
                sourceSlot = (!explicitSourceSlots.Contains(pos.Slot) && HasMotionInDir(srcDir)) ? pos.Slot : 0;
            }

            if (sourceSlot > 0 && !claimedSources.Add(sourceSlot))
                throw new InvalidDataException($"多个目标槽位重复引用了同一源槽位 {sourceSlot}");

            if (sourceSlot == 0 && string.IsNullOrEmpty(pos.MotionVmd))
                throw new InvalidDataException($"新增 {targetSlot} 号位缺少动作 VMD 文件");

            existingMetaBySlot.TryGetValue(sourceSlot, out JsonObject? existingMeta);
            float existingSourceFps = sourceSlot > 0
                ? ReadJsonFloat(existingMeta, "source_fps", sourceSlot == 1 ? topSourceFps : 30.0f)
                : 0.0f;
            if (existingSourceFps <= 0.0f) existingSourceFps = 30.0f;

            float existingOutputFps = topOutputFps > 0.0f ? topOutputFps : existingSourceFps;
            if (sourceSlot > 0)
            {
                string? clipFile = FindMotionFileInDir(SlotDirectory(stage, sourceSlot));
                if (clipFile != null && Path.GetExtension(clipFile).Equals(".gkclip", StringComparison.OrdinalIgnoreCase))
                {
                    try
                    {
                        float clipFps = GkClipDecoder.Decode(File.ReadAllBytes(clipFile)).Fps;
                        if (clipFps > 0.0f) existingOutputFps = clipFps;
                    }
                    catch { }
                }
            }

            bool needsRebake =
                sourceSlot == 0 ||
                pos.RebakeOutput ||
                pos.DisableMorph ||
                pos.ClearMorph ||
                !string.IsNullOrEmpty(pos.MorphVmd) ||
                (!pos.PreserveExisting && !string.IsNullOrEmpty(pos.MotionVmd)) ||
                (pos.SourceFps > 0.0f && (!pos.PreserveExisting || Math.Abs(pos.SourceFps - existingSourceFps) > 1e-4f));

            plan.Add((targetSlot, sourceSlot, pos, existingMeta, needsRebake, existingSourceFps, existingOutputFps));
        }

        // Snapshot existing slot directories outside stage to prevent collisions on renumber/reorder.
        string oldPositionsDir = Path.Combine(work, "old_positions");
        string stagePositionsDir = Path.Combine(stage, "positions");
        if (Directory.Exists(stagePositionsDir))
            Directory.Move(stagePositionsDir, oldPositionsDir);

        string oldSlot1Dir = Path.Combine(work, "old_slot1");
        Directory.CreateDirectory(oldSlot1Dir);
        foreach (string name in PerSlotFiles)
        {
            string srcPath = Path.Combine(stage, name);
            if (File.Exists(srcPath))
                File.Move(srcPath, Path.Combine(oldSlot1Dir, name), overwrite: true);
        }

        // Reconstruct target slot directories 1..M.
        for (int i = 0; i < plan.Count; i++)
        {
            var item = plan[i];
            string destDir = SlotDirectory(stage, item.TargetSlot);
            Directory.CreateDirectory(destDir);
            if (item.SourceSlot == 1)
                CopySlotDirectoryContents(oldSlot1Dir, destDir);
            else if (item.SourceSlot >= 2)
                CopySlotDirectoryContents(Path.Combine(oldPositionsDir, $"slot_{item.SourceSlot}"), destDir);
        }

        string? existingAudio = AudioProbe.FindExistingAudioFile(stage, info);
        var metadataEntries = new List<SlotMetadataEntry>(plan.Count);
        bool anyRebaked = false;
        int maxFrames = 0;
        int maxBones = 0;
        double maxDuration = 0.0;
        bool anyFacial = false;

        for (int i = 0; i < plan.Count; i++)
        {
            var item = plan[i];
            int targetSlot = item.TargetSlot;
            PositionRequest pos = item.Pos;
            string slotDir = SlotDirectory(stage, targetSlot);
            float outX = targetSlot == 1 ? 0.0f : CleanZero(pos.X);
            float outY = targetSlot == 1 ? 0.0f : CleanZero(pos.Y);
            float outZ = targetSlot == 1 ? 0.0f : CleanZero(pos.Z);
            float outYaw = targetSlot == 1 ? 0.0f : CleanZero(pos.YawDegrees);

            if (item.NeedsRebake)
            {
                anyRebaked = true;
                if (File.Exists(Path.Combine(slotDir, TargetGeometryName)))
                    throw new ImportStepException("unsupported_target_geometry",
                        $"槽位 {targetSlot} 含目标腿部几何；原生后端尚不支持保留该几何的重新烘焙");

                bool replaceBody = !string.IsNullOrEmpty(pos.MotionVmd);
                string body = replaceBody ? Path.GetFullPath(pos.MotionVmd) : Path.Combine(slotDir, "source.vmd");
                if (!File.Exists(body))
                    throw new InvalidDataException($"槽位 {targetSlot} 缺少原始动作备份 source.vmd，请重新指定动作 .vmd 文件");

                bool replaceMorph = !string.IsNullOrEmpty(pos.MorphVmd);
                bool stripStandaloneMorph = pos.DisableMorph || pos.ClearMorph;
                string morph = stripStandaloneMorph ? string.Empty
                    : replaceMorph ? Path.GetFullPath(pos.MorphVmd)
                    : File.Exists(Path.Combine(slotDir, "source_morph.vmd")) ? Path.Combine(slotDir, "source_morph.vmd") : string.Empty;

                float srcFps = pos.SourceFps > 0.0f ? pos.SourceFps : (replaceBody ? 0.0f : item.ExistingSourceFps);
                float outFps = request.Fps > 0.0f ? request.Fps : item.ExistingOutputFps;

                int progressBase = 10 + (75 * i) / plan.Count;
                int progressSpan = Math.Max(5, 75 / plan.Count);
                BakedSlotResult baked = BakeSingleSlot(
                    body,
                    morph,
                    pos.DisableMorph,
                    srcFps,
                    outFps,
                    request.ToothChannel,
                    existingAudio,
                    log,
                    (p, s, m) => log.Progress(p, s, m),
                    progressBase,
                    progressSpan);

                WriteBakedSlotFiles(slotDir, baked, stripStandaloneMorph);

                timing.ParseMs += baked.Timing.ParseMs;
                timing.EvaluateMs += baked.Timing.EvaluateMs;
                timing.RetargetEncodeMs += baked.Timing.RetargetEncodeMs;
                timing.FacialMs += baked.Timing.FacialMs;
                timing.VmdBytes += baked.VmdBytes;
                timing.ClipBytes += baked.ClipBytes.Length;

                bool hasStandaloneMorph = File.Exists(Path.Combine(slotDir, "source_morph.vmd"));
                string existingSourceVmd = ReadJsonString(item.ExistingMeta, "source_vmd");
                string preservedSourceVmd = replaceBody ? body : existingSourceVmd;
                string preservedSourceMorphVmd = pos.DisableMorph
                    ? string.Empty
                    : pos.ClearMorph
                        ? (baked.Clip.BlendshapeNames.Count > 0 ? preservedSourceVmd : string.Empty)
                        : replaceMorph
                            ? Path.GetFullPath(pos.MorphVmd)
                            : !hasStandaloneMorph
                                ? (baked.Clip.BlendshapeNames.Count > 0 ? preservedSourceVmd : string.Empty)
                                : ReadJsonString(item.ExistingMeta, "source_morph_vmd");

                var entry = new SlotMetadataEntry(
                    Slot: targetSlot,
                    X: outX,
                    Y: outY,
                    Z: outZ,
                    YawDegrees: outYaw,
                    HasMotion: true,
                    HasFacial: baked.Clip.BlendshapeNames.Count > 0,
                    SourceVmd: preservedSourceVmd,
                    SourceMorphVmd: preservedSourceMorphVmd,
                    SourceVmdSha256: baked.SourceVmdSha256,
                    SourceFps: baked.SourceFps,
                    Frames: baked.Clip.TotalFrames,
                    Bones: baked.Clip.Bones.Count,
                    DurationSeconds: baked.Clip.DurationSeconds,
                    OutputFps: baked.Clip.Fps);
                metadataEntries.Add(entry);
            }
            else
            {
                SlotMetadataEntry entry = ReadSlotMetadataFromClip(
                    slotDir, targetSlot, item.ExistingMeta, outX, outY, outZ, outYaw, item.ExistingOutputFps);
                metadataEntries.Add(entry);
            }

            SlotMetadataEntry last = metadataEntries[^1];
            if (last.Frames > maxFrames) maxFrames = last.Frames;
            if (last.Bones > maxBones) maxBones = last.Bones;
            if (last.DurationSeconds > maxDuration) maxDuration = last.DurationSeconds;
            if (last.HasFacial) anyFacial = true;
        }

        SlotMetadataEntry slot1 = metadataEntries[0];
        if (anyRebaked)
        {
            timing.Frames = maxFrames;
            timing.Bones = maxBones;
            timing.Fps = slot1.OutputFps;
            info["import_backend"] = "csharp";
            info["import_candidate_b_tree"] = CandidateBTreeSha256;
        }

        info["is_group"] = metadataEntries.Count >= 2;
        info["position_count"] = metadataEntries.Count;
        var posArray = new JsonArray();
        foreach (SlotMetadataEntry entry in metadataEntries)
            posArray.Add((JsonNode)BuildPositionJsonObject(entry));
        info["positions"] = posArray;

        info["duration_seconds"] = maxDuration;
        info["total_frames"] = maxFrames;
        info["bone_count"] = maxBones;
        info["has_facial"] = anyFacial;
        info["fps"] = slot1.OutputFps;
        info["source_fps"] = slot1.SourceFps;
        info["source_vmd"] = slot1.SourceVmd;
        info["source_vmd_sha256"] = slot1.SourceVmdSha256;
        info["source_morph_vmd"] = slot1.SourceMorphVmd;
    }

    // Content hashes detect same-size changes even when the modification time
    // was restored. Directory entries also detect additions/removals/renames,
    // including empty directories. Reparse points retain the copy policy.
    internal sealed record EditTreeEntry(string RelativePath, bool IsDirectory,
        long Length, long LastWriteUtcTicks, string Sha256);

    internal static IReadOnlyList<EditTreeEntry> CaptureEditSignature(string directory)
    {
        if (!Directory.Exists(directory)) throw EditConflict();
        var entries = new List<EditTreeEntry>();
        Visit(directory);
        return entries;

        void Visit(string current)
        {
            if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("曲目编辑不接受链接或重解析点");
            foreach (string entry in Directory.EnumerateFileSystemEntries(current).Order(StringComparer.Ordinal))
            {
                FileAttributes attributes = File.GetAttributes(entry);
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidDataException("曲目编辑不接受链接或重解析点: " + Path.GetFileName(entry));
                string relative = Path.GetRelativePath(directory, entry);
                if ((attributes & FileAttributes.Directory) != 0)
                {
                    entries.Add(new EditTreeEntry(relative, true, 0, 0, string.Empty));
                    Visit(entry);
                }
                else
                {
                    var before = new FileInfo(entry);
                    long length = before.Length, written = before.LastWriteTimeUtc.Ticks;
                    using var stream = new FileStream(entry, FileMode.Open, FileAccess.Read, FileShare.Read);
                    string hash = Convert.ToHexString(SHA256.HashData(stream));
                    var after = new FileInfo(entry);
                    if (after.Length != length || after.LastWriteTimeUtc.Ticks != written) throw EditConflict();
                    entries.Add(new EditTreeEntry(relative, false, length, written, hash));
                }
            }
        }
    }

    private static InvalidDataException EditConflict() => new(
        "曲目目录在编辑期间已被外部修改或移除，未覆盖原曲目。请重新打开管理编辑后重试。");

    private static void EnsureEditUnchanged(string target, IReadOnlyList<EditTreeEntry> original)
    {
        if (!CaptureEditSignature(target).SequenceEqual(original)) throw EditConflict();
    }

    /// <summary>Optimistic conflict check at the real edit publication boundary.</summary>
    internal static void PublishEditedTrack(string stage, string target, string backup,
        IReadOnlyList<EditTreeEntry> original)
    {
        EnsureEditUnchanged(target, original);
        Directory.Move(target, backup);
        try
        {
            // Check the moved original again before publishing, so changes
            // between the first check and directory move are not discarded.
            EnsureEditUnchanged(backup, original);
            Directory.Move(stage, target);
        }
        catch
        {
            if (!Directory.Exists(target) && !File.Exists(target) && Directory.Exists(backup))
                Directory.Move(backup, target);
            throw;
        }
    }

    private static void CopyEditTree(string source, string destination)
    {
        if ((File.GetAttributes(source) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException("曲目编辑不接受链接或重解析点");
        Directory.CreateDirectory(destination);
        foreach (string entry in Directory.EnumerateFileSystemEntries(source))
        {
            FileAttributes attributes = File.GetAttributes(entry);
            if ((attributes & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("曲目编辑不接受链接或重解析点: " + Path.GetFileName(entry));
            string output = Path.Combine(destination, Path.GetFileName(entry));
            if ((attributes & FileAttributes.Directory) != 0) CopyEditTree(entry, output);
            else File.Copy(entry, output, overwrite: false);
        }
    }

    private static JsonObject ReadEditInfo(string path)
    {
        try { return JsonNode.Parse(File.ReadAllBytes(path)) as JsonObject ?? new JsonObject(); }
        catch (JsonException) { return new JsonObject(); }
        catch (FileNotFoundException) { return new JsonObject(); }
    }

    private static void WriteEditInfo(string stage, JsonObject info)
    {
        using var stream = File.Create(Path.Combine(stage, "info.json"));
        using var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true });
        info.WriteTo(writer);
    }

    private static float EditFps(JsonObject info)
    {
        JsonNode? node = info["source_fps"] ?? info["fps"];
        if (node is null) return 30;
        if (node is JsonValue value && value.TryGetValue<float>(out float fps) && float.IsFinite(fps) && fps > 0)
            return fps;
        throw new InvalidDataException("曲目 info.json 中的采样率无效");
    }

    private static ImportOutcome Fail(ImportLog log, ImportTiming timing, Stopwatch total, string message)
    {
        total.Stop();
        timing.TotalMs = total.Elapsed.TotalMilliseconds;
        log.Error(message);
        log.Line(timing.ToLogLine(CandidateBTreeSha256));
        return new ImportOutcome { Ok = false, Error = message, Timing = timing };
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

    /// <summary>Stages a track outside the scanned library, then commits it atomically.</summary>
    private static string CommitTrack(ImportRequest request, IReadOnlyList<SlotSpec> slots,
        IReadOnlyList<BakedSlotResult> bakedSlots, string? audio, string cameraPath, bool cameraIsBaked,
        ImportTiming timing, ImportLog log, Action<int, string, string> progress)
    {
        BakedSlotResult slot1Baked = bakedSlots[0];
        string motionsRoot = Path.GetFullPath(request.MotionsRoot);
        Directory.CreateDirectory(motionsRoot);
        string commit = NewTrackDestination(request, slot1Baked.VmdPath, out string title);
        EnsureNewTrackAvailable(commit);

        string parent = TransactionParent(request.TransactionRoot, motionsRoot);
        string stage = Path.Combine(parent, "gkmotion-cs-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(stage);

        try
        {
            var slotMeta = new List<SlotMetadataEntry>(slots.Count);
            for (int i = 0; i < slots.Count; i++)
            {
                SlotSpec spec = slots[i];
                BakedSlotResult baked = bakedSlots[i];
                string slotDir = SlotDirectory(stage, spec.Slot);
                WriteBakedSlotFiles(slotDir, baked, spec.DisableMorph);

                string slotSourceMorph = spec.DisableMorph
                    ? string.Empty
                    : (string.IsNullOrEmpty(baked.MorphPath) ? baked.VmdPath : baked.MorphPath);
                slotMeta.Add(new SlotMetadataEntry(
                    Slot: spec.Slot,
                    X: spec.Slot == 1 ? 0.0f : CleanZero(spec.X),
                    Y: spec.Slot == 1 ? 0.0f : CleanZero(spec.Y),
                    Z: spec.Slot == 1 ? 0.0f : CleanZero(spec.Z),
                    YawDegrees: spec.Slot == 1 ? 0.0f : CleanZero(spec.YawDegrees),
                    HasMotion: true,
                    HasFacial: baked.Clip.BlendshapeNames.Count > 0,
                    SourceVmd: baked.VmdPath,
                    SourceMorphVmd: slotSourceMorph,
                    SourceVmdSha256: baked.SourceVmdSha256,
                    SourceFps: baked.SourceFps,
                    Frames: baked.Clip.TotalFrames,
                    Bones: baked.Clip.Bones.Count,
                    DurationSeconds: baked.Clip.DurationSeconds,
                    OutputFps: baked.Clip.Fps));
            }

            AudioProbeResult? audioProbe = null;
            string copiedAudioFile = string.Empty;
            string? coverFile = null;
            if (audio != null)
            {
                audioProbe = AudioProbe.Probe(audio, extractCover: true);
                if (!audioProbe.Valid) throw new InvalidDataException(audioProbe.Error);
                copiedAudioFile = "song" + Path.GetExtension(audio).ToLowerInvariant();
                File.Copy(audio, Path.Combine(stage, copiedAudioFile), overwrite: true);
                coverFile = AudioProbe.SaveCoverToDirectory(audioProbe.CoverBytes, audioProbe.CoverMime, stage);
            }

            WriteInfoJson(stage, title, slotMeta, false, string.Empty, null);

            // --- Camera ---------------------------------------------------------
            string sourceCamera = string.Empty;
            string cameraNotice = string.Empty;
            CameraRegistrationMetadata? cameraRegistration = null;
            if (!string.IsNullOrEmpty(cameraPath))
            {
                progress(94, "CAMERA", "正在烘焙运镜轨道...");
                var cameraWatch = Stopwatch.StartNew();
                string target = Path.Combine(stage, "camera.gkcamera");
                if (cameraIsBaked)
                {
                    CameraBaker.Validate(cameraPath);
                    File.Copy(cameraPath, target, overwrite: true);
                    cameraRegistration = new CameraRegistrationMetadata(
                        "not_applicable", null, "prebaked_camera", null,
                        "baked .gkcamera is copied as-is; root registration is bypassed to avoid double correction");
                }
                else
                {
                    double cameraFps = request.CameraFps > 0.0f ? request.CameraFps : DefaultCameraFps;
                    double cameraScale = request.CameraScale > 0.0f ? request.CameraScale : DefaultCameraScale;
                    string mode = string.IsNullOrEmpty(request.CameraRootRegistration)
                        ? CameraRegistration.ModeAuto
                        : request.CameraRootRegistration;
                    CameraSample[] samples = CameraBaker.Bake(
                        VmdReader.Parse(cameraPath).CameraKeyframes, cameraFps, cameraScale, detectCuts: true);
                    CameraRegistrationResult registration =
                        CameraRegistration.Register(samples, stage, cameraFps, cameraScale, mode, slot1Baked.Clip);
                    CameraBaker.Write(target, registration.Samples, cameraFps);
                    cameraNotice = registration.Description;
                    cameraRegistration = new CameraRegistrationMetadata(
                        registration.StatusText, registration.Mode, registration.Reason,
                        registration.ResidualMeters, registration.Description);
                    log.Line($"Camera root registration: {registration.StatusText} " +
                             $"(mode={registration.Mode}, reason={registration.Reason}) — {registration.Description}");
                    if (registration.Status == CameraRegistrationStatus.NotApplicable && mode == CameraRegistration.ModeAuto)
                    {
                        log.Line("提示：该镜头无法与本案动作认证为同一根轨迹（或时钟不支持），已保留原始相机坐标并正常输出镜头；" +
                                 "这不是导入失败。如需强制校正请改用 root-registration=required，如需跳过诊断请用 off。");
                    }
                }

                (uint count, float rate) = CameraBaker.Validate(target);
                timing.CameraMs = cameraWatch.Elapsed.TotalMilliseconds;
                log.Line($"Camera imported: {count} frames at {rate:g} fps, duration {(count - 1) / rate:F3}s");
                if (!cameraIsBaked)
                {
                    File.Copy(cameraPath, Path.Combine(stage, "source_camera.vmd"), overwrite: true);
                    sourceCamera = cameraPath;
                }
                WriteInfoJson(stage, title, slotMeta, File.Exists(target), sourceCamera, cameraRegistration);
            }

            // Save inside the same directory transaction as all four sources.
            JsonObject info = ReadEditInfo(Path.Combine(stage, "info.json"));
            if (request.HasSubtitle && !string.IsNullOrWhiteSpace(request.Subtitle))
                info["subtitle"] = request.Subtitle.Trim();
            else if (audioProbe != null && !string.IsNullOrWhiteSpace(audioProbe.Artist) &&
                     (info["subtitle"] is null || string.IsNullOrWhiteSpace(info["subtitle"]!.ToString())))
                info["subtitle"] = audioProbe.Artist.Trim();
            if (!string.IsNullOrEmpty(copiedAudioFile))
            {
                info["audio_file"] = copiedAudioFile;
                double dur = audioProbe!.DurationSeconds > 0 ? audioProbe.DurationSeconds : request.AudioDurationHint;
                if (dur > 0) info["audio_duration"] = dur;
                if (!string.IsNullOrWhiteSpace(audioProbe.Artist)) info["audio_artist"] = audioProbe.Artist;
                if (!string.IsNullOrWhiteSpace(audioProbe.Title)) info["audio_title"] = audioProbe.Title;
            }
            if (!string.IsNullOrEmpty(coverFile))
            {
                info["has_cover"] = true;
                info["cover_file"] = coverFile;
            }
            else if (audio != null)
            {
                info["has_cover"] = false;
            }
            info["track_timing"] = (request.TrackTiming ?? new PerformanceTimingSettings()).ToJson();
            WriteEditInfo(stage, info);
            PublishNewTrack(stage, commit);
            if (!string.IsNullOrEmpty(cameraNotice)) log.Line("Camera: " + cameraNotice);
            log.Line($"曲目已写入: {commit}");
            return commit;
        }
        finally
        {
            if (Directory.Exists(stage)) TryDelete(stage);
        }
    }

    private static string NewTrackDestination(ImportRequest request, string vmdPath, out string title)
    {
        title = string.IsNullOrWhiteSpace(request.Title)
            ? Path.GetFileNameWithoutExtension(vmdPath) : request.Title.Trim();
        return Path.Combine(Path.GetFullPath(request.MotionsRoot), SafeTrackName(title));
    }

    private static string TransactionParent(string requested, string library)
    {
        string fallback = Path.GetDirectoryName(library.TrimEnd(Path.DirectorySeparatorChar,
            Path.AltDirectorySeparatorChar)) ?? library;
        if (string.IsNullOrWhiteSpace(requested)) return fallback;
        string root = Path.GetFullPath(requested);
        // Publish uses directory rename, never a cross-volume copy fallback.
        if (!string.Equals(Path.GetPathRoot(root), Path.GetPathRoot(library), StringComparison.OrdinalIgnoreCase))
            throw new IOException("事务临时目录必须与曲库同卷；未修改曲目");
        if (string.Equals(root.TrimEnd(Path.DirectorySeparatorChar), library.TrimEnd(Path.DirectorySeparatorChar),
            StringComparison.OrdinalIgnoreCase) || root.StartsWith(library.TrimEnd(Path.DirectorySeparatorChar) +
            Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new IOException("事务临时目录不能位于曲库扫描目录内；未修改曲目");
        Directory.CreateDirectory(root);
        return root;
    }

    private static void WriteTransaction(string work, string kind, string target, string state)
    {
        Directory.CreateDirectory(work);
        string path = Path.Combine(work, "transaction.json");
        string temporary = path + ".tmp";
        using var buffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(buffer))
        {
            writer.WriteStartObject(); writer.WriteNumber("schemaVersion", 1);
            writer.WriteString("kind", kind); writer.WriteString("target", target);
            writer.WriteString("state", state); writer.WriteEndObject();
        }
        File.WriteAllBytes(temporary, buffer.ToArray());
        File.Move(temporary, path, overwrite: true);
    }

    private static void EnsureNewTrackAvailable(string destination)
    {
        if (Directory.Exists(destination) || File.Exists(destination)) throw NewTrackConflict(destination);
    }

    private static InvalidDataException NewTrackConflict(string destination) => new(
        $"曲目名称与已有目录或文件冲突（{Path.GetFileName(destination)}），未覆盖已有内容。请更改名称；要修改已有曲目，请使用管理编辑。");

    internal static void PublishNewTrack(string stage, string destination)
    {
        EnsureNewTrackAvailable(destination);
        try { Directory.Move(stage, destination); }
        catch (IOException) when (Directory.Exists(destination) || File.Exists(destination))
        {
            // Directory.Move itself never replaces a destination. Translate a
            // destination created after our check into the same useful error.
            throw NewTrackConflict(destination);
        }
    }

    private static void WriteInfoJson(string stage, string title, IReadOnlyList<SlotMetadataEntry> slots,
        bool hasCamera, string sourceCamera, CameraRegistrationMetadata? registration)
    {
        SlotMetadataEntry slot1 = slots[0];
        double maxDuration = slots.Max(s => s.DurationSeconds);
        int maxFrames = slots.Max(s => s.Frames);
        int maxBones = slots.Max(s => s.Bones);
        bool anyFacial = slots.Any(s => s.HasFacial);
        bool isGroup = slots.Count >= 2;

        var sb = new StringBuilder(1024);
        sb.Append("{\n");
        sb.Append("  \"title\": ").Append(Quote(title)).Append(",\n");
        sb.Append("  \"duration_seconds\": ")
          .Append(maxDuration.ToString("R", CultureInfo.InvariantCulture)).Append(",\n");
        sb.Append("  \"total_frames\": ").Append(maxFrames.ToString(CultureInfo.InvariantCulture)).Append(",\n");
        sb.Append("  \"bone_count\": ").Append(maxBones.ToString(CultureInfo.InvariantCulture)).Append(",\n");
        // fps is the rate the stored frames advance at; source_fps is the rate of
        // the VMD tick timeline. They differ only for a resampled (non-source-clock)
        // body, and duration_seconds is always total_frames / fps.
        sb.Append("  \"fps\": ").Append(slot1.OutputFps.ToString("R", CultureInfo.InvariantCulture)).Append(",\n");
        sb.Append("  \"source_vmd\": ").Append(Quote(slot1.SourceVmd)).Append(",\n");
        sb.Append("  \"source_fps\": ").Append(slot1.SourceFps.ToString("R", CultureInfo.InvariantCulture)).Append(",\n");
        sb.Append("  \"source_vmd_sha256\": \"").Append(slot1.SourceVmdSha256).Append("\",\n");
        sb.Append("  \"has_facial\": ").Append(anyFacial ? "true" : "false").Append(",\n");
        sb.Append("  \"has_camera\": ").Append(hasCamera ? "true" : "false").Append(",\n");
        sb.Append("  \"source_morph_vmd\": ").Append(Quote(slot1.SourceMorphVmd)).Append(",\n");
        sb.Append("  \"source_camera_vmd\": ").Append(Quote(sourceCamera)).Append(",\n");
        sb.Append("  \"is_group\": ").Append(isGroup ? "true" : "false").Append(",\n");
        sb.Append("  \"position_count\": ").Append(slots.Count.ToString(CultureInfo.InvariantCulture)).Append(",\n");
        sb.Append("  \"positions\": [\n");
        for (int i = 0; i < slots.Count; i++)
        {
            SlotMetadataEntry s = slots[i];
            sb.Append("    {\n");
            sb.Append("      \"slot\": ").Append(s.Slot.ToString(CultureInfo.InvariantCulture)).Append(",\n");
            sb.Append("      \"x\": ").Append(CleanZero(s.X).ToString("R", CultureInfo.InvariantCulture)).Append(",\n");
            sb.Append("      \"y\": ").Append(CleanZero(s.Y).ToString("R", CultureInfo.InvariantCulture)).Append(",\n");
            sb.Append("      \"z\": ").Append(CleanZero(s.Z).ToString("R", CultureInfo.InvariantCulture)).Append(",\n");
            sb.Append("      \"yaw_degrees\": ").Append(CleanZero(s.YawDegrees).ToString("R", CultureInfo.InvariantCulture)).Append(",\n");
            sb.Append("      \"has_motion\": ").Append(s.HasMotion ? "true" : "false").Append(",\n");
            sb.Append("      \"has_facial\": ").Append(s.HasFacial ? "true" : "false").Append(",\n");
            sb.Append("      \"source_vmd\": ").Append(Quote(s.SourceVmd)).Append(",\n");
            sb.Append("      \"source_morph_vmd\": ").Append(Quote(s.SourceMorphVmd)).Append(",\n");
            sb.Append("      \"source_vmd_sha256\": ").Append(Quote(s.SourceVmdSha256)).Append(",\n");
            sb.Append("      \"source_fps\": ").Append(s.SourceFps.ToString("R", CultureInfo.InvariantCulture)).Append(",\n");
            sb.Append("      \"frames\": ").Append(s.Frames.ToString(CultureInfo.InvariantCulture)).Append(",\n");
            sb.Append("      \"bones\": ").Append(s.Bones.ToString(CultureInfo.InvariantCulture)).Append("\n");
            sb.Append(i + 1 < slots.Count ? "    },\n" : "    }\n");
        }
        sb.Append("  ],\n");
        if (registration != null)
        {
            sb.Append("  \"camera_root_registration\": {\n");
            sb.Append("    \"result\": ").Append(Quote(registration.Result)).Append(",\n");
            sb.Append("    \"mode\": ")
              .Append(registration.Mode is null ? "null" : Quote(registration.Mode)).Append(",\n");
            sb.Append("    \"reason\": ").Append(Quote(registration.Reason)).Append(",\n");
            sb.Append("    \"residual_m\": ")
              .Append(registration.ResidualMeters is { } residual
                  ? residual.ToString("G9", CultureInfo.InvariantCulture)
                  : "null").Append(",\n");
            sb.Append("    \"detail\": ").Append(Quote(registration.Detail)).Append("\n");
            sb.Append("  },\n");
        }
        sb.Append("  \"import_backend\": \"csharp\",\n");
        sb.Append("  \"import_candidate_b_tree\": \"").Append(CandidateBTreeSha256).Append("\"\n");
        sb.Append("}");
        File.WriteAllText(Path.Combine(stage, "info.json"), sb.ToString(), new UTF8Encoding(false));
    }

    /// <summary>Mirrors import_motion.py's folder naming rules.</summary>
    private static string SafeTrackName(string title)
    {
        var sb = new StringBuilder(title.Length);
        foreach (char c in title)
        {
            bool illegal = c is '<' or '>' or ':' or '"' or '/' or '\\' or '|' or '?' or '*' || c < 0x20;
            sb.Append(illegal ? '_' : c);
        }
        string name = sb.ToString().Trim(' ', '.');
        if (name.Length > 100) name = name[..100];
        if (name.Length == 0) name = "Imported motion";

        string stem = name.Split('.')[0].ToUpperInvariant();
        if (stem is "CON" or "PRN" or "AUX" or "NUL" ||
            (stem.Length == 4 &&
             (stem.StartsWith("COM", StringComparison.Ordinal) || stem.StartsWith("LPT", StringComparison.Ordinal)) &&
             stem[3] is >= '1' and <= '9'))
            name = "_" + name;

        bool ascii = name.All(c => c is >= '\x20' and <= '\x7e');
        if (!ascii)
        {
            var asciiBuilder = new StringBuilder(name.Length);
            foreach (Rune rune in name.EnumerateRunes())
                asciiBuilder.Append(rune.Value is >= 0x20 and <= 0x7e ? (char)rune.Value : '_');
            string asciiName = asciiBuilder.ToString().Trim(' ', '_', '.');
            if (asciiName.Length == 0) asciiName = "Imported";
            using var sha = SHA256.Create();
            string digest = Convert.ToHexString(sha.ComputeHash(Encoding.UTF8.GetBytes(title))).ToLowerInvariant();
            name = asciiName + "-" + digest[..8];
        }
        return name;
    }

    private static string Quote(string value)
    {
        var sb = new StringBuilder(value.Length + 8);
        sb.Append('"');
        foreach (char c in value)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    else sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
        return sb.ToString();
    }

    private static void TryDelete(string directory)
    {
        try
        {
            foreach (string file in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories))
                File.SetAttributes(file, FileAttributes.Normal);
            Directory.Delete(directory, recursive: true);
        }
        catch
        {
            // Leftover staging directories are harmless; they live outside the library.
        }
    }

    private static void AppendTimingRecord(ImportRequest request, ImportTiming timing, string vmdPath)
    {
        if (string.IsNullOrEmpty(request.TimingJsonl)) return;
        try
        {
            string? dir = Path.GetDirectoryName(request.TimingJsonl);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            File.AppendAllText(request.TimingJsonl,
                timing.ToJsonlLine(CandidateBTreeSha256, vmdPath, "csharp", "ok") + Environment.NewLine,
                new UTF8Encoding(false));
        }
        catch
        {
            // Timing collection is best-effort and must not fail an import.
        }
    }
}
