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

    private const string TargetGeometryName = "target_leg_geometry.json";

    private static readonly string[] EyeOnlyBones = { "両目", "左目", "右目" };
    private const double DefaultCameraFps = 60.0;
    private const double DefaultCameraScale = 0.083;

    public static ImportOutcome Run(ImportRequest request, ImportLog log)
    {
        if (!string.IsNullOrEmpty(request.EditDir)) return RunEdit(request, log);
        var timing = new ImportTiming();
        var total = Stopwatch.StartNew();
        var sw = new Stopwatch();

        void Progress(int percent, string stage, string message) => log.Progress(percent, stage, message);

        try
        {
            log.Line($"GakuMotion C# VMD importer (Candidate B tree {CandidateBTreeSha256})");
            Progress(3, "INIT", "正在校验输入文件...");

            string vmdPath = Path.GetFullPath(request.MotionVmd);
            if (!File.Exists(vmdPath)) return Fail(log, timing, total, "找不到 VMD 文件: " + vmdPath);
            if (!string.Equals(Path.GetExtension(vmdPath), ".vmd", StringComparison.OrdinalIgnoreCase))
                return Fail(log, timing, total, "请选择 .vmd 动作文件");

            // Adding is never an implicit edit, including names that collide
            // only after Windows-safe folder-name normalization.
            EnsureNewTrackAvailable(NewTrackDestination(request, vmdPath, out _));

            // Python's automatic target-IK bake is not part of this backend.
            // Refuse instead of silently emitting a different leg solve.
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

            timing.VmdBytes = new FileInfo(vmdPath).Length;

            // --- Morph VMD (optional standalone facial track) ------------------
            string morphPath = string.Empty;
            VmdMotion? morphSource = null;
            if (!string.IsNullOrEmpty(request.MorphVmd))
            {
                morphPath = Path.GetFullPath(request.MorphVmd);
                if (!File.Exists(morphPath)) return Fail(log, timing, total, "找不到表情 VMD 文件: " + morphPath);
                if (!string.Equals(Path.GetExtension(morphPath), ".vmd", StringComparison.OrdinalIgnoreCase))
                    return Fail(log, timing, total, "表情文件必须是 .vmd");
                morphSource = VmdReader.Parse(morphPath);
                if (morphSource.MorphKeyframes.Count == 0 && !EyeProfile.HasEyeTracks(morphSource))
                    return Fail(log, timing, total, "所选表情文件不包含表情或眼神关键帧");
            }
            else if (request.DisableMorph)
            {
                log.Line("表情轨道：用户显式关闭（不生成 blendshape 通道）。");
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

            // --- Audio (PCM WAV only) ------------------------------------------
            string? wavPath = ResolveAudio(vmdPath, request.Audio, request.ClearAudio);
            if (wavPath != null && !TryValidatePcmWav(wavPath, out _, out string audioError))
                return Fail(log, timing, total, audioError);

            sw.Restart();
            VmdMotion motion = VmdReader.Parse(vmdPath);
            timing.ParseMs = sw.Elapsed.TotalMilliseconds;
            Progress(12, "PARSE",
                $"VMD 已解析：{motion.BoneKeyframes.Count} 条骨骼轨道 / {motion.MorphKeyframes.Count} 条表情轨道，最大帧 {motion.MaxFrame}");

            if (motion.BoneKeyframes.Count == 0)
                return Fail(log, timing, total, "VMD 中没有骨骼动作（仅运镜/表情的 VMD 不支持）");
            bool hasBody = motion.BoneKeyframes.Keys.Any(name => !EyeOnlyBones.Contains(name));
            if (!hasBody)
                return Fail(log, timing, total, "VMD 中没有躯干骨骼动作（仅表情/眼动的 VMD 不支持作为动作）");

            // The VMD timeline has a clock of its own (info.json / WAV duration /
            // the "60 fps" stem). request.Fps selects the rate the clip is
            // *stored* at, not how fast the source ticks advance: 0 keeps the
            // source clock, which is the historical 1 tick = 1 frame behaviour.
            float sourceFps = DetectSourceFps(vmdPath, motion, wavPath);
            float outputFps = request.Fps > 0.0f ? request.Fps : sourceFps;
            timing.Fps = outputFps;

            // A standalone facial VMD may also carry the gaze tracks; when it
            // does, it replaces the body VMD's eye tracks before evaluation.
            bool overrideEyes = !request.DisableMorph && morphSource != null &&
                                !string.Equals(morphPath, vmdPath, StringComparison.OrdinalIgnoreCase);
            VmdMotion effective = overrideEyes ? WithEyeTracks(motion, morphSource!) : motion;
            effective = DeformLegTracks.Prepare(effective, out string[] deformLegSides);
            if (deformLegSides.Length != 0)
                log.Line("Detected baked leg tracks; adapted sides: " + string.Join(", ", deformLegSides));

            sw.Restart();
            var evaluator = new SourceEvaluator();
            SourceEvaluationResult result = evaluator.Evaluate(effective, fps: sourceFps);
            timing.EvaluateMs = sw.Elapsed.TotalMilliseconds;
            Progress(65, "RETARGET", "正在转换全身骨骼局部姿态...");

            sw.Restart();
            bool hasEyeTracks = EyeProfile.HasEyeTracks(effective);
            GakumasBoneProfile profile = hasEyeTracks
                ? EyeProfile.Extend(GakumasBoneProfile.Default)
                : GakumasBoneProfile.Default;
            int version = hasEyeTracks ? 5 : GkClipFormat.DefaultVersion;

            GkClipData clip = GkClipEncoder.FromEvaluationResult(result, profile, evaluator.Rig, version);
            DisabledLegIkFk.Apply(effective, result, evaluator.Rig, clip);
            timing.RetargetEncodeMs = sw.Elapsed.TotalMilliseconds;

            // --- Facial ---------------------------------------------------------
            sw.Restart();
            Progress(80, "MORPH", "正在映射表情形态与眼神轨道...");
            IReadOnlyDictionary<string, IReadOnlyList<MorphKeyframe>> morphTracks =
                request.DisableMorph
                    ? new Dictionary<string, IReadOnlyList<MorphKeyframe>>()
                    : (morphSource?.MorphKeyframes ?? motion.MorphKeyframes);
            string? toothChannel = request.DisableMorph
                ? null
                : (string.IsNullOrEmpty(request.ToothChannel) ? MorphTranslator.DefaultToothChannel : request.ToothChannel);

            (List<string> names, float[,] weights, MorphReport report) =
                MorphTranslator.Translate(morphTracks, clip.TotalFrames, toothChannel);
            clip.BlendshapeNames = names;
            clip.BlendshapeWeights = names.Count > 0 ? weights : null;

            // Bone deltas, root, camera payload and blendshape weights were all
            // sampled on the source clock above; re-time them together so the
            // header's fps/frames/duration describe one consistent timeline.
            if (!ClipTimebase.IsIdentity(sourceFps, outputFps))
            {
                int sourceFrames = clip.TotalFrames;
                clip = ClipTimebase.Retime(clip, sourceFps, outputFps);
                log.Line($"timebase: source {sourceFps:g} fps x {sourceFrames} ticks -> " +
                         $"body {clip.Fps:g} fps x {clip.TotalFrames} frames, " +
                         $"duration {clip.DurationSeconds:F3}s unchanged");
            }

            byte[] bytes = GkClipEncoder.Encode(clip);
            timing.FacialMs = sw.Elapsed.TotalMilliseconds;
            timing.Frames = clip.TotalFrames;
            timing.Bones = clip.Bones.Count;
            timing.ClipBytes = bytes.Length;

            log.Line($"morphs: {morphTracks.Count} source tracks -> {names.Count} named blendshapes; " +
                     $"{report.UnmappedActive.Count} unmapped active tracks" +
                     (report.ToothStatus != null ? $" | tooth={report.ToothStatus}" : string.Empty));
            if (report.UnmappedActive.Count > 0)
                log.Line("  WARNING unmapped active morphs: " + string.Join(", ", report.UnmappedActive));

            Progress(90, "PACK", "正在写入 .gkclip 动作包...");

            sw.Restart();
            string committed = CommitTrack(request, vmdPath, morphPath, clip, bytes, report, sourceFps,
                cameraPath, cameraIsBaked, timing, log, Progress);
            timing.PackMs = sw.Elapsed.TotalMilliseconds;

            total.Stop();
            timing.TotalMs = total.Elapsed.TotalMilliseconds;
            log.Line(timing.ToUserLine());
            log.Line(timing.ToLogLine(CandidateBTreeSha256));
            AppendTimingRecord(request, timing, vmdPath);
            Progress(100, "DONE", "曲目导入完成");

            return new ImportOutcome { Ok = true, Directory = committed, Timing = timing };
        }
        catch (Exception ex)
        {
            return Fail(log, timing, total, ex is InvalidDataException ? ex.Message : ex.GetType().Name + ": " + ex.Message);
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
            if (!Directory.Exists(target) || !new[] { "motion.gkclip", "motion.gk_world_dump", "motion.gkmotion" }
                    .Any(name => File.Exists(Path.Combine(target, name))))
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

            bool replaceBody = !string.IsNullOrEmpty(request.MotionVmd);
            bool replaceMorph = !string.IsNullOrEmpty(request.MorphVmd);
            bool rebake = replaceBody || replaceMorph || request.DisableMorph;
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
                float fps = request.Fps > 0 ? request.Fps : EditFps(info);
                string existingAudio = Path.Combine(stage, "song.wav");
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
                    Audio = File.Exists(existingAudio) ? existingAudio : string.Empty,
                    ClearAudio = !File.Exists(existingAudio),
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
                if (request.DisableMorph) info["source_morph_vmd"] = string.Empty;
                else if (replaceMorph) info["source_morph_vmd"] = Path.GetFullPath(request.MorphVmd);
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
            else if (!string.IsNullOrEmpty(request.CameraVmd))
            {
                string camera = Path.GetFullPath(request.CameraVmd);
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
                    File.Copy(camera, Path.Combine(stage, "source_camera.vmd"), overwrite: true);
                else File.Delete(Path.Combine(stage, "source_camera.vmd"));
                info["source_camera_vmd"] = camera;
                // A previous camera's registration receipt cannot describe this one.
                info.Remove("camera_root_registration");
            }
            info["has_camera"] = File.Exists(Path.Combine(stage, "camera.gkcamera"));

            if (request.ClearAudio) File.Delete(Path.Combine(stage, "song.wav"));
            else if (!string.IsNullOrEmpty(request.Audio))
            {
                string audio = Path.GetFullPath(request.Audio);
                if (!File.Exists(audio)) throw new InvalidDataException("找不到音频文件: " + audio);
                if (!TryValidatePcmWav(audio, out _, out string error)) throw new InvalidDataException(error);
                File.Copy(audio, Path.Combine(stage, "song.wav"), overwrite: true);
            }
            if (!string.IsNullOrWhiteSpace(request.Title)) info["title"] = request.Title.Trim();
            else if (info["title"] is null || info["title"]!.ToString().Length == 0) info["title"] = Path.GetFileName(target);
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
    private static float DetectSourceFps(string vmdPath, VmdMotion motion, string? wavPath)
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

        if (wavPath != null && TryValidatePcmWav(wavPath, out double seconds, out _) && seconds > 0.0)
        {
            double ratio = (motion.MaxFrame + 1) / seconds;
            if (ratio is >= 54.0 and <= 66.0) return 60.0f;
            if (ratio is >= 24.0 and <= 36.0) return 30.0f;
            if (ratio >= 45.0) return 60.0f;
            return 30.0f;
        }

        if (Regex.IsMatch(Path.GetFileNameWithoutExtension(vmdPath), @"60\s*fps", RegexOptions.IgnoreCase))
            return 60.0f;
        return 30.0f;
    }

    /// <summary>import_motion's audio resolution: explicit file, else adjacent WAV.</summary>
    private static string? ResolveAudio(string vmdPath, string audioPath, bool clearAudio)
    {
        if (clearAudio) return null;
        if (!string.IsNullOrEmpty(audioPath))
        {
            string explicitPath = Path.GetFullPath(audioPath);
            return File.Exists(explicitPath) ? explicitPath : null;
        }
        string sibling = Path.ChangeExtension(vmdPath, ".wav");
        if (File.Exists(sibling)) return sibling;
        string song = Path.Combine(Path.GetDirectoryName(vmdPath)!, "song.wav");
        return File.Exists(song) ? song : null;
    }

    /// <summary>
    /// PCM WAV contract. The runtime plays song.wav through MCI `type waveaudio`,
    /// so a container that is merely *named* .wav produces a track that shows a
    /// music badge and plays silence. Reject it before anything is written.
    /// </summary>
    private static bool TryValidatePcmWav(string path, out double seconds, out string error)
    {
        seconds = 0.0;
        error = string.Empty;
        try
        {
            if (!string.Equals(Path.GetExtension(path), ".wav", StringComparison.OrdinalIgnoreCase))
            {
                error = $"音频轨道仅支持 PCM WAV：{Path.GetFileName(path)} 不是 .wav 文件。";
                return false;
            }

            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            using var reader = new BinaryReader(stream, Encoding.ASCII, leaveOpen: true);
            if (stream.Length < 44)
            {
                error = $"音频文件不是有效的 WAV（长度不足）：{path}";
                return false;
            }
            if (new string(reader.ReadChars(4)) != "RIFF")
            {
                error = $"音频文件不是有效的 RIFF/WAVE（可能是改名的 mp3 等）：{path}";
                return false;
            }
            reader.ReadUInt32();
            if (new string(reader.ReadChars(4)) != "WAVE")
            {
                error = $"音频文件不是有效的 RIFF/WAVE（可能是改名的 mp3 等）：{path}";
                return false;
            }

            bool hasFormat = false, hasData = false, pcm = false;
            int channels = 0, sampleRate = 0, bitsPerSample = 0;
            long dataBytes = 0;
            while (stream.Position + 8 <= stream.Length)
            {
                string chunk = new(reader.ReadChars(4));
                uint size = reader.ReadUInt32();
                long next = stream.Position + size + (size % 2);
                if (chunk == "fmt ")
                {
                    if (size < 16) break;
                    ushort format = reader.ReadUInt16();
                    channels = reader.ReadUInt16();
                    sampleRate = reader.ReadInt32();
                    reader.ReadInt32();
                    reader.ReadUInt16();
                    bitsPerSample = reader.ReadUInt16();
                    if (format == 0xFFFE && size >= 40)
                    {
                        // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with
                        // the actual tag; only the PCM tag is accepted.
                        reader.ReadUInt16();
                        reader.ReadUInt16();
                        reader.ReadUInt32();
                        pcm = reader.ReadUInt16() == 1;
                    }
                    else
                    {
                        pcm = format == 1;
                    }
                    hasFormat = true;
                }
                else if (chunk == "data")
                {
                    dataBytes = Math.Min(size, stream.Length - stream.Position);
                    hasData = true;
                }
                if (next > stream.Length) break;
                stream.Seek(next, SeekOrigin.Begin);
            }

            if (!hasFormat || channels <= 0 || sampleRate <= 0 || bitsPerSample <= 0)
            {
                error = $"音频文件缺少有效的 WAV 格式块：{path}";
                return false;
            }
            if (!pcm)
            {
                error = $"音频文件不是 PCM WAV（可能是改名的 mp3 等）：{path}";
                return false;
            }
            if (!hasData || dataBytes <= 0)
            {
                error = $"音频文件没有音频数据：{path}";
                return false;
            }

            int blockAlign = channels * (bitsPerSample / 8);
            if (blockAlign <= 0)
            {
                error = $"音频文件的采样格式无效：{path}";
                return false;
            }
            long frames = dataBytes / blockAlign;
            seconds = frames / (double)sampleRate;
            return true;
        }
        catch (Exception ex)
        {
            error = $"读取音频文件失败：{path} ({ex.Message})";
            return false;
        }
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
    private static string CommitTrack(ImportRequest request, string vmdPath, string morphPath, GkClipData clip,
        byte[] bytes, MorphReport report, float sourceFps, string cameraPath, bool cameraIsBaked, ImportTiming timing,
        ImportLog log, Action<int, string, string> progress)
    {
        string motionsRoot = Path.GetFullPath(request.MotionsRoot);
        Directory.CreateDirectory(motionsRoot);
        string commit = NewTrackDestination(request, vmdPath, out string title);
        EnsureNewTrackAvailable(commit);

        string parent = TransactionParent(request.TransactionRoot, motionsRoot);
        string stage = Path.Combine(parent, "gkmotion-cs-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(stage);

        try
        {
            using (var stream = new FileStream(Path.Combine(stage, "motion.gkclip"), FileMode.CreateNew, FileAccess.Write))
            {
                stream.Write(bytes, 0, bytes.Length);
            }

            using (var sha = SHA256.Create())
            {
                string clipDigest = Convert.ToHexString(sha.ComputeHash(bytes)).ToLowerInvariant();
                MorphTranslator.WriteReport(Path.Combine(stage, "motion.morph_report.json"), report, clipDigest);
            }

            File.Copy(vmdPath, Path.Combine(stage, "source.vmd"), overwrite: true);
            bool distinctMorph = !string.IsNullOrEmpty(morphPath) &&
                                 !string.Equals(morphPath, vmdPath, StringComparison.OrdinalIgnoreCase);
            if (distinctMorph) File.Copy(morphPath, Path.Combine(stage, "source_morph.vmd"), overwrite: true);

            string? wav = ResolveAudio(vmdPath, request.Audio, request.ClearAudio);
            if (wav != null) File.Copy(wav, Path.Combine(stage, "song.wav"), overwrite: true);

            string sourceMorph = request.DisableMorph
                ? string.Empty
                : (string.IsNullOrEmpty(morphPath) ? vmdPath : morphPath);
            WriteInfoJson(stage, title, vmdPath, clip, sourceFps, sourceMorph, false, string.Empty, null);

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
                        CameraRegistration.Register(samples, stage, cameraFps, cameraScale, mode, clip);
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
                WriteInfoJson(stage, title, vmdPath, clip, sourceFps,
                    sourceMorph, File.Exists(target), sourceCamera, cameraRegistration);
            }

            // Save inside the same directory transaction as all four sources.
            JsonObject info = ReadEditInfo(Path.Combine(stage, "info.json"));
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

    private static void WriteInfoJson(string stage, string title, string vmdPath, GkClipData clip, float sourceFps,
        string sourceMorph, bool hasCamera, string sourceCamera, CameraRegistrationMetadata? registration)
    {
        using var sha = SHA256.Create();
        string digest = Convert.ToHexString(sha.ComputeHash(File.ReadAllBytes(vmdPath))).ToLowerInvariant();

        var sb = new StringBuilder(640);
        sb.Append("{\n");
        sb.Append("  \"title\": ").Append(Quote(title)).Append(",\n");
        sb.Append("  \"duration_seconds\": ")
          .Append(clip.DurationSeconds.ToString("R", CultureInfo.InvariantCulture)).Append(",\n");
        sb.Append("  \"total_frames\": ").Append(clip.TotalFrames.ToString(CultureInfo.InvariantCulture)).Append(",\n");
        sb.Append("  \"bone_count\": ").Append(clip.Bones.Count.ToString(CultureInfo.InvariantCulture)).Append(",\n");
        // fps is the rate the stored frames advance at; source_fps is the rate of
        // the VMD tick timeline. They differ only for a resampled (non-source-clock)
        // body, and duration_seconds is always total_frames / fps.
        sb.Append("  \"fps\": ").Append(clip.Fps.ToString("R", CultureInfo.InvariantCulture)).Append(",\n");
        sb.Append("  \"source_vmd\": ").Append(Quote(vmdPath)).Append(",\n");
        sb.Append("  \"source_fps\": ").Append(sourceFps.ToString("R", CultureInfo.InvariantCulture)).Append(",\n");
        sb.Append("  \"source_vmd_sha256\": \"").Append(digest).Append("\",\n");
        sb.Append("  \"has_facial\": ").Append(clip.BlendshapeNames.Count > 0 ? "true" : "false").Append(",\n");
        sb.Append("  \"has_camera\": ").Append(hasCamera ? "true" : "false").Append(",\n");
        sb.Append("  \"source_morph_vmd\": ").Append(Quote(sourceMorph)).Append(",\n");
        sb.Append("  \"source_camera_vmd\": ").Append(Quote(sourceCamera)).Append(",\n");
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
