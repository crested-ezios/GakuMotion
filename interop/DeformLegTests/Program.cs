using System.Numerics;
using GakuMotion.VmdImporter.Native;
using GakuMotion.VmdImporter.Output;
using GakuMotion.VmdImporter.Rig;
using GakuMotion.VmdImporter.Vmd;

if (args.Length != 0)
{
    if (args.Length != 3 || args[0] != "--import")
        throw new ArgumentException("Usage: --import <source.vmd> <isolated-output-root>");
    var request = new ImportRequest
    {
        MotionVmd = args[1], MotionsRoot = args[2], Title = "D-leg-check",
        Fps = 30, ClearAudio = true, LogPath = Path.Combine(args[2], "import.log")
    };
    using var log = new ImportLog(request.LogPath);
    var result = ImportPipeline.Run(request, log);
    if (!result.Ok) throw new Exception($"import failed [{result.ErrorCode}]: {result.Error}");
    Console.WriteLine("Imported to: " + result.Directory);
    return;
}

int checks = 0;
void Check(bool condition, string message)
{
    checks++;
    if (!condition) throw new Exception(message);
    Console.WriteLine("PASS: " + message);
}

BoneKeyframe Key(string name, uint frame, float degrees = 0, Vector3 position = default)
{
    var curve = new BezierControlPoints(0, 0, 1, 1);
    return new BoneKeyframe(name, frame, position,
        Quaternion.CreateFromAxisAngle(Vector3.UnitX, degrees * MathF.PI / 180), curve, curve, curve, curve);
}
IReadOnlyList<BoneKeyframe> Track(string name, float lastDegrees = 0, Vector3 lastPosition = default) =>
    [Key(name, 0), Key(name, 2, lastDegrees, lastPosition)];
VmdMotion Motion(Dictionary<string, IReadOnlyList<BoneKeyframe>> bones, params PropertyKeyframe[] properties) => new()
{
    Version = "Vocaloid Motion Data 0002", ModelName = "synthetic", MaxFrame = 2,
    BoneKeyframes = bones, PropertyKeyframes = properties
};
Dictionary<string, IReadOnlyList<BoneKeyframe>> BakedLeft()
{
    var bones = new Dictionary<string, IReadOnlyList<BoneKeyframe>>();
    foreach (string part in new[] { "足", "ひざ", "足首" })
    {
        bones["左" + part] = Track("左" + part);
        bones["左" + part + "D"] = Track("左" + part + "D", part == "ひざ" ? 60 : 20);
    }
    bones["左足ＩＫ"] = Track("左足ＩＫ");
    bones["上半身"] = Track("上半身", 12);
    return bones;
}
GkClipData Convert(VmdMotion input)
{
    var effective = DeformLegTracks.Prepare(input, out _);
    var evaluator = new SourceEvaluator();
    var result = evaluator.Evaluate(effective);
    var clip = GkClipEncoder.FromEvaluationResult(result, rig: evaluator.Rig);
    DisabledLegIkFk.Apply(effective, result, evaluator.Rig, clip);
    return clip;
}
bool Same(Quaternion a, Quaternion b) => MathF.Abs(Quaternion.Dot(Quaternion.Normalize(a), Quaternion.Normalize(b))) > 0.99999f;

var input = Motion(BakedLeft(), new PropertyKeyframe(1, false, new Dictionary<string, bool> { ["右足ＩＫ"] = true }));
var adapted = DeformLegTracks.Prepare(input, out var sides);
Check(sides.SequenceEqual(new[] { "左:D" }), "baked D tracks select only the matching side");
Check(!ReferenceEquals(input, adapted) && input.BoneKeyframes["左ひざ"][1].Rotation == Quaternion.Identity,
    "adaptation does not mutate the input motion");
Check(adapted.BoneKeyframes["左ひざ"][1].Rotation == input.BoneKeyframes["左ひざD"][1].Rotation,
    "authored D knee rotation is transferred to the ordinary knee");
Check(!adapted.PropertyKeyframes[0].IkSwitches["左足ＩＫ"] &&
      !adapted.PropertyKeyframes[0].IkSwitches["左つま先ＩＫ"] &&
      adapted.PropertyKeyframes[1].IkSwitches["右足ＩＫ"] && !adapted.PropertyKeyframes[1].Visible,
    "neutral IK is disabled only for the adapted side and other properties survive");

var fixedClip = Convert(input);
var baselineEvaluator = new SourceEvaluator();
var baselineResult = baselineEvaluator.Evaluate(input);
var baselineClip = GkClipEncoder.FromEvaluationResult(baselineResult, rig: baselineEvaluator.Rig);
DisabledLegIkFk.Apply(input, baselineResult, baselineEvaluator.Rig, baselineClip);
int leftKnee = fixedClip.Bones.FindIndex(b => b.Name == "LeftLowerLeg");
int rightKnee = fixedClip.Bones.FindIndex(b => b.Name == "RightLowerLeg");
int upperBody = fixedClip.Bones.FindIndex(b => b.Name == "Spine");
Check(leftKnee >= 0 && !Same(fixedClip.BoneDeltas[0, leftKnee], fixedClip.BoneDeltas[2, leftKnee]),
    "end-to-end gkclip output preserves changing D-knee rotation");
