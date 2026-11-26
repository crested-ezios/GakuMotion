using GakuMotion.VmdImporter.Native.Facial;
using GakuMotion.VmdImporter.Native;
using GakuMotion.VmdImporter.Vmd;
using System.Text;

int checks = 0;
void Require(bool value, string label) { checks++; if (!value) throw new Exception(label); }
Dictionary<string, float[]> Translate(params (string Name, float[] Values)[] tracks)
{
    var input = tracks.ToDictionary(t => t.Name, t => (IReadOnlyList<MorphKeyframe>)t.Values.Select((v,i) => new MorphKeyframe(t.Name,(uint)i,v)).ToArray());
    var result = MorphTranslator.Translate(input, 3, null);
    return result.Names.Select((name,i) => (name, values: Enumerable.Range(0,3).Select(f => result.Weights[f,i]).ToArray())).ToDictionary(t=>t.name,t=>t.values);
}
const string P = "miku.raw.v1.";
var e = Translate(("え",new[]{.3f,.6f,.9f}));
Require(e[P+"e"].SequenceEqual(new[]{.3f,.6f,.9f}),"raw E retained");
Require(e[P+"a"].All(v=>v==0) && e[P+"present"].All(v=>v==1),"missing/intent explicit");
Require(e.Keys.Any(n=>n.StartsWith("b_mouth.")),"legacy E mapped curve retained");
Require(!Translate(("まばたき",new[]{.7f,.7f,.7f})).ContainsKey(P+"present"),"no vowels no intent");
var eye = Translate(("あ",new float[3]),("まばたき",new[]{.7f,.7f,.7f}));
Require(eye[P+"unsupported"].All(v=>v==0),"known eyes supported");
foreach (var name in new[]{"mystery","a"}) {
    var c = Translate(("あ",new float[3]),(name,new[]{0f,.25f,.5f}));
    Require(c[P+"unsupported"].SequenceEqual(new[]{0f,.25f,.5f}),"unknown/alias activity blocks");
}
foreach (var name in new[]{"b_mouth.mouth_028","tooth"})
    Require(Translate(("あ",new[]{.5f,.5f,.5f}),(name,new float[3]))[P+"native_authored"].All(v=>v==1),"explicit zero native blocks");
var invalid = Translate(("あ",new[]{-.2f,.5f,1.2f}),("い",new[]{.8f,.8f,.8f}));
Require(invalid[P+"unsupported"].SequenceEqual(new[]{1f,0f,1f}),"invalid values guarded");
Require(invalid[P+"a"].SequenceEqual(new[]{0f,.5f,1f}),"valid serialization");
Require(invalid[P+"a"][1]+invalid[P+"i"][1]>1,"no hidden simplex normalization");

const string E = "miku.raweye.v1.";
var rawEye = Translate(("真面目",new[]{.2f,.5f,.9f}));
Require(rawEye[E+"真面目"].SequenceEqual(new[]{.2f,.5f,.9f}),"raw eye key retained verbatim");
Require(rawEye[E+"present"].All(v=>v==1),"eye intent explicit");
Require(rawEye[E+"unsupported"].All(v=>v==0),"a fitted key is not an unsupported source");
Require(rawEye[E+"native_authored"].All(v=>v==0),"a mapped curve is not native eye authorship");
Require(rawEye.Keys.Count(n=>n.StartsWith(E,StringComparison.Ordinal))==MikuRawEyeKeys.Names.Count+3,
    "every table key is emitted even when the clip leaves it at zero");
Require(!Translate(("え",new[]{.3f,.3f,.3f})).Keys.Any(n=>n.StartsWith(E,StringComparison.Ordinal)),
    "no fitted eye source, no eye contract");
// Eight of the twelve accepted keys have no morph-map entry at all; the fit
// must still see them, which is the whole reason the key table is embedded.
Require(Translate(("たれ目",new float[3]))[E+"たれ目"].All(v=>v==0),"unmapped accepted key keeps its identity");
Require(Translate(("まばたき",new[]{.7f,.7f,.7f}))[E+"まばたき"].All(v=>v==.7f),"a key that also maps natively stays raw");
foreach (var name in new[]{"b_eye.eye_001","b_brow.brow_003"})
    Require(Translate(("真面目",new float[3]),(name,new float[3]))[E+"native_authored"].All(v=>v==1),"explicit zero native eye blocks");
