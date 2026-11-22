namespace GakuMotion.VmdImporter.Native;

using System.Diagnostics;
using GakuMotion.VmdImporter.Native.Facial;
using GakuMotion.VmdImporter.Output;
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
internal static partial class ImportPipeline
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

    private static ImportOutcome Fail(ImportLog log, ImportTiming timing, Stopwatch total, string message)
    {
        total.Stop();
        timing.TotalMs = total.Elapsed.TotalMilliseconds;
        log.Error(message);
        log.Line(timing.ToLogLine(CandidateBTreeSha256));
        return new ImportOutcome { Ok = false, Error = message, Timing = timing };
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
}