Check(rightKnee >= 0 && upperBody >= 0 && Same(fixedClip.BoneDeltas[2, rightKnee], baselineClip.BoneDeltas[2, rightKnee]) &&
      Same(fixedClip.BoneDeltas[2, upperBody], baselineClip.BoneDeltas[2, upperBody]) &&
      fixedClip.RootPositions.SequenceEqual(baselineClip.RootPositions),
    "opposite leg, upper body and root remain unchanged");

var ordinary = BakedLeft(); ordinary["左ひざ"] = Track("左ひざ", 30);
var ordinaryMotion = Motion(ordinary);
Check(ReferenceEquals(ordinaryMotion, DeformLegTracks.Prepare(ordinaryMotion, out sides)) && sides.Length == 0,
    "an animated ordinary leg is never replaced");
var movingIk = BakedLeft(); movingIk["左足ＩＫ"] = Track("左足ＩＫ", 0, new Vector3(0, 1, 0));
var ikMotion = Motion(movingIk);
Check(ReferenceEquals(ikMotion, DeformLegTracks.Prepare(ikMotion, out sides)) && sides.Length == 0,
    "an animated IK target is never replaced");
var explicitIk = Motion(BakedLeft(), new PropertyKeyframe(0, true, new Dictionary<string, bool> { ["左足ＩＫ"] = true }));
Check(ReferenceEquals(explicitIk, DeformLegTracks.Prepare(explicitIk, out sides)) && sides.Length == 0,
    "an explicit IK switch prevents automatic reinterpretation");
var missing = BakedLeft(); missing.Remove("左足首D");
var missingMotion = Motion(missing);
Check(ReferenceEquals(missingMotion, DeformLegTracks.Prepare(missingMotion, out sides)) && sides.Length == 0,
    "an incomplete D chain remains untouched");
var invalid = BakedLeft(); invalid["左ひざD"] = [Key("左ひざD", 0), Key("左ひざD", 2, float.NaN)];
var invalidMotion = Motion(invalid);
Check(ReferenceEquals(invalidMotion, DeformLegTracks.Prepare(invalidMotion, out sides)) && sides.Length == 0,
    "invalid D rotations are rejected rather than copied");

IReadOnlyList<BoneKeyframe> Dense(string name, float middle = 0, float last = 0) =>
    [Key(name, 0), Key(name, 1, middle), Key(name, 2, last)];
var bakedFk = new Dictionary<string, IReadOnlyList<BoneKeyframe>>
{
    ["左足"] = Dense("左足", 15, 25), ["左ひざ"] = Dense("左ひざ", 30, 60),
    ["左足首"] = Dense("左足首", 5, 10), ["左足ＩＫ"] = Dense("左足ＩＫ"),
    ["左つま先ＩＫ"] = Dense("左つま先ＩＫ"), ["上半身"] = Dense("上半身", 3, 6)
};
var fkMotion = Motion(bakedFk);
var fkEffective = DeformLegTracks.Prepare(fkMotion, out sides);
Check(sides.SequenceEqual(new[] { "左:FK" }) &&
      ReferenceEquals(fkEffective.BoneKeyframes["左ひざ"], fkMotion.BoneKeyframes["左ひざ"]),
    "dense baked FK uses authored knee keys without replacing them");
Check(!fkEffective.PropertyKeyframes[0].IkSwitches["左足ＩＫ"] && !fkEffective.PropertyKeyframes[0].IkSwitches["左つま先ＩＫ"],
    "only neutral baked foot and toe IK are disabled");
var fkClip = Convert(fkMotion);
leftKnee = fkClip.Bones.FindIndex(b => b.Name == "LeftLowerLeg");
Check(leftKnee >= 0 && !Same(fkClip.BoneDeltas[0, leftKnee], fkClip.BoneDeltas[2, leftKnee]),
    "end-to-end baked FK preserves knee motion");
var sparseIk = new Dictionary<string, IReadOnlyList<BoneKeyframe>>(bakedFk) { ["左足ＩＫ"] = Track("左足ＩＫ") };
var sparseMotion = Motion(sparseIk);
Check(ReferenceEquals(sparseMotion, DeformLegTracks.Prepare(sparseMotion, out sides)) && sides.Length == 0,
    "sparse stationary IK is left alone as a possible planted foot");
var movingDenseIk = new Dictionary<string, IReadOnlyList<BoneKeyframe>>(bakedFk)
    { ["左足ＩＫ"] = [Key("左足ＩＫ", 0), Key("左足ＩＫ", 1, 0, new Vector3(0, 1, 0)), Key("左足ＩＫ", 2)] };
var movingDenseMotion = Motion(movingDenseIk);
Check(ReferenceEquals(movingDenseMotion, DeformLegTracks.Prepare(movingDenseMotion, out sides)) && sides.Length == 0,
    "dense but moving IK is not reinterpreted as baked FK");
var explicitFk = Motion(bakedFk, new PropertyKeyframe(0, true, new Dictionary<string, bool> { ["左足ＩＫ"] = true }));
Check(ReferenceEquals(explicitFk, DeformLegTracks.Prepare(explicitFk, out sides)) && sides.Length == 0,
    "explicit IK control prevents FK auto-detection");
Console.WriteLine($"D-leg importer: {checks} checks passed");
