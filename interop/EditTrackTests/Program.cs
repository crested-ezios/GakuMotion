namespace GakuMotion.EditTrackTests;

using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using GakuMotion.TimebaseTests;
using GakuMotion.VmdImporter.Native;
using GakuMotion.VmdImporter.Output;
using GakuMotion.VmdImporter.Vmd;

internal static unsafe class Program
{
    private static delegate* unmanaged<byte*, byte*, int, int> _run;
    private static delegate* unmanaged<byte*, byte*, int, int> _runCamera;
    private static string _root = string.Empty;
    private static int _cases, _failed;

    private static int Main(string[] args)
    {
        nint native = 0;
        if (args.Length == 2 && args[0] == "--native-dll")
        {
            native = NativeLibrary.Load(Path.GetFullPath(args[1]));
            _run = (delegate* unmanaged<byte*, byte*, int, int>)NativeLibrary.GetExport(native, "GkVmdImport_Run");
            _runCamera = (delegate* unmanaged<byte*, byte*, int, int>)NativeLibrary.GetExport(native, "GkCameraImport_Run");
        }
        else
        {
            _run = &NativeExports.Run;
            _runCamera = &NativeExports.RunCamera;
        }
        _root = Fixtures.NewTempRoot("edit-track");
        try
        {
            Case("body clock detection ignores a late facial tail", () =>
            {
                string dir = Path.Combine(_root, "clock-detection");
                string source = Fixtures.WriteSyntheticVmd(dir, frames: 61);
                string wav = Path.Combine(dir, "two.wav");
                using (var w = new BinaryWriter(File.Create(wav))) {
                    const int rate = 8000, length = rate * 2 * 2;
                    w.Write(Encoding.ASCII.GetBytes("RIFF")); w.Write(36 + length); w.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
                    w.Write(16); w.Write((short)1); w.Write((short)1); w.Write(rate); w.Write(rate * 2);
                    w.Write((short)2); w.Write((short)16); w.Write(Encoding.ASCII.GetBytes("data")); w.Write(length); w.Write(new byte[length]);
                }
                var parsed = VmdReader.Parse(source);
                var longTail = new VmdMotion { Version = parsed.Version, ModelName = parsed.ModelName,
                    BoneKeyframes = parsed.BoneKeyframes, MorphKeyframes = parsed.MorphKeyframes,
                    CameraKeyframes = parsed.CameraKeyframes, PropertyKeyframes = parsed.PropertyKeyframes, MaxFrame = 120 };
                Require(ImportPipeline.DetectSourceFps(source, longTail, wav) == 30, "facial tail changed body clock");
                string sixty = Fixtures.WriteSyntheticVmd(Path.Combine(dir, "sixty"), frames: 121);
                Require(ImportPipeline.DetectSourceFps(sixty, VmdReader.Parse(sixty), wav) == 60, "60 fps body was not detected");
                Require(ImportPipeline.DetectSourceFps(source, longTail, null) == 30, "missing audio should use the default clock");
            });
            Case("explicit source clock rebakes without changing output sampling semantics", () =>
            {
                string track = Track();
                File.Delete(Path.Combine(track, "source_camera.vmd")); // This fixture's placeholder is not a VMD.
                var result = Edit(track, ("source_fps", 60), ("fps", 30));
                Require(result.Ok, "source override failed: " + result.Error);
                Require(Info(track)["source_fps"]!.GetValue<float>() == 60 && Info(track)["fps"]!.GetValue<float>() == 30,
                    "source and output clocks were not stored independently");
                var before = Snapshot(track);
                Require(!Edit(track, ("source_fps", 42)).Ok, "unsupported source clock accepted");
                var after = Snapshot(track);
                foreach (string path in before.Keys) Require(before[path] == after[path], "invalid clock changed " + path);
            });
            Case("edit-only JSON permits omitted body/library fields", () =>
            {
                string track = Track();
                Result result = Edit(track, ("title", "  新标题  "));
                Require(result.Ok && result.Directory == track, result.Error);
                Require(Info(track)["title"]!.GetValue<string>() == "新标题", "title was not trimmed");
            });
            Case("metadata edit preserves body/source/camera/audio and custom metadata", () =>
            {
                string track = Track();
                var before = Snapshot(track);
                Require(Edit(track, ("title", "Changed"), ("fps", 60)).Ok, "metadata edit failed");
                var after = Snapshot(track);
                foreach (string path in before.Keys.Where(p => p != "info.json")) Require(before[path] == after[path], path);
                Require(Info(track)["custom"]!["value"]!.GetValue<int>() == 17, "custom metadata lost");
                Require(Info(track)["fps"]!.GetValue<float>() == 30, "fps-only edit must not rebake the body");
            });
            Case("clear flags take precedence and remove both camera files", () =>
            {
                string track = Track();
                string body = Digest(Path.Combine(track, "motion.gkclip"));
                Require(Edit(track, ("clear_audio", true), ("audio", "missing.wav"),
                    ("clear_camera", true), ("camera_vmd", "missing.vmd")).Ok, "clear edit failed");
                Require(!File.Exists(Path.Combine(track, "song.wav")), "audio not cleared");
                Require(!File.Exists(Path.Combine(track, "camera.gkcamera")), "camera not cleared");
                Require(!File.Exists(Path.Combine(track, "source_camera.vmd")), "camera source not cleared");
                Require(!Info(track)["has_camera"]!.GetValue<bool>(), "camera badge stale");
                Require(body == Digest(Path.Combine(track, "motion.gkclip")), "body changed");
            });
            Case("per-file timing edits preserve source payload without requiring VMD backups", () =>
            {
                string track = Track();
                File.Delete(Path.Combine(track, "source.vmd"));
                var before = Snapshot(track);
                var settings = new JsonObject
                {
                    ["motion"] = new JsonObject { ["speed"] = 0.5f, ["offset_seconds"] = 2.0f },
                    ["facial"] = new JsonObject { ["speed"] = 1.5f, ["offset_seconds"] = -1.0f },
                    ["camera"] = new JsonObject { ["speed"] = 0.0f, ["offset_seconds"] = 0.25f },
                    ["audio"] = new JsonObject { ["speed"] = 2.0f, ["offset_seconds"] = -0.5f }
                };
                Require(RunRequest(new JsonObject { ["edit_dir"] = track, ["track_timing"] = settings }).Ok,
                    "timing-only edit failed");
                Require(JsonNode.DeepEquals(Info(track)["track_timing"], settings), "timing values differ after edit");
                Require(Info(track)["custom"]!["value"]!.GetValue<int>() == 17, "custom metadata lost");
                var after = Snapshot(track);
                foreach (string path in before.Keys.Where(p => p != "info.json")) Require(before[path] == after[path], path);
                Require(Edit(track, ("title", "Legacy metadata caller")).Ok, "later title edit failed");
                Require(JsonNode.DeepEquals(Info(track)["track_timing"], settings), "legacy edit erased timing metadata");
            });
            Case("invalid timing fails atomically before changing source or metadata", () =>
            {
                string track = Track();
                var before = Snapshot(track);
                foreach (JsonNode? invalid in new JsonNode?[]
                {
                    JsonValue.Create("bad"),
                    new JsonObject { ["audio"] = new JsonObject { ["speed"] = -0.1f } },
                    new JsonObject { ["motion"] = new JsonObject { ["offset_seconds"] = "bad" } }
                })
                {
                    Require(!RunRequest(new JsonObject { ["edit_dir"] = track, ["track_timing"] = invalid }).Ok,
                        "invalid timing accepted");
                    Same(before, Snapshot(track));
                }
            });
            Case("new import persists independent timing settings inside its transaction", () =>
            {
                string source = Fixtures.WriteSyntheticVmd(Path.Combine(_root, "timed-new-source"), frames: 11);
                var settings = new JsonObject
                {
                    ["motion"] = new JsonObject { ["speed"] = 0.0f, ["offset_seconds"] = -2.0f },
                    ["audio"] = new JsonObject { ["speed"] = 1.2f, ["offset_seconds"] = 3.0f }
                };
                Result result = RunRequest(new JsonObject
                {
                    ["motion_vmd"] = source, ["motions_root"] = Path.Combine(_root, "timed-library"),
                    ["title"] = "Timing", ["clear_audio"] = true, ["track_timing"] = settings
                });
                Require(result.Ok, result.Error);
                JsonObject info = Info(result.Directory);
                Require(info["track_timing"]!["motion"]!["speed"]!.GetValue<float>() == 0,
                    "zero speed was not preserved");
                Require(info["track_timing"]!["audio"]!["offset_seconds"]!.GetValue<float>() == 3,
                    "positive offset was not preserved");
                Require(info["track_timing"]!["facial"]!["speed"]!.GetValue<float>() == 1,
                    "missing track did not get default speed");
            });
            Case("WAV replacement changes only audio and metadata", () =>
            {
                string track = Track(), audio = Path.Combine(_root, "replacement.wav");
                Wav(audio, sample: 123);
                string body = Digest(Path.Combine(track, "motion.gkclip"));
                Require(Edit(track, ("audio", audio)).Ok, "audio edit failed");
                Require(Digest(audio) == Digest(Path.Combine(track, "song.wav")), "audio differs");
                Require(body == Digest(Path.Combine(track, "motion.gkclip")), "body changed");
            });
            Case("invalid audio fails atomically", () =>
            {
                string track = Track(), audio = Path.Combine(_root, "bad.wav");
                File.WriteAllText(audio, "not wave");
                var before = Snapshot(track);
                Require(!Edit(track, ("title", "Must not commit"), ("audio", audio)).Ok, "bad audio accepted");
                Same(before, Snapshot(track));
            });
            Case("prebaked camera replaces camera and clears old VMD provenance", () =>
            {
                string track = Track(), source = Path.Combine(_root, "replacement.gkcamera");
                Camera(source, x: 4);
                string body = Digest(Path.Combine(track, "motion.gkclip"));
                Require(Edit(track, ("camera_vmd", source)).Ok, "camera edit failed");
                Require(Digest(source) == Digest(Path.Combine(track, "camera.gkcamera")), "camera differs");
                Require(!File.Exists(Path.Combine(track, "source_camera.vmd")), "old camera VMD kept");
                Require(Info(track)["source_camera_vmd"]!.GetValue<string>() == source, "camera source metadata wrong");
                Require(body == Digest(Path.Combine(track, "motion.gkclip")), "body changed");
            });
            Case("invalid camera and body inputs fail atomically", () =>
            {
                string track = Track(), bad = Path.Combine(_root, "bad.vmd");
                File.WriteAllText(bad, "invalid");
                var before = Snapshot(track);
                Require(!Edit(track, ("camera_vmd", bad)).Ok, "invalid camera accepted");
                Same(before, Snapshot(track));
                Require(!Edit(track, ("motion_vmd", bad)).Ok, "invalid body accepted");
                Same(before, Snapshot(track));
            });
            Case("body replacement preserves unrelated payload and retimes at requested fps", () =>
            {
                string track = Track(), input = Path.Combine(_root, "new-body");
                string vmd = Fixtures.WriteSyntheticVmd(input, frames: 11);
                File.Delete(Path.Combine(track, "song.wav"));
                Require(Edit(track, ("motion_vmd", vmd), ("fps", 60), ("title", "New body")).Ok, "body edit failed");
                GkClipData clip = GkClipDecoder.Decode(File.ReadAllBytes(Path.Combine(track, "motion.gkclip")));
                Require(clip.Fps == 60 && clip.TotalFrames == 22, "body clock mismatch");
                Require(File.ReadAllText(Path.Combine(track, "custom", "keep.bin")) == "keep", "custom file lost");
                Require(Info(track)["custom"]!["value"]!.GetValue<int>() == 17, "custom metadata lost");
                Require(Info(track)["source_vmd"]!.GetValue<string>() == vmd, "source provenance wrong");
            });
            Case("standalone eye/morph input is backed up and kept on a later body edit", () =>
            {
                string track = Track();
                string morph = Fixtures.WriteSyntheticVmd(Path.Combine(_root, "morph"), frames: 5);
                Require(Edit(track, ("morph_vmd", morph)).Ok, "morph replacement failed");
                string hash = Digest(Path.Combine(track, "source_morph.vmd"));
                Require(hash == Digest(morph), "morph backup differs");
                Require(Info(track)["source_morph_vmd"]!.GetValue<string>() == morph, "morph provenance wrong");
                string body = Fixtures.WriteSyntheticVmd(Path.Combine(_root, "body-after-morph"), frames: 7);
                Require(Edit(track, ("motion_vmd", body)).Ok, "body replacement failed");
                Require(hash == Digest(Path.Combine(track, "source_morph.vmd")), "existing standalone morph discarded");
                Require(Info(track)["source_morph_vmd"]!.GetValue<string>() == morph, "morph provenance changed to staging path");
            });
            Case("disable morph rebakes source backup and preserves original source identity", () =>
            {
                string track = Track(), source = Info(track)["source_vmd"]!.GetValue<string>();
                Require(Edit(track, ("disable_morph", true)).Ok, "disable morph failed");
                Require(!File.Exists(Path.Combine(track, "source_morph.vmd")), "morph backup not removed");
                Require(!Info(track)["has_facial"]!.GetValue<bool>(), "facial badge still set");
                Require(Info(track)["source_morph_vmd"]!.GetValue<string>() == "", "morph source metadata stale");
                Require(Info(track)["source_vmd"]!.GetValue<string>() == source, "source identity replaced by temporary path");
            });
            Case("missing source backup and geometry-dependent rebake fail atomically", () =>
            {
                string track = Track();
                File.Delete(Path.Combine(track, "source.vmd"));
                var before = Snapshot(track);
                Require(!Edit(track, ("disable_morph", true)).Ok, "missing backup accepted");
                Same(before, Snapshot(track));
                string geo = Track();
                File.WriteAllText(Path.Combine(geo, "target_leg_geometry.json"), "{}");
                var original = Snapshot(geo);
                Require(!Edit(geo, ("disable_morph", true)).Ok, "geometry silently discarded");
                Same(original, Snapshot(geo));
                Require(Edit(geo, ("title", "Geometry metadata edit")).Ok, "geometry must not block metadata-only edit");
            });
            Case("directory move failure keeps original track intact", () =>
            {
                string track = Track();
                var before = Snapshot(track);
                using (var held = new FileStream(Path.Combine(track, "motion.gkclip"), FileMode.Open, FileAccess.Read, FileShare.Read))
                    Require(!Edit(track, ("title", "Must not commit")).Ok, "locked track unexpectedly committed");
                Same(before, Snapshot(track));
            });
            Case("add rejects existing normalized names before parsing and preserves track", () =>
            {
                string track = Track(), invalid = Path.Combine(_root, "not-parsed.vmd");
                File.WriteAllText(invalid, "invalid VMD: collision must be reported before parsing");
                var before = Snapshot(track);
                foreach (string title in new[] { "Existing", " Existing... ", "existing" })
                {
                    Result result = RunRequest(new JsonObject
                    {
                        ["motion_vmd"] = invalid, ["motions_root"] = Path.GetDirectoryName(track), ["title"] = title
                    });
                    Require(!result.Ok && result.Error.Contains("管理编辑"), "missing useful name-conflict error: " + result.Error);
                    Same(before, Snapshot(track));
                }
            });
            Case("add rejects illegal-character and truncated-name collisions", () =>
            {
                string seed = Path.Combine(_root, "name-collisions"), library = Path.Combine(seed, "motions");
                string source = Fixtures.WriteSyntheticVmd(seed, frames: 3);
                foreach (var (title, folder) in new[] { ("Collision?", "Collision_"), (new string('X', 101), new string('X', 100)) })
                {
                    string existing = Path.Combine(library, folder);
                    Directory.CreateDirectory(existing);
                    File.WriteAllText(Path.Combine(existing, "keep.txt"), "external data");
                    var before = Snapshot(existing);
                    Result result = RunRequest(new JsonObject
                    {
                        ["motion_vmd"] = source, ["motions_root"] = library, ["title"] = title
                    });
                    Require(!result.Ok && result.Error.Contains("管理编辑"), result.Error);
                    Same(before, Snapshot(existing));
                }
            });
            Case("new-track publication refuses a destination created during preparation", () =>
            {
                string parent = Path.Combine(_root, "late-add"), stage = Path.Combine(parent, "stage"), destination = Path.Combine(parent, "track");
                Directory.CreateDirectory(stage);
                File.WriteAllText(Path.Combine(stage, "new.txt"), "new payload");
                Directory.CreateDirectory(destination);
                File.WriteAllText(Path.Combine(destination, "keep.txt"), "external payload");
                var before = Snapshot(destination);
                ExpectConflict(() => ImportPipeline.PublishNewTrack(stage, destination), "管理编辑");
                Same(before, Snapshot(destination));
                Require(File.Exists(Path.Combine(stage, "new.txt")), "staging was moved over existing track");
                string fileDestination = Path.Combine(parent, "file-track");
                File.WriteAllText(fileDestination, "keep file");
                ExpectConflict(() => ImportPipeline.PublishNewTrack(stage, fileDestination), "管理编辑");
                Require(File.ReadAllText(fileDestination) == "keep file", "existing file overwritten");
            });
            Case("edit publication rejects external content and membership changes", () =>
            {
                Action<string>[] changes =
                {
                    track =>
                    {
                        string path = Path.Combine(track, "custom", "keep.bin");
                        DateTime written = File.GetLastWriteTimeUtc(path);
                        File.WriteAllText(path, "KEEP"); // same byte length, restored timestamp
                        File.SetLastWriteTimeUtc(path, written);
                    },
                    track => File.WriteAllText(Path.Combine(track, "external.txt"), "external add"),
                    track => File.Delete(Path.Combine(track, "custom", "keep.bin")),
                    track => File.Move(Path.Combine(track, "custom", "keep.bin"), Path.Combine(track, "custom", "renamed.bin")),
                    track => Directory.CreateDirectory(Path.Combine(track, "external-empty-directory")),
                    track => File.SetLastWriteTimeUtc(Path.Combine(track, "custom", "keep.bin"), DateTime.UtcNow.AddMinutes(1))
                };
                foreach (Action<string> change in changes)
                {
                    string track = Track(), parent = Path.GetDirectoryName(track)!;
                    var original = ImportPipeline.CaptureEditSignature(track);
                    string stage = Path.Combine(parent, "private-stage"), backup = Path.Combine(parent, "private-backup");
                    Directory.CreateDirectory(stage);
                    File.WriteAllText(Path.Combine(stage, "edited.txt"), "staged edit");
                    change(track);
                    var external = Snapshot(track);
                    ExpectConflict(() => ImportPipeline.PublishEditedTrack(stage, track, backup, original), "外部修改");
                    Same(external, Snapshot(track));
                    Require(Directory.Exists(stage) && !Directory.Exists(backup), "conflict moved original or staging directory");
                }
            });
            Case("edit publication does not recreate an externally removed track", () =>
            {
                string track = Track(), parent = Path.GetDirectoryName(track)!;
                var original = ImportPipeline.CaptureEditSignature(track);
                string stage = Path.Combine(parent, "removed-stage"), backup = Path.Combine(parent, "removed-backup");
                Directory.CreateDirectory(stage);
                Directory.Delete(track, recursive: true);
                ExpectConflict(() => ImportPipeline.PublishEditedTrack(stage, track, backup, original), "外部修改");
                Require(!Directory.Exists(track) && Directory.Exists(stage), "removed track was recreated");
            });
            Case("no private edit stages remain in the scanned library", () =>
            {
                Require(!Directory.EnumerateDirectories(_root, "gkmotion-cs-edit-*", SearchOption.AllDirectories).Any(), "edit staging leaked");
            });
            Case("managed scratch edits preserve full payload and clean completed transaction", () =>
            {
                string track = Track(), scratch = Path.Combine(_root, "managed-temp", "transactions");
                Require(Edit(track, ("title", "Managed scratch"), ("transaction_root", scratch)).Ok, "managed scratch edit failed");
                Require(File.ReadAllText(Path.Combine(track, "custom", "keep.bin")) == "keep", "custom payload changed");
                Require(Directory.Exists(scratch) && !Directory.EnumerateFileSystemEntries(scratch).Any(), "completed transaction was not cleaned");
            });
            Case("managed scratch rejects the scanned library before changing original", () =>
            {
                string track = Track(), library = Path.GetDirectoryName(track)!;
                var before = Snapshot(track);
                Require(!Edit(track, ("title", "Must not apply"), ("transaction_root", library)).Ok, "scratch accepted library root");
                Same(before, Snapshot(track));
            });
            Case("imports MP3 with ID3v2 artist and cover art, auto-filling subtitle", () =>
            {
                string seed = Path.Combine(_root, "mp3-import"), library = Path.Combine(seed, "library");
                string vmd = Fixtures.WriteSyntheticVmd(seed, frames: 30);
                string mp3 = Path.Combine(seed, "stage.mp3");
                byte[] fakePng = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 1 };
                WriteSyntheticMp3WithId3(mp3, title: "Luna", artist: "Sakiko", coverMime: "image/png", coverBytes: fakePng, frameCount: 40);
                Result r = RunRequest(new JsonObject
                {
                    ["motion_vmd"] = vmd, ["motions_root"] = library, ["title"] = "Luna Track", ["audio"] = mp3
                });
                Require(r.Ok, "mp3 import failed: " + r.Error);
                Require(File.Exists(Path.Combine(r.Directory, "song.mp3")), "song.mp3 missing");
                Require(!File.Exists(Path.Combine(r.Directory, "song.wav")), "unexpected song.wav");
                Require(File.Exists(Path.Combine(r.Directory, "cover.png")), "cover.png missing");
                JsonObject meta = Info(r.Directory);
                Require(meta["audio_file"]?.GetValue<string>() == "song.mp3", "audio_file wrong");
                Require(meta["subtitle"]?.GetValue<string>() == "Sakiko", "subtitle not auto-filled from artist");
                Require(meta["has_cover"]?.GetValue<bool>() == true && meta["cover_file"]?.GetValue<string>() == "cover.png", "cover metadata wrong");
                Require(meta["audio_duration"]?.GetValue<double>() > 0.5, "audio_duration missing");
            });
            Case("replaces WAV with FLAC cover and clear_audio removes both song.* and cover.*", () =>
            {
                string track = Track(), flac = Path.Combine(_root, "track.flac");
                byte[] fakeJpg = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 0x4A, 0x46, 0x49, 0x46, 0, 1 };
                WriteSyntheticFlac(flac, title: "Sora", artist: "Kotone", coverMime: "image/jpeg", coverBytes: fakeJpg, totalSamples: 44100);
                Require(Edit(track, ("audio", flac)).Ok, "flac replace failed");
                Require(!File.Exists(Path.Combine(track, "song.wav")), "old song.wav kept");
                Require(File.Exists(Path.Combine(track, "song.flac")), "song.flac missing");
                Require(File.Exists(Path.Combine(track, "cover.jpg")), "cover.jpg missing");
                JsonObject meta = Info(track);
                Require(meta["audio_file"]?.GetValue<string>() == "song.flac", "audio_file not updated");
                Require(meta["audio_artist"]?.GetValue<string>() == "Kotone", "audio_artist missing");
                Require(meta["has_cover"]?.GetValue<bool>() == true, "has_cover false");

                Require(Edit(track, ("clear_audio", true)).Ok, "clear_audio failed");
                Require(!File.Exists(Path.Combine(track, "song.flac")), "song.flac not cleared");
                Require(!File.Exists(Path.Combine(track, "cover.jpg")), "cover.jpg not cleared");
                JsonObject cleared = Info(track);
                Require(cleared["has_cover"]?.GetValue<bool>() == false && cleared["audio_file"] is null, "cleared metadata stale");
            });
            Case("rejects network streaming audio URLs", () =>
            {
                string track = Track();
                var before = Snapshot(track);
                Require(!Edit(track, ("audio", "https://example.com/stream.mp3")).Ok, "network audio accepted");
                Same(before, Snapshot(track));
            });
            Case("group song import writes slot 1 at root, slots 2+ under positions/slot_{k}, and shared camera/audio", () =>
            {
                string seed = Path.Combine(_root, "group-new"), library = Path.Combine(seed, "motions");
                string vmd1 = Fixtures.WriteSyntheticVmd(Path.Combine(seed, "s1"), frames: 5);
                string vmd2 = Fixtures.WriteSyntheticVmd(Path.Combine(seed, "s2"), frames: 12);
                string vmd3 = Fixtures.WriteSyntheticVmd(Path.Combine(seed, "s3"), frames: 8);
                string morph2 = Fixtures.WriteSyntheticVmd(Path.Combine(seed, "m2"), frames: 6);
                string wav = Path.Combine(seed, "shared.wav"); Wav(wav, sample: 77);
                string cam = Path.Combine(seed, "shared.gkcamera"); Camera(cam, x: 3);

                Result r = RunRequest(new JsonObject
                {
                    ["motions_root"] = library,
                    ["title"] = "GroupThree",
                    ["audio"] = wav,
                    ["camera_vmd"] = cam,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["motion_vmd"] = vmd1, ["disable_morph"] = true, ["x"] = 9.0f, ["yaw_degrees"] = 45.0f },
                        new JsonObject { ["slot"] = 2, ["motion_vmd"] = vmd2, ["morph_vmd"] = morph2, ["x"] = -1.25f, ["y"] = 0.5f, ["z"] = 2.0f, ["yaw_degrees"] = 30.0f },
                        new JsonObject { ["slot"] = 3, ["motion_vmd"] = vmd3, ["disable_morph"] = true, ["x"] = 1.25f, ["y"] = 0.0f, ["z"] = -0.5f, ["yaw_degrees"] = -15.0f }
                    }
                });
                Require(r.Ok, "group import failed: " + r.Error);
                string track = r.Directory;
                Require(File.Exists(Path.Combine(track, "motion.gkclip")), "slot 1 clip missing");
                Require(File.Exists(Path.Combine(track, "source.vmd")), "slot 1 source.vmd missing");
                Require(File.Exists(Path.Combine(track, "camera.gkcamera")), "shared camera missing");
                Require(File.Exists(Path.Combine(track, "song.wav")), "shared audio missing");
                foreach (int k in new[] { 2, 3 })
                {
                    string slotDir = Path.Combine(track, "positions", $"slot_{k}");
                    Require(File.Exists(Path.Combine(slotDir, "motion.gkclip")), $"slot {k} clip missing");
                    Require(File.Exists(Path.Combine(slotDir, "source.vmd")), $"slot {k} source.vmd missing");
                    Require(File.Exists(Path.Combine(slotDir, "motion.morph_report.json")), $"slot {k} morph report missing");
                    Require(!File.Exists(Path.Combine(slotDir, "camera.gkcamera")), $"slot {k} must not duplicate camera");
                    Require(!File.Exists(Path.Combine(slotDir, "song.wav")), $"slot {k} must not duplicate audio");
                }
                Require(File.Exists(Path.Combine(track, "positions", "slot_2", "source_morph.vmd")), "slot 2 source_morph.vmd missing");