var strayEye = Translate(("blink",new[]{0f,.25f,.5f}));
Require(strayEye[E+"unsupported"].SequenceEqual(new[]{0f,.25f,.5f}),"eye-family source outside the table refuses the clip");
Require(strayEye[E+"present"].All(v=>v==1),"the refusal is still an eye contract, not a legacy clip");
Require(Translate(("真面目",new[]{-1f,.5f,2f}))[E+"真面目"].SequenceEqual(new[]{0f,.5f,1f}),"eye keys clamp to [0,1]");

const string M = "miku.rawmouth.v1.";
var expanded = Translate(("あ", new[] { .2f, .4f, .6f }), ("∧", new[] { .1f, .2f, .3f }),
    ("口角上げ", new[] { .7f, .6f, .5f }), ("たれ目", new[] { .3f, .3f, .3f }));
Require(expanded[M+"a"].SequenceEqual(new[] { .2f, .4f, .6f }) &&
    expanded[M+"∧"].SequenceEqual(new[] { .1f, .2f, .3f }) &&
    expanded[M+"口角上げ"].SequenceEqual(new[] { .7f, .6f, .5f }), "seven-input source keys survive translation");
Require(expanded[M+"unsupported"].All(value => value == 0), "fitted close/corners and known eye keys admit new mouth source");
Require(expanded[P+"unsupported"].Any(value => value != 0), "legacy five-vowel unsupported behavior is preserved");
Require(Translate(("口角上げ", new float[3]))[M+"present"].All(value => value == 1), "authored zero corner-only source is present");
Require(!Translate(("たれ目", new[] { .3f, .3f, .3f })).ContainsKey(M+"present"), "eye-only source creates no mouth intent");
foreach (var name in new[] { "mystery", "a" })
    Require(Translate(("あ", new float[3]), (name, new[] { 0f, .25f, .5f }))[M+"unsupported"]
        .SequenceEqual(new[] { 0f, .25f, .5f }), "expanded unknown/duplicate activity remains explicit");
Require(Translate(("あ", new[] { -.2f, .5f, 1.2f }))[M+"unsupported"].SequenceEqual(new[] { 1f, 0f, 1f }),
    "expanded invalid source weights marked unsupported");
Require(Translate(("あ", new float[3]), ("b_mouth.mouth_028", new float[3]))[M+"native_authored"].All(value => value == 1),
    "expanded explicit zero native authoring remains marked");

const string S = "miku.rawmouth.extra.v1.";
var extra = Translate(("あ", new[] { .5f, .5f, .5f }), ("にやり２", new[] { .3f, .2f, 0f }),
    ("口角下げ", new[] { .1f, 0f, .4f }), ("mystery", new[] { 0f, .25f, 0f }));
Require(extra[S+"present"].All(value => value == 1) &&
    extra[S+"にやり2"].SequenceEqual(new[] { .3f, .2f, 0f }) &&
    extra[S+"口角下げ"].SequenceEqual(new[] { .1f, 0f, .4f }),
    "normalized smirk and downward corner inputs are preserved independently");
Require(extra[S+"unsupported"].SequenceEqual(new[] { 0f, .25f, 0f }) &&
    extra[M+"unsupported"].SequenceEqual(new[] { .3f, .25f, .4f }),
    "raw v1 keeps the aggregate guard while the extra guard retains unknown activity");
Require(Translate(("あ", new float[3]), ("にやり2", new[] { 0f, .1f, 0f }),
    ("にやり２", new[] { 0f, 0f, .3f }))[S+"unsupported"].SequenceEqual(new[] { 0f, .1f, .3f }),
    "duplicate normalized extra inputs stay protected");
Require(Translate(("あ", new float[3]), ("口角下げ", new[] { -.1f, .25f, 1.2f }))[S+"unsupported"]
    .SequenceEqual(new[] { 1f, 0f, 1f }), "invalid extra amounts stay protected");
