namespace GakuMotion.VmdImporter.Native;

using System.Diagnostics;
using System.Text.Json.Nodes;
using GakuMotion.VmdImporter.Native.Camera;

internal static partial class ImportPipeline
{
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
}
