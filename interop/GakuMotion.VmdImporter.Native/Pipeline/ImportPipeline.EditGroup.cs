namespace GakuMotion.VmdImporter.Native;

using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using GakuMotion.VmdImporter.Output;
using GakuMotion.VmdImporter.Rig;
using GakuMotion.VmdImporter.Vmd;

internal static partial class ImportPipeline
{
    private static JsonObject BuildPositionJsonObject(SlotMetadataEntry entry)
    {
        var obj = new JsonObject
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
            ["bones"] = entry.Bones,
            ["track_timing"] = entry.Timing.ToJson()
        };
        if (entry.SourceRootOrigin is Vector3 origin)
        {
            obj["source_root_origin"] = new JsonObject
            {
                ["x"] = CleanZero(origin.X),
                ["y"] = CleanZero(origin.Y),
                ["z"] = CleanZero(origin.Z)
            };
        }
        return obj;
    }

    private static Vector3? ReadSourceRootOrigin(JsonObject? metaObj)
    {
        if (metaObj?["source_root_origin"] is not JsonObject originObj)
            return null;
        if (!PerformanceTimingSettings.TryReadFiniteFloat(originObj["x"], out float x) ||
            !PerformanceTimingSettings.TryReadFiniteFloat(originObj["y"], out float y) ||
            !PerformanceTimingSettings.TryReadFiniteFloat(originObj["z"], out float z))
            return null;
        return new Vector3(CleanZero(x), CleanZero(y), CleanZero(z));
    }

    private static Vector3? TryReadSourceRootOriginFromVmd(string slotDir)
    {
        string sourceVmd = Path.Combine(slotDir, "source.vmd");
        if (!File.Exists(sourceVmd) || File.Exists(Path.Combine(slotDir, TargetGeometryName)))
            return null;
        try
        {
            VmdMotion motion = VmdReader.Parse(sourceVmd);
            if (motion.BoneKeyframes.Count == 0)
                return null;
            return new SourceEvaluator().Evaluate(motion, startFrame: 0, endFrame: 0).RootOrigin;
        }
        catch
        {
            return null;
        }
    }

    private static SlotMetadataEntry ReadSlotMetadataFromClip(string slotDir, int slot, JsonObject? metaObj,
        float x, float y, float z, float yaw, float defaultFps, SlotTimingSettings fallbackTiming)
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

        SlotTimingSettings timing = SlotTimingSettings.ReadFromInfoObject(metaObj, fallbackTiming);
        Vector3? sourceRootOrigin = ReadSourceRootOrigin(metaObj);

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
            OutputFps: outFps,
            Timing: timing,
            SourceRootOrigin: sourceRootOrigin);
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
        SlotTimingSettings? existingTopTiming = SlotTimingSettings.ReadOptionalFromInfoObject(info);
        SlotTimingSettings requestTopTiming = SlotTimingSettings.FromTopLevel(request.TrackTiming);
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
            SlotTimingSettings fallbackTiming = item.SourceSlot > 0
                ? (existingTopTiming ?? requestTopTiming)
                : requestTopTiming;
            SlotTimingSettings? existingSlotTiming = item.SourceSlot > 0
                ? (SlotTimingSettings.ReadOptionalFromInfoObject(item.ExistingMeta) ?? existingTopTiming)
                : null;
            SlotTimingSettings outTiming = pos.TrackTiming ?? existingSlotTiming ?? requestTopTiming;

            if (item.NeedsRebake)
            {
                anyRebaked = true;
                if (File.Exists(Path.Combine(slotDir, TargetGeometryName)))
                    throw new ImportStepException("unsupported_target_geometry",
                        $"槽位 {targetSlot} 含目标腿部几何；原生后端尚不支持保留该几何的重新烘焙");

                bool replaceBody = !string.IsNullOrEmpty(pos.MotionVmd);
                string body = replaceBody ? Path.GetFullPath(pos.MotionVmd) : Path.Combine(slotDir, "source.vmd");
                if (!File.Exists(body))
                    throw new InvalidDataException($"槽位 {targetSlot} 缺少原始动作备份 source.vmd，请在动作栏重新指定动作 .vmd 文件");

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
                    X: 0.0f,
                    Y: 0.0f,
                    Z: 0.0f,
                    YawDegrees: 0.0f,
                    HasMotion: true,
                    HasFacial: baked.Clip.BlendshapeNames.Count > 0,
                    SourceVmd: preservedSourceVmd,
                    SourceMorphVmd: preservedSourceMorphVmd,
                    SourceVmdSha256: baked.SourceVmdSha256,
                    SourceFps: baked.SourceFps,
                    Frames: baked.Clip.TotalFrames,
                    Bones: baked.Clip.Bones.Count,
                    DurationSeconds: baked.Clip.DurationSeconds,
                    OutputFps: baked.Clip.Fps,
                    Timing: outTiming,
                    SourceRootOrigin: baked.RootOrigin);
                metadataEntries.Add(entry);
            }
            else
            {
                SlotMetadataEntry entry = ReadSlotMetadataFromClip(
                    slotDir, targetSlot, item.ExistingMeta, 0.0f, 0.0f, 0.0f, 0.0f, item.ExistingOutputFps, fallbackTiming);
                if (pos.TrackTiming != null)
                    entry = entry with { Timing = pos.TrackTiming };
                metadataEntries.Add(entry);
            }

            SlotMetadataEntry last = metadataEntries[^1];
            if (last.Frames > maxFrames) maxFrames = last.Frames;
            if (last.Bones > maxBones) maxBones = last.Bones;
            if (last.DurationSeconds > maxDuration) maxDuration = last.DurationSeconds;
            if (last.HasFacial) anyFacial = true;
        }

        bool slot1ChoreographyChanged = plan[0].SourceSlot != 1 ||
            (plan[0].NeedsRebake && !string.IsNullOrEmpty(plan[0].Pos.MotionVmd));
        for (int i = 0; i < metadataEntries.Count; i++)
        {
            var item = plan[i];
            (float outX, float outY, float outZ, float outYaw) = ResolveEditedSlotPlacement(
                stage,
                item.TargetSlot,
                item.SourceSlot,
                item.NeedsRebake,
                slot1ChoreographyChanged,
                item.Pos,
                item.ExistingMeta,
                metadataEntries,
                i);
            metadataEntries[i] = metadataEntries[i] with
            {
                X = outX,
                Y = outY,
                Z = outZ,
                YawDegrees = outYaw
            };
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

        PerformanceTimingSettings topTimingBase = request.TrackTiming ?? PerformanceTimingSettings.ReadFromInfoObject(info);
        info["track_timing"] = topTimingBase.WithSlot1Timing(slot1.Timing).ToJson();
    }

    private static (float X, float Y, float Z, float Yaw) ResolveEditedSlotPlacement(
        string stage,
        int targetSlot,
        int sourceSlot,
        bool needsRebake,
        bool slot1ChoreographyChanged,
        PositionRequest pos,
        JsonObject? existingMeta,
        List<SlotMetadataEntry> entries,
        int index)
    {
        if (targetSlot == 1)
            return (0.0f, 0.0f, 0.0f, 0.0f);

        bool hasExplicitPlacement = pos.HasExplicitPlacement || pos.X != 0.0f || pos.Y != 0.0f || pos.Z != 0.0f;
        if (hasExplicitPlacement)
            return (CleanZero(pos.X), CleanZero(pos.Y), CleanZero(pos.Z), CleanZero(pos.YawDegrees));

        float resolvedYaw = pos.YawDegrees != 0.0f
            ? CleanZero(pos.YawDegrees)
            : ReadJsonFloat(existingMeta, "yaw_degrees", 0.0f);

        if (needsRebake)
        {
            Vector3 slotOrigin = EnsureSlotOrigin(stage, targetSlot, entries, index) ?? Vector3.Zero;
            Vector3 slot1Origin = EnsureSlotOrigin(stage, 1, entries, 0) ?? Vector3.Zero;
            (float dx, float dy, float dz) = ResolveRelativePlacementOffset(slotOrigin, slot1Origin);
            return (dx, dy, dz, CleanZero(pos.YawDegrees));
        }

        if (sourceSlot == 1 || slot1ChoreographyChanged)
        {
            Vector3? slotOriginOpt = EnsureSlotOrigin(stage, targetSlot, entries, index);
            Vector3? slot1OriginOpt = EnsureSlotOrigin(stage, 1, entries, 0);
            if (slotOriginOpt is Vector3 slotOrigin && slot1OriginOpt is Vector3 slot1Origin)
            {
                (float dx, float dy, float dz) = ResolveRelativePlacementOffset(slotOrigin, slot1Origin);
                return (dx, dy, dz, resolvedYaw);
            }
        }

        return (
            CleanZero(ReadJsonFloat(existingMeta, "x", 0.0f)),
            CleanZero(ReadJsonFloat(existingMeta, "y", 0.0f)),
            CleanZero(ReadJsonFloat(existingMeta, "z", 0.0f)),
            resolvedYaw);
    }

    private static Vector3? EnsureSlotOrigin(string stage, int slot, List<SlotMetadataEntry> entries, int index)
    {
        if (entries[index].SourceRootOrigin is Vector3 cached)
            return cached;
        Vector3? peeked = TryReadSourceRootOriginFromVmd(SlotDirectory(stage, slot));
        if (peeked is Vector3 resolved)
            entries[index] = entries[index] with { SourceRootOrigin = resolved };
        return peeked;
    }
}