Require(!Translate(("まばたき", new float[3])).ContainsKey(S+"present"), "eye-only clips have no extra mouth intent");

var curves = new Dictionary<string, IReadOnlyList<MorphKeyframe>>
{
    ["あ"] = new[] { new MorphKeyframe("あ", 0, .2f), new MorphKeyframe("あ", 2, .8f), new MorphKeyframe("あ", 2, .9f) },
    ["∧"] = new[] { new MorphKeyframe("∧", 0, .04f), new MorphKeyframe("∧", 2, .08f) },
    ["口角上げ"] = new[] { new MorphKeyframe("口角上げ", 0, .5f) },
    ["にやり２"] = new[] { new MorphKeyframe("にやり２", 0, 0f), new MorphKeyframe("にやり２", 2, .6f) },
    ["口角下げ"] = new[] { new MorphKeyframe("口角下げ", 0, 0f), new MorphKeyframe("口角下げ", 2, .2f) }
};
var sampled = MouthSourceReader.Sample(curves, 30, 60, 7);
var sampledExtra = MouthSourceReader.Sample(curves, 30, 60, 7, extraOnly: true);
Require(sampled.Length == 7 * 18 && Math.Abs(sampled[2 * 18] - .55f) < 1e-6f && sampled[6 * 18] == .9f,
    "runtime source resampling uses source/clip fps, last duplicate, and endpoint hold");
Require(sampled[2 * 18 + 8] == sampled[2 * 18] && Math.Abs(sampled[2 * 18 + 13] - .06f) < 1e-6f,
    "runtime source layout retains legacy raw8 then expanded rawmouth10");
Require(sampled[15] == 1 && sampled[16] == 0 && sampled[17] == 0 && sampled[6] != 0,
    "runtime markers distinguish legacy and expanded source support");
Require(sampled[2 * 18 + 16] == .3f && sampledExtra.Length == 7 * 4 &&
    sampledExtra.AsSpan(2 * 4, 4).SequenceEqual(new[] { 1f, .3f, .1f, 0f }),
    "extra frame-major order and resampling preserve the original aggregate marker");
var cornerOnly = MouthSourceReader.Sample(new Dictionary<string, IReadOnlyList<MorphKeyframe>>
    { ["口角上げ"] = curves["口角上げ"] }, 30, 30, 3);
Require(Enumerable.Range(0, 3).All(frame => cornerOnly[frame * 18 + 5] == 0 && cornerOnly[frame * 18 + 15] == 1),
    "corner-only source retains empty legacy contract and present expanded contract");