                JsonObject meta = Info(track);
                Require(meta["is_group"]!.GetValue<bool>(), "is_group should be true");
                Require(meta["position_count"]!.GetValue<int>() == 3, "position_count != 3");
                Require(meta["total_frames"]!.GetValue<int>() == 12, "top-level total_frames should be max across slots (12)");
                Require(meta["source_vmd"]!.GetValue<string>() == Path.GetFullPath(vmd1), "top-level source_vmd must stay slot 1");

                var positions = (JsonArray)meta["positions"]!;
                Require(positions.Count == 3, "positions array length != 3");
                Require(positions[0]!["x"]!.GetValue<float>() == 0.0f && positions[0]!["yaw_degrees"]!.GetValue<float>() == 0.0f,
                    "slot 1 placement must be normalized to 0,0,0,0");
                Require(Math.Abs(positions[1]!["x"]!.GetValue<float>() - (-1.25f)) < 1e-4f &&
                        Math.Abs(positions[1]!["y"]!.GetValue<float>() - 0.5f) < 1e-4f &&
                        Math.Abs(positions[1]!["z"]!.GetValue<float>() - 2.0f) < 1e-4f &&
                        Math.Abs(positions[1]!["yaw_degrees"]!.GetValue<float>() - 30.0f) < 1e-4f,
                    "slot 2 placement mismatch");
                Require(positions[1]!["frames"]!.GetValue<int>() == 12, "slot 2 frames mismatch");
                Require(positions[2]!["frames"]!.GetValue<int>() == 8, "slot 3 frames mismatch");
                Require(!string.IsNullOrEmpty(positions[1]!["source_vmd_sha256"]!.GetValue<string>()), "slot 2 sha256 missing");
            });
            Case("group song initial_placement-only and metadata-only edit performs zero rebake", () =>
            {
                string track = GroupTrack(3);
                // Delete source.vmd in slots 1 and 2 to prove evaluator cannot run on placement-only edit.
                File.Delete(Path.Combine(track, "source.vmd"));
                File.Delete(Path.Combine(track, "positions", "slot_2", "source.vmd"));
                var before = Snapshot(track);

                Result r = RunRequest(new JsonObject
                {
                    ["edit_dir"] = track,
                    ["title"] = "GroupZeroRebake",
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["preserve_existing"] = true },
                        new JsonObject { ["slot"] = 2, ["preserve_existing"] = true, ["x"] = -2.5f, ["y"] = 0.25f, ["z"] = 1.5f, ["yaw_degrees"] = 45.0f },
                        new JsonObject { ["slot"] = 3, ["preserve_existing"] = true, ["x"] = 2.5f, ["y"] = 0.0f, ["z"] = -1.5f, ["yaw_degrees"] = -60.0f }
                    }
                });
                Require(r.Ok, "zero-rebake edit failed: " + r.Error);
                var after = Snapshot(track);
                foreach (string rel in before.Keys.Where(k => k != "info.json"))
                    Require(before[rel] == after[rel], "zero-rebake edit mutated " + rel);

                JsonObject meta = Info(track);
                Require(meta["title"]!.GetValue<string>() == "GroupZeroRebake", "title not updated");
                var pos = (JsonArray)meta["positions"]!;
                Require(Math.Abs(pos[1]!["x"]!.GetValue<float>() - (-2.5f)) < 1e-4f &&
                        Math.Abs(pos[1]!["yaw_degrees"]!.GetValue<float>() - 45.0f) < 1e-4f,
                    "slot 2 placement not updated");
                Require(Math.Abs(pos[2]!["x"]!.GetValue<float>() - 2.5f) < 1e-4f &&
                        Math.Abs(pos[2]!["yaw_degrees"]!.GetValue<float>() - (-60.0f)) < 1e-4f,
                    "slot 3 placement not updated");
            });
            Case("group song selective edit rebakes only modified slot and preserves unmodified slots & sidecars", () =>
            {
                string track = GroupTrack(3);
                foreach (int slot in new[] { 1, 2, 3 })
                {
                    string dir = slot == 1 ? track : Path.Combine(track, "positions", $"slot_{slot}");
                    File.WriteAllText(Path.Combine(dir, "bone_overrides.json"), $"{{\"slot\":{slot}}}");
                    File.WriteAllText(Path.Combine(dir, "root_height_calibration.json"), $"{{\"h\":{slot}}}");
                    File.WriteAllText(Path.Combine(dir, "aux_replay.json"), $"{{\"aux\":{slot}}}");
                }
                string clip1Before = Digest(Path.Combine(track, "motion.gkclip"));
                string src1Before = Digest(Path.Combine(track, "source.vmd"));
                string clip2Before = Digest(Path.Combine(track, "positions", "slot_2", "motion.gkclip"));
                string clip3Before = Digest(Path.Combine(track, "positions", "slot_3", "motion.gkclip"));
                string src3Before = Digest(Path.Combine(track, "positions", "slot_3", "source.vmd"));

                string newVmd2 = Fixtures.WriteSyntheticVmd(Path.Combine(_root, "sel-vmd2"), frames: 15);
                Result r = RunRequest(new JsonObject
                {
                    ["edit_dir"] = track,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["preserve_existing"] = true },
                        new JsonObject { ["slot"] = 2, ["motion_vmd"] = newVmd2, ["x"] = -1.0f, ["yaw_degrees"] = 12.0f },
                        new JsonObject { ["slot"] = 3, ["preserve_existing"] = true, ["x"] = 1.0f }
                    }
                });
                Require(r.Ok, "selective edit failed: " + r.Error);
                Require(clip1Before == Digest(Path.Combine(track, "motion.gkclip")), "slot 1 clip changed");
                Require(src1Before == Digest(Path.Combine(track, "source.vmd")), "slot 1 source.vmd changed");
                Require(clip3Before == Digest(Path.Combine(track, "positions", "slot_3", "motion.gkclip")), "slot 3 clip changed");
                Require(src3Before == Digest(Path.Combine(track, "positions", "slot_3", "source.vmd")), "slot 3 source.vmd changed");
                Require(clip2Before != Digest(Path.Combine(track, "positions", "slot_2", "motion.gkclip")), "slot 2 clip not rebaked");
                Require(Info(track)["total_frames"]!.GetValue<int>() == 15, "top-level total_frames not updated to 15");

                foreach (int slot in new[] { 1, 2, 3 })
                {
                    string dir = slot == 1 ? track : Path.Combine(track, "positions", $"slot_{slot}");
                    Require(File.ReadAllText(Path.Combine(dir, "bone_overrides.json")) == $"{{\"slot\":{slot}}}", $"slot {slot} bone_overrides lost");
                    Require(File.ReadAllText(Path.Combine(dir, "root_height_calibration.json")) == $"{{\"h\":{slot}}}", $"slot {slot} calibration lost");
                    Require(File.ReadAllText(Path.Combine(dir, "aux_replay.json")) == $"{{\"aux\":{slot}}}", $"slot {slot} aux_replay lost");
                }
            });
            Case("group song slot deletion renumbers positions/slot_{k} and shrinking to 1 clears is_group", () =>
            {
                string track = GroupTrack(3);
                string slot3Dir = Path.Combine(track, "positions", "slot_3");
                File.WriteAllText(Path.Combine(slot3Dir, "bone_overrides.json"), "{\"surviving\":3}");
                File.WriteAllText(Path.Combine(slot3Dir, "root_height_calibration.json"), "{\"cal\":3}");
                string clip3Before = Digest(Path.Combine(slot3Dir, "motion.gkclip"));
                string clip1Before = Digest(Path.Combine(track, "motion.gkclip"));

                // Delete slot 2 using source_slot renumbering (3 -> 2).
                Result r1 = RunRequest(new JsonObject
                {
                    ["edit_dir"] = track,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["source_slot"] = 1, ["preserve_existing"] = true },
                        new JsonObject { ["slot"] = 2, ["source_slot"] = 3, ["preserve_existing"] = true, ["x"] = 3.0f, ["yaw_degrees"] = 15.0f }
                    }
                });
                Require(r1.Ok, "slot delete failed: " + r1.Error);
                Require(!Directory.Exists(slot3Dir), "old positions/slot_3 should be removed");
                string slot2Dir = Path.Combine(track, "positions", "slot_2");
                Require(Directory.Exists(slot2Dir), "renumbered positions/slot_2 missing");
                Require(Digest(Path.Combine(slot2Dir, "motion.gkclip")) == clip3Before, "slot 3 clip did not move to slot 2");
                Require(Digest(Path.Combine(track, "motion.gkclip")) == clip1Before, "slot 1 clip mutated");
                Require(File.ReadAllText(Path.Combine(slot2Dir, "bone_overrides.json")) == "{\"surviving\":3}", "slot 3 bone_overrides lost");
                Require(File.ReadAllText(Path.Combine(slot2Dir, "root_height_calibration.json")) == "{\"cal\":3}", "slot 3 calibration lost");
                JsonObject meta1 = Info(track);
                Require(meta1["is_group"]!.GetValue<bool>() && meta1["position_count"]!.GetValue<int>() == 2, "group metadata wrong after delete");

                // Shrink from 2 slots to 1 slot -> removes positions/ and clears is_group.
                Result r2 = RunRequest(new JsonObject
                {
                    ["edit_dir"] = track,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["source_slot"] = 1, ["preserve_existing"] = true }
                    }
                });
                Require(r2.Ok, "shrink to 1 failed: " + r2.Error);
                Require(!Directory.Exists(Path.Combine(track, "positions")), "positions/ directory should be removed when shrunk to 1");
                Require(Digest(Path.Combine(track, "motion.gkclip")) == clip1Before, "slot 1 clip changed when shrinking");
                JsonObject meta2 = Info(track);
                Require(!meta2["is_group"]!.GetValue<bool>() && meta2["position_count"]!.GetValue<int>() == 1, "is_group/position_count wrong after shrink");
            });
            Case("group song edit appends new positions without rebaking existing slots", () =>
            {
                string seed = Path.Combine(_root, "append-seed"), library = Path.Combine(seed, "motions");
                string vmd1 = Fixtures.WriteSyntheticVmd(Path.Combine(seed, "s1"), frames: 5);
                Result initRes = RunRequest(new JsonObject { ["motion_vmd"] = vmd1, ["motions_root"] = library, ["title"] = "Expandable" });
                Require(initRes.Ok, initRes.Error);
                string track = initRes.Directory;
                string clip1Before = Digest(Path.Combine(track, "motion.gkclip"));

                string vmd2 = Fixtures.WriteSyntheticVmd(Path.Combine(seed, "s2"), frames: 11);
                Result append2 = RunRequest(new JsonObject
                {
                    ["edit_dir"] = track,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["preserve_existing"] = true },
                        new JsonObject { ["slot"] = 2, ["motion_vmd"] = vmd2, ["x"] = 1.5f, ["yaw_degrees"] = -20.0f }
                    }
                });
                Require(append2.Ok, "append slot 2 failed: " + append2.Error);
                Require(clip1Before == Digest(Path.Combine(track, "motion.gkclip")), "slot 1 rebaked on append");
                Require(File.Exists(Path.Combine(track, "positions", "slot_2", "motion.gkclip")), "slot 2 clip not created");
                JsonObject meta = Info(track);
                Require(meta["is_group"]!.GetValue<bool>() && meta["position_count"]!.GetValue<int>() == 2 &&
                        meta["total_frames"]!.GetValue<int>() == 11, "appended metadata mismatch");
            });
            Case("group song invalid inputs fail atomically without mutating original track", () =>
            {
                string track = GroupTrack(2);
                var before = Snapshot(track);

                // 1. Empty positions array
                Require(!RunRequest(new JsonObject { ["edit_dir"] = track, ["positions"] = new JsonArray() }).Ok, "empty positions accepted");
                Same(before, Snapshot(track));

                // 2. Non-finite placement coordinate (raw JSON text since JsonObject rejects NaN serialization depending on writer)
                Require(!RunRawJson($"{{\"edit_dir\":{JsonSerializer.Serialize(track)},\"positions\":[{{\"slot\":1,\"preserve_existing\":true}},{{\"slot\":2,\"preserve_existing\":true,\"x\":\"bad\"}}]}}").Ok,
                    "invalid x accepted");
                Same(before, Snapshot(track));

                // 3. Invalid slot source_fps (42)
                Require(!RunRequest(new JsonObject
                {
                    ["edit_dir"] = track,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["preserve_existing"] = true },
                        new JsonObject { ["slot"] = 2, ["preserve_existing"] = true, ["source_fps"] = 42 }
                    }
                }).Ok, "invalid slot source_fps accepted");
                Same(before, Snapshot(track));

                // 4. Duplicate slot numbers
                Require(!RunRequest(new JsonObject
                {
                    ["edit_dir"] = track,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["preserve_existing"] = true },
                        new JsonObject { ["slot"] = 1, ["preserve_existing"] = true }
                    }
                }).Ok, "duplicate slot accepted");
                Same(before, Snapshot(track));

                // 5. Missing source.vmd on slot-2 disable_morph rebake
                string trackNoSrc = GroupTrack(2);
                File.Delete(Path.Combine(trackNoSrc, "positions", "slot_2", "source.vmd"));
                var beforeNoSrc = Snapshot(trackNoSrc);
                Require(!RunRequest(new JsonObject
                {
                    ["edit_dir"] = trackNoSrc,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["preserve_existing"] = true },
                        new JsonObject { ["slot"] = 2, ["disable_morph"] = true }
                    }
                }).Ok, "missing slot 2 source.vmd accepted on disable_morph");
                Same(beforeNoSrc, Snapshot(trackNoSrc));

                // 6. Bound target_leg_geometry.json in positions/slot_2/ blocks rebake atomically
                string trackGeo = GroupTrack(2);
                File.WriteAllText(Path.Combine(trackGeo, "positions", "slot_2", "target_leg_geometry.json"), "{}");
                var beforeGeo = Snapshot(trackGeo);
                Require(!RunRequest(new JsonObject
                {
                    ["edit_dir"] = trackGeo,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["preserve_existing"] = true },
                        new JsonObject { ["slot"] = 2, ["disable_morph"] = true }
                    }
                }).Ok, "target_leg_geometry in slot 2 accepted on rebake");
                Same(beforeGeo, Snapshot(trackGeo));
            });
            Case("group song slot reorder with new VMD at vacated slot succeeds without false source_slot collision", () =>
            {
                // Case A: 2-slot [1, 2] -> [new->1, 1->2]
                string track2 = GroupTrack(2);
                File.WriteAllText(Path.Combine(track2, "bone_overrides.json"), "{\"from\":1}");
                File.WriteAllText(Path.Combine(track2, "root_height_calibration.json"), "{\"cal\":1}");
                string slot1ClipBefore = Digest(Path.Combine(track2, "motion.gkclip"));
                string newVmd1 = Fixtures.WriteSyntheticVmd(Path.Combine(_root, "reorder-new1"), frames: 18);

                Result rA = RunRequest(new JsonObject
                {
                    ["edit_dir"] = track2,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["motion_vmd"] = newVmd1, ["x"] = 9.0f, ["yaw_degrees"] = 45.0f },
                        new JsonObject { ["slot"] = 2, ["source_slot"] = 1, ["preserve_existing"] = true, ["x"] = -1.5f, ["yaw_degrees"] = 30.0f }
                    }
                });
                Require(rA.Ok, "reorder [1->2, new->1] failed: " + rA.Error);
                string slot2Dir = Path.Combine(track2, "positions", "slot_2");
                Require(Digest(Path.Combine(slot2Dir, "motion.gkclip")) == slot1ClipBefore, "old slot 1 clip did not move to slot 2");
                Require(File.ReadAllText(Path.Combine(slot2Dir, "bone_overrides.json")) == "{\"from\":1}", "old slot 1 bone_overrides lost");
                Require(File.ReadAllText(Path.Combine(slot2Dir, "root_height_calibration.json")) == "{\"cal\":1}", "old slot 1 calibration lost");
                JsonObject metaA = Info(track2);
                var posA = (JsonArray)metaA["positions"]!;
                Require(metaA["source_vmd"]!.GetValue<string>() == Path.GetFullPath(newVmd1), "new slot 1 source_vmd mismatch");
                Require(posA[0]!["x"]!.GetValue<float>() == 0.0f && posA[0]!["yaw_degrees"]!.GetValue<float>() == 0.0f, "new slot 1 placement not zeroed");
                Require(Math.Abs(posA[1]!["x"]!.GetValue<float>() - (-1.5f)) < 1e-4f, "moved slot 2 x mismatch");

                // Case B: 3-slot [1, 2, 3] -> delete slot 2, promote 3->2, place new VMD at 3
                string track3 = GroupTrack(3);
                string slot3DirBefore = Path.Combine(track3, "positions", "slot_3");
                File.WriteAllText(Path.Combine(slot3DirBefore, "bone_overrides.json"), "{\"from\":3}");
                string slot3ClipBefore = Digest(Path.Combine(slot3DirBefore, "motion.gkclip"));
                string track3Slot1ClipBefore = Digest(Path.Combine(track3, "motion.gkclip"));
                string newVmd3 = Fixtures.WriteSyntheticVmd(Path.Combine(_root, "reorder-new3"), frames: 22);

                Result rB = RunRequest(new JsonObject
                {
                    ["edit_dir"] = track3,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["source_slot"] = 1, ["preserve_existing"] = true },
                        new JsonObject { ["slot"] = 2, ["source_slot"] = 3, ["preserve_existing"] = true, ["x"] = 2.0f },
                        new JsonObject { ["slot"] = 3, ["motion_vmd"] = newVmd3, ["x"] = -2.0f }
                    }
                });
                Require(rB.Ok, "reorder [3->2, new->3] failed: " + rB.Error);
                Require(Digest(Path.Combine(track3, "motion.gkclip")) == track3Slot1ClipBefore, "slot 1 clip changed");
                string track3Slot2Dir = Path.Combine(track3, "positions", "slot_2");
                Require(Digest(Path.Combine(track3Slot2Dir, "motion.gkclip")) == slot3ClipBefore, "old slot 3 clip did not move to slot 2");
                Require(File.ReadAllText(Path.Combine(track3Slot2Dir, "bone_overrides.json")) == "{\"from\":3}", "old slot 3 bone_overrides lost");
                JsonObject metaB = Info(track3);
                var posB = (JsonArray)metaB["positions"]!;
                Require(posB[2]!["source_vmd"]!.GetValue<string>() == Path.GetFullPath(newVmd3), "new slot 3 source_vmd mismatch");
                Require(posB[2]!["frames"]!.GetValue<int>() == 22, "new slot 3 frames mismatch");
            });
            Case("replacing embedded-morph body VMD with morph-free body VMD clears source_morph_vmd", () =>
            {
                string seed = Path.Combine(_root, "morph-clear-provenance");
                string library = Path.Combine(seed, "motions");
                string embeddedVmd1 = WriteSyntheticVmdWithMorph(Path.Combine(seed, "e1.vmd"), frames: 6, "あ");
                string embeddedVmd2 = WriteSyntheticVmdWithMorph(Path.Combine(seed, "e2.vmd"), frames: 8, "あ");
                string cleanVmd1 = Fixtures.WriteSyntheticVmd(Path.Combine(seed, "clean1"), frames: 14);
                string cleanVmd2 = Fixtures.WriteSyntheticVmd(Path.Combine(seed, "clean2"), frames: 12);
                string wav = Path.Combine(seed, "song.wav"); Wav(wav, sample: 33);

                Result imp = RunRequest(new JsonObject
                {
                    ["motions_root"] = library,
                    ["title"] = "EmbeddedMorphReplace",
                    ["audio"] = wav,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["motion_vmd"] = embeddedVmd1 },
                        new JsonObject { ["slot"] = 2, ["motion_vmd"] = embeddedVmd2, ["x"] = 1.0f }
                    }
                });
                Require(imp.Ok, "seed import failed: " + imp.Error);
                string track = imp.Directory;
                JsonObject initMeta = Info(track);
                Require(initMeta["has_facial"]!.GetValue<bool>(), "seed should have facial");
                Require(initMeta["source_morph_vmd"]!.GetValue<string>() == Path.GetFullPath(embeddedVmd1), "slot 1 initial source_morph_vmd");
                var initPos = (JsonArray)initMeta["positions"]!;
                Require(initPos[1]!["source_morph_vmd"]!.GetValue<string>() == Path.GetFullPath(embeddedVmd2), "slot 2 initial source_morph_vmd");

                // 1. Replace slot 2 via positions[] with morph-free body VMD
                Result rSlot2 = RunRequest(new JsonObject
                {
                    ["edit_dir"] = track,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["preserve_existing"] = true },
                        new JsonObject { ["slot"] = 2, ["motion_vmd"] = cleanVmd2, ["x"] = 1.0f }
                    }
                });
                Require(rSlot2.Ok, "slot 2 body replace failed: " + rSlot2.Error);
                JsonObject metaAfterSlot2 = Info(track);
                var posAfterSlot2 = (JsonArray)metaAfterSlot2["positions"]!;
                Require(!posAfterSlot2[1]!["has_facial"]!.GetValue<bool>(), "slot 2 has_facial should be false");
                Require(posAfterSlot2[1]!["source_morph_vmd"]!.GetValue<string>() == "", "slot 2 source_morph_vmd must be cleared");
                Require(posAfterSlot2[0]!["has_facial"]!.GetValue<bool>() &&
                        posAfterSlot2[0]!["source_morph_vmd"]!.GetValue<string>() == Path.GetFullPath(embeddedVmd1),
                    "slot 1 provenance should stay intact");

                // 2. Replace slot 1 via legacy positions: null path with morph-free body VMD
                Result rSlot1 = RunRequest(new JsonObject
                {
                    ["edit_dir"] = track,
                    ["motion_vmd"] = cleanVmd1
                });
                Require(rSlot1.Ok, "legacy slot 1 body replace failed: " + rSlot1.Error);
                JsonObject metaAfterSlot1 = Info(track);
                var posAfterSlot1 = (JsonArray)metaAfterSlot1["positions"]!;
                Require(!metaAfterSlot1["has_facial"]!.GetValue<bool>(), "top-level has_facial should be false");
                Require(metaAfterSlot1["source_morph_vmd"]!.GetValue<string>() == "", "top-level source_morph_vmd must be cleared");
                Require(!posAfterSlot1[0]!["has_facial"]!.GetValue<bool>(), "positions[0] has_facial should be false");
                Require(posAfterSlot1[0]!["source_morph_vmd"]!.GetValue<string>() == "", "positions[0] source_morph_vmd must be cleared");
            });
            Case("rebake_output re-enables embedded morphs after disable_morph and restores original source_morph_vmd", () =>
            {
                string seed = Path.Combine(_root, "morph-reenable-provenance");
                string library = Path.Combine(seed, "motions");
                string embeddedVmd1 = WriteSyntheticVmdWithMorph(Path.Combine(seed, "emb1.vmd"), frames: 6, "あ");
                string embeddedVmd2 = WriteSyntheticVmdWithMorph(Path.Combine(seed, "emb2.vmd"), frames: 8, "あ");
                string wav = Path.Combine(seed, "song.wav"); Wav(wav, sample: 37);

                Result imp = RunRequest(new JsonObject
                {
                    ["motions_root"] = library,
                    ["title"] = "ReenableEmbeddedMorph",
                    ["audio"] = wav,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["motion_vmd"] = embeddedVmd1, ["disable_morph"] = true },
                        new JsonObject { ["slot"] = 2, ["motion_vmd"] = embeddedVmd2, ["disable_morph"] = true, ["x"] = 1.0f }
                    }
                });
                Require(imp.Ok, "seed import failed: " + imp.Error);
                string track = imp.Directory;
                JsonObject initMeta = Info(track);
                Require(!initMeta["has_facial"]!.GetValue<bool>(), "initial has_facial must be false");
                Require(initMeta["source_morph_vmd"]!.GetValue<string>() == "", "initial slot 1 source_morph_vmd must be empty");
                var initPos = (JsonArray)initMeta["positions"]!;
                Require(!initPos[1]!["has_facial"]!.GetValue<bool>(), "initial slot 2 has_facial must be false");
                Require(initPos[1]!["source_morph_vmd"]!.GetValue<string>() == "", "initial slot 2 source_morph_vmd must be empty");

                // 1. Re-enable slot 2 via positions[] rebake_output: true without replacing motion_vmd/morph_vmd
                Result rSlot2 = RunRequest(new JsonObject
                {
                    ["edit_dir"] = track,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["preserve_existing"] = true },
                        new JsonObject { ["slot"] = 2, ["rebake_output"] = true, ["x"] = 1.0f }
                    }
                });
                Require(rSlot2.Ok, "positions[] slot 2 rebake failed: " + rSlot2.Error);
                JsonObject metaAfterSlot2 = Info(track);
                var posAfterSlot2 = (JsonArray)metaAfterSlot2["positions"]!;
                Require(posAfterSlot2[1]!["has_facial"]!.GetValue<bool>(), "slot 2 has_facial must be true after rebake");
                Require(posAfterSlot2[1]!["source_morph_vmd"]!.GetValue<string>() == Path.GetFullPath(embeddedVmd2),
                    "slot 2 source_morph_vmd mismatch: " + posAfterSlot2[1]!["source_morph_vmd"]!.GetValue<string>());
                Require(!posAfterSlot2[0]!["has_facial"]!.GetValue<bool>() &&
                        posAfterSlot2[0]!["source_morph_vmd"]!.GetValue<string>() == "",
                    "slot 1 must remain disable_morph until rebaked");

                // 2. Re-enable slot 1 via legacy positions: null rebake_output: true without replacing motion_vmd/morph_vmd
                Result rSlot1 = RunRequest(new JsonObject
                {
                    ["edit_dir"] = track,
                    ["rebake_output"] = true
                });
                Require(rSlot1.Ok, "legacy positions:null rebake failed: " + rSlot1.Error);
                JsonObject metaAfterSlot1 = Info(track);
                var posAfterSlot1 = (JsonArray)metaAfterSlot1["positions"]!;
                Require(metaAfterSlot1["has_facial"]!.GetValue<bool>(), "top-level has_facial must be true after legacy rebake");
                Require(metaAfterSlot1["source_morph_vmd"]!.GetValue<string>() == Path.GetFullPath(embeddedVmd1),
                    "top-level source_morph_vmd mismatch: " + metaAfterSlot1["source_morph_vmd"]!.GetValue<string>());
                Require(posAfterSlot1[0]!["has_facial"]!.GetValue<bool>(), "positions[0] has_facial must be true");
                Require(posAfterSlot1[0]!["source_morph_vmd"]!.GetValue<string>() == Path.GetFullPath(embeddedVmd1),
                    "positions[0] source_morph_vmd mismatch: " + posAfterSlot1[0]!["source_morph_vmd"]!.GetValue<string>());

                // 3. Standalone solo song legacy disable_morph -> rebake_output: true
                Result soloImp = RunRequest(new JsonObject
                {
                    ["motions_root"] = library,
                    ["title"] = "SoloReenableEmbeddedMorph",
                    ["motion_vmd"] = embeddedVmd1,
                    ["disable_morph"] = true,
                    ["clear_audio"] = true
                });
                Require(soloImp.Ok, "solo seed import failed: " + soloImp.Error);
                Result soloRebake = RunRequest(new JsonObject
                {
                    ["edit_dir"] = soloImp.Directory,
                    ["rebake_output"] = true
                });
                Require(soloRebake.Ok, "solo legacy rebake failed: " + soloRebake.Error);
                JsonObject soloMeta = Info(soloImp.Directory);
                Require(soloMeta["has_facial"]!.GetValue<bool>(), "solo has_facial must be true after rebake");
                Require(soloMeta["source_morph_vmd"]!.GetValue<string>() == Path.GetFullPath(embeddedVmd1),
                    "solo source_morph_vmd mismatch: " + soloMeta["source_morph_vmd"]!.GetValue<string>());
            });
            Case("new group import backfills empty slot 1 motion_vmd from top-level motion_vmd", () =>
            {
                string seed = Path.Combine(_root, "backfill-slot1-motion");
                string library = Path.Combine(seed, "motions");
                string vmd1 = Fixtures.WriteSyntheticVmd(Path.Combine(seed, "s1"), frames: 7);
                string vmd2 = Fixtures.WriteSyntheticVmd(Path.Combine(seed, "s2"), frames: 9);
                string wav = Path.Combine(seed, "song.wav"); Wav(wav, sample: 41);

                Result imp = RunRequest(new JsonObject
                {
                    ["motions_root"] = library,
                    ["title"] = "BackfillSlot1",
                    ["motion_vmd"] = vmd1,
                    ["audio"] = wav,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["motion_vmd"] = "", ["disable_morph"] = true },
                        new JsonObject { ["slot"] = 2, ["motion_vmd"] = vmd2, ["x"] = 1.5f }
                    }
                });
                Require(imp.Ok, "group import with empty slot 1 motion_vmd failed: " + imp.Error);
                JsonObject meta = Info(imp.Directory);
                Require(meta["is_group"]!.GetValue<bool>() && meta["position_count"]!.GetValue<int>() == 2, "group metadata mismatch");
                Require(meta["source_vmd"]!.GetValue<string>() == Path.GetFullPath(vmd1), "top-level source_vmd mismatch");
                var pos = (JsonArray)meta["positions"]!;
                Require(pos[0]!["source_vmd"]!.GetValue<string>() == Path.GetFullPath(vmd1), "slot 1 source_vmd mismatch");
                Require(!pos[0]!["has_facial"]!.GetValue<bool>(), "slot 1 disable_morph flag was lost");
                Require(pos[1]!["source_vmd"]!.GetValue<string>() == Path.GetFullPath(vmd2), "slot 2 source_vmd mismatch");
            });
            Case("Bug1D-Legacy: rebake without replaceBody when source_vmd is empty or missing never leaks staging path", () =>
            {
                string seed = Path.Combine(_root, "bug1d-legacy");
                string library = Path.Combine(seed, "motions");
                string embeddedVmd = WriteSyntheticVmdWithMorph(Path.Combine(seed, "emb.vmd"), frames: 6, "あ");

                // 1. Empty source_vmd
                Result impEmpty = RunRequest(new JsonObject
                {
                    ["motions_root"] = library,
                    ["title"] = "Bug1DLegacyEmpty",
                    ["motion_vmd"] = embeddedVmd,
                    ["disable_morph"] = true,
                    ["clear_audio"] = true
                });
                Require(impEmpty.Ok, "seed import failed: " + impEmpty.Error);
                JsonObject metaEmpty = Info(impEmpty.Directory);
                metaEmpty["source_vmd"] = "";
                if (metaEmpty["positions"] is JsonArray arrEmpty && arrEmpty.Count > 0 && arrEmpty[0] is JsonObject p0Empty)
                    p0Empty["source_vmd"] = "";
                File.WriteAllText(Path.Combine(impEmpty.Directory, "info.json"), metaEmpty.ToJsonString());

                Result editEmpty = Edit(impEmpty.Directory, ("rebake_output", true));
                Require(editEmpty.Ok, "Bug1D-Legacy empty rebake failed: " + editEmpty.Error);
                JsonObject afterEmpty = Info(impEmpty.Directory);
                string srcEmpty = afterEmpty["source_vmd"]?.GetValue<string>() ?? "";
                string morphEmpty = afterEmpty["source_morph_vmd"]?.GetValue<string>() ?? "";
                Require(afterEmpty["has_facial"]!.GetValue<bool>(), "Bug1D-Legacy empty did not re-enable facial");
                Require(!srcEmpty.Contains("gkmotion-cs-edit-", StringComparison.OrdinalIgnoreCase), "staging path leaked in source_vmd: " + srcEmpty);
                Require(!morphEmpty.Contains("gkmotion-cs-edit-", StringComparison.OrdinalIgnoreCase), "staging path leaked in source_morph_vmd: " + morphEmpty);

                // 2. Missing source_vmd
                Result impMissing = RunRequest(new JsonObject
                {
                    ["motions_root"] = library,
                    ["title"] = "Bug1DLegacyMissing",
                    ["motion_vmd"] = embeddedVmd,
                    ["disable_morph"] = true,
                    ["clear_audio"] = true
                });
                Require(impMissing.Ok, "seed import (missing) failed: " + impMissing.Error);
                JsonObject metaMissing = Info(impMissing.Directory);
                metaMissing.Remove("source_vmd");
                if (metaMissing["positions"] is JsonArray arrMissing && arrMissing.Count > 0 && arrMissing[0] is JsonObject p0Missing)
                    p0Missing.Remove("source_vmd");
                File.WriteAllText(Path.Combine(impMissing.Directory, "info.json"), metaMissing.ToJsonString());

                Result editMissing = Edit(impMissing.Directory, ("rebake_output", true));
                Require(editMissing.Ok, "Bug1D-Legacy missing rebake failed: " + editMissing.Error);
                JsonObject afterMissing = Info(impMissing.Directory);
                string srcMissing = afterMissing["source_vmd"]?.GetValue<string>() ?? "";
                string morphMissing = afterMissing["source_morph_vmd"]?.GetValue<string>() ?? "";
                Require(afterMissing["has_facial"]!.GetValue<bool>(), "Bug1D-Legacy missing did not re-enable facial");
                Require(!srcMissing.Contains("gkmotion-cs-edit-", StringComparison.OrdinalIgnoreCase), "staging path leaked in source_vmd (missing): " + srcMissing);
                Require(!morphMissing.Contains("gkmotion-cs-edit-", StringComparison.OrdinalIgnoreCase), "staging path leaked in source_morph_vmd (missing): " + morphMissing);
            });
            Case("Bug1D-Group: group slot rebake without replaceBody when source_vmd is empty or missing never leaks staging path", () =>
            {
                string seed = Path.Combine(_root, "bug1d-group");
                string library = Path.Combine(seed, "motions");
                string emb1 = WriteSyntheticVmdWithMorph(Path.Combine(seed, "emb1.vmd"), frames: 6, "あ");
                string emb2 = WriteSyntheticVmdWithMorph(Path.Combine(seed, "emb2.vmd"), frames: 8, "あ");
                string wav = Path.Combine(seed, "song.wav"); Wav(wav, sample: 39);

                Result imp = RunRequest(new JsonObject
                {
                    ["motions_root"] = library,
                    ["title"] = "Bug1DGroup",
                    ["audio"] = wav,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["motion_vmd"] = emb1, ["disable_morph"] = true },
                        new JsonObject { ["slot"] = 2, ["motion_vmd"] = emb2, ["disable_morph"] = true, ["x"] = 1.0f }
                    }
                });
                Require(imp.Ok, "Bug1D-Group seed import failed: " + imp.Error);
                JsonObject meta = Info(imp.Directory);
                var posArr = (JsonArray)meta["positions"]!;
                ((JsonObject)posArr[1]!).Remove("source_vmd");
                File.WriteAllText(Path.Combine(imp.Directory, "info.json"), meta.ToJsonString());

                Result edit = RunRequest(new JsonObject
                {
                    ["edit_dir"] = imp.Directory,
                    ["positions"] = new JsonArray
                    {
                        new JsonObject { ["slot"] = 1, ["preserve_existing"] = true },
                        new JsonObject { ["slot"] = 2, ["rebake_output"] = true, ["x"] = 1.0f }
                    }
                });
                Require(edit.Ok, "Bug1D-Group rebake failed: " + edit.Error);
                JsonObject after = Info(imp.Directory);
                var afterPos = (JsonArray)after["positions"]!;
                var slot2 = (JsonObject)afterPos[1]!;
                string slot2Src = slot2["source_vmd"]?.GetValue<string>() ?? "";
                string slot2Morph = slot2["source_morph_vmd"]?.GetValue<string>() ?? "";
                Require(slot2["has_facial"]!.GetValue<bool>(), "Bug1D-Group slot 2 did not re-enable facial");
                Require(!slot2Src.Contains("gkmotion-cs-edit-", StringComparison.OrdinalIgnoreCase), "staging path leaked in group slot 2 source_vmd: " + slot2Src);
                Require(!slot2Morph.Contains("gkmotion-cs-edit-", StringComparison.OrdinalIgnoreCase), "staging path leaked in group slot 2 source_morph_vmd: " + slot2Morph);
            });
            Case("parse-error returns BufferTooSmall (rc=3) when responseCapacity is too small", () =>
            {
                byte[] badJson = Encoding.UTF8.GetBytes("{invalid json\0");
                byte[] tiny = new byte[10];
                int rcBody, rcCamera;
                fixed (byte* p = badJson) fixed (byte* q = tiny)
                {
                    rcBody = _run(p, q, tiny.Length);
                    rcCamera = _runCamera(p, q, tiny.Length);
                }
                Require(rcBody == 3, $"GkVmdImport_Run parse error with tiny buffer returned rc={rcBody}, expected 3");
                Require(rcCamera == 3, $"GkCameraImport_Run parse error with tiny buffer returned rc={rcCamera}, expected 3");

                byte[] big = new byte[4096];
                fixed (byte* p = badJson) fixed (byte* q = big)
                {
                    rcBody = _run(p, q, big.Length);
                    rcCamera = _runCamera(p, q, big.Length);
                }
                Require(rcBody == 1, $"GkVmdImport_Run parse error with adequate buffer returned rc={rcBody}, expected 1");
                Require(rcCamera == 1, $"GkCameraImport_Run parse error with adequate buffer returned rc={rcCamera}, expected 1");
            });
        }
        finally
        {
            Fixtures.TryDelete(_root);
            if (native != 0) NativeLibrary.Free(native);
        }
        Console.WriteLine($"{_cases - _failed} PASS / {_failed} FAIL");
        return _failed == 0 ? 0 : 1;
    }

    private static void Case(string name, Action run)
    {
        _cases++;
        try { run(); Console.WriteLine("PASS " + name); }
        catch (Exception ex) { _failed++; Console.WriteLine("FAIL " + name + ": " + ex.Message); }
    }

    private static string Track()
    {
        string seed = Path.Combine(_root, "case-" + _cases + "-" + Guid.NewGuid().ToString("N"));
        string vmd = Fixtures.WriteSyntheticVmd(seed, frames: 5);
        string track = Fixtures.ImportDirectory(vmd, seed, "Existing", requestFps: 0);
        var info = Info(track);
        info["custom"] = new JsonObject { ["value"] = 17 };
        File.WriteAllText(Path.Combine(track, "info.json"), info.ToJsonString());
        Directory.CreateDirectory(Path.Combine(track, "custom"));
        File.WriteAllText(Path.Combine(track, "custom", "keep.bin"), "keep");
        Wav(Path.Combine(track, "song.wav"), sample: 10);
        Camera(Path.Combine(track, "camera.gkcamera"), x: 1);
        File.WriteAllText(Path.Combine(track, "source_camera.vmd"), "old source backup");
        return track;
    }

    private sealed record Result(bool Ok, string Directory, string Error);
    private static Result Edit(string directory, params (string Name, object Value)[] fields)
    {
        var request = new JsonObject { ["edit_dir"] = directory };
        foreach (var (name, value) in fields) request[name] = value switch
        {
            string text => JsonValue.Create(text), bool flag => JsonValue.Create(flag),
            int number => JsonValue.Create(number), _ => throw new InvalidOperationException("test value")
        };
        return RunRequest(request);
    }
    private static Result RunRequest(JsonObject request)
    {
        return RunRawJson(request.ToJsonString());
    }
    private static Result RunRawJson(string json)
    {
        byte[] input = Encoding.UTF8.GetBytes(json + '\0'), output = new byte[65536];
        int code;
        fixed (byte* p = input) fixed (byte* q = output) code = _run(p, q, output.Length);
        using JsonDocument response = JsonDocument.Parse(output.AsMemory(0, Array.IndexOf(output, (byte)0)));
        JsonElement root = response.RootElement;
        return new Result(code == 0 && root.GetProperty("ok").GetBoolean(),
            root.GetProperty("dir").GetString() ?? "", root.GetProperty("error").GetString() ?? "");
    }
    private static string GroupTrack(int slotCount)
    {
        string seed = Path.Combine(_root, "grp-" + _cases + "-" + Guid.NewGuid().ToString("N"));
        string library = Path.Combine(seed, "motions");
        var posArr = new JsonArray();
        for (int s = 1; s <= slotCount; s++)
        {
            string vmd = Fixtures.WriteSyntheticVmd(Path.Combine(seed, $"s{s}"), frames: 4 + s);
            posArr.Add((JsonNode)new JsonObject
            {
                ["slot"] = s,
                ["motion_vmd"] = vmd,
                ["x"] = s == 1 ? 0.0f : (s - 1.5f) * 2.0f,
                ["y"] = 0.0f,
                ["z"] = s == 1 ? 0.0f : -0.5f * s,
                ["yaw_degrees"] = s == 1 ? 0.0f : 15.0f * (s - 1)
            });
        }
        string wav = Path.Combine(seed, "song.wav"); Wav(wav, sample: 42);
        Result r = RunRequest(new JsonObject
        {
            ["motions_root"] = library,
            ["title"] = "GroupFixture",
            ["audio"] = wav,
            ["positions"] = posArr
        });
        Require(r.Ok, "GroupTrack fixture import failed: " + r.Error);
        return r.Directory;
    }
    private static void ExpectConflict(Action action, string detail)
    {
        try { action(); }
        catch (InvalidDataException ex)
        {
            Require(ex.Message.Contains(detail), "unhelpful conflict message: " + ex.Message);
            return;
        }
        throw new Exception("external change did not prevent publication");
    }
    private static JsonObject Info(string track) => (JsonObject)JsonNode.Parse(File.ReadAllBytes(Path.Combine(track, "info.json")))!;
    private static string Digest(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
    private static Dictionary<string, string> Snapshot(string directory) => Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories)
        .ToDictionary(p => Path.GetRelativePath(directory, p), Digest);
    private static void Same(Dictionary<string, string> a, Dictionary<string, string> b) =>
        Require(a.Count == b.Count && a.All(p => b.TryGetValue(p.Key, out string? value) && value == p.Value), "track changed on failure");
    private static void Require(bool test, string message) { if (!test) throw new Exception(message); }
    private static void Wav(string path, short sample)
    {
        using var writer = new BinaryWriter(File.Create(path));
        writer.Write(Encoding.ASCII.GetBytes("RIFF")); writer.Write(36 + 200);
        writer.Write(Encoding.ASCII.GetBytes("WAVEfmt ")); writer.Write(16);
        writer.Write((short)1); writer.Write((short)1); writer.Write(3000); writer.Write(6000);
        writer.Write((short)2); writer.Write((short)16); writer.Write(Encoding.ASCII.GetBytes("data")); writer.Write(200);
        for (int i = 0; i < 100; i++) writer.Write(sample);
    }
    private static void Camera(string path, float x)
    {
        using var writer = new BinaryWriter(File.Create(path));
        writer.Write(Encoding.ASCII.GetBytes("GKCAM\0\0\0")); writer.Write(1u); writer.Write(60f); writer.Write(1u);
        foreach (float value in new[] { x, 2, 3, 0, 0, 0, 1, 45, 5 }) writer.Write(value);
        writer.Write(0u);
    }

    private static void WriteSyncsafeBE(Stream s, uint value)
    {
        s.WriteByte((byte)((value >> 21) & 0x7F));
        s.WriteByte((byte)((value >> 14) & 0x7F));
        s.WriteByte((byte)((value >> 7) & 0x7F));
        s.WriteByte((byte)(value & 0x7F));
    }

    private static void WriteUInt32BE(Stream s, uint value)
    {
        s.WriteByte((byte)(value >> 24));
        s.WriteByte((byte)(value >> 16));
        s.WriteByte((byte)(value >> 8));
        s.WriteByte((byte)value);
    }

    private static void WriteSyntheticMp3WithId3(string path, string title, string artist,
        string coverMime, byte[] coverBytes, int frameCount)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var body = new MemoryStream();

        void AddTextFrame(string id, string text)
        {
            byte[] utf8 = Encoding.UTF8.GetBytes(text);
            body.Write(Encoding.ASCII.GetBytes(id));
            WriteUInt32BE(body, (uint)(1 + utf8.Length));
            body.WriteByte(0); body.WriteByte(0);
            body.WriteByte(3); // UTF-8
            body.Write(utf8);
        }

        AddTextFrame("TIT2", title);
        AddTextFrame("TPE1", artist);

        byte[] mimeAscii = Encoding.ASCII.GetBytes(coverMime);
        uint apicLen = (uint)(1 + mimeAscii.Length + 1 + 1 + 1 + coverBytes.Length);
        body.Write(Encoding.ASCII.GetBytes("APIC"));
        WriteUInt32BE(body, apicLen);
        body.WriteByte(0); body.WriteByte(0);
        body.WriteByte(0); // latin1 mime/desc
        body.Write(mimeAscii);
        body.WriteByte(0);
        body.WriteByte(3); // Cover (front)
        body.WriteByte(0); // empty description
        body.Write(coverBytes);

        using var file = File.Create(path);
        file.Write(Encoding.ASCII.GetBytes("ID3"));
        file.WriteByte(3); file.WriteByte(0); // ID3v2.3
        file.WriteByte(0);
        WriteSyncsafeBE(file, (uint)body.Length);
        body.Position = 0;
        body.CopyTo(file);

        // MPEG1 Layer 3 128kbps 44100Hz Stereo: frame size = 417 bytes
        byte[] frame = new byte[417];
        frame[0] = 0xFF; frame[1] = 0xFB; frame[2] = 0x90; frame[3] = 0x00;
        for (int i = 0; i < frameCount; ++i)
            file.Write(frame);
    }

    private static void WriteSyntheticFlac(string path, string title, string artist,
        string coverMime, byte[] coverBytes, ulong totalSamples)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var file = File.Create(path);
        file.Write(Encoding.ASCII.GetBytes("fLaC"));

        // STREAMINFO (block 0, 34 bytes)
        file.WriteByte(0x00);
        file.WriteByte(0); file.WriteByte(0); file.WriteByte(34);
        byte[] si = new byte[34];
        si[10] = (byte)(44100 >> 12);
        si[11] = (byte)((44100 >> 4) & 0xFF);
        si[12] = (byte)(((44100 & 0xF) << 4) | (1 << 1)); // 2 channels, 16 bps (15) upper bit=0
        si[13] = (byte)((15 << 4) | ((int)(totalSamples >> 32) & 0xF));
        WriteUInt32BE(new MemoryStream(si, 14, 4, writable: true), (uint)(totalSamples & 0xFFFFFFFFUL));
        file.Write(si);

        // VORBIS_COMMENT (block 4)
        using (var vc = new MemoryStream())
        using (var w = new BinaryWriter(vc, Encoding.UTF8, leaveOpen: true))
        {
            byte[] vendor = Encoding.UTF8.GetBytes("GakuMotionTest");
            w.Write((uint)vendor.Length); w.Write(vendor);
            string[] tags = { "TITLE=" + title, "ARTIST=" + artist };
            w.Write((uint)tags.Length);
            foreach (string t in tags)
            {
                byte[] b = Encoding.UTF8.GetBytes(t);
                w.Write((uint)b.Length); w.Write(b);
            }
            w.Flush();
            uint len = (uint)vc.Length;
            file.WriteByte(0x04);
            file.WriteByte((byte)((len >> 16) & 0xFF));
            file.WriteByte((byte)((len >> 8) & 0xFF));
            file.WriteByte((byte)(len & 0xFF));
            vc.Position = 0; vc.CopyTo(file);
        }

        // PICTURE (block 6, isLast=1 -> 0x86)
        using (var pic = new MemoryStream())
        {
            WriteUInt32BE(pic, 3); // Front cover
            byte[] mime = Encoding.ASCII.GetBytes(coverMime);
            WriteUInt32BE(pic, (uint)mime.Length);
            pic.Write(mime);
            WriteUInt32BE(pic, 0); // description
            WriteUInt32BE(pic, 0); WriteUInt32BE(pic, 0); WriteUInt32BE(pic, 0); WriteUInt32BE(pic, 0);
            WriteUInt32BE(pic, (uint)coverBytes.Length);
            pic.Write(coverBytes);
            uint len = (uint)pic.Length;
            file.WriteByte(0x86);
            file.WriteByte((byte)((len >> 16) & 0xFF));
            file.WriteByte((byte)((len >> 8) & 0xFF));
            file.WriteByte((byte)(len & 0xFF));
            pic.Position = 0; pic.CopyTo(file);
        }
    }

    private static string WriteSyntheticVmdWithMorph(string path, int frames, string morphName)
    {
        Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        Encoding shiftJis = Encoding.GetEncoding(932);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);

        using var ms = new MemoryStream();
        using (var writer = new BinaryWriter(ms, Encoding.ASCII, leaveOpen: true))
        {
            byte[] header = new byte[30];
            Encoding.ASCII.GetBytes("Vocaloid Motion Data 0002", header);
            writer.Write(header);

            byte[] model = new byte[20];
            shiftJis.GetBytes("morph probe", model);
            writer.Write(model);

            // Bone keyframes: 上半身 across frames
            writer.Write((uint)frames);
            for (int f = 0; f < frames; f++)
            {
                byte[] boneName = new byte[15];
                shiftJis.GetBytes("上半身", boneName);
                writer.Write(boneName);
                writer.Write((uint)f);
                writer.Write(0f); writer.Write(0f); writer.Write(0f);
                writer.Write(0f); writer.Write(0f); writer.Write(0f); writer.Write(1f);
                byte[] curves = new byte[64];
                for (int c = 0; c < 4; c++)
                {
                    int at = c * 16;
                    curves[at + 0] = 20; curves[at + 4] = 20; curves[at + 8] = 107; curves[at + 12] = 107;
                }
                writer.Write(curves);
            }

            // Morph keyframes
            writer.Write((uint)frames);
            byte[] morphBytes = new byte[15];
            shiftJis.GetBytes(morphName, morphBytes);
            for (int f = 0; f < frames; f++)
            {
                writer.Write(morphBytes);
                writer.Write((uint)f);
                writer.Write(f == 0 ? 0.0f : 0.8f);
            }

            writer.Write((uint)0); // camera
            writer.Write((uint)0); // light
            writer.Write((uint)0); // self-shadow
            writer.Write((uint)0); // property
        }

        File.WriteAllBytes(path, ms.ToArray());
        return path;
    }
}
