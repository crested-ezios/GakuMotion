namespace GakuMotion.VmdImporter.Native;

using System.Diagnostics;
using System.Globalization;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using GakuMotion.VmdImporter.Native.Camera;
using GakuMotion.VmdImporter.Vmd;

internal static partial class ImportPipeline
{
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

    private static (float X, float Y, float Z) ResolveRelativePlacementOffset(Vector3 slotOrigin, Vector3 slot1Origin)
    {
        Vector3 delta = slotOrigin - slot1Origin;
        float dx = CleanZero(delta.X);
        float dz = CleanZero(delta.Z);
        float dy = MathF.Abs(delta.Y) < 0.02f ? 0.0f : CleanZero(delta.Y);
        return (dx, dy, dz);
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
            Vector3 slot1Origin = slot1Baked.RootOrigin;
            for (int i = 0; i < slots.Count; i++)
            {
                SlotSpec spec = slots[i];
                BakedSlotResult baked = bakedSlots[i];
                string slotDir = SlotDirectory(stage, spec.Slot);
                WriteBakedSlotFiles(slotDir, baked, spec.DisableMorph);

                float outX, outY, outZ, outYaw;
                if (spec.Slot == 1)
                {
                    outX = 0.0f;
                    outY = 0.0f;
                    outZ = 0.0f;
                    outYaw = 0.0f;
                }
                else
                {
                    if (spec.HasExplicitPlacement)
                    {
                        outX = CleanZero(spec.X);
                        outY = CleanZero(spec.Y);
                        outZ = CleanZero(spec.Z);
                    }
                    else
                    {
                        (outX, outY, outZ) = ResolveRelativePlacementOffset(baked.RootOrigin, slot1Origin);
                    }
                    outYaw = CleanZero(spec.YawDegrees);
                }

                string slotSourceMorph = spec.DisableMorph
                    ? string.Empty
                    : (string.IsNullOrEmpty(baked.MorphPath) ? baked.VmdPath : baked.MorphPath);
                slotMeta.Add(new SlotMetadataEntry(
                    Slot: spec.Slot,
                    X: outX,
                    Y: outY,
                    Z: outZ,
                    YawDegrees: outYaw,
                    HasMotion: true,
                    HasFacial: baked.Clip.BlendshapeNames.Count > 0,
                    SourceVmd: baked.VmdPath,
                    SourceMorphVmd: slotSourceMorph,
                    SourceVmdSha256: baked.SourceVmdSha256,
                    SourceFps: baked.SourceFps,
                    Frames: baked.Clip.TotalFrames,
                    Bones: baked.Clip.Bones.Count,
                    DurationSeconds: baked.Clip.DurationSeconds,
                    OutputFps: baked.Clip.Fps,
                    Timing: spec.Timing,
                    SourceRootOrigin: baked.RootOrigin));
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
            PerformanceTimingSettings topTiming =
                (request.TrackTiming ?? new PerformanceTimingSettings()).WithSlot1Timing(slotMeta[0].Timing);
            info["track_timing"] = topTiming.ToJson();
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
            sb.Append("      \"bones\": ").Append(s.Bones.ToString(CultureInfo.InvariantCulture)).Append(",\n");
            sb.Append("      \"track_timing\": {\n");
            sb.Append("        \"motion\": { \"speed\": ")
              .Append(CleanZero(s.Timing.Motion.Speed).ToString("R", CultureInfo.InvariantCulture))
              .Append(", \"offset_seconds\": ")
              .Append(CleanZero(s.Timing.Motion.OffsetSeconds).ToString("R", CultureInfo.InvariantCulture))
              .Append(" },\n");
            sb.Append("        \"facial\": { \"speed\": ")
              .Append(CleanZero(s.Timing.Facial.Speed).ToString("R", CultureInfo.InvariantCulture))
              .Append(", \"offset_seconds\": ")
              .Append(CleanZero(s.Timing.Facial.OffsetSeconds).ToString("R", CultureInfo.InvariantCulture))
              .Append(" }\n");
            if (s.SourceRootOrigin is Vector3 origin)
            {
                sb.Append("      },\n");
                sb.Append("      \"source_root_origin\": { \"x\": ")
                  .Append(CleanZero(origin.X).ToString("R", CultureInfo.InvariantCulture))
                  .Append(", \"y\": ")
                  .Append(CleanZero(origin.Y).ToString("R", CultureInfo.InvariantCulture))
                  .Append(", \"z\": ")
                  .Append(CleanZero(origin.Z).ToString("R", CultureInfo.InvariantCulture))
                  .Append(" }\n");
            }
            else
            {
                sb.Append("      }\n");
            }
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