string sourceDirectory = Path.Combine(Path.GetTempPath(), "gkm-mouth-source-" + Guid.NewGuid());
Directory.CreateDirectory(sourceDirectory);
string sourcePath = Path.Combine(sourceDirectory, "口型.vmd");
Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
Encoding shiftJis = Encoding.GetEncoding(932);
using (var writer = new BinaryWriter(File.Create(sourcePath)))
{
    byte[] header = new byte[30];
    Encoding.ASCII.GetBytes("Vocaloid Motion Data 0002").CopyTo(header, 0);
    writer.Write(header);
    writer.Write(new byte[20]);
    writer.Write(0u);
    writer.Write((uint)curves.Values.Sum(keys => keys.Count));
    foreach (var keys in curves.Values)
        foreach (var key in keys)
        {
            byte[] name = new byte[15];
            shiftJis.GetBytes(key.Name).CopyTo(name, 0);
            writer.Write(name);
            writer.Write(key.Frame);
            writer.Write(key.Weight);
        }
    writer.Write(0u);
    writer.Write(0u);
    writer.Write(0u);
    writer.Write(0u);
}
try
{
    var expectedSource = File.ReadAllBytes(sourcePath);
    Require(MouthSourceReader.Read(sourcePath, 30, 60, 7).SequenceEqual(sampled), "runtime source reader retains morph file order");
    unsafe
    {
        delegate* unmanaged<byte*, float, float, int, float*, int, int> read = &NativeExports.ReadMouthSource;
        byte[] sourceUtf8 = Encoding.UTF8.GetBytes(sourcePath + '\0');
        float[] output = Enumerable.Repeat(-123f, sampled.Length).ToArray();
        fixed (byte* pathPointer = sourceUtf8)
        fixed (float* outputPointer = output)
        {
            Require(read(pathPointer, 30, 60, 7, outputPointer, sampled.Length - 1) == 3 && output.All(value => value == -123f),
                "mouth source ABI refuses undersized output without writes");
            Require(read(pathPointer, float.NaN, 60, 7, outputPointer, output.Length) == 2 && output.All(value => value == -123f),
                "mouth source ABI refuses invalid rates without writes");
            Require(read(pathPointer, 30, 60, 7, outputPointer, output.Length) == 0 && output.SequenceEqual(sampled),
                "mouth source ABI returns exact frame-major data");
        }
        byte[] missingUtf8 = Encoding.UTF8.GetBytes(sourcePath + ".missing\0");
        Array.Fill(output, -123f);
        fixed (byte* pathPointer = missingUtf8)
        fixed (float* outputPointer = output)
            Require(read(pathPointer, 30, 60, 7, outputPointer, output.Length) == 1 && output.All(value => value == -123f),
                "mouth source ABI contains file errors without output writes");

        delegate* unmanaged<int> extraVersion = &NativeExports.MouthSourceExtraAbiVersion;
        delegate* unmanaged<byte*, float, float, int, float*, int, int> readExtra = &NativeExports.ReadMouthSourceExtra;
        output = Enumerable.Repeat(-123f, sampledExtra.Length + 1).ToArray();
        Require(extraVersion() == 1, "extra source reader ABI version");
        fixed (byte* pathPointer = sourceUtf8)
        fixed (float* outputPointer = output)
        {
            Require(readExtra(pathPointer, 30, 60, 7, outputPointer, sampledExtra.Length - 1) == 3 &&
                output.All(value => value == -123f), "extra reader refuses short buffers without writes");
            Require(readExtra(pathPointer, 30, 60, 7, outputPointer, output.Length) == 0 &&
                output.AsSpan(0, sampledExtra.Length).SequenceEqual(sampledExtra) && output[^1] == -123f,
                "extra reader writes only the exact four frame-major channels");
        }
    }
    Require(File.ReadAllBytes(sourcePath).SequenceEqual(expectedSource) && Directory.GetFiles(sourceDirectory).Length == 1,
        "runtime source extraction leaves the VMD and directory untouched");
    using var duplicateStream = new MemoryStream();
    using (var writer = new BinaryWriter(duplicateStream, Encoding.ASCII, leaveOpen: true))
    {
        writer.Write(expectedSource.AsSpan(0, 54));
        writer.Write(40u);
        for (int index = 39; index >= 0; index--)
        {
            byte[] name = new byte[15];
            shiftJis.GetBytes("あ").CopyTo(name, 0);
            writer.Write(name);
            writer.Write(2u);
            writer.Write(index / 40f);
        }
    }
    duplicateStream.Position = 0;
    var duplicateSource = MouthSourceReader.ParseMorphs(duplicateStream);
    Require(duplicateSource["あ"].Select(key => key.Weight).SequenceEqual(Enumerable.Range(0, 40).Reverse().Select(index => index / 40f)),
        "forty duplicate source frames retain authored reverse order");
    Require(MouthSourceReader.Sample(duplicateSource, 30, 30, 4).All(value => value == 0 || value == 1),
        "last authored duplicate determines the zero vowel while present markers remain one");
    Require(MorphCurve.Sample(duplicateSource["あ"], 4).All(value => value == 0),
        "source-only reader preserves last-file duplicate semantics beyond sort thresholds");
    byte[] truncatedSource = expectedSource[..^20];
    bool truncatedRejected = false;
    try { MouthSourceReader.ParseMorphs(new MemoryStream(truncatedSource[..60])); }
    catch (InvalidDataException) { truncatedRejected = true; }
    Require(truncatedRejected, "source-only parser rejects a truncated morph payload");
}
finally
{
    Directory.Delete(sourceDirectory, true);
}
Console.WriteLine($"Miku raw-source product translator: {checks} checks passed.");
