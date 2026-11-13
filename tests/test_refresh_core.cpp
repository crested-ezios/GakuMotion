// Synthetic release regression adapted from tests/test_core.cpp; no recorded vectors or user motion inputs.
// =============================================================================
// tests/test_core.cpp — GakuMotion 阶段一离线单元测试
// =============================================================================
// 用途：在「不启动游戏」的前提下，对阶段一新增的控制层做确定性验证：
//   1. GkJsonLite            零依赖 JSON 解析器（BOM/注释/尾随逗号/转义/错误定位）
//   2. GkRuntimeConfig       控制文件模板生成、解析、command.seq 去重
//   3. GkBoneOverride        骨骼覆盖解析、优先级合并、总开关、动态条目
//   4. GkMotionData          真实 .gkmotion 载入与采样
//   5. GkIdolController      覆盖注入的旋转合成公式（local/world）与既有臂部偏移回归
//   6. GkMotionPlayer        Seek 与 MCI 音频时钟的 settle 校正、断点续播；
//                             播放速度倍率与音频时钟回退容差（playback_speed_fix）
//   7. GakuMotion::Core      曲目库扫描、声明式热加载、一次性命令端到端
//
// 编译：tests/run_tests.ps1（MSVC, /std:c++20 /utf-8）
// 运行：test_core.exe <repoRoot>   （repoRoot 用于定位真实动作包）
// =============================================================================

#define GKMOTION_CAPTURE_DIAGNOSTICS 1
#define GKMOTION_SCRUB_TRACE 1
#include "GkMotionData.hpp"
#include "GkJsonLite.hpp"
#include "GkBoneOverride.hpp"
#include "GkRuntimeConfig.hpp"
#include "GkIdolController.hpp"
#include "GkMotionPlayer.hpp"
#include "GkHotkey.hpp"
#include "GkFaceRuntime.hpp"
#include "GakuMotion.hpp"
#ifdef _WIN32
#endif

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include <filesystem>
#include <unordered_map>
#include <limits>

using namespace GakuMotion;

// The offline binary links this translation unit alone, so the log sink that
// normally lives in GakuMotionPlugin.cpp has no home here. Before v0.1.1 the
// facade was header-inline; now every GM_LOG_* site in an inlined controller
// path (test_playback_timing.hpp drives ApplyMotionToBinding) needs this symbol
// or the whole suite stops linking. Keep it a silent sink: the tests assert on
// behaviour, not on diagnostics text.
namespace GakuMotion::Log
{
    unsigned g_testSinkCalls = 0;

    void Write(Level level, const char* format, ...)
    {
        (void)level;
        (void)format;
        ++g_testSinkCalls;
    }
}

// -----------------------------------------------------------------------------
// 极简测试框架
// -----------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;
static std::string g_current;

static void Check(bool condition, const std::string& what)
{
	if (condition)
	{
		++g_pass;
	}
	else
	{
		++g_fail;
		std::printf("  [FAIL] %s :: %s\n", g_current.c_str(), what.c_str());
	}
}

static void BeginTest(const char* name)
{
	g_current = name;
	std::printf("[TEST] %s\n", name);
}

// 四元数方向一致性比较（允许 q 与 -q 等价）
static bool QuatNear(const Quaternion& a, const Quaternion& b, float eps = 1e-4f)
{
	return std::fabs(std::fabs(Quaternion::Dot(a, b)) - 1.0f) < eps;
}

static void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << text;
}

// =============================================================================
// 1. JSON 解析器
// =============================================================================
static void TestJsonLite()
{
	BeginTest("jsonlite");

	const std::string text =
		"\xEF\xBB\xBF" // UTF-8 BOM
		"// 顶栏注释\n"
		"{\n"
		"  \"a\": 1, \"b\": [1, 2.5, -3],   // 行内注释\n"
		"  \"c\": \"x\\u3042y\\n\",\n" // \u3042 -> あ
		"  \"nested\": { \"deep\": { \"v\": true } },\n"
		"  \"trail\": [1, 2,],\n" // 尾随逗号
		"  /* 块注释 */ \"obj\": { \"x\": 1, \"y\": 2, \"z\": 3 }\n"
		"}\n";

	jsonlite::Value root;
	std::string err;
	Check(jsonlite::ParseText(text, root, err), "解析 BOM/注释/尾随逗号文档: " + err);
	Check(root.IntPath("a", -1) == 1, "a == 1");
	Check(root.FindPath("b.2") != nullptr && std::fabs(root.FindPath("b.2")->AsFloat(0.0f) + 3.0f) < 1e-6f,
	      "数组下标路径 b.2 == -3");
	Check(root.StringPath("c", "") == std::string("x\xE3\x81\x82y\n"), "\\u3042 正确解码为 UTF-8");
	Check(root.BoolPath("nested.deep.v", false), "深层对象路径取值");
	Check(root.HasPath("trail.1"), "尾随逗号数组可用");
	Check(root.FloatPath("obj.y", 0.0f) == 2.0f, "对象字段取值");

	jsonlite::Value bad;
	std::string err2;
	Check(!jsonlite::ParseText("{\"a\": }", bad, err2), "非法 JSON 必须返回 false");
	Check(err2.find("line") != std::string::npos, "错误信息包含行列定位: " + err2);

	jsonlite::Value dup;
	std::string err3;
	jsonlite::ParseText("{\"k\":1,\"k\":2}", dup, err3);
	Check(dup.IntPath("k", 0) == 2, "重复键以最后一个为准");

	jsonlite::Value empty;
	std::string err4;
	Check(!jsonlite::ParseText("", empty, err4), "空内容必须返回 false");
}

// =============================================================================
// 2. 控制文件模板生成
// =============================================================================
static void TestConfigTemplate()
{
	BeginTest("config_template");

	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "gkmotion_cfg_template";
	fs::remove_all(dir);
	fs::create_directories(dir);

	RuntimeConfig cfg;
	cfg.rootDir = dir.string();
	cfg.path = (dir / "control.json").string();

	Check(cfg.EnsureFileExists(), "首次运行生成控制文件模板");
	cfg.Poll(0.0f, true);
	Check(cfg.fileFound && cfg.lastError.empty(), "模板文件可被解析（含 // 注释）: " + cfg.lastError);
	// The shipped template leaves the track empty on purpose ("留空 = 启动时不选曲",
	// GkRuntimeConfig.hpp); the assertion still expected the older hard-coded name.
	Check(cfg.selectedMotion.empty(), "模板默认 selected_motion 为空（启动时不选曲）");
	Check(std::fabs(cfg.playbackSpeedMultiplier - 1.0f) < 1e-4f, "模板默认 playback_speed_multiplier = 1.0");
	Check(std::fabs(cfg.eyeWeight - 0.7f) < 1e-4f, "模板默认 eye_weight = 0.7");
	Check(cfg.gazeMode == 0, "模板默认 gaze.mode = 0 (Auto)");
	Check(!cfg.HasPendingCommands(), "模板默认无待执行命令（seq = 0）");
	Check(cfg.facialMikuMouthFit == 1, "Miku mouth experiment is enabled by default in a fresh configuration");
	WriteTextFile(cfg.path, R"({"facial":{"miku_mouth_fit":true}})");
	cfg.Poll(0.0f, true);
	Check(cfg.facialMikuMouthFit == 1, "Miku mouth experiment can be explicitly enabled from the active config");
	WriteTextFile(cfg.path, R"({"facial":{"miku_mouth_fit":false,"character":"kcna"}})");
	cfg.Poll(0.0f, true);
	Check(cfg.facialMikuMouthFit == 0, "Miku mouth experiment retains an explicit off choice");
	Check(cfg.facialCharacterOverride == "auto", "legacy facial identity override cannot replace automatic actor identity");
}

// =============================================================================
// 3. command.seq 去重语义
// =============================================================================
static void TestCommandSeq()
{
	BeginTest("command_seq");

	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "gkmotion_cfg_seq";
	fs::remove_all(dir);
	fs::create_directories(dir);
	const fs::path file = dir / "control.json";

	RuntimeConfig cfg;
	cfg.rootDir = dir.string();
	cfg.path = file.string();

	WriteTextFile(file, R"({
      "command": { "seq": 1, "seek_seconds": 12.5, "play": true }
    })");
	cfg.Poll(0.0f, true);
	Check(cfg.HasPendingCommands(), "seq=1 首次出现应挂起命令");
	Check(cfg.cmdSeek && std::fabs(cfg.cmdSeekSeconds - 12.5f) < 1e-3f, "seek_seconds 解析正确");
	Check(cfg.cmdPlay, "play 标志解析正确");
	cfg.ClearCommands();
	Check(!cfg.HasPendingCommands(), "命令消费后清空");

	// 相同 seq：即使再次重读文件也不应重新触发
	cfg.Poll(0.0f, true);
	Check(!cfg.HasPendingCommands(), "相同 seq 不重复执行");

	// 递增 seq：重新触发
	WriteTextFile(file, R"({
      "command": { "seq": 2, "stop": true }
    })");
	cfg.Poll(0.0f, true);
	Check(cfg.HasPendingCommands() && cfg.cmdStop, "seq 递增后重新触发");
	Check(cfg.appliedCommandSeq == 2, "appliedCommandSeq 已更新为 2");
}

// =============================================================================
// 4. 骨骼覆盖集合
// =============================================================================
static void TestBoneOverride()
{
	BeginTest("bone_overrides");

	BoneOverrideSet global;
	const std::string globalText = R"({
      "enabled": true,
      "overrides": [
        { "bone": "LeftUpperArm", "humanoid_id": 13, "euler_deg": [-5.0, 0.0, 3.0] },
        { "humanoid_id": 10, "space": "world", "euler_deg": [10.0, 0.0, 0.0] }
      ]
    })";
	Check(global.LoadFromText(globalText, "global.json"), "全局覆盖解析: " + global.lastError);
	Check(global.offsets.size() == 2, "条目数为 2");
	Check(global.Find(13, "") != nullptr, "按 humanoid_id 命中");

	BoneOverrideSet motion;
	const std::string motionText = R"({
      "enabled": true,
      "overrides": [
        { "humanoid_id": 13, "euler_deg": [0.0, 0.0, 20.0] },
        { "bone": "Head", "humanoid_id": 10, "euler_deg": [0.0, 0.0, 0.0], "enabled": false }
      ]
    })";
	Check(motion.LoadFromText(motionText, "motion.json"), "曲目级覆盖解析");

	BoneOverrideSet eff;
	BoneOverrideSet::BuildEffective(global, motion, true, eff);
	Check(eff.offsets.size() == 2, "同键合并后条目数仍为 2");
	Check(eff.enabled, "有生效条目时集合处于启用态");
	const BoneRotationOffset* e13 = eff.Find(13, "LeftUpperArm");
	Check(e13 != nullptr && std::fabs(e13->eulerDeg.z - 20.0f) < 1e-3f,
	      "曲目级同键覆盖全局（z=20 而非 3）");
	Check(eff.Find(10, "Head") == nullptr, "被禁用的条目不可命中");

	BoneOverrideSet off;
	BoneOverrideSet::BuildEffective(global, motion, false, off);
	Check(!off.enabled && off.offsets.empty(), "总开关关闭 -> 集合为空且不生效");

	// 动态条目（快捷键微调）优先级最高
	BoneOverrideSet dyn;
	BoneOverrideSet::BuildEffective(global, motion, true, dyn);
	std::vector<BoneRotationOffset> dynamicEntries;
	BoneRotationOffset d;
	d.humanoidId = 13;
	d.eulerDeg = Vector3(0.0f, 0.0f, 90.0f);
	dynamicEntries.push_back(d);
	dyn.SetDynamic(dynamicEntries);
	const BoneRotationOffset* dyn13 = dyn.Find(13, "");
	Check(dyn13 != nullptr && dyn13->isDynamic && std::fabs(dyn13->eulerDeg.z - 90.0f) < 1e-3f,
	      "动态条目覆盖文件条目");

	// 按名称匹配 / 非法条目告警
	BoneOverrideSet byName;
	Check(byName.LoadFromText(R"({"overrides":[{"bone":"Head","euler_deg":[1,2,3]}]})", "n.json"),
	      "按名称解析");
	Check(byName.Find(-1, "Head") != nullptr, "按骨骼名命中");

	BoneOverrideSet bad;
	bad.LoadFromText(R"({"overrides":[{"euler_deg":[1,2,3]}]})", "bad.json");
	Check(bad.offsets.empty() && !bad.warnings.empty(), "缺少 bone/hid 的条目被跳过并产生告警");

	BoneOverrideSet malformed;
	Check(!malformed.LoadFromText("{ not json", "broken.json"), "语法错误必须返回 false");
	Check(!malformed.lastError.empty(), "语法错误带定位信息");

	// MMDD 对称骨骼与字典断言
	Check(BoneOverrideSet::GetSymmetricalHumanoidId(13) == 14, "左上臂对称骨骼为右上臂 (13 <-> 14)");
	Check(BoneOverrideSet::GetSymmetricalHumanoidId(14) == 13, "右上臂对称骨骼为左上臂 (14 <-> 13)");
	Check(BoneOverrideSet::GetSymmetricalHumanoidId(15) == 16, "左手肘对称骨骼为右手肘 (15 <-> 16)");
	Check(BoneOverrideSet::GetSymmetricalHumanoidId(1) == 2, "左大腿对称骨骼为右大腿 (1 <-> 2)");
	Check(BoneOverrideSet::GetSymmetricalHumanoidId(3) == 4, "左小腿对称骨骼为右小腿 (3 <-> 4)");
	Check(BoneOverrideSet::GetSymmetricalHumanoidId(24) == 39, "左拇指根对称骨骼为右拇指根 (24 <-> 39)");
	Check(BoneOverrideSet::GetSymmetricalHumanoidId(0) == -1, "脊柱/骨盆中心骨骼无对称对 (hid=0 -> -1)");

	// 内存中操作单条骨骼与 SaveToFile 测试
	BoneOverrideSet liveSet;
	auto& liveArm = liveSet.GetOrAddOffset(13, "LeftUpperArm");
	liveArm.eulerDeg = Vector3(10.0f, -5.0f, 30.0f);
	liveArm.spaceWorld = false;
	liveArm.enabled = true;
	Check(liveSet.offsets.size() == 1, "内存 GetOrAddOffset 成功添加条目");
	Check(liveSet.Find(13, "") != nullptr, "内存覆盖表可查找到新添加条目");

	const std::string tmpSavePath = "build/tests/test_saved_overrides.json";
	Check(liveSet.SaveToFile(tmpSavePath), "SaveToFile 成功写入临时文件");
	BoneOverrideSet reloaded;
	Check(reloaded.LoadFromFile(tmpSavePath), "LoadFromFile 成功重新读取保存的文件");
	const auto* r13 = reloaded.Find(13, "");
	Check(r13 != nullptr && std::fabs(r13->eulerDeg.x - 10.0f) < 1e-2f && std::fabs(r13->eulerDeg.z - 30.0f) < 1e-2f,
	      "SaveToFile 保存的数值重载后精确匹配");
	Check(liveSet.RemoveOffset(13, ""), "RemoveOffset 成功移除条目");
	Check(liveSet.offsets.empty(), "移除后覆盖集合为空");
}

// =============================================================================
// 5. 真实动作包载入与采样
// =============================================================================
static void TestMotionClip(const std::string& repoRoot)
{
	BeginTest("motion_clip");

	MotionClip clip;
	namespace fs = std::filesystem;
	fs::path p = fs::path(repoRoot) / "Motions" / "Like JENNIE" / "motion.gkmotion";
	if (!fs::exists(p)) p = fs::path(repoRoot) / "test_motion" / "motion.gkmotion";
	const std::string path = p.string();
	Check(clip.LoadFromFile(path), "载入 " + path);
	Check(clip.total_frames == 530, "总帧数 = 530");
	Check(clip.bone_count == 52, "骨骼数 = 52 (含 UpperChest)");
	Check(clip.duration_seconds > 17.0f && clip.duration_seconds < 18.0f, "时长 ≈ 17.67s");

	Vector3 pos;
	std::vector<Quaternion> rots;
	clip.Sample(5.0f, pos, rots);
	Check(rots.size() == clip.bone_count, "采样返回全部骨骼旋转");

	bool allFinite = std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z);
	for (const auto& q : rots)
	{
		if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
		{
			allFinite = false;
		}
	}
	Check(allFinite, "采样结果全部为有限值");

	// 采样连续性：相邻帧之间的四元数应保持接近（Slerp 不应出现跳变）
	clip.Sample(5.0f, pos, rots);
	std::vector<Quaternion> rots2;
	clip.Sample(5.0f + 1.0f / clip.fps, pos, rots2);
	float worst = 1.0f;
	for (size_t i = 0; i < rots.size(); ++i)
	{
		worst = std::min(worst, std::fabs(Quaternion::Dot(rots[i], rots2[i])));
	}
	Check(worst > 0.85f, "相邻帧姿态连续（最小点积 > 0.85）");
}

// =============================================================================
// 6. 覆盖注入的旋转合成
// =============================================================================
namespace fake
{
	std::unordered_map<void*, Quaternion> worldRot;
	std::unordered_map<void*, Vector3> worldPos;
	std::vector<std::pair<void*, Quaternion>> rotationCalls;
	int wristDiagnosticLines = 0;
	void WristDiagnosticLog(const char*, ...) { ++wristDiagnosticLines; }

	void SetWorldRot(void* t, float x, float y, float z, float w)
	{
		rotationCalls.emplace_back(t, Quaternion(x, y, z, w));
	}

	void SetWorldPos(void* t, float x, float y, float z) { worldPos[t] = Vector3(x, y, z); }

	void GetWorldRot(void* t, float* out)
	{
		Quaternion q = Quaternion::Identity();
		auto it = worldRot.find(t);
		if (it != worldRot.end()) q = it->second;
		out[0] = q.x;
		out[1] = q.y;
		out[2] = q.z;
		out[3] = q.w;
	}

	void GetWorldPos(void* t, float* out)
	{
		Vector3 v;
		auto it = worldPos.find(t);
		if (it != worldPos.end()) v = it->second;
		out[0] = v.x;
		out[1] = v.y;
		out[2] = v.z;
	}

	void* GetBone(void*, int hid)
	{
		return reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000 + static_cast<uintptr_t>(hid)));
	}

	void* GetComponentTransform(void*)
	{
		return reinterpret_cast<void*>(static_cast<uintptr_t>(0x20000));
	}
} // namespace fake

static MotionClip MakeSmallClip()
{
	MotionClip clip;
	clip.total_frames = 2;
	clip.fps = 30.0f;
	clip.duration_seconds = 2.0f / 30.0f;
	clip.bone_count = 2;
	clip.bones = {
		BoneMeta{13, "LeftUpperArm", false},
		BoneMeta{14, "RightUpperArm", false},
	};
	clip.root_positions = {Vector3(0, 0, 0), Vector3(0, 0, 0)};
	const Quaternion r = Quaternion::FromEulerDegreesXYZ(0.0f, 0.0f, 30.0f);
	clip.bone_rotations = {r, Quaternion::Identity(), r, Quaternion::Identity()};
	return clip;
}

static void WireControllerCallbacks(IdolController& ctrl)
{
	ctrl.fnGetBoneTransform = fake::GetBone;
	ctrl.fnGetComponentTransform = fake::GetComponentTransform;
	ctrl.fnSetWorldRotation = fake::SetWorldRot;
	ctrl.fnSetWorldPosition = fake::SetWorldPos;
	ctrl.fnGetWorldRotation = fake::GetWorldRot;
	ctrl.fnGetWorldPosition = fake::GetWorldPos;
}

static void TestOverrideInjection()
{
	BeginTest("override_injection");

	MotionClip clip = MakeSmallClip();
	IdolController ctrl;
	WireControllerCallbacks(ctrl);

	auto actor = reinterpret_cast<void*>(0x1);
	ctrl.BindActor(actor, clip, nullptr);
	Check(ctrl.currentBinding.bindPoseCaptured, "绑定姿态已捕获");
	Check(ctrl.currentBinding.boundBoneCount == 2, "绑定骨骼数 = 2");
	Check(ctrl.currentBinding.restRotations.size() == 2, "静息旋转基准已记录");

	const Quaternion R = clip.bone_rotations[0];

	// (1) 无覆盖：rTarget == R_vmd（静息基准为单位四元数）
	fake::rotationCalls.clear();
	ctrl.ApplyMotion(clip, 0.0f);
	Check(fake::rotationCalls.size() == 2, "两根骨骼均被写入旋转");
	Check(QuatNear(fake::rotationCalls[0].second, R), "无覆盖时 rTarget == R_vmd");
	Check(ctrl.lastAppliedOverrideCount == 0, "无覆盖时生效计数为 0");

	// (2) local 覆盖：rTarget == R * Q
	BoneOverrideSet ovr;
	std::vector<BoneRotationOffset> entries;
	BoneRotationOffset off;
	off.humanoidId = 13;
	off.eulerDeg = Vector3(0.0f, 0.0f, 45.0f);
	entries.push_back(off);
	ovr.SetDynamic(entries);
	ctrl.SetBoneOverrides(&ovr);

	fake::rotationCalls.clear();
	ctrl.ApplyMotion(clip, 0.0f);
	const Quaternion Q = Quaternion::FromEulerDegreesXYZ(0.0f, 0.0f, 45.0f);
	Check(QuatNear(fake::rotationCalls[0].second, R * Q), "local 空间：rTarget == R * Q");
	Check(ctrl.lastAppliedOverrideCount == 1, "命中 1 条覆盖");
	Check(QuatNear(fake::rotationCalls[1].second, Quaternion::Identity()), "未覆盖骨骼不受影响");

	// (3) world 覆盖：rTarget == Q * R
	entries[0].spaceWorld = true;
	ovr.SetDynamic(entries);
	fake::rotationCalls.clear();
	ctrl.ApplyMotion(clip, 0.0f);
	Check(QuatNear(fake::rotationCalls[0].second, Q * R), "world 空间：rTarget == Q * R");

	// (4) 集合禁用：完全还原 VMD 姿态
	ovr.enabled = false;
	fake::rotationCalls.clear();
	ctrl.ApplyMotion(clip, 0.0f);
	Check(QuatNear(fake::rotationCalls[0].second, R), "覆盖集合禁用 -> 严格还原原始姿态");

	// (5) 既有臂部偏移行为回归保护
	ctrl.SetBoneOverrides(nullptr);
	ctrl.armOffsetRoll = 10.0f;
	fake::rotationCalls.clear();
	ctrl.ApplyMotion(clip, 0.0f);
	const Quaternion armQ = Quaternion::FromEulerDegreesXYZ(0.0f, 0.0f, 10.0f);
	Check(QuatNear(fake::rotationCalls[0].second, R * armQ), "臂部 roll 偏移叠加保持既有行为");
	ctrl.armOffsetRoll = 0.0f;
}

static void TestGkClipWristOverrideSpaces()
{
	BeginTest("gkclip_wrist_override_spaces");
	MotionClip clip = MakeSmallClip();
	clip.format = MotionClip::Format::kGkClip;
	clip.version = 5;
	clip.bones = {{15, "LeftLowerArm", true}, {17, "LeftHand", true}};
	clip.bone_parent_hids = {-1, 15};
	const Quaternion parent = Quaternion::FromEulerDegreesXYZ(0, 55, 25);
	const Quaternion hand = Quaternion::FromEulerDegreesXYZ(15, 5, -20);
	clip.bone_rotations = {parent, hand, parent, hand};
	IdolController controller;
	WireControllerCallbacks(controller);
	controller.fnSetLocalRotation = fake::SetWorldRot;
	IdolBoneBinding binding;
	binding.actor = reinterpret_cast<void*>(0x901);
	binding.bindPoseCaptured = true;
	binding.boneTransforms = {reinterpret_cast<void*>(0x915), reinterpret_cast<void*>(0x917)};
	binding.restRotations.resize(2);
	BoneOverrideSet overrides;
	BoneRotationOffset offset;
	offset.humanoidId = 17;
	offset.eulerDeg = {30, 0, 0};
	overrides.SetDynamic({offset});
	controller.SetBoneOverrides(&overrides);
	const Quaternion q = offset.Resolve();
	fake::rotationCalls.clear();
	controller.ApplyMotionToBinding(binding, clip, 0);
	Check(fake::rotationCalls.size() == 2 && QuatNear(fake::rotationCalls[1].second, hand * q),
	      "gkclip wrist local override remains in the hand's local frame under a rotated parent");
	offset.spaceWorld = true;
	overrides.SetDynamic({offset});
	fake::rotationCalls.clear();
	controller.ApplyMotionToBinding(binding, clip, 0);
	Check(fake::rotationCalls.size() == 2 && QuatNear(parent * fake::rotationCalls[1].second, q * parent * hand),
	      "gkclip wrist world override rotates the final world orientation");
}

static void TestWristDiagnosticIsOneShot()
{
	BeginTest("wrist_diagnostic_is_one_shot");
	IdolController controller;
	controller.fnLogInfo = fake::WristDiagnosticLog;
	controller.fnGetLocalRotation = fake::GetWorldRot;
	controller.fnGetWorldRotation = fake::GetWorldRot;
	controller.fnGetLocalPosition = fake::GetWorldPos;
	controller.fnGetWorldPosition = fake::GetWorldPos;
	controller.fnSetLocalRotation = fake::SetWorldRot;
	IdolBoneBinding binding;
	binding.actor = reinterpret_cast<void*>(0x901);
	binding.leftArmAux.hand = reinterpret_cast<void*>(0x917);
	fake::rotationCalls.clear(); fake::wristDiagnosticLines = 0;
	controller.CaptureWristDiagnostic(binding, 20.23f, 0);
	Check(fake::wristDiagnosticLines == 0, "wrist diagnostics are silent until requested");
	controller.wristDiagnosticPending = true;
	controller.CaptureWristDiagnostic(binding, 20.23f, 0);
	Check(!controller.wristDiagnosticPending && controller.wristDiagnosticStatus == 2 && controller.wristDiagnosticCount == 1 &&
	      std::fabs(controller.wristDiagnosticTime - 20.23f) < 0.001f &&
	      fake::wristDiagnosticLines == 13 && fake::rotationCalls.empty(),
	      "wrist diagnostics capture a bounded read-only snapshot including missing bones");
	controller.CaptureWristDiagnostic(binding, 20.23f, 0);
	Check(fake::wristDiagnosticLines == 13, "wrist diagnostics do not repeat without another request");
	controller.wristDiagnosticPending = true;
	controller.CaptureWristDiagnostic(binding, 20.23f, 0);
	Check(controller.wristDiagnosticCount == 2 && fake::wristDiagnosticLines == 26,
	      "repeated capture at the same time increments the visible completion count");
	controller.fnLogInfo = nullptr; controller.wristDiagnosticPending = true; controller.wristDiagnosticStatus = 1;
	controller.CaptureWristDiagnostic(binding, 20.23f, 0);
	Check(!controller.wristDiagnosticPending && controller.wristDiagnosticStatus == 3,
	      "missing logger reports failure instead of a false completion");
}

static void TestWristTwistRedistribution()
{
	BeginTest("wrist_twist_redistribution");
	MotionClip clip = MakeSmallClip();
	clip.format = MotionClip::Format::kGkClip;
	clip.version = 5;
	clip.bones = {{15, "LeftLowerArm", true}, {17, "LeftHand", true}};
	clip.bone_parent_hids = {-1, 15};
	const Quaternion forearm = Quaternion::FromEulerDegreesXYZ(10, 50, 25);
	const Quaternion hand = Quaternion::FromEulerDegreesXYZ(-60, 20, 10);
	clip.bone_rotations = {forearm, hand, forearm, hand};
	IdolController controller;
	WireControllerCallbacks(controller);
	controller.fnSetLocalRotation = fake::SetWorldRot;
	IdolBoneBinding binding;
	binding.actor = reinterpret_cast<void*>(0x901);
	binding.bindPoseCaptured = true;
	binding.boneTransforms = {reinterpret_cast<void*>(0x915), reinterpret_cast<void*>(0x917)};
	binding.restRotations.resize(2);
	fake::rotationCalls.clear(); controller.ApplyMotionToBinding(binding, clip, 0);
	Check(fake::rotationCalls.size() == 2 && QuatNear(fake::rotationCalls[0].second, forearm) &&
	      QuatNear(fake::rotationCalls[1].second, hand), "zero share leaves the authored pose unchanged");
	controller.wristTwistShare = 0.5f;
	fake::rotationCalls.clear(); controller.ApplyMotionToBinding(binding, clip, 0);
	Check(fake::rotationCalls.size() == 2 && !QuatNear(fake::rotationCalls[0].second, forearm) &&
	      QuatNear(fake::rotationCalls[0].second * fake::rotationCalls[1].second, forearm * hand),
	      "twist share rotates the forearm while preserving the hand world orientation");
	controller.wristTwistShare = -0.5f;
	fake::rotationCalls.clear(); controller.ApplyMotionToBinding(binding, clip, 0);
	Check(fake::rotationCalls.size() == 2 && !QuatNear(fake::rotationCalls[0].second, forearm) &&
	      QuatNear(fake::rotationCalls[0].second * fake::rotationCalls[1].second, forearm * hand),
	      "negative twist share also preserves hand orientation");
	controller.wristTwistShare = 0.0f;
	controller.forearmAxisAdjustmentDeg = 30.0f;
	fake::rotationCalls.clear(); controller.ApplyMotionToBinding(binding, clip, 0);
	const auto axisCorrection = Quaternion::FromEulerDegreesXYZ(30, 0, 0);
	Check(fake::rotationCalls.size() == 2 && QuatNear(fake::rotationCalls[0].second, forearm * axisCorrection) &&
	      QuatNear(fake::rotationCalls[0].second * fake::rotationCalls[1].second, forearm * hand),
	      "fixed forearm axis adjustment preserves the hand world orientation");
	const Quaternion share = IdolController::WristTwistShareRotation(hand, 0.5f);
	const Quaternion neg(-hand.x, -hand.y, -hand.z, -hand.w);
	Check(QuatNear(share, IdolController::WristTwistShareRotation(neg, 0.5f)), "twist share is quaternion-sign invariant");
	clip.bone_parent_hids[1] = -1;
	fake::rotationCalls.clear(); controller.ApplyMotionToBinding(binding, clip, 0);
	Check(fake::rotationCalls.size() == 2 && QuatNear(fake::rotationCalls[0].second, forearm) &&
	      QuatNear(fake::rotationCalls[1].second, hand), "missing forearm parent disables twist sharing");
}

static void TestAuxQuaternionSignInvariance()
{
	BeginTest("auxiliary model uses rotation, not quaternion sign");
	static Quaternion rotations[32];
	IdolController ctrl;
	ctrl.fnGetLocalRotation = [](void* t, float* out)
	{
		const auto q = rotations[reinterpret_cast<uintptr_t>(t)];
		out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
	};
	ctrl.fnSetLocalRotation = [](void* t, float x, float y, float z, float w)
	{
		rotations[reinterpret_cast<uintptr_t>(t)] = {x, y, z, w};
	};
	auto handle = [](int id) { return reinterpret_cast<void*>(static_cast<uintptr_t>(id)); };
	IdolBoneBinding binding;
	binding.leftArmAux.upperArm = handle(1); binding.leftArmAux.foreArm = handle(2); binding.leftArmAux.hand = handle(3);
	binding.rightArmAux.upperArm = handle(4); binding.rightArmAux.foreArm = handle(5); binding.rightArmAux.hand = handle(6);
	binding.leftArmAux.upperArmRollH = handle(10); binding.leftArmAux.upperArmH = handle(11);
	binding.leftArmAux.foreArmRollH = handle(12); binding.leftArmAux.foreArmH = handle(13); binding.leftArmAux.handH = handle(14);
	binding.rightArmAux.upperArmRollH = handle(15); binding.rightArmAux.upperArmH = handle(16);
	binding.rightArmAux.foreArmRollH = handle(17); binding.rightArmAux.foreArmH = handle(18); binding.rightArmAux.handH = handle(19);
	rotations[1] = Quaternion::FromEulerDegreesXYZ(30, 20, 10);
	rotations[2] = Quaternion::FromEulerDegreesXYZ(-20, 40, 15);
	rotations[3] = Quaternion::FromEulerDegreesXYZ(35, 0, 0);
	rotations[4] = Quaternion::FromEulerDegreesXYZ(-25, 35, 5);
	rotations[5] = Quaternion::FromEulerDegreesXYZ(15, -45, 20);
	rotations[6] = Quaternion::FromEulerDegreesXYZ(-35, 0, 0);
	ctrl.ApplyAuxModelToBinding(binding);
	Quaternion expected[10];
	for (int i = 0; i < 10; ++i) expected[i] = rotations[10 + i];
	Check(QuatNear(rotations[14], Quaternion::FromEulerDegreesXYZ(23.1f, 0, 0)) &&
	      QuatNear(rotations[19], Quaternion::FromEulerDegreesXYZ(-23.1f, 0, 0)),
	      "positive-hemisphere wrist input retains the calibrated half-angle model");
	for (int i = 1; i <= 6; ++i)
	{
		const auto q = rotations[i]; rotations[i] = {-q.x, -q.y, -q.z, -q.w};
	}
	ctrl.ApplyAuxModelToBinding(binding);
	for (int i = 0; i < 10; ++i)
		Check(QuatNear(expected[i], rotations[10 + i]), "equivalent q and -q give the same auxiliary pose for channel " + std::to_string(i));
	// At the exact 180-degree boundary, w cannot choose a hemisphere; the
	// vector components must provide a deterministic tie break.
	rotations[3] = {0.8f, 0.6f, 0, 0};
	ctrl.ApplyAuxModelToBinding(binding);
	const auto halfTurn = rotations[14];
	rotations[3] = {-0.8f, -0.6f, 0, 0};
	ctrl.ApplyAuxModelToBinding(binding);
	Check(QuatNear(halfTurn, rotations[14]), "exact half turns are also invariant to quaternion sign");
}

static void TestQuartzAuxiliaryBones()
{
	BeginTest("quartz_auxiliary_bones");

	const Vector3 uLeft(-1.0f, 0.0f, 0.0f);
	const Quaternion qIdentity = Quaternion::Identity();

	// 测试 1: 静息增量不变性（无动态旋转时，所有输出均为 Identity）
	auto resZero = IdolController::SolveQuartzAuxiliaryBones(qIdentity, uLeft, true);
	Check(QuatNear(resZero.qMainLocal, qIdentity), "静息不变性：主骨骼局部增量为 Identity");
	Check(QuatNear(resZero.qRollHLocal, qIdentity), "静息不变性：_Roll_H 局部增量为 Identity");
	Check(QuatNear(resZero.qJointHLocal, qIdentity), "静息不变性：_H 半角支撑骨局部增量为 Identity");

	// 测试 2: 肘关节纯 Y 轴弯曲 90度，半角骨严格转动 45度 (零 Roll, 零 Z 轴偏角)
	Quaternion qBend90Y = Quaternion::FromEulerDegreesXYZ(0.0f, 90.0f, 0.0f);
	auto resElbow90 = IdolController::SolveQuartzAuxiliaryBones(qBend90Y, uLeft, false);
	Quaternion qExpected45Y = Quaternion::FromEulerDegreesXYZ(0.0f, 45.0f, 0.0f);
	Check(QuatNear(resElbow90.qJointHLocal, qExpected45Y), "屈肘 90度：半角骨纯局部保角插值精准处于 45度");
	Check(QuatNear(resElbow90.qRollHLocal, qIdentity), "前臂 1-DOF 铰链：_Roll_H 保持 Identity 零额外扭转");

	// 测试 3: 肘关节深屈曲 120度（双手抱胸极限场景），半角骨严格处于 60度且无任何杂轴翻转
	Quaternion qBend120Y = Quaternion::FromEulerDegreesXYZ(0.0f, 120.0f, 0.0f);
	auto resElbow120 = IdolController::SolveQuartzAuxiliaryBones(qBend120Y, uLeft, false);
	Quaternion qExpected60Y = Quaternion::FromEulerDegreesXYZ(0.0f, 60.0f, 0.0f);
	Check(QuatNear(resElbow120.qJointHLocal, qExpected60Y), "深屈肘 120度：半角骨严格处于 60度无符号翻转与奇异点");

	// 测试 4: 肩袖纯 Z 轴抬臂 60度，肩袖半角骨精确处于 30度
	Quaternion qArmRaise60Z = Quaternion::FromEulerDegreesXYZ(0.0f, 0.0f, 60.0f);
	auto resArmRaise = IdolController::SolveQuartzAuxiliaryBones(qArmRaise60Z, uLeft, true);
	Quaternion qExpected30Z = Quaternion::FromEulerDegreesXYZ(0.0f, 0.0f, 30.0f);
	Check(QuatNear(resArmRaise.qJointHLocal, qExpected30Z), "肩袖抬臂 60度：肩袖半角骨精准处于 30度");

	// 测试 5: 大臂纯 Roll 扭转 60度（主骨骼 100%，_Roll_H 50% 均摊）
	Quaternion qArmRoll60X = Quaternion::FromEulerDegreesXYZ(60.0f, 0.0f, 0.0f); // 沿 [-1, 0, 0] 轴扭转
	auto resArmRoll = IdolController::SolveQuartzAuxiliaryBones(qArmRoll60X, uLeft, true);
	Quaternion qExpected30X = Quaternion::FromEulerDegreesXYZ(30.0f, 0.0f, 0.0f);
	Check(QuatNear(resArmRoll.qMainLocal, qArmRoll60X), "大臂主骨骼承担 100% 原始动力学 Twist");
	Check(QuatNear(resArmRoll.qRollHLocal, qExpected30X), "_Roll_H 承载 100% Bend + 50% (30度) Twist，维持饱满袖筒");
	Check(QuatNear(resArmRoll.qJointHLocal, qIdentity), "大臂 Roll 分流：纯 Roll 不对 _H 弯曲半角产生干扰");

	// 测试 6: 大臂复合运动（抬臂 60度 + 扭转 40度: q = qBend * qRoll）解耦验证
	Quaternion qArmRoll40X = Quaternion::FromEulerDegreesXYZ(40.0f, 0.0f, 0.0f);
	Quaternion qComposite = qArmRaise60Z * qArmRoll40X;
	auto resComposite = IdolController::SolveQuartzAuxiliaryBones(qComposite, uLeft, true);
	Quaternion qExpectedRollH = qArmRaise60Z * Quaternion::FromEulerDegreesXYZ(20.0f, 0.0f, 0.0f);
	Check(QuatNear(resComposite.qMainLocal, qComposite), "复合运动解耦：主骨骼承担 100% 原始复合位姿");
	Check(QuatNear(resComposite.qRollHLocal, qExpectedRollH), "复合运动解耦：_Roll_H 准确承载 100% 抬臂 + 50% 扭转");
	Check(QuatNear(resComposite.qJointHLocal, qExpected30Z), "复合运动解耦：_H 准确提取 50% (30度) 纯保角侧摆半角");
}

// =============================================================================
// 6b. AuxGroundTruth —— 官方辅助骨真值直灌数据源（aux.mode=3 的数据链路）
// =============================================================================
// 这条链路存在的意义：不要再离线假设，实机录到什么就灌什么。
static float g_audioPos = -1.0f;
static float g_lastSeekArg = -1.0f;
static int g_seekCalls = 0;
static int g_playCalls = 0;
static int g_resumeCalls = 0;
static float g_lastResumeArg = -1.0f;

static void FakePlayAudio(const std::string&)
{
	++g_playCalls;
	g_audioPos = 0.0f;
}

static void FakePauseAudio()
{
}

static void FakeStopAudio() { g_audioPos = -1.0f; }
// 故意模拟「MCI 在 play from X 之后 position 从 0 重新计数」这种最不利语义
static void FakeSeekAudio(float seconds)
{
	++g_seekCalls;
	g_lastSeekArg = seconds;
	g_audioPos = 0.0f;
}

static void FakeResumeAudio(float seconds)
{
	++g_resumeCalls;
	g_lastResumeArg = seconds;
	if (g_audioPos < 0.0f) g_audioPos = seconds;
}

static float FakeGetAudioTime()
{
	if (g_audioPos < 0.0f) return -1.0f;
	g_audioPos += 1.0f / 30.0f;
	return g_audioPos;
}

// =============================================================================
// 9. 真值标定辅助骨模型离线回归（Plan A）
// =============================================================================
// 这里的期望值不是由公式算出来的，而是 GakuRecorder 从实机内存逐帧录下来的官方值。
// 用例断言：
//   (a) 单轴的干净性 —— 模型输出必须是绕辅助骨自身轴的纯旋转，另外两分量恒为 0；
//   (b) 数值一致性 —— twist 通道误差 < 0.01°，surrogate 通道误差 < 0.25°；
//   (c) 源骨语义 —— 每根辅助骨只能由标定出来的那根源骨驱动（例如前臂辅助骨由 Hand 驱动）。
// 注意：此用例的目的从来不是「证明数学自洽」，而是防止真值回归。实机裁决优先。
static void TestNativeTalkOwnership()
{
	BeginTest("native_talk_ownership");
	static bool enabled[4];
	static void* slots[2];
	enabled[0]=true;enabled[1]=false;enabled[2]=true;enabled[3]=true;
	slots[0]=&enabled[0];slots[1]=&enabled[1];
	MotionPlayer player;player.clip=MakeSmallClip();WireControllerCallbacks(player.controller);
	player.controller.faceApi.getTalk=[](void*,int slot) {return slots[slot];};
	player.controller.faceApi.readTalk=[](void* talk,bool& value) {value=*static_cast<bool*>(talk);return true;};
	player.controller.faceApi.writeTalk=[](void* talk,bool value) {*static_cast<bool*>(talk)=value;return true;};
	player.state=PlaybackState::Preview;
	const auto actor=reinterpret_cast<void*>(0x1);
	player.Tick(0,actor);
	Check(!enabled[0]&&!enabled[1],"body-only Preview suppresses regular and CRI native talk without facial API");
	player.state=PlaybackState::Paused;player.Tick(0,actor);player.Seek(0);
	Check(!enabled[0]&&!enabled[1],"pause and seek retain native talk ownership");
	player.Stop();player.Stop();
	Check(enabled[0]&&!enabled[1],"Stop restores original enabled and disabled talk states once");
	player.Play();player.Tick(0,actor);
	slots[0]=&enabled[2];player.Tick(0,actor);
	Check(!enabled[2],"replacement talk component is captured and suppressed");
	player.Stop();Check(enabled[2]&&!enabled[0],"Stop restores only the live component, never a stale replaced object");
	player.Play();player.Tick(0,actor);player.AbandonScene();
	Check(!enabled[2],"scene abandonment discards talk snapshots without touching stale actors");
}

static void TestPlayerSeekSync()
{
	BeginTest("player_seek_sync");

	MotionPlayer player;
	player.clip = MakeSmallClip();
	player.clip.total_frames = 600;
	player.clip.fps = 30.0f;
	player.clip.duration_seconds = 20.0f;
	player.clip.bone_count = 1;
	player.clip.bones = {BoneMeta{13, "LeftUpperArm", false}};
	player.clip.root_positions.assign(600, Vector3(0, 0, 0));
	player.clip.bone_rotations.assign(600, Quaternion::Identity());
	WireControllerCallbacks(player.controller);

	player.fnPlayAudio = FakePlayAudio;
	player.fnPauseAudio = FakePauseAudio;
	player.fnStopAudio = FakeStopAudio;
	player.fnGetAudioTime = FakeGetAudioTime;
	player.fnSeekAudio = FakeSeekAudio;
	player.fnResumeAudio = FakeResumeAudio;

	auto actor = reinterpret_cast<void*>(0x1);

	// A. 正常播放 1 秒
	g_playCalls = 0;
	g_seekCalls = 0;
	g_resumeCalls = 0;
	g_audioPos = -1.0f;
	player.Play();
	Check(g_playCalls == 1, "Play 触发音频播放");
	for (int i = 0; i < 30; ++i) player.Tick(1.0f / 30.0f, actor, nullptr, nullptr);
	Check(player.currentTime > 0.8f && player.currentTime < 1.4f,
	      "正常播放 1 秒后 currentTime ≈ 1.0s");
	Check(g_seekCalls == 0, "从 0 起播时无需额外音频跳转");

	// B. 播放中跳转到 10s：音频必须同步跳转，且动作不能被拉回
	player.Seek(10.0f);
	Check(g_seekCalls == 1 && std::fabs(g_lastSeekArg - 10.0f) < 1e-3f,
	      "Seek 已同步下发音频跳转（10s）");
	for (int i = 0; i < 20; ++i) player.Tick(1.0f / 30.0f, actor, nullptr, nullptr);
	Check(player.currentTime > 9.5f, "跳转后不会被音频时钟拉回起点（settle 校正生效）");
	Check(player.currentTime < 11.5f, "跳转后时间推进不超过合理范围");

	const float tAfterSettle = player.currentTime;
	for (int i = 0; i < 30; ++i) player.Tick(1.0f / 30.0f, actor, nullptr, nullptr);
	Check(std::fabs((player.currentTime - tAfterSettle) - 1.0f) < 0.2f,
	      "跳转后每帧推进速率与音频一致（1 秒 ≈ 1.0s）");

	// C. 暂停 / 继续：断点续播，而不是从头重放
	player.Pause();
	const float tPaused = player.currentTime;
	player.Resume();
	Check(g_resumeCalls == 1, "Resume 走断点续播回调");
	Check(std::fabs(g_lastResumeArg - tPaused) < 1e-3f, "续播位置等于暂停时刻");
	Check(g_playCalls == 1, "Resume 未重新触发「从头播放」");

	// D. 非循环播到末尾后自动停止
	player.loop = false;
	player.Seek(19.95f);
	for (int i = 0; i < 20 && player.state == PlaybackState::Playing; ++i)
	{
		player.Tick(1.0f / 30.0f, actor, nullptr, nullptr);
	}
	Check(player.state == PlaybackState::Stopped, "非循环模式播放到结尾后自动停止");
	Check(player.clip.total_frames == 600, "Stop 后保留已载入的 clip 数据（不被抹零）");

	// E. 活跃 Actor 实例变更生命周期安全保护
	auto newActor = reinterpret_cast<void*>(0x2);
	player.OnActorChanged(newActor);
	Check(player.controller.currentBinding.actor == actor,
	      "同场景其他角色加入不清空正在驱动的绑定");
	player.OnActorChanged(nullptr);
	Check(player.controller.bindings.empty(), "OnActorChanged 彻底清空旧 Actor 悬垂绑定");
	Check(player.controller.currentBinding.actor == nullptr, "currentBinding 干净复位");
}

// =============================================================================
// 7b. 播放速度倍率与音频时钟容差（scratch/playback_speed_fix.md 回归）
// =============================================================================
// 脚本化音频时钟：脚本队列非空时每次查询弹出一个值（精确控制 settle 采样），
// 耗尽后从 g_scriptAudioPos 起每次查询步进 1/30 秒。
static std::vector<float> g_audioScript;
static float g_scriptAudioPos = -1.0f;

static float ScriptedGetAudioTime()
{
	if (!g_audioScript.empty())
	{
		const float v = g_audioScript.front();
		g_audioScript.erase(g_audioScript.begin());
		return v;
	}
	g_scriptAudioPos += 1.0f / 30.0f;
	return g_scriptAudioPos;
}

// 抖动音频时钟：每次查询步进 1/30 秒；到达指定查询序号时回退 50ms，
// 模拟 MCI position 偏发回退（毫秒量化抖动）。
static float g_jitterAudioPos = -1.0f;
static int g_jitterQueries = 0;
static int g_jitterBackstepQuery = -1;

static float JitterGetAudioTime()
{
	if (g_jitterAudioPos < 0.0f) return -1.0f;
	++g_jitterQueries;
	g_jitterAudioPos += 1.0f / 30.0f;
	if (g_jitterBackstepQuery > 0 && g_jitterQueries == g_jitterBackstepQuery)
	{
		g_jitterAudioPos -= 0.05f;
	}
	return g_jitterAudioPos;
}

static void SetupLongClipPlayer(MotionPlayer& player)
{
	player.clip = MakeSmallClip();
	player.clip.total_frames = 600;
	player.clip.fps = 30.0f;
	player.clip.duration_seconds = 20.0f;
	player.clip.bone_count = 1;
	player.clip.bones = {BoneMeta{13, "LeftUpperArm", false}};
	player.clip.root_positions.assign(600, Vector3(0, 0, 0));
	player.clip.bone_rotations.assign(600, Quaternion::Identity());
	WireControllerCallbacks(player.controller);
}

static void TestPlaybackSpeedAndClock()
{
	BeginTest("playback_speed_and_clock");
	auto actor = reinterpret_cast<void*>(0x1);

	// ---- A. 无音频：本地时钟按倍率推进 ----
	{
		MotionPlayer player;
		SetupLongClipPlayer(player);

		player.playbackSpeedMultiplier = 2.0f;
		player.Play();
		for (int i = 0; i < 30; ++i) player.Tick(1.0f / 30.0f, actor);
		Check(std::fabs(player.currentTime - 2.0f) < 0.01f, "无音频时倍率 2.0：1 秒推进 2.0s");

		player.Stop();
		player.playbackSpeedMultiplier = 0.5f;
		player.Play();
		for (int i = 0; i < 30; ++i) player.Tick(1.0f / 30.0f, actor);
		Check(std::fabs(player.currentTime - 0.5f) < 0.01f, "无音频时倍率 0.5：1 秒推进 0.5s");
	}

	// ---- B. settle 结束连采 3 次取平均（抑制 MCI 抖动）----
	{
		MotionPlayer player;
		SetupLongClipPlayer(player);
		player.fnGetAudioTime = ScriptedGetAudioTime;

		// settle 第 3 帧连续 3 次采样：0.010 / 0.000 / 0.020，平均 = 0.010
		g_audioScript = {0.010f, 0.000f, 0.020f};
		g_scriptAudioPos = 0.020f;
		player.Play();
		for (int i = 0; i < 3; ++i) player.Tick(1.0f / 30.0f, actor);
		Check(std::fabs(player.audioSeekOffset - 0.09f) < 1e-3f,
		      "settle 采样取平均：offset = 0.1 - avg(0.010) = 0.090");

		player.Tick(1.0f / 30.0f, actor);
		const float expected = 0.020f + (1.0f / 30.0f) + 0.09f;
		Check(std::fabs(player.currentTime - expected) < 1e-3f,
		      "settle 结束后按「音频 + offset」推进");
	}

	// ---- C. 音频时钟回退（MCI 抖动）不回拉动 ----
	{
		MotionPlayer player;
		SetupLongClipPlayer(player);
		player.fnGetAudioTime = JitterGetAudioTime;

		g_jitterAudioPos = 0.0f;
		g_jitterQueries = 0;
		g_jitterBackstepQuery = -1;
		player.Play();
		for (int i = 0; i < 33; ++i) player.Tick(1.0f / 30.0f, actor);
		const float before = player.currentTime;

		g_jitterBackstepQuery = g_jitterQueries + 1; // 下一次查询回退 50ms
		player.Tick(1.0f / 30.0f, actor);
		Check(std::fabs(player.currentTime - before) < 1e-4f,
		      "音频 position 回退时 currentTime 保持不变（不被拉回）");

		for (int i = 0; i < 5; ++i) player.Tick(1.0f / 30.0f, actor);
		Check(player.currentTime > before, "音频恢复前进后时钟继续推进");
		Check(player.currentTime < before + 0.3f, "恢复后不会异常前跳");
	}

	// ---- D. Unsupported audio rates fall back to the multiplied local clock. ----
	{
		MotionPlayer player;
		SetupLongClipPlayer(player);
		player.fnGetAudioTime = ScriptedGetAudioTime;

		g_audioScript.clear();
		g_scriptAudioPos = 0.0f;
		player.playbackSpeedMultiplier = 2.0f;
		player.Play();
		for (int i = 0; i < 13; ++i) player.Tick(1.0f / 30.0f, actor);
		const float t1 = player.currentTime;
		for (int i = 0; i < 30; ++i) player.Tick(1.0f / 30.0f, actor);
		const float t2 = player.currentTime;
		Check(std::fabs((t2 - t1) - 2.0f) < 0.01f,
		      "非正常倍速禁用音频主时钟并按 2.0x 推进");
	}

	// ---- E. 配置解析与夹取 ----
	{
		namespace fs = std::filesystem;
		const fs::path dir = fs::temp_directory_path() / "gkmotion_cfg_speed";
		fs::remove_all(dir);
		fs::create_directories(dir);
		const fs::path file = dir / "control.json";

		RuntimeConfig cfg;
		cfg.rootDir = dir.string();
		cfg.path = file.string();

		WriteTextFile(file, R"({ "playback_speed_multiplier": 1.75 })");
		cfg.Poll(0.0f, true);
		Check(std::fabs(cfg.playbackSpeedMultiplier - 1.75f) < 1e-3f, "playback_speed_multiplier 解析正确");

		WriteTextFile(file, R"({ "playback_speed_multiplier": 0.0 })");
		cfg.Poll(0.0f, true);
		Check(std::fabs(cfg.playbackSpeedMultiplier - 0.1f) < 1e-3f, "倍率下限夹取到 0.1");

		WriteTextFile(file, R"({ "playback_speed_multiplier": 12.0 })");
		cfg.Poll(0.0f, true);
		Check(std::fabs(cfg.playbackSpeedMultiplier - 5.0f) < 1e-3f, "倍率上限夹取到 5.0");
	}
}

// =============================================================================
// 8. 曲目库扫描
// =============================================================================
static void TestMotionLibrary(const std::string& repoRoot)
{
	BeginTest("motion_library");

	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "gkmotion_lib_test";
	fs::remove_all(root);
	fs::create_directories(root / "song_b");
	fs::create_directories(root / "song_a");
	fs::create_directories(root / "not_a_song");

	fs::path src = fs::path(repoRoot) / "Motions" / "Like JENNIE" / "motion.gkmotion";
	if (!fs::exists(src)) src = fs::path(repoRoot) / "Motions" / "Fighting My Way" / "motion.gkmotion";
	if (!fs::exists(src)) src = fs::path(repoRoot) / "test_motion" / "motion.gkmotion";
	fs::copy_file(src, root / "song_a" / "motion.gkmotion", fs::copy_options::overwrite_existing);
	fs::copy_file(src, root / "song_b" / "motion.gkmotion", fs::copy_options::overwrite_existing);
	WriteTextFile(root / "song_a" / "info.json",
	              R"({"title":"甲曲","total_frames":530,"fps":30.0,"duration_seconds":17.6667,"bone_count":51})");

	std::vector<MotionEntry> motions;
	std::string err;
	Check(ScanMotionLibrary(root.string(), motions, err), "曲目库扫描成功: " + err);
	Check(motions.size() == 2, "只有含 motion.gkmotion 的目录被收录");
	Check(motions[0].name == "song_a" && motions[1].name == "song_b", "按名称稳定排序");
	Check(motions[0].metadataLoaded && motions[0].title == "甲曲", "info.json 标题读取（UTF-8）");
	Check(std::fabs(motions[0].durationSeconds - 17.6667f) < 1e-2f, "info.json 时长读取");

	Check(FindMotionIndex(motions, "song_b") == 1, "按目录名查找");
	Check(FindMotionIndex(motions, "甲曲") == 0, "按标题查找");
	Check(FindMotionIndex(motions, "SONG_A") == 0, "大小写不敏感查找");
	Check(FindMotionIndex(motions, "no_such") == -1, "未命中返回 -1");

	std::string err2;
	std::vector<MotionEntry> emptyList;
	Check(ScanMotionLibrary((root / "not_a_song").string(), emptyList, err2) && emptyList.empty() && err2.empty(),
	      "可访问的空曲库扫描成功");
}

// =============================================================================
// 9. Core 端到端（声明式热加载 + 一次性命令）
// =============================================================================
static void TestCoreIntegration(const std::string& repoRoot)
{
	BeginTest("core_integration");

	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "gkmotion_core_test";
	fs::remove_all(root);
	fs::create_directories(root / "motions" / "song_a");
	fs::create_directories(root / "motions" / "song_b");

	fs::path src = fs::path(repoRoot) / "Motions" / "Like JENNIE" / "motion.gkmotion";
	if (!fs::exists(src)) src = fs::path(repoRoot) / "Motions" / "Fighting My Way" / "motion.gkmotion";
	if (!fs::exists(src)) src = fs::path(repoRoot) / "test_motion" / "motion.gkmotion";
	fs::copy_file(src, root / "motions" / "song_a" / "motion.gkmotion", fs::copy_options::overwrite_existing);
	fs::copy_file(src, root / "motions" / "song_b" / "motion.gkmotion", fs::copy_options::overwrite_existing);
	WriteTextFile(root / "motions" / "song_a" / "info.json",
	              R"({"title":"Song A","total_frames":530,"fps":30.0,"duration_seconds":17.6667,"bone_count":51})");

	// 预置控制文件（避免使用自动生成的模板）
	const fs::path control = root / "GakuMotion" / "control.json";
	WriteTextFile(control, R"({
      "poll_interval_seconds": 0.5,
      "playback_speed_multiplier": 1.5,
      "selected_motion": "song_b",
      "gaze": { "mode": 2, "desktop_enabled": true, "eye_weight": 0.25, "head_weight": 0.5 },
      "use_rest_rotations": false,
      "overrides": { "enabled": false }
    })");

	auto& core = Core::Instance();
	core.Initialize(root.string());

	Check(core.motions.size() == 2, "Core 扫描到 2 个曲目");
	Check(core.currentMotionIndex == 1, "按 selected_motion 选中 song_b");
	Check(static_cast<int>(core.lookAt.gazeMode.load()) == 2, "视线模式按声明应用 (DesktopOn)");
	Check(std::fabs(core.lookAt.eyeWeight.load() - 0.25f) < 1e-4f, "眼神权重按声明应用");
	Check(!core.masterOverridesEnabled, "骨骼覆盖总开关按声明关闭");
	Check(!core.player.controller.useRestRotations, "use_rest_rotations 按声明应用");
	Check(std::fabs(core.player.playbackSpeedMultiplier - 1.5f) < 1e-3f,
	      "playback_speed_multiplier 按声明应用 (1.5x)");
	Check(core.player.clip.total_frames == 530, "曲目已载入（530 帧）");
	Check(core.player.state == PlaybackState::Stopped,
	      "startup selection only loads clip; home Animator remains official");

	// 热加载：切曲 + 一次性命令
	WriteTextFile(control, R"({
      "poll_interval_seconds": 0.5,
      "playback_speed_multiplier": 0.75,
      "selected_motion": "song_a",
      "gaze": { "mode": 0, "eye_weight": 0.7 },
      "overrides": { "enabled": true },
      "command": { "seq": 1, "seek_seconds": 3.0 }
    })");
	core.OnLateUpdate(nullptr, nullptr, nullptr, 0.6f);

	Check(core.currentMotionIndex == 0, "热加载后切换到 song_a");
	Check(static_cast<int>(core.lookAt.gazeMode.load()) == 0, "视线模式随文件更新");
	Check(core.masterOverridesEnabled, "覆盖总开关随文件更新");
	Check(std::fabs(core.player.currentTime - 3.0f) < 1e-3f, "command.seek_seconds 生效");
	Check(std::fabs(core.player.playbackSpeedMultiplier - 0.75f) < 1e-3f,
	      "playback_speed_multiplier 随热加载更新 (0.75x)");
	Check(core.config.appliedCommandSeq == 1, "命令 seq 已记录");

	// 相同 seq 不得重复执行
	core.player.Seek(1.0f);
	core.OnLateUpdate(nullptr, nullptr, nullptr, 0.6f);
	Check(std::fabs(core.player.currentTime - 1.0f) < 1e-3f, "相同 seq 不重复执行");

	// 曲目级覆盖：随曲目切换被加载
	WriteTextFile(root / "motions" / "song_a" / "bone_overrides.json",
	              R"({"enabled":true,"overrides":[{"humanoid_id":13,"euler_deg":[0,0,15]}]})");
	core.LoadOverrides(true);
	Check(core.motionOverrides.offsets.size() == 1, "曲目级覆盖文件被加载");
	Check(core.effectiveOverrides.Find(13, "") != nullptr, "合成结果可命中曲目级条目");

	// 总开关关闭后，合成结果必须为空
	core.ToggleOverrides();
	Check(!core.masterOverridesEnabled && core.effectiveOverrides.offsets.empty(),
	      "总开关切换后合成结果清空");

	std::printf("  状态摘要: %s\n", core.StatusSummary().c_str());
}

// =============================================================================
// 10. 热键上升沿检测（scratch/hotkey_phantom_switch 回归）
// =============================================================================
// 对应实机故障：暂停一段时间后无人操作，日志出现
//   [Config] switch prev -> like JENNIE
//   [Config] switch next -> Fighting My Way
// 旧实现用 `GetAsyncKeyState(vk) & 1`，低位bit 语义是「自上次调用以来是否被
// 按下过」，是粘滞的：帧循环停止期间（掉帧/切场景/失焦）在别处敲的键会堆积，
// 恢复后第一次读取即补发。这里穷举验证新契约。
static void TestHotkeyEdgeGate()
{
	BeginTest("hotkey_edge_gate");
	{
		IdolController ctrl;
		const int modes[] = {kAuxSilence, kAuxIdentity, kAuxReplay, kAuxLegacy, kAuxNormal};
		for (int mode : modes)
		{
			ctrl.CycleAuxDriveMode();
			Check(ctrl.auxDriveMode == mode, "auxiliary diagnostic hotkey cycles each mode and returns to normal");
		}
	}

	constexpr int kComma = 0xBC; // VK_OEM_COMMA
	constexpr int kPeriod = 0xBE; // VK_OEM_PERIOD
	// 本 TU 不含 windows.h（离线测试不引入 Win32 头），直接用 VK_F9 的虚拟键码。
	constexpr int kF9 = 0x78;

	// A. 正常单击 -> 恰好一次触发
	{
		HotkeyGate gate;
		gate.SetForegroundWindowFocused(true);
		gate.NewFrame();
		Check(gate.Pressed(kComma, false) == false, "未按下时不触发");
		gate.NewFrame();
		Check(gate.Pressed(kComma, true) == true, "按下瞬间触发一次");
		gate.NewFrame();
		Check(gate.Pressed(kComma, true) == false, "按住不放（系统自动重复）不再重复触发");
		gate.NewFrame();
		Check(gate.Pressed(kComma, false) == false, "松开不触发");
		gate.NewFrame();
		Check(gate.Pressed(kComma, true) == true, "第二次按下再触发一次");
	}

	// B. 失焦期间的按键永不补发（幽灵切换的直接回归点）
	{
		HotkeyGate gate;
		gate.SetForegroundWindowFocused(true);
		gate.NewFrame();
		// 失焦
		gate.SetForegroundWindowFocused(false);
		gate.NewFrame();
		// 帧循环停摆期间，用户在别的程序里敲了逗号和句号
		for (int i = 0; i < 300; ++i)
		{
			gate.NewFrame();
			gate.Pressed(kComma, true);
			gate.Pressed(kComma, false);
			gate.Pressed(kPeriod, true);
			gate.Pressed(kPeriod, false);
		}
		// 焦点回到游戏
		gate.SetForegroundWindowFocused(true);
		bool fired = false;
		for (int i = 0; i < 300; ++i)
		{
			gate.NewFrame();
			if (gate.Pressed(kComma, false)) fired = true;
			if (gate.Pressed(kPeriod, false)) fired = true;
		}
		Check(!fired, "失焦期间的历史按键在焦点恢复后不补发（幽灵切换已消除）");
	}

	// C. 焦点切回时玩家正好按着逗号：那一个重建帧也不应触发
	{
		HotkeyGate gate;
		gate.SetForegroundWindowFocused(true);
		gate.NewFrame();
		gate.SetForegroundWindowFocused(false);
		gate.NewFrame();
		gate.SetForegroundWindowFocused(true);
		gate.NewFrame(); // 基线重建帧
		Check(gate.Pressed(kComma, true) == false,
		      "焦点切回的基线重建帧不触发（即使此刻正按住逗号）");
		gate.NewFrame();
		Check(gate.Pressed(kComma, true) == false,
		      "重建帧后仍按住也不触发（不是新的按下沿）");
		gate.NewFrame();
		gate.Pressed(kComma, false); // 松开
		gate.NewFrame();
		Check(gate.Pressed(kComma, true) == true,
		      "重建后重新按下才触发（未把失焦/重建期的按键算作新按下）");
	}

	// D. 失焦时单个键的按下沿必须被拒绝，但基线照常更新
	{
		HotkeyGate gate;
		gate.SetForegroundWindowFocused(false);
		gate.NewFrame();
		Check(gate.Pressed(kPeriod, false) == false, "失焦：抬起不触发");
		Check(gate.Pressed(kPeriod, true) == false, "失焦：按下不触发");
		// 但内部基线必须已更新，否则焦点切回后会补发
		Check(gate.BaselineDown(kPeriod), "失焦期间仍更新按键基线");
	}

	// E. 装载插件后的第一帧是重建帧：玩家恰好按着播放键也不应立即起播
	{
		HotkeyGate gate; // 初始 m_focusDirty = false
		gate.Reset(); // 模拟装载/场景重载时复位
		gate.NewFrame();
		Check(gate.Pressed(kF9, true) == false, "复位后的第一帧为基线重建帧，不触发");
		gate.NewFrame();
		gate.Pressed(kF9, false);
		gate.NewFrame();
		Check(gate.Pressed(kF9, true) == true, "复位后重新按下才触发");
	}
}

// =============================================================================
// 11. 进度条拖动不回头 + Preview 姿态保持
// =============================================================================
static void TestScrubAndPreview()
{
	BeginTest("scrub_and_preview");

	MotionPlayer player;
	player.clip = MakeSmallClip();
	player.clip.total_frames = 600;
	player.clip.fps = 30.0f;
	player.clip.duration_seconds = 20.0f;
	player.clip.bone_count = 1;
	player.clip.bones = {BoneMeta{13, "LeftUpperArm", false}};
	player.clip.root_positions.assign(600, Vector3(0, 0, 0));
	player.clip.bone_rotations.assign(600, Quaternion::Identity());
	WireControllerCallbacks(player.controller);

	auto actor = reinterpret_cast<void*>(0x1);

	// --- A. 拖动：冻结主时钟，逐帧 Update 不被时钟拉回 ---
	g_playCalls = 0;
	g_seekCalls = 0;
	g_audioPos = -1.0f;
	player.fnGetAudioTime = FakeGetAudioTime; // 音频时钟存在（最恶劣情况）
	player.fnSeekAudio = FakeSeekAudio;
	player.Play();
	for (int i = 0; i < 30; ++i) player.Tick(1.0f / 30.0f, actor, nullptr, nullptr);
	Check(player.currentTime > 0.8f, "拖动前已正常推进");

	g_seekCalls = 0;
	player.BeginScrub(12.0f);
	Check(player.IsScrubbing(), "BeginScrub 进入拖动态");
	Check(std::fabs(player.currentTime - 12.0f) < 1e-3f, "BeginScrub 立即定位到 12s");

	// 模拟拖动过程：每帧 UpdateScrub，中间还夹着 Tick
	for (int i = 0; i < 20; ++i)
	{
		player.UpdateScrub(12.0f + 0.1f * static_cast<float>(i));
		player.Tick(1.0f / 30.0f, actor, nullptr, nullptr);
	}
	Check(std::fabs(player.currentTime - 13.9f) < 1e-2f,
	      "拖动过程中位置不被播放/音频时钟拉回（旧实现即在此回弹）");
	Check(g_seekCalls == 0, "拖动过程中不下发音频跳转（避免每帧 seek 音频）");

	// 松手提交
	player.EndScrub(14.0f);
	Check(!player.IsScrubbing(), "EndScrub 退出拖动态");
	Check(std::fabs(player.currentTime - 14.0f) < 1e-3f, "EndScrub 提交最终位置 14s");
	Check(g_seekCalls == 1, "EndScrub 一次性下发音频跳转");

	// --- B. Preview：载入后静止保持第 0 帧，且仍会绑定+灌姿态 ---
	MotionPlayer pv;
	pv.clip = MakeSmallClip();
	pv.clip.total_frames = 60;
	pv.clip.fps = 30.0f;
	pv.clip.duration_seconds = 2.0f;
	pv.clip.bone_count = 1;
	pv.clip.bones = {BoneMeta{13, "LeftUpperArm", false}};
	pv.clip.root_positions.assign(60, Vector3(0, 0, 0));
	pv.clip.bone_rotations.assign(60, Quaternion::Identity());
	WireControllerCallbacks(pv.controller);

	pv.state = PlaybackState::Preview;
	Check(pv.IsPreviewing(), "Preview 状态可识别");
	for (int i = 0; i < 10; ++i) pv.Tick(1.0f / 30.0f, actor, nullptr, nullptr);
	Check(std::fabs(pv.currentTime) < 1e-6f, "Preview 下时钟不推进（静止保持第 0 帧）");
	Check(pv.controller.currentBinding.actor == actor,
	      "Preview 下仍然绑定骨骼（否则角色退回官方待机）");

	// Preview -> Playing 正常起播
	pv.Play();
	Check(pv.state == PlaybackState::Playing, "Preview 可直接起播");
	for (int i = 0; i < 30; ++i) pv.Tick(1.0f / 30.0f, actor, nullptr, nullptr);
	Check(pv.currentTime > 0.5f, "Preview 起播后正常推进");

	// Stopped 才交还给官方待机
	pv.Stop();
	Check(pv.state == PlaybackState::Stopped, "Stop 后为 Stopped");
	pv.controller.currentBinding.Reset();
	for (int i = 0; i < 10; ++i) pv.Tick(1.0f / 30.0f, actor, nullptr, nullptr);
	Check(pv.controller.currentBinding.actor == nullptr,
	      "Stopped 下不再绑定（角色交还官方待机动画）");
}

// =============================================================================
// 12. scrub 真实链路回归：GUI -> pendingGuiCmd -> Core -> MotionPlayer
// =============================================================================
// 上一轮教训：单测只覆盖了 MotionPlayer.BeginScrub/UpdateScrub/EndScrub，
// 而实机故障恰恰发生在「GUI 写命令 -> Unity 主线程消费」这一段。
// 本测试按实机真实时序驱动：GUI 侧连续 Queue，主线程侧每 Tick 消费一次。
// 核心断言是**最终 currentTime 必须等于拖动目标**，而不是只看某个方法被调用过。

// 记录探针输出，便于断言「命令是否真的到了播放器」。
static std::vector<std::string> g_probeLines;

static void CollectProbe(const char* line)
{
	g_probeLines.emplace_back(line ? line : "");
}

static bool ProbeContains(const char* needle)
{
	for (const auto& l : g_probeLines)
	{
		if (l.find(needle) != std::string::npos) return true;
	}
	return false;
}

static void TestScrubCommandPipeline(const std::string& repoRoot)
{
	BeginTest("scrub_command_pipeline");

	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "gkmotion_scrub_test";
	fs::remove_all(root);
	fs::create_directories(root / "motions" / "song_x");
	fs::create_directories(root / "GakuMotion");

	const fs::path repo(repoRoot);
	fs::path src = repo / "Motions" / "Like JENNIE" / "motion.gkmotion";
	if (!fs::exists(src)) src = repo / "Motions" / "Fighting My Way" / "motion.gkmotion";
	if (!fs::exists(src)) src = repo / "test_motion" / "motion.gkmotion";
	Check(fs::exists(src), "找到可用的真实动作包作为测试输入");
	if (!fs::exists(src)) return;

	fs::copy_file(src, root / "motions" / "song_x" / "motion.gkmotion", fs::copy_options::overwrite_existing);
	WriteTextFile(root / "GakuMotion" / "control.json",
	              R"({"poll_interval_seconds": 999.0, "playback_speed_multiplier": 1.0})");

	Core core; // Keep the audio-clock fixture independent of earlier rate tests.
	g_probeLines.clear();
	core.Initialize(root.string());
	Check(core.motions.size() >= 1, "扫描到测试曲目");
	core.player.fnProbe = &CollectProbe;
	WireControllerCallbacks(core.player.controller);
	auto actor = reinterpret_cast<void*>(0x1);

	// 进入 Playing，并挂上音频主时钟（最恶劣情况：拖动会被音频时钟拉回）
	g_audioPos = -1.0f;
	g_playCalls = 0;
	g_seekCalls = 0;
	g_resumeCalls = 0;
	// Status reads in one frame observe the same hardware position, including
	// the three settle samples. Advance this fixture once per simulated frame.
	core.player.fnGetAudioTime = []() { return g_audioPos; };
	core.player.fnSeekAudio = FakeSeekAudio;
	core.player.fnPlayAudio = FakePlayAudio;
	core.player.fnStopAudio = FakeStopAudio;
	core.player.fnPauseAudio = FakePauseAudio;
	core.player.fnResumeAudio = FakeResumeAudio;
	core.player.Play();
	Check(core.player.state == PlaybackState::Playing, "已进入播放态");
	for (int i = 0; i < 30; ++i)
	{
		if (g_audioPos >= 0.0f) g_audioPos += 1.0f / 30.0f;
		core.OnLateUpdate(actor, nullptr, nullptr, 1.0f / 30.0f);
	}
	Check(core.player.currentTime > 0.5f, "播放已推进");

	const float target = core.player.clip.duration_seconds * 0.75f;
	Check(target > 1.0f, "拖动目标有效");

	// ---- 关键时序模拟：GUI 在多次渲染中写 begin/update，主线程每帧消费 ----
	g_probeLines.clear();
	g_seekCalls = 0;
	core.QueueScrubBegin(5.0f);
	for (int i = 0; i < 20; ++i)
	{
		// GUI 侧持续写 update（每帧一次），主线程侧每帧消费一次
		core.QueueScrubUpdate(5.0f + 0.1f * static_cast<float>(i));
		core.OnLateUpdate(actor, nullptr, nullptr, 1.0f / 30.0f);
		// 拖动中音频时钟在走（脚本化），模拟真实播放中拖动
		if (g_audioPos >= 0.0f) g_audioPos += 1.0f / 30.0f;
	}
	Check(core.player.IsScrubbing(), "拖动期间处于 scrubbing 态");
	Check(ProbeContains("BEGIN req=5.000"), "BEGIN 命令抵达播放器");
	Check(ProbeContains("CORE] dequeued"), "Core 消费了 scrub 命令");
	Check(!ProbeContains("DROPPED"), "拖动中的 UPDATE 未被丢弃");

	// 松手
	core.QueueScrubEnd(target);
	core.OnLateUpdate(actor, nullptr, nullptr, 1.0f / 30.0f);
	Check(!core.player.IsScrubbing(), "END 后退出 scrubbing");
	Check(ProbeContains("END req="), "END 命令抵达播放器");
	Check(g_seekCalls == 1, "松手恰好下发一次音频跳转");

	// ---- 最终断言：位置必须真的落在目标附近，且不随后被音频时钟拽回 ----
	const float afterEnd = core.player.currentTime;
	Check(std::fabs(afterEnd - target) < 0.35f,
	      "松手瞬间位置落在目标附近（拖动真正生效）");
	if (std::fabs(afterEnd - target) >= 0.35f)
	{
		std::printf("    [diag] target=%.3f afterEnd=%.3f seeks=%d hasSeekAudio=%d\n",
		            target, afterEnd, core.player.SeekCount(),
		            core.player.fnSeekAudio ? 1 : 0);
	}

	// 再跑 2 秒：若音频跳转没落到音频上，这段时间位置会被拽回，探针会报 OVERRIDE
	g_probeLines.clear();
	for (int i = 0; i < 60; ++i)
	{
		if (g_audioPos >= 0.0f) g_audioPos += 1.0f / 30.0f;
		core.OnLateUpdate(actor, nullptr, nullptr, 1.0f / 30.0f);
	}
	const float after2s = core.player.currentTime;
	Check(after2s > target - 0.1f,
	      "提交后位置未被音频主时钟拽回（真正的时序跳转成立）");
	Check(!ProbeContains("audio-clock OVERRIDE"),
	      "提交后没有出现音频时钟覆写（无回弹）");
	if (ProbeContains("audio-clock OVERRIDE"))
	{
		std::printf("    [diag] %s\n", g_probeLines.back().c_str());
	}

	core.player.Stop();
	fs::remove_all(root);
}

// =============================================================================
static void TestClipGroundAnchor()
{
	g_current = "clip ground anchor";
	MotionClip clip;
	clip.format = MotionClip::Format::kGkClip;
	clip.version = 3;
	// Deliberately shuffled: bone index must never be used as humanoid id.
	for (int hid : {20, 4, 0, 19, 1, 5, 2, 6, 3})
	{
		BoneMeta bone{};
		bone.humanoid_id = hid;
		clip.bones.push_back(bone);
	}
	clip.bone_count = static_cast<uint32_t>(clip.bones.size());
	clip.total_frames = 3;
	clip.root_positions = {{0, 0, 0}, {0, 0.5f, 0}, {0, -0.2f, 0}};
	clip.bone_rotations.assign(clip.bone_count * 3, Quaternion::Identity());
	std::array<Vector3, 21> offsets{};
	std::array<bool, 21> present{};
	for (const auto& bone : clip.bones) present[bone.humanoid_id] = true;
	offsets[1] = {-0.1f, -0.1f, 0};
	offsets[2] = {0.1f, -0.1f, 0};
	offsets[3] = offsets[4] = {0, -0.4f, 0};
	offsets[5] = offsets[6] = {0, -0.4f, 0};
	offsets[19] = offsets[20] = {0, -0.1f, 0.2f};
	float lift = 0;
	Check(ComputeClipGroundLift(clip, offsets, present, lift), "complete leg chains produce anchor");
	Check(std::fabs(lift - 1.2f) < 1e-5f, "lowest toe over whole clip defines floor, not first frame");
	Check(std::fabs((lift + clip.root_positions[1].y - 1.0f) - 0.7f) < 1e-5f,
	      "constant correction preserves jump height");
	offsets[0] = {10, 1.6548f, -5};
	float rebound = 0;
	Check(ComputeClipGroundLift(clip, offsets, present, rebound) && std::fabs(rebound - lift) < 1e-5f,
	      "polluted hips bind position cannot change ground anchor");
	present[20] = false;
	Check(!ComputeClipGroundLift(clip, offsets, present, rebound), "missing toe fails closed");
	present[20] = true;
	clip.version = 1;
	Check(!ComputeClipGroundLift(clip, offsets, present, rebound), "old clip grounding remains compatible");
}

static void TestClipSwitchKeepsActorRest()
{
	g_current = "clip switch actor rest";
	IdolController controller;
	controller.fnGetBoneTransform = [](void*, int) -> void* { return nullptr; };
	auto actor = reinterpret_cast<void*>(0x1234);
	IdolBoneBinding b;
	b.actor = actor;
	b.bindPoseCaptured = true;
	b.restRootPos = {0.1f, 0.88f, -0.2f};
	b.restActorRot = Quaternion::FromEulerDegreesXYZ(0, 90, 0);
	b.groundLift = b.bindGroundLift = 0.75f;
	b.boneHids = {0, 3, 5};
	b.boneTransforms = {reinterpret_cast<void*>(0x10), reinterpret_cast<void*>(0x30), reinterpret_cast<void*>(0x50)};
	b.restRotations = {
		Quaternion::Identity(), Quaternion::FromEulerDegreesXYZ(0, 0, 20), Quaternion::FromEulerDegreesXYZ(0, 0, 40)
	};
	b.restLocals = b.restRotations;
	b.boundBoneCount = 3;
	controller.bindings[actor] = b;
	controller.currentBinding = b;
	controller.SetInitialPositionOffset({1.0f, 0.25f, -2.0f});
	MotionClip next;
	next.format = MotionClip::Format::kGkClip;
	next.version = 2;
	for (int hid : {5, 0, 3})
	{
		BoneMeta meta{};
		meta.humanoid_id = hid;
		next.bones.push_back(meta);
	}
	controller.InvalidateClipBindings();
	controller.BindActor(actor, next);
	const auto& rebound = controller.currentBinding;
	Check(std::fabs(rebound.restRootPos.y - 0.88f) < 1e-6f, "switch keeps original hips anchor");
	Check(std::fabs(Quaternion::Dot(rebound.restActorRot, b.restActorRot) - 1) < 1e-5f,
	      "switch keeps original actor orientation");
	Check(rebound.boneTransforms[0] == b.boneTransforms[2] && rebound.boneHids[0] == 5,
	      "same-sized shuffled clip remaps by humanoid id");
	Check(std::fabs(Quaternion::Dot(rebound.restLocals[0], b.restLocals[2]) - 1) < 1e-5f,
	      "switch keeps rest rotations attached to correct humanoid id");
	Check(!rebound.clipBindingDirty && rebound.boundBoneCount == 3,
	      "switch completes without recapturing animated pose");
	Check(controller.initialPositionOffset.x == 1.0f && controller.initialPositionOffset.y == 0.25f && controller.
	      initialPositionOffset.z == -2.0f,
	      "switch keeps initial placement separate from actor rest snapshot");
	controller.OnActorChanged(nullptr);
	Check(controller.bindings.empty(), "scene teardown still discards original snapshots");
}

static void TestPreviewWaitsForHumanoid()
{
	g_current = "preview waits for humanoid";
	static int muteCalls = 0;
	muteCalls = 0;
	MotionPlayer player;
	player.state = PlaybackState::Preview;
	player.controller.fnGetBoneTransform = [](void*, int) -> void* { return nullptr; };
	player.controller.fnSetAnimatorEnabled = [](void*, bool) { ++muteCalls; };
	player.Tick(1.0f / 30.0f, reinterpret_cast<void*>(0x10), reinterpret_cast<void*>(0x20));
	Check(player.controller.bindings.empty(), "not-ready humanoid creates no empty binding");
	Check(muteCalls == 0, "preview does not disable Animator before skeleton initialization");
}

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static void TestBlendshapePlayback()
{
	g_current = "named blendshape playback and lifecycle";
	struct FakeFace
	{
		std::vector<std::string> names;
		std::vector<float> weights;
	};
	static FakeFace first, second;
	static FakeFace* live = nullptr;
	first = {{"untouched", "mouth", "b_eye.eye_001"}, {11, 20, 30}};
	second = {{"b_eye.eye_001", "mouth"}, {40, 60}};
	live = &first;
	FaceApi api{
		[](void*) -> void* { return live; },
		[](void* p) { return static_cast<int>(static_cast<FakeFace*>(p)->names.size()); },
		[](void* p, int i) { return static_cast<FakeFace*>(p)->names[i]; },
		[](void* p, int i) { return static_cast<FakeFace*>(p)->weights[i]; },
		[](void* p, int i, float w) { static_cast<FakeFace*>(p)->weights[i] = w; }
	};
	MotionClip clip;
	clip.total_frames = 2;
	clip.fps = 30;
	clip.blendshape_names = {"b_eye.eye_001", "mouth", "missing"};
	clip.blendshape_weights = {0, .2f, .9f, 1, .8f, .1f};
	auto actor = reinterpret_cast<void*>(0x1234);
	BlendshapeBinding binding;
	binding.Apply(actor, clip, 1.0f / 60, api);
	Check(Near(first.weights[2], 50) && Near(first.weights[1], 50),
	      "unit curves interpolate then convert to native percentages by name");
	Check(Near(first.weights[0], 11), "unknown channels do not write unrelated shapes");
	binding.Apply(actor, clip, 100, api);
	Check(Near(first.weights[2], 100) && Near(first.weights[1], 80),
	      "past-end seek holds final facial frame at native scale");
	binding.Apply(actor, clip, -1, api);
	Check(Near(first.weights[2], 0), "negative seek holds first facial frame");
	binding.Restore(actor, api);
	Check(Near(first.weights[2], 30) && Near(first.weights[1], 20),
	      "stop restores native saved weights without scaling twice");
	binding.Apply(actor, clip, 0, api);
	live = &second;
	binding.Apply(actor, clip, 0, api);
	Check(Near(second.weights[0], 0) && Near(second.weights[1], 20), "new face model resolves its own shape indices");
	Check(Near(first.weights[2], 0), "replaced model is never restored through a stale pointer");
	MotionClip legacy;
	binding.Apply(actor, legacy, 0, api);
	Check(Near(second.weights[0], 40) && Near(second.weights[1], 60),
	      "switch to a clip without morphs restores previous face");
	binding.Apply(actor, clip, 0, api);
	IdolController controller;
	controller.faceApi = api;
	controller.currentBinding.actor = actor;
	controller.currentBinding.bindPoseCaptured = true;
	controller.currentBinding.face = binding;
	controller.RestoreMute();
	Check(Near(second.weights[0], 40) && !controller.currentBinding.face.written,
	      "player Stop restoration reaches the face binding");
	FaceApi unavailable;
	binding.Apply(actor, clip, 0, unavailable);
	Check(Near(second.weights[0], 40), "missing runtime API safely skips face playback");

	// The observer distinguishes a refused write from a later external change.
	api.getTicks = []() -> uint64_t { return 123456; };
	api.getUnityFrame = []() { return 42; };
	binding.Observe();
	binding.Apply(actor, clip, 1.0f / 60, api);
	Check(binding.diagnosticStatus == GAKU_FACE_BOUND && binding.diagnosticTicks == 123456 &&
	      binding.diagnosticUnityFrame == 42, "observer stamps the actual write and Unity frame");
	Check(binding.diagnosticChannels.size() == 3 && binding.diagnosticChannels[0].index == 0 &&
	      binding.diagnosticChannels[2].index == -1, "observer records matched and unmatched names");
	Check(Near(binding.diagnosticChannels[0].target, 50) &&
	      Near(binding.diagnosticChannels[0].after, 50), "observer target and readback share native percentage units");
	second.weights[0] = 27;
	api.setWeight = [](void*, int, float)
	{
	}; // Simulate an API that refuses a write.
	binding.Apply(actor, clip, 1.0f / 60, api);
	Check(Near(binding.diagnosticChannels[0].before, 27) &&
	      Near(binding.diagnosticChannels[0].after, 27) &&
	      Near(binding.diagnosticChannels[0].target, 50), "refused setter remains visible in native readback");
	auto sequence = binding.diagnosticSequence;
	binding.observerUntil = {};
	binding.Apply(actor, clip, 0, api);
	Check(binding.diagnosticSequence == sequence, "expired observer lease stops readback probes");
	binding.Observe();
	binding.Apply(actor, clip, 0, unavailable);
	Check(binding.diagnosticStatus == GAKU_FACE_API_MISSING &&
	      binding.diagnosticChannels[0].index == -1, "missing API reports targets without fabricated matches");

	struct FakeFacial
	{
		bool useBlink, autoDisabled;
	};
	static FakeFacial facialA, facialB;
	static FakeFacial* liveFacial = nullptr;
	static int blinkWrites, highlightCalls, faceBinds;
	static float highlightEye;
	facialA = {true, false};
	facialB = {false, false};
	liveFacial = &facialA;
	blinkWrites = highlightCalls = faceBinds = 0;
	live = &first;
	first.weights = {11, 20, 30};
	api.setWeight = [](void* p, int i, float w) { static_cast<FakeFace*>(p)->weights[i] = w; };
	api.getFacial = [](void*) -> void* { return liveFacial; };
	api.readBlink = [](void* p, bool& u, bool& d)
	{
		auto& f = *static_cast<FakeFacial*>(p);
		u = f.useBlink;
		d = f.autoDisabled;
		return true;
	};
	api.writeBlink = [](void* p, bool u, bool d)
	{
		auto& f = *static_cast<FakeFacial*>(p);
		f.useBlink = u;
		f.autoDisabled = d;
		++blinkWrites;
		return true;
	};
	api.refreshHighlight = [](void*)
	{
		++highlightCalls;
		auto i = std::find(live->names.begin(), live->names.end(), "b_eye.eye_001") - live->names.begin();
		highlightEye = live->weights[i];
		return true;
	};
	api.beginBinding = []() { ++faceBinds; };
	BlendshapeBinding controlled;
	controlled.Apply(actor, clip, 100, api);
	Check(!facialA.useBlink && facialA.autoDisabled,
	      "eye curves suppress both automatic and in-progress blink control");
	Check(highlightCalls == 1 && Near(highlightEye, 100), "highlight refresh follows the completed custom eye write");
	facialA = {true, false}; // Simulate another component changing its flags.
	controlled.Apply(actor, clip, 100, api);
	Check(!facialA.useBlink && facialA.autoDisabled,
	      "playback reasserts blink ownership without recapturing changed flags");
	Check(faceBinds == 1, "runtime notices reset once per model binding rather than every frame");
	controlled.Restore(actor, api);
	Check(facialA.useBlink && !facialA.autoDisabled && Near(highlightEye, 30),
	      "Stop restores original blink configuration and native highlight inputs");
	int writes = blinkWrites;
	controlled.Restore(actor, api);
	Check(blinkWrites == writes, "repeated Stop does not mutate blink configuration again");

	MotionClip mouthOnly = clip;
	mouthOnly.blendshape_names = {"mouth"};
	mouthOnly.blendshape_weights = {.25f, .5f};
	int highlights = highlightCalls;
	controlled.Apply(actor, mouthOnly, 0, api);
	Check(blinkWrites == writes && highlightCalls == highlights && Near(first.weights[1], 25),
	      "mouth-only clips preserve automatic blink and do not refresh eye material");
	controlled.Restore(actor, api);
	MotionClip unmatchedEye = mouthOnly;
	unmatchedEye.blendshape_names = {"b_eye.unknown"};
	controlled.Apply(actor, unmatchedEye, 0, api);
	Check(blinkWrites == writes && !controlled.ownsEyes, "unmatched eye names cannot seize native blink");
	controlled.Restore(actor, api);

	facialA = {true, true};
	controlled.Apply(actor, clip, 0, api);
	controlled.Restore(actor, api);
	Check(facialA.useBlink && facialA.autoDisabled, "Stop preserves originally disabled automatic blink");
	facialA = {true, false};
	controlled.Apply(actor, clip, 0, api);
	writes = blinkWrites;
	liveFacial = &facialB;
	controlled.Apply(actor, clip, 0, api);
	Check(blinkWrites == writes + 1 && !facialA.useBlink && facialA.autoDisabled,
	      "component replacement never restores a stale facial pointer");
	controlled.Restore(actor, api);
	Check(!facialB.useBlink && !facialB.autoDisabled, "replacement facial restores its own original flags");

	facialB = {true, false};
	FaceApi noBlink = api;
	noBlink.readBlink = nullptr;
	controlled.Apply(actor, clip, 100, noBlink);
	Check(facialB.useBlink && !facialB.autoDisabled && Near(first.weights[2], 100),
	      "missing blink API leaves percentage face playback available");
	controlled.Restore(actor, noBlink);
	api.writeBlink = [](void*, bool, bool) { return false; };
	controlled.Apply(actor, clip, 0, api);
	Check(!controlled.blinkSuppressed && facialB.useBlink, "refused blink writes do not fabricate ownership");
	controlled.Restore(actor, api);
}

static void TestFaceRuntimeMetadata()
{
	g_current = "face metadata and guarded highlight refresh";
	struct Object
	{
		uint8_t useBlink, disabled;
		void* facial;
	};
	struct Field
	{
		int type;
		uint32_t flags;
	};
	struct Method
	{
		int type;
		uint32_t flags;
	};
	static Object actor, facial;
	static Field reference, use, disabled;
	static Method refresh;
	static int writes, invokes;
	static bool throwManaged, methodAvailable;
	facial = {1, 0, nullptr};
	actor = {0, 0, &facial};
	reference = {0x12, 0};
	use = disabled = {0x02, 0};
	refresh = {0x01, 0};
	writes = invokes = 0;
	throwManaged = false;
	methodAvailable = true;
	FaceRuntimeAccess runtime;
	runtime.objectClass = [](void* p) -> void* { return p; };
	runtime.findField = [](void*, const char* n) -> void*
	{
		if (strcmp(n, "_facial") == 0 || strcmp(n, "_talk") == 0 || strcmp(n, "_criTalk") == 0) return &reference;
		if (strcmp(n, "useBlink") == 0 || strcmp(n, "_talkEnabled") == 0) return &use;
		if (strcmp(n, "autoBlinkDisabled") == 0) return &disabled;
		return nullptr;
	};
	runtime.fieldType = [](void* f) -> void* { return &static_cast<Field*>(f)->type; };
	runtime.fieldFlags = [](void* f) { return static_cast<Field*>(f)->flags; };
	runtime.typeCode = [](void* t) { return *static_cast<int*>(t); };
	runtime.readField = [](void* p, void* f, void* out)
	{
		auto& o = *static_cast<Object*>(p);
		if (f == &reference) *static_cast<void**>(out) = o.facial;
		else *static_cast<uint8_t*>(out) = f == &use ? o.useBlink : o.disabled;
	};
	runtime.writeField = [](void* p, void* f, void* value)
	{
		++writes;
		auto& o = *static_cast<Object*>(p);
		(f == &use ? o.useBlink : o.disabled) = *static_cast<uint8_t*>(value);
	};
	runtime.findMethod = [](void*, const char* n, int args) -> const void*
	{
		return methodAvailable && args == 0 && strcmp(n, "UpdateHighLightMaterial") == 0 ? &refresh : nullptr;
	};
	runtime.returnType = [](const void* m) -> void* { return const_cast<int*>(&static_cast<const Method*>(m)->type); };
	runtime.methodFlags = [](const void* m, uint32_t*) { return static_cast<const Method*>(m)->flags; };
	runtime.invoke = [](const void*, void* p, void** args, void** exception) -> void*
	{
		++invokes;
		if (throwManaged || args || p != &actor) *exception = reinterpret_cast<void*>(1);
		return nullptr;
	};
	Check(runtime.Facial(&actor) == &facial, "facial reference comes from a declared managed field");
	bool u = false, d = true;
	Check(runtime.ReadBlink(&facial, u, d) && u && !d, "metadata bool fields retain native flags");
	Check(runtime.WriteBlink(&facial, false, true) && writes == 2 && !facial.useBlink && facial.disabled,
	      "typed fields disable blink without guessed byte offsets");
	disabled.type = 0x08;
	Check(!runtime.ReadBlink(&facial, u, d) && !runtime.WriteBlink(&facial, true, false) && writes == 2,
	      "wrong field type is rejected before either blink mutation");
	disabled.type = 0x02;
	disabled.flags = 0x10;
	Check(!runtime.WriteBlink(&facial, true, false) && writes == 2,
	      "static blink fields cannot be mutated as instance data");
	disabled.flags = 0x20;
	Check(!runtime.WriteBlink(&facial, true, false) && writes == 2, "readonly blink fields cannot be mutated");
	disabled.flags = 0;
	reference.type = 0x08;
	Check(!runtime.Facial(&actor), "non-reference actor fields are never dereferenced");
	reference.type = 0x12;
	FaceRuntimeAccess missing = runtime;
	missing.writeField = nullptr;
	Check(!missing.WriteBlink(&facial, true, false), "missing metadata export safely skips blink control");
	Check(runtime.RefreshHighlight(&actor) && invokes == 1,
	      "highlight invokes only the instance zero-argument void method");
	refresh.type = 0x0c;
	Check(!runtime.RefreshHighlight(&actor) && invokes == 1, "wrong highlight return type cannot be invoked");
	refresh.type = 0x01;
	refresh.flags = 0x10;
	Check(!runtime.RefreshHighlight(&actor) && invokes == 1, "static highlight overload cannot be invoked on actor");
	refresh.flags = 0;
	methodAvailable = false;
	Check(!runtime.RefreshHighlight(&actor) && invokes == 1,
	      "unavailable highlight signature preserves native updates");
	methodAvailable = true;
	throwManaged = true;
	Check(!runtime.RefreshHighlight(&actor) && invokes == 2,
	      "managed highlight exceptions return failure instead of unwinding host");
	Check(runtime.Talk(&actor, 0)==&facial && runtime.Talk(&actor, 1)==&facial,
	      "regular and CRI talk resolve through declared reference fields");
	bool enabled=true;
	Check(runtime.WriteTalk(&facial,false) && runtime.ReadTalk(&facial,enabled) && !enabled,
	      "talk disable writes a metadata-checked bool and reads it back");
	use.flags=0x20;const int beforeTalk=writes;
	Check(!runtime.WriteTalk(&facial,true) && writes==beforeTalk,"readonly talk state is never modified");
	use.flags=0;reference.type=0x08;
	Check(!runtime.Talk(&actor,0),"non-reference talk fields are rejected");
	reference.type=0x15;
	Check(runtime.Talk(&actor,0)==&facial,"generic native talk references are supported");
}

static void TestSceneIsolation()
{
	g_current = "scene ownership and teardown";
	Check(!IsCustomMotionScene("OutGame"), "shared OutGame name alone never proves the costume page");
	Check(!IsCustomMotionScene("Live") && !IsCustomMotionScene("<unavailable>"),
	      "official Live and unavailable scene identities remain excluded");
	ActorFrameTracker actors;
	auto home = reinterpret_cast<void*>(0x10);
	auto costume = reinterpret_cast<void*>(0x20);
	Check(actors.Note(1, home) && !actors.Note(1, home) && !actors.Note(2, home),
	      "same actor and duplicate loader callbacks preserve a playback session");
	Check(actors.Note(3, costume) && !actors.Note(4, costume) && actors.Note(5, home),
	      "returning to a previously seen home actor starts a new actor session");
	static int unityCalls = 0, audioStops = 0;
	unityCalls = audioStops = 0;
	MotionPlayer player;
	player.state = PlaybackState::Playing;
	player.controller.fnCanDriveActor = [](void*) { return false; };
	player.controller.fnGetBoneTransform = [](void*, int) -> void*
	{
		++unityCalls;
		return nullptr;
	};
	player.controller.fnSetAnimatorEnabled = [](void*, bool) { ++unityCalls; };
	player.controller.fnAnimatorRebind = [](void*) { ++unityCalls; };
	player.clip = MakeSmallClip();
	player.fnCanPlay = []() { return false; };
	player.state = PlaybackState::Stopped;
	player.Play();
	Check(player.state == PlaybackState::Stopped, "Play is refused outside the custom-motion scene");
	player.state = PlaybackState::Playing;
	player.Tick(.1f, reinterpret_cast<void*>(0x10), reinterpret_cast<void*>(0x20));
	Check(unityCalls == 0 && player.currentTime == 0,
	      "home/work/official Live exclusion precedes skeleton access and clock advancement");
	IdolBoneBinding old;
	old.actor = reinterpret_cast<void*>(0xdead);
	old.cachedAnimator = reinterpret_cast<void*>(0xbeef);
	old.bindPoseCaptured = old.defensivelyMuted = true;
	player.controller.bindings[old.actor] = old;
	player.controller.currentBinding = old;
	player.controller.cachedAnimator = old.cachedAnimator;
	player.controller.SetTargetActor(old.actor);
	player.fnStopAudio = []() { ++audioStops; };
	player.AbandonScene();
	player.Stop();
	Check(unityCalls == 0 && audioStops == 2,
	      "scene teardown and subsequent Stop never call destroyed Unity objects");
	Check(player.state == PlaybackState::Stopped && player.controller.bindings.empty() &&
	      !player.controller.currentBinding.actor && !player.controller.cachedAnimator &&
	      !player.controller.targetActor, "scene teardown clears binding, animator and selected actor");

	Core core;
	core.player.state = PlaybackState::Stopped;
	core.player.controller.bindings[old.actor] = old;
	core.player.controller.currentBinding = old;
	core.player.controller.fnAnimatorRebind = [](void*) { ++unityCalls; };
	core.player.controller.fnSetAnimatorEnabled = [](void*, bool) { ++unityCalls; };
	core.sceneActors.push_back(old.actor);
	Check(core.DiscardPreviousActorBindings() && core.player.controller.bindings.empty() &&
	      !core.player.controller.currentBinding.actor && core.sceneActors.empty() && unityCalls == 0,
	      "actor arrival after Stop discards old bindings without touching destroyed objects");
	Check(!core.DiscardPreviousActorBindings(), "additional actor arrivals with no old bindings are inert");

	IdolController controller;
	controller.fnAnimatorRebind = [](void*) { ++unityCalls; };
	controller.fnSetAnimatorEnabled = [](void*, bool) { ++unityCalls; };
	old.animatorStateCaptured = old.animatorWasDisabled = true;
	old.actor = reinterpret_cast<void*>(0x11);
	controller.bindings[old.actor] = old;
	old.actor = reinterpret_cast<void*>(0x12);
	controller.bindings[old.actor] = old;
	controller.currentBinding = old;
	controller.cachedAnimator = old.cachedAnimator;
	controller.RestoreMute();
	Check(unityCalls == 4, "Stop restores both muted actors once without a duplicate cached Animator rebind");
	controller.RestoreMute();
	Check(unityCalls == 4, "repeated Stop leaves official animation untouched");
}

static void TestBlendshapeBinary(const std::string& repoRoot)
{
	g_current = "gkclip v4 binary reader";
	const std::string path = repoRoot + "/build/tests/blendshape_fixture.gkclip";
	std::vector<char> bytes;
	auto write = [&bytes](const auto& value)
	{
		auto p = reinterpret_cast<const char*>(&value);
		bytes.insert(bytes.end(), p, p + sizeof(value));
	};
	const char magic[] = "GKCLIPCL";
	bytes.insert(bytes.end(), magic, magic + 8);
	write(uint32_t{4});
	write(30.0f);
	write(uint32_t{3});
	write(.1f);
	write(uint32_t{1});
	write(.07795f);
	write(uint16_t{0});
	write(uint8_t{4});
	write(uint8_t{1});
	for (char c : std::string("Hips")) bytes.push_back(c);
	for (int f = 0; f < 3; ++f)
	{
		write(Vector3{});
		write(Quaternion{});
	}
	write(uint8_t{1}); // camera-present flag, seven floats per frame
	for (int i = 0; i < 21; ++i) write(static_cast<float>(i));
	write(uint32_t{1});
	const std::string name = "日本語表情";
	write(static_cast<uint16_t>(name.size()));
	bytes.insert(bytes.end(), name.begin(), name.end());
	write(0.0f);
	write(.5f);
	write(1.0f);
	auto save = [&]()
	{
		std::ofstream file(path, std::ios::binary);
		file.write(bytes.data(), bytes.size());
	};
	save();
	MotionClip clip;
	Check(clip.LoadFromFile(path), "v4 loads camera payload followed by morph table");
	Check(clip.blendshape_names == std::vector<std::string>{name}, "v4 keeps UTF-8 target names");
	std::vector<float> sampled;
	clip.SampleBlendshapes(1.0f / 60, sampled);
	Check(sampled.size() == 1 && Near(sampled[0], .25f), "v4 weights sampled on shared playback clock");
	bytes.pop_back();
	save();
	Check(!clip.LoadFromFile(path), "truncated v4 weight data is rejected");
	bytes.push_back(0); // restore size, then corrupt the last float to NaN
	const float bad = std::numeric_limits<float>::quiet_NaN();
	std::memcpy(bytes.data() + bytes.size() - sizeof(float), &bad, sizeof(float));
	save();
	Check(!clip.LoadFromFile(path), "nonfinite facial weight is rejected");
	std::error_code ec;
	std::filesystem::remove(path, ec);
	for (const char* song : {"Fighting My Way", "Boom Boom Pow", "Like JENNIE"})
	{
		MotionClip distributed;
		std::string clipPath = repoRoot + "/Motions/" + song + "/motion.gkclip";
		if (!std::filesystem::exists(clipPath)) clipPath = repoRoot + "/test_motion/motion.gkclip";
		if (!std::filesystem::exists(clipPath)) continue;
		Check(distributed.LoadFromFile(clipPath),
		      "Python-produced distributed v4 file loads in C++");
		Check(distributed.version == 4 && !distributed.blendshape_names.empty(),
		      "distributed clips contain named facial channels");
		Check(distributed.blendshape_weights.size() == distributed.total_frames * distributed.blendshape_names.size(),
		      "distributed facial payload matches frame count");
		Check(std::all_of(distributed.blendshape_names.begin(), distributed.blendshape_names.end(),
		                 [](const std::string& n) { return FacialFamily(n) != 0 || FacialSemantic(n) != nullptr; }),
		      "distributed facial names are native channels or recognized adapter semantics");
	}
}

namespace animatorFake
{
	struct State
	{
		bool enabled{true};
		void* controller{reinterpret_cast<void*>(0x700)};
		int enableWrites{0};
		int controllerWrites{0};
		int rebinds{0};
	};

	static std::unordered_map<void*, State> states;
	static int directorPauses = 0;
	static void PauseDirector(void*) { ++directorPauses; }
	static bool GetEnabled(void* a) { return states.at(a).enabled; }

	static void SetEnabled(void* a, bool enabled)
	{
		auto& s = states.at(a);
		s.enabled = enabled;
		++s.enableWrites;
	}

	static void* GetController(void* a) { return states.at(a).controller; }

	static void SetController(void* a, void* controller)
	{
		auto& s = states.at(a);
		s.controller = controller;
		++s.controllerWrites;
	}

	static void Rebind(void* a) { ++states.at(a).rebinds; }

	static void Wire(MotionPlayer& player)
	{
		WireControllerCallbacks(player.controller);
		player.clip = MakeSmallClip();
		player.clip.duration_seconds = 10.0f;
		player.state = PlaybackState::Preview;
		player.controller.fnGetAnimatorEnabled = GetEnabled;
		player.controller.fnSetAnimatorEnabled = SetEnabled;
		player.controller.fnGetAnimatorController = GetController;
		player.controller.fnSetAnimatorController = SetController;
		player.controller.fnAnimatorRebind = Rebind;
		player.controller.fnPausePlayableDirector = PauseDirector;
	}
}

static void TestSecondaryPhysicsAnimatorLifecycle()
{
	BeginTest("secondary physics Animator lifecycle (mock Unity; no visual claim)");
	using namespace animatorFake;
	states.clear();
	directorPauses = 0;
	auto actor = reinterpret_cast<void*>(0x71);
	auto animator = reinterpret_cast<void*>(0x72);
	states[animator] = State{};
	MotionPlayer player;
	Wire(player);
	player.controller.preserveAnimatorForSecondaryPhysics = true;
	player.controller.useAnimatorRebind = true; // preserve takes precedence
	player.controller.SetTargetActor(actor);
	player.Tick(1.0f / 60.0f, actor, animator, reinterpret_cast<void*>(0x74));
	Check(states[animator].enabled && states[animator].controller == reinterpret_cast<void*>(0x700),
	      "first Preview bind keeps enabled Animator and controller attached");
	Check(states[animator].controllerWrites == 0 && states[animator].enableWrites == 0 && states[animator].rebinds == 0,
	      "preserve mode leaves the native graph untouched, including bind-time Rebind");
	Check(directorPauses == 0, "preserve mode does not pause a supplied Timeline graph");
	player.Play();
	player.Tick(1.0f / 60.0f, actor, animator);
	player.Pause();
	player.Seek(0.02f);
	fake::rotationCalls.clear();
	player.Tick(1.0f / 60.0f, actor, animator);
	Check(std::fabs(player.currentTime - 0.02f) < 1e-6f,
	      "pause + Seek retains the requested motion time");
	bool bodyWritten = false;
	for (const auto& call : fake::rotationCalls)
	{
		if (call.first == fake::GetBone(actor, 13) && QuatNear(call.second, player.clip.bone_rotations[0])) bodyWritten
			= true;
	}
	Check(bodyWritten, "paused preserve mode still writes the sampled body pose");
	const auto originalRest = player.controller.currentBinding.restRotations;
	const float originalGround = player.controller.currentBinding.groundLift;
	// Hot A/B switch without rebinding bones or capturing a dancing rest pose.
	player.controller.preserveAnimatorForSecondaryPhysics = false;
	player.Tick(1.0f / 60.0f, actor, animator);
	Check(!states[animator].enabled && states[animator].controller == nullptr,
	      "legacy mode still detaches and disables the official Animator");
	Check(directorPauses == 1, "legacy retains its defensive Timeline pause");
	player.controller.preserveAnimatorForSecondaryPhysics = true;
	player.Tick(1.0f / 60.0f, actor, animator);
	Check(states[animator].enabled && states[animator].controller == reinterpret_cast<void*>(0x700) && states[animator].
	      rebinds == 0,
	      "hot preserve switch restores our legacy changes without resetting dynamic state");
	Check(QuatNear(player.controller.currentBinding.restRotations[0], originalRest[0]) &&
	      player.controller.currentBinding.groundLift == originalGround,
	      "A/B switch preserves bone rest and root-height snapshots");
	player.Stop();
	player.Stop();
	Check(states[animator].rebinds == 0 && states[animator].enabled,
	      "Stop does not rebind an untouched live graph; repeated Stop is harmless");

	states[animator] = State{};
	MotionPlayer liveChanges;
	Wire(liveChanges);
	liveChanges.controller.preserveAnimatorForSecondaryPhysics = true;
	liveChanges.Tick(1.0f / 60.0f, actor, animator);
	// Changes made by the game while we were leaving its graph alone belong
	// to the game. Snapshot at the actual legacy takeover, not the early bind.
	states[animator].enabled = false;
	states[animator].controller = reinterpret_cast<void*>(0x702);
	liveChanges.Tick(1.0f / 60.0f, actor, animator);
	Check(!states[animator].enabled && states[animator].enableWrites == 0,
	      "preserve never forces a game-disabled Animator on");
	liveChanges.controller.preserveAnimatorForSecondaryPhysics = false;
	liveChanges.Tick(1.0f / 60.0f, actor, animator);
	liveChanges.Stop();
	Check(!states[animator].enabled && states[animator].controller == reinterpret_cast<void*>(0x702),
	      "switch to legacy snapshots the current game state rather than stale bind-time state");

	states[animator] = State{};
	states[animator].enabled = false;
	MotionPlayer disabled;
	Wire(disabled);
	disabled.Tick(1.0f / 60.0f, actor, animator);
	Check(!states[animator].enabled && states[animator].controller == nullptr, "default remains legacy suppression");
	disabled.Stop();
	disabled.Stop();
	Check(!states[animator].enabled && states[animator].controller == reinterpret_cast<void*>(0x700) && states[animator]
	      .rebinds == 1,
	      "Stop restores initially disabled Animator and performs a single Rebind");

	states[animator] = State{};
	MotionPlayer replacement;
	Wire(replacement);
	replacement.Tick(1.0f / 60.0f, actor, animator);
	states[animator].controller = reinterpret_cast<void*>(0x701);
	replacement.Tick(1.0f / 60.0f, actor, animator);
	replacement.Stop();
	Check(states[animator].controller == reinterpret_cast<void*>(0x701),
	      "timeline controller replacement is returned on Stop");
	replacement.state = PlaybackState::Preview;
	replacement.Tick(1.0f / 60.0f, actor, animator);
	replacement.Stop();
	Check(states[animator].enabled && states[animator].rebinds == 2,
	      "second takeover captures live Animator state with retained bone binding");

	states[animator] = State{};
	MotionPlayer gated;
	Wire(gated);
	gated.controller.SetTargetActor(actor);
	auto other = reinterpret_cast<void*>(0x73);
	gated.Tick(1.0f / 60.0f, other, animator);
	gated.controller.DefensiveMute(other, animator, nullptr);
	Check(gated.controller.bindings.empty() && states[animator].enableWrites == 0 && states[animator].controllerWrites
	      == 0,
	      "non-target actor receives no Animator takeover even through direct mute entry");
	gated.Tick(1.0f / 60.0f, actor, nullptr);
	gated.Tick(1.0f / 60.0f, actor, animator);
	gated.Stop();
	Check(states[animator].enabled && states[animator].controller == reinterpret_cast<void*>(0x700),
	      "late Animator resolution captures state before the first mute");
	gated.controller.OnActorChanged(nullptr);
	const int releases = states[animator].rebinds;
	gated.Stop();
	Check(states[animator].rebinds == releases, "scene teardown never dereferences the discarded Animator snapshot");
}

static void TestSecondaryPhysicsMultiActorRestore()
{
	BeginTest("merged dev: per-actor Animator and face restoration");
	using namespace animatorFake;
	struct Face
	{
		float weight;
	};
	Face first{20.0f}, second{40.0f};
	auto animatorA = reinterpret_cast<void*>(0x81);
	auto animatorB = reinterpret_cast<void*>(0x82);
	states.clear();
	states[animatorA] = State{};
	states[animatorB] = State{};
	states[animatorB].enabled = false;
	states[animatorB].controller = reinterpret_cast<void*>(0x800);
	MotionPlayer player;
	Wire(player);
	player.clip.blendshape_names = {"mouth"};
	player.clip.blendshape_weights.assign(player.clip.total_frames, .75f);
	player.controller.faceApi = {
		[](void* actor) -> void* { return actor; },
		[](void*) { return 1; },
		[](void*, int) { return std::string("mouth"); },
		[](void* model, int) { return static_cast<Face*>(model)->weight; },
		[](void* model, int, float weight) { static_cast<Face*>(model)->weight = weight; }
	};
	player.Tick(1.0f / 60.0f, &first, animatorA);
	player.Tick(1.0f / 60.0f, &second, animatorB);
	Check(player.controller.bindings.size() == 2 && Near(first.weight, 75) && Near(second.weight, 75),
	      "ALL-target mode applies both face and body while owning two Animators");
	player.Stop();
	Check(states[animatorA].enabled && !states[animatorB].enabled &&
	      states[animatorA].controller == reinterpret_cast<void*>(0x700) &&
	      states[animatorB].controller == reinterpret_cast<void*>(0x800),
	      "Stop returns each actor's original Animator state and controller");
	Check(Near(first.weight, 20) && Near(second.weight, 40) &&
	      states[animatorA].rebinds == 1 && states[animatorB].rebinds == 1,
	      "both actors' faces restore and each Animator rebinds once");
	player.Stop();
	Check(states[animatorA].rebinds == 1 && states[animatorB].rebinds == 1,
	      "repeated Stop cannot duplicate a cached Animator restore");
	player.controller.preserveAnimatorForSecondaryPhysics = true;
	player.state = PlaybackState::Preview;
	player.Tick(1.0f / 60.0f, &first, animatorA);
	player.Tick(1.0f / 60.0f, &second, animatorB);
	player.Stop();
	Check(Near(first.weight, 20) && Near(second.weight, 40) &&
	      states[animatorA].rebinds == 1 && states[animatorB].rebinds == 1 && !states[animatorB].enabled,
	      "preserve mode restores both faces without rebinding or enabling native graphs");
}

static void TestSecondaryPhysicsConfig()
{
	BeginTest("secondary physics config declaration");
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "gkmotion_secondary_physics";
	const fs::path file = dir / "control.json";
	Core core;
	core.config.path = file.string();
	Check(core.config.preserveAnimatorForSecondaryPhysics == -1 && !core.player.controller.
	                                                                     preserveAnimatorForSecondaryPhysics,
	      "missing experiment is unset and legacy behavior stays default");
	WriteTextFile(file, R"({"secondary_physics":{"preserve_animator":true}})");
	Check(core.config.Reload(), "load preserve declaration");
	core.ApplyDeclarativeConfig();
	Check(core.player.controller.preserveAnimatorForSecondaryPhysics &&
	      core.StatusSummary().find("secondary=native") != std::string::npos,
	      "Core preserves requested Animator state while defaulting to native");
	WriteTextFile(file, R"({"gaze":{"mode":1}})");
	core.config.Reload();
	core.ApplyDeclarativeConfig();
	Check(core.player.controller.preserveAnimatorForSecondaryPhysics,
	      "unrelated config edit keeps current physics policy");
	WriteTextFile(file, R"({"secondary_physics":{"preserve_animator":"false"}})");
	core.config.Reload();
	core.ApplyDeclarativeConfig();
	Check(core.player.controller.preserveAnimatorForSecondaryPhysics,
	      "string is not accepted as an experimental boolean");
	WriteTextFile(file, R"({"secondary_physics":{"preserve_animator":false}})");
	core.config.Reload();
	core.ApplyDeclarativeConfig();
	Check(core.player.controller.preserveAnimatorForSecondaryPhysics, "native preserves its solver graph even with preserve_animator false");
	WriteTextFile(file, RuntimeConfig::DefaultTemplate());
	core.config.Reload();
	Check(core.config.preserveAnimatorForSecondaryPhysics == 0, "generated template keeps experiment off");
	Check(core.config.secondaryPhysicsBackend == 2, "native is the default backend");
	Check(!core.config.beforeQuartzExperiment && !core.config.sleeveMuscleDiagnostics,
	      "generated template keeps BeforeQuartz and reader diagnostics off");
	WriteTextFile(file, R"({"secondary_physics":{"before_quartz_experiment":true,"sleeve_muscle_diagnostics":true},"command":{"seq":1,"burst_trace":true}})");
	core.config.Reload();
	Check(core.config.beforeQuartzExperiment && core.config.sleeveMuscleDiagnostics,
	      "explicit boolean opt-in enables the two independent experiment flags");
	core.ConsumeConfigCommands();
	Check(core.burstTraceRequestedSeq == 1 && !core.config.HasPendingCommands(),
	      "bounded trace request survives command consumption for main-thread dispatch");
	core.burstTraceRequestedSeq = 0;
	core.config.Reload();
	core.ConsumeConfigCommands();
	Check(core.burstTraceRequestedSeq == 0, "same trace sequence cannot replay");
	WriteTextFile(file, R"({"secondary_physics":{"before_quartz_experiment":"true","sleeve_muscle_diagnostics":1}})");
	core.config.Reload();
	Check(!core.config.beforeQuartzExperiment && !core.config.sleeveMuscleDiagnostics,
	      "invalid types fail closed instead of retaining enabled experiment flags");
	WriteTextFile(file, R"({"secondary_physics":{"before_quartz_experiment":true,"sleeve_muscle_diagnostics":true}})");
	core.config.Reload();
	WriteTextFile(file, R"({"secondary_physics":{"backend":"native"}})");
	core.config.Reload();
	Check(!core.config.beforeQuartzExperiment && !core.config.sleeveMuscleDiagnostics,
	      "removing explicit experiment keys restores the original native path");
	core.ApplyDeclarativeConfig();
	Check(core.config.secondaryPhysicsBackend == 2, "native backend selection is retained");
	Check(core.player.controller.preserveAnimatorForSecondaryPhysics, "native keeps original solver graph running");
	WriteTextFile(file, R"({"secondary_physics":{"backend":"unknown"}})");
	core.config.Reload();
	Check(core.config.secondaryPhysicsBackend == 2, "unknown backend cannot replace a valid selection");
	WriteTextFile(file, R"({"secondary_physics":{"backend":"fallback","preserve_animator":false}})");
	core.config.Reload();
	core.ApplyDeclarativeConfig();
	Check(core.config.secondaryPhysicsBackend == 1 && !core.player.controller.preserveAnimatorForSecondaryPhysics,
	      "fallback has no native graph dependency");
	WriteTextFile(file, R"({"secondary_physics":{"backend":"off"}})");
	core.config.Reload();
	Check(core.config.secondaryPhysicsBackend == 0, "off explicitly disables secondary writes");
	core.ApplyDeclarativeConfig();
	core.QueuePhysicsBackend(2);
	Check(core.config.secondaryPhysicsBackend == 0 && core.guiPhysicsBackend.load() == 0,
	      "GUI backend request stays queued until Unity main-thread processing");
	core.ProcessPendingGuiCommands();
	Check(core.config.secondaryPhysicsBackend == 2 && core.player.controller.preserveAnimatorForSecondaryPhysics &&
	      core.guiPhysicsBackend.load() == 2, "GUI native selection applies graph policy and published selection");
	Check(!core.guiBeforeQuartzExperiment.load(), "GUI BeforeQuartz starts disabled");
	core.QueueBeforeQuartzExperiment(true);
	Check(!core.config.beforeQuartzExperiment && !core.guiBeforeQuartzExperiment.load(),
	      "GUI BeforeQuartz callback only queues until Unity main-thread processing");
	core.ProcessPendingGuiCommands();
	Check(core.config.beforeQuartzExperiment && core.guiBeforeQuartzExperiment.load(),
	      "GUI BeforeQuartz opt-in applies and republishes the native experiment");
	core.QueueBeforeQuartzExperiment(false);
	core.ProcessPendingGuiCommands();
	Check(!core.config.beforeQuartzExperiment && !core.guiBeforeQuartzExperiment.load(),
	      "GUI BeforeQuartz can return to the original native input path");
	core.QueueBeforeQuartzExperiment(true);
	core.ProcessPendingGuiCommands();
	core.QueuePhysicsBackend(1);
	core.ProcessPendingGuiCommands();
	Check(core.config.secondaryPhysicsBackend == 1 && !core.player.controller.preserveAnimatorForSecondaryPhysics,
	      "GUI SpringBone selection releases native graph policy");
	Check(!core.config.beforeQuartzExperiment && !core.guiBeforeQuartzExperiment.load(),
	      "leaving native switches BeforeQuartz off and republishes the cleared checkbox");
	core.QueueBeforeQuartzExperiment(true);
	core.ProcessPendingGuiCommands();
	Check(!core.config.beforeQuartzExperiment, "queued BeforeQuartz opt-in cannot enable in fallback");
	core.QueuePhysicsBackend(2);
	core.ProcessPendingGuiCommands();
	Check(!core.config.beforeQuartzExperiment, "returning to native does not restore an earlier opt-in");
	core.QueuePhysicsBackend(1);
	core.QueueBeforeQuartzExperiment(true);
	core.ProcessPendingGuiCommands();
	Check(!core.config.beforeQuartzExperiment,
	      "stale GUI enable after a queued backend switch remains fail closed");
	core.QueuePhysicsBackend(99);
	core.ProcessPendingGuiCommands();
	Check(core.config.secondaryPhysicsBackend == 1, "invalid GUI backend cannot alter current selection");
	WriteTextFile(file, R"({"secondary_physics":{"backend":"native","before_quartz_experiment":true}})");
	core.config.Reload();
	core.ApplyDeclarativeConfig();
	Check(core.guiBeforeQuartzExperiment.load(), "file opt-in republishes the GUI checkbox");
	WriteTextFile(file, R"({"secondary_physics":{"backend":"native"}})");
	core.config.Reload();
	core.ApplyDeclarativeConfig();
	Check(!core.guiBeforeQuartzExperiment.load(), "omitted experiment key clears the GUI checkbox on reload");
	WriteTextFile(file, R"({"secondary_physics":{"backend":"off","before_quartz_experiment":true}})");
	core.config.Reload();
	core.ApplyDeclarativeConfig();
	Check(!core.config.beforeQuartzExperiment && !core.guiBeforeQuartzExperiment.load(),
	      "declarative physics off cannot retain an enabled BeforeQuartz experiment");
	fs::remove(file);
}


#include "test_library_safety.hpp"
#include "test_ensemble_integration.hpp"

static void TestPlaylistLifecycle(const std::string& repoRoot)
{
	BeginTest("playlist_lifecycle");
	namespace fs = std::filesystem;
	const auto root = fs::path(repoRoot) / "build" / "tests" / ("gkmotion_playlist_regression_" +
		std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	for (const char* name : {"song", "song_b", "song_c"})
		LibraryDeleteFixture::WriteClip(root / "motions" / name / "motion.gkclip");
	Core core;
	core.Initialize(root.string());
	core.SelectMotionByIndex(1, true);
	const auto loaded = core.player.loadedMotionFile;
	Check(core.MoveMotion("song_b", -1), "move selected track upwards");
	Check(core.currentMotionIndex == 0 && core.player.loadedMotionFile == loaded,
	      "reordering preserves selected track and running clip");
	core.QueueSelectMotion(2, false);
	Check(core.currentMotionIndex == 0, "GUI queues selection without changing main-thread index");
	Core::GuiCommand deletion{Core::GuiAction::DeleteFiles, 0, true, "song"};
	deletion.libraryRevision = core.libraryRevision;
	core.QueueGui(std::move(deletion));
	core.ProcessPendingGuiCommands();
	Check(core.CurrentMotion() && core.CurrentMotion()->name == "song_c",
	      "commands identify songs independently of row renumbering");
	core.RefreshMotionLibrary();
	Check(core.motions.size() == 2 && FindPlaylistIndex(core.motions, "song") < 0,
	      "refresh does not resurrect deleted song or confuse name prefixes");
	Check(!fs::exists(root / "motions" / "song"), "confirmed deletion removes motion directory permanently");
	Core restarted;
	restarted.Initialize(root.string());
	Check(restarted.motions.size() == 2 && restarted.motions.front().name == "song_b",
	      "restart retains remaining playlist order");
	const auto before = restarted.playlist.order;
	WriteTextFile(root / "bad.json", R"({"version":1,"mode":3,"order":[4],"removed":[]})");
	std::string error;
	Check(!restarted.playlist.Load((root / "bad.json").string(), error) && restarted.playlist.order == before,
	      "malformed saved list cannot partially mutate current list");
	core.DeleteMotionFiles("song_c", true);
	Check(core.currentMotionIndex == -1 && core.player.clip.total_frames == 0 && core.player.state ==
	      PlaybackState::Stopped,
	      "deleting current song stops sound, restores pose and clears clip");
	core.DeleteMotionFiles("song_b", true);
	Check(core.motions.empty(), "last song can be permanently deleted");
	for (const char* name : {"song", "song_b", "song_c"})
		LibraryDeleteFixture::WriteClip(root / "motions" / name / "motion.gkclip");
	core.RefreshMotionLibrary();
	Check(core.motions.size() == 3, "new external imports become visible on refresh");
	core.playlist.mode = PlaylistMode::Shuffle;
	std::unordered_set<int> seen;
	int index = 0;
	for (int i = 0; i < 3; ++i)
	{
		index = core.playlist.Next(core.motions, index);
		seen.insert(index);
	}
	Check(seen.size() == 3, "shuffle visits every song once in each round");
	const int last = index;
	Check(core.playlist.Next(core.motions, index) != last, "shuffle does not repeat at round boundary");
	core.player.fnPlayAudio = FakePlayAudio;
	core.SelectMotionByIndex(0, true);
	core.player.Pause();
	core.SelectMotionByIndex(1, false);
	g_playCalls = 0;
	Check(core.player.state == PlaybackState::Paused, "switching songs while paused preserves pause state");
	core.player.Resume();
	Check(g_playCalls == 1 && core.player.audioStarted, "resuming a new paused song opens its own audio");
	core.playlist.mode = PlaylistMode::ListLoop;
	core.player.loop = false;
	core.SelectMotionByIndex(0, true);
	core.OnLateUpdate(reinterpret_cast<void*>(0x1000), nullptr, nullptr, 1000);
	Check(core.currentMotionIndex == 1 && core.player.state == PlaybackState::Playing,
	      "reaching end automatically starts next playlist song");
	core.playlist.mode = PlaylistMode::Once;
	core.OnLateUpdate(reinterpret_cast<void*>(0x1000), nullptr, nullptr, 1000);
	Check(core.player.state == PlaybackState::Stopped, "play-once ends without changing song");
	core.player.Stop();
	restarted.player.Stop();
	fs::remove_all(root);
}

static void TestInitialYaw()
{
	BeginTest("initial_yaw_turns_pose_in_place");
	MotionClip clip = MakeSmallClip();
	clip.format = MotionClip::Format::kGkClip;
	clip.version = 4;
	clip.bones = {BoneMeta{0, "Hips", false}, BoneMeta{13, "LeftUpperArm", false}};
	clip.bone_parent_hids = {-1, 0};
	clip.root_positions = {{.4f, .2f, .8f}, {.4f, .2f, .8f}};
	const Quaternion original = Quaternion::FromEulerDegreesXYZ(12, 5, -8);
	const Quaternion child = Quaternion::FromEulerDegreesXYZ(15, 0, 30);
	clip.bone_rotations = {original, child, original, child};
	IdolController controller;
	WireControllerCallbacks(controller);
	controller.fnSetLocalRotation = fake::SetWorldRot;
	controller.fnGetTransformParent = [](void*) -> void* { return reinterpret_cast<void*>(0x909); };
	IdolBoneBinding binding;
	binding.actor = reinterpret_cast<void*>(0x901);
	binding.actorTransform = reinterpret_cast<void*>(0x902);
	binding.rootTransform = reinterpret_cast<void*>(0x903);
	binding.bindPoseCaptured = true;
	binding.restActorPos = {3, 0, 4};
	binding.restRootPos = {.1f, .8f, -.2f};
	binding.boneTransforms = {binding.rootTransform, reinterpret_cast<void*>(0x904)};
	binding.restRotations = {Quaternion::Identity(), Quaternion::Identity()};
	const Quaternion parent = Quaternion::FromEulerDegreesXYZ(20, 35, 10);
	fake::worldPos[binding.actorTransform] = binding.restActorPos;
	fake::worldRot[binding.actorTransform] = Quaternion::FromEulerDegreesXYZ(0, 20, 0);
	fake::worldRot[reinterpret_cast<void*>(0x909)] = parent;
	controller.currentBinding = binding;
	fake::rotationCalls.clear();
	controller.ApplyMotion(clip, 0);
	const auto originalPosition = fake::worldPos[binding.rootTransform];
	const auto originalRoot = fake::rotationCalls[0].second;
	const auto originalChild = fake::rotationCalls[1].second;
	Check(controller.SetInitialYawDegrees(90), "finite yaw accepted");
	fake::rotationCalls.clear();
	controller.ApplyMotion(clip, 0);
	const auto turn = Quaternion::FromEulerDegreesXYZ(0, 90, 0);
	Check(QuatNear(parent * fake::rotationCalls[0].second, turn * parent * originalRoot),
	      "world-up turn is transformed to the actual root parent frame");
	Check(QuatNear(fake::rotationCalls[1].second, originalChild),
	      "child local motion stays authored under the rotated root");
	const auto turnedPosition = fake::worldPos[binding.rootTransform];
	Check(std::fabs(turnedPosition.x-originalPosition.x) < 1e-5f &&
	      std::fabs(turnedPosition.y-originalPosition.y) < 1e-5f &&
	      std::fabs(turnedPosition.z-originalPosition.z) < 1e-5f,
	      "turning never moves the character's current root position");
	fake::rotationCalls.clear(); controller.ApplyMotion(clip, 0);
	Check(QuatNear(parent * fake::rotationCalls[0].second, turn * parent * originalRoot),
	      "repeated samples do not accumulate yaw");
	fake::worldRot[reinterpret_cast<void*>(0x909)] = {0, 0, 0, 0};
	fake::rotationCalls.clear(); controller.ApplyMotion(clip, 0);
	Check(QuatNear(fake::rotationCalls[0].second, originalRoot),
	      "invalid parent rotation leaves the authored root pose intact");
	fake::worldRot[reinterpret_cast<void*>(0x909)] = parent;
	Check(!controller.SetInitialYawDegrees(std::numeric_limits<float>::quiet_NaN()) &&
	      controller.initialYawDegrees == 90.0f, "invalid yaw cannot overwrite valid state");
	controller.SetInitialYawDegrees(0);
	fake::rotationCalls.clear(); controller.ApplyMotion(clip, 0);
	Check(QuatNear(fake::rotationCalls[0].second, originalRoot), "reset restores the original root orientation");

	// Motion translation follows the yaw around the clip's first root sample,
	// including a rotated actor basis and a world-space placement offset.
	clip.root_positions[1] = {1.4f, .5f, 2.8f};
	controller.SetInitialPositionOffset({2, .1f, -3});
	controller.SetInitialYawDegrees(0);
	controller.ApplyMotion(clip, 0);
	const Vector3 start = fake::worldPos[binding.rootTransform];
	controller.ApplyMotion(clip, 1.0f / clip.fps);
	const Vector3 unturnedEnd = fake::worldPos[binding.rootTransform];
	controller.SetInitialYawDegrees(90);
	controller.ApplyMotion(clip, 0);
	const Vector3 turnedStart = fake::worldPos[binding.rootTransform];
	controller.ApplyMotion(clip, 1.0f / clip.fps);
	const Vector3 turnedEnd = fake::worldPos[binding.rootTransform];
	const Vector3 expectedEnd = start + turn.Rotate(unturnedEnd - start);
	auto nearPosition = [](const Vector3& a, const Vector3& b) {
		return std::fabs(a.x-b.x) < 1e-5f && std::fabs(a.y-b.y) < 1e-5f && std::fabs(a.z-b.z) < 1e-5f;
	};
	Check(nearPosition(turnedStart, start), "yaw preserves the first-frame position including world placement offset");
	Check(nearPosition(turnedEnd, expectedEnd), "yaw rotates animated travel around the first frame in world coordinates");
	controller.ApplyMotion(clip, 1.0f / clip.fps);
	Check(nearPosition(fake::worldPos[binding.rootTransform], expectedEnd), "yawed travel does not accumulate on repeated samples");
	controller.fnSetWorldPosition = nullptr;
	controller.fnSetLocalPosition = fake::SetWorldPos;
	controller.ApplyMotion(clip, 1.0f / clip.fps);
	const Vector3 fallbackWorld = binding.restActorPos + fake::worldRot[binding.actorTransform].Rotate(fake::worldPos[binding.rootTransform]);
	Check(nearPosition(fallbackWorld, expectedEnd), "local-position fallback produces the same yawed world travel");
	controller.fnSetLocalPosition = nullptr;
	controller.fnSetWorldPosition = fake::SetWorldPos;
	controller.SetInitialYawDegrees(0);
	controller.fnSetLocalRotation = nullptr;
	fake::rotationCalls.clear(); controller.ApplyMotion(clip, 0);
	const auto gkWorldBefore = fake::rotationCalls[0].second;
	controller.SetInitialYawDegrees(90);
	fake::rotationCalls.clear(); controller.ApplyMotion(clip, 0);
	Check(QuatNear(fake::rotationCalls[0].second, turn * gkWorldBefore),
	      "degraded gkclip world-write path applies yaw in world coordinates");

	clip.format = MotionClip::Format::kGkWorldDump;
	controller.SetInitialYawDegrees(0);
	fake::rotationCalls.clear(); controller.ApplyMotion(clip, 0);
	const auto worldBefore = fake::rotationCalls[0].second;
	controller.SetInitialYawDegrees(-90);
	fake::rotationCalls.clear(); controller.ApplyMotion(clip, 0);
	Check(QuatNear(fake::rotationCalls[0].second, Quaternion::FromEulerDegreesXYZ(0, -90, 0) * worldBefore),
	      "world-rotation streams turn in world coordinates too");
}

static void TestInitialPosition()
{
	BeginTest("initial_position_world_translation");
	const auto near = [](const Vector3& a, const Vector3& b)
	{
		return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f && std::fabs(a.z - b.z) < 1e-5f;
	};
	MotionClip clip = MakeSmallClip();
	clip.root_positions[1] = {0.4f, 0.2f, 0.8f};
	IdolController controller;
	WireControllerCallbacks(controller);
	controller.useRestRotations = false;
	IdolBoneBinding binding;
	binding.actor = reinterpret_cast<void*>(0x901);
	binding.actorTransform = reinterpret_cast<void*>(0x902);
	binding.rootTransform = reinterpret_cast<void*>(0x903);
	binding.bindPoseCaptured = true;
	binding.defensivelyMuted = true; // Synthetic fixture represents an owned, driven pose.
	binding.restRootPos = {0.1f, 0.8f, -0.2f};
	binding.restActorPos = {3.0f, 0.0f, 4.0f};
	binding.restActorRot = Quaternion::FromEulerDegreesXYZ(0, 90, 0);
	binding.groundLift = 0.7f;
	binding.boneTransforms = {reinterpret_cast<void*>(0x904), reinterpret_cast<void*>(0x905)};
	binding.restRotations = {Quaternion::Identity(), Quaternion::Identity()};
	fake::worldPos[binding.actorTransform] = binding.restActorPos;
	fake::worldRot[binding.actorTransform] = binding.restActorRot;
	controller.currentBinding = binding;
	controller.ApplyMotion(clip, 0);
	const Vector3 before0 = fake::worldPos[binding.rootTransform];
	fake::rotationCalls.clear();
	controller.ApplyMotion(clip, 1.0f / clip.fps);
	const Vector3 before1 = fake::worldPos[binding.rootTransform];
	const auto rotations = fake::rotationCalls;

	const Vector3 offset(0.6f, 0.3f, -1.2f);
	Check(controller.SetInitialPositionOffset(offset), "valid XYZ initial placement accepted");
	fake::rotationCalls.clear();
	controller.ApplyMotion(clip, 1.0f / clip.fps);
	const Vector3 after1 = fake::worldPos[binding.rootTransform];
	Check(near(after1 - before1, offset), "XYZ shifts world position without rotating with actor yaw");
	Check(fake::rotationCalls.size() == rotations.size() &&
	      QuatNear(fake::rotationCalls[0].second, rotations[0].second),
	      "placement does not change bone rotation output");
	controller.ApplyMotion(clip, 0);
	const Vector3 after0 = fake::worldPos[binding.rootTransform];
	Check(near(after1 - after0, before1 - before0),
	      "initial placement preserves full animated displacement including jump height");
	for (int i = 0; i < 10; ++i) controller.ApplyMotion(clip, 0);
	Check(near(fake::worldPos[binding.rootTransform], after0), "replaying a pose cannot accumulate initial placement");
	Check(near(controller.currentBinding.restRootPos, binding.restRootPos) && controller.currentBinding.groundLift ==
	      binding.groundLift,
	      "initial placement leaves captured rest and ground correction intact");
	controller.currentBinding.clipGroundAnchored = true;
	controller.SetInitialPositionOffset({0, 0, 0});
	controller.ApplyMotion(clip, 0);
	const auto groundBefore = fake::worldPos[binding.rootTransform];
	controller.SetInitialPositionOffset(offset);
	controller.ApplyMotion(clip, 0);
	Check(near(fake::worldPos[binding.rootTransform] - groundBefore, offset),
	      "XYZ also applies to clip-wide floor anchored playback");
	const Vector3 worldTarget = fake::worldPos[binding.rootTransform];
	controller.fnSetWorldPosition = nullptr;
	controller.fnSetLocalPosition = fake::SetWorldPos;
	controller.ApplyMotion(clip, 0);
	Check(near(binding.restActorPos + binding.restActorRot.Rotate(fake::worldPos[binding.rootTransform]), worldTarget),
	      "local-position fallback produces same XYZ scene displacement");
	controller.fnSetWorldPosition = fake::SetWorldPos;
	controller.RestoreMute();
	Check(near(fake::worldPos[binding.rootTransform],
	           binding.restActorPos + binding.restActorRot.Rotate(binding.restRootPos)),
	      "stop restores original scene position without baking user offset into rest pose");
	Check(!controller.SetInitialPositionOffset({0, std::numeric_limits<float>::quiet_NaN(), 0}) && near(
		      controller.initialPositionOffset, offset),
	      "nonfinite position cannot replace valid placement");
	controller.SetInitialPositionOffset({0, 0, 0});
	controller.ApplyMotion(clip, 0);
	Check(near(fake::worldPos[binding.rootTransform], groundBefore),
	      "all-axis reset restores default playback placement");

	BeginTest("initial_position_commands_and_persistence");
	namespace fs = std::filesystem;
	const auto root = fs::temp_directory_path() / "gkmotion_initial_position_regression";
	fs::remove_all(root);
	Core core;
	core.configRoot = (root / "GakuMotion").string();
	core.initialPositionPath = (root / "GakuMotion" / "initial_position.json").string();
	core.QueueInitialPosition({1, 2, 3});
	core.QueueInitialPosition(offset);
	core.QueueInitialYaw(45.0f);
	Check(near(core.player.controller.initialPositionOffset, {0, 0, 0}),
	      "GUI placement command does not mutate main-thread state early");
	core.QueueGui({Core::GuiAction::SaveInitialPosition});
	core.ProcessPendingGuiCommands();
	Check(near(core.player.controller.initialPositionOffset, offset) && core.player.controller.initialYawDegrees == 45,
	      "queued placement and save use final XYZ and yaw values in order");
	Core restarted;
	restarted.Initialize(root.string());
	Check(near(restarted.player.controller.initialPositionOffset, offset) && restarted.player.controller.initialYawDegrees == 45,
	      "saved initial position and yaw load on next startup");
	WriteTextFile(core.initialPositionPath, R"({"version":1,"space":"world","offset_m":[1,"bad",2]})");
	Check(!core.LoadInitialPosition() && near(core.player.controller.initialPositionOffset, offset),
	      "invalid saved XYZ leaves valid runtime placement intact");
	core.QueueInitialPosition({0, 0, 0});
	core.QueueInitialYaw(0);
	core.QueueGui({Core::GuiAction::SaveInitialPosition});
	core.ProcessPendingGuiCommands();
	Check(restarted.LoadInitialPosition() && near(restarted.player.controller.initialPositionOffset, {0, 0, 0}) &&
	      restarted.player.controller.initialYawDegrees == 0,
	      "saved position and yaw reset persists across restart");
	WriteTextFile(core.initialPositionPath, R"({"version":1,"space":"world","offset_m":[1,2,3]})");
	Check(restarted.LoadInitialPosition() && near(restarted.player.controller.initialPositionOffset, {1, 2, 3}) &&
	      restarted.player.controller.initialYawDegrees == 0, "legacy position files load with zero yaw");
	core.player.controller.facialWeights = {0.8f, 0.9f, 0.7f, 0.5f};
	core.QueueGui({Core::GuiAction::SaveFacialWeights});
	core.ProcessPendingGuiCommands();
	Check(restarted.LoadFacialWeights() && Near(restarted.player.controller.facialWeights.overall, 0.8f) &&
	      Near(restarted.player.controller.facialWeights.mouth, 0.5f),
	      "facial weight settings persist and reload");
	WriteTextFile(core.configRoot + "/facial_weights.json", R"({"version":1,"overall":1,"brow":1,"eye":1,"mouth":3})");
	Check(!restarted.LoadFacialWeights() && Near(restarted.player.controller.facialWeights.mouth, 0.5f),
	      "invalid facial settings leave current weights intact");
	static float appliedVolume = -1.0f;
	core.player.fnSetAudioVolume = [](float value) { appliedVolume = value; return true; };
	core.QueueGui({Core::GuiAction::MusicVolume, 0.35f});
	core.QueueGui({Core::GuiAction::SaveMusicVolume});
	core.ProcessPendingGuiCommands();
	Check(Near(core.player.musicVolume, 0.35f) && Near(appliedVolume, 0.35f),
	      "queued music volume reaches the separate audio callback");
	Check(restarted.LoadMusicVolume() && Near(restarted.player.musicVolume, 0.35f),
	      "music volume persists across restart");
	WriteTextFile(core.configRoot + "/music_volume.json", R"({"version":1,"volume":2})");
	Check(!restarted.LoadMusicVolume() && Near(restarted.player.musicVolume, 0.35f),
	      "invalid persisted volume leaves the current setting intact");
	Check(!core.player.SetMusicVolume(std::numeric_limits<float>::quiet_NaN()) && Near(appliedVolume, 0.35f),
	      "nonfinite volume is rejected without calling the backend");
	Check(core.player.SetMusicVolume(-1.0f) && Near(appliedVolume, 0.0f) &&
	      core.player.SetMusicVolume(2.0f) && Near(appliedVolume, 1.0f),
	      "music volume is bounded between silence and full volume");
	fs::remove_all(root);
}

static void TestOrderedPlaybackCommands()
{
	BeginTest("ordered_playback_commands");
	Core core;
	core.player.clip.total_frames = 900;
	core.player.clip.duration_seconds = 30;
	core.player.state = PlaybackState::Playing;
	core.player.loadedMotionFile = "first";
	core.player.fnSeekAudio = FakeSeekAudio;
	g_seekCalls = 0;
	core.QueueScrubBegin(1);
	core.QueueScrubUpdate(5);
	core.QueueScrubEnd(7);
	core.QueueScrubBegin(8);
	core.QueueScrubUpdate(12);
	core.QueueScrubEnd(15);
	core.ProcessPendingGuiCommands();
	Check(!core.player.scrubbing && core.player.currentTime == 15 && g_seekCalls == 2,
	      "two drags between game frames commit in order and seek twice");
	core.QueueSeek(-5);
	core.ProcessPendingGuiCommands();
	Check(core.player.currentTime == 0, "negative GUI seek clamps to beginning instead of disappearing");
	core.QueueScrubBegin(3);
	core.QueueStop();
	core.ProcessPendingGuiCommands();
	Check(!core.player.scrubbing && core.player.state == PlaybackState::Stopped, "stop cancels pending active scrub");
	core.QueueScrubBegin(5);
	core.QueueScrubEnd(10);
	core.player.loadedMotionFile = "second";
	core.ProcessPendingGuiCommands();
	Check(!core.player.scrubbing && core.player.currentTime == 0, "old clip drag events do not seek a new song");
	core.player.SetMuted(true);
	core.player.Play();
	core.player.AdvanceClock(0.5f);
	Check(core.player.currentTime == 0.5f, "muted playback advances with local clock");
	core.player.loop = true;
	core.player.fnPlayAudio = FakePlayAudio;
	g_playCalls = 0;
	core.player.Tick(1000, reinterpret_cast<void*>(0x1000));
	Check(g_playCalls == 0, "looping cannot restart sound during muted preview");
	core.player.BeginScrub(1);
	core.player.LoadMotion("missing");
	core.player.Stop();
	Check(!core.player.scrubbing, "stop always releases scrub clock");
}

#include "test_camera.hpp"
#include "test_camera_core.hpp"
#include "test_face_ownership.hpp"
#include "test_facial_continuity.hpp"
#include "test_miku_facial_fit.hpp"
#include "test_rinha_facial_fit.hpp"
#include "test_scene_actor_selection.hpp"
#include "test_waist_support.hpp"
#include "test_waist_support_core.hpp"
#include "test_waist_support_integration.hpp"
#include "test_scene_nodes.hpp"

static void TestInjectionRuntimeBoundary()
{
	BeginTest("runtime injection boundary and capture resume");
	Core core;
	core.enforceInjectionPolicy = true;
	core.player.clip = MakeSmallClip();
	core.player.loadedMotionFile = "capture-fixture";
	core.currentActor = reinterpret_cast<void*>(0x1234);
	core.QueuePlay();
	core.QueueScrubBegin(.4f);
	core.ProcessPendingGuiCommands();
	Check(core.player.state == PlaybackState::Stopped && !core.player.scrubbing,
		"unknown scene drops transport and scrub controls before actor access");
	core.injectionPolicy.SetContext(injection::Context::Photography);
	core.QueuePlay(); core.ProcessPendingGuiCommands();
	Check(core.player.state == PlaybackState::Stopped, "missing capture protection prevents playback");
	core.injectionPolicy.SetCaptureHooksReady(true);
	core.fnBeforeInjection = [] { return false; };
	core.QueuePlay(); core.ProcessPendingGuiCommands();
	Check(core.player.state == PlaybackState::Stopped, "local pre-injection refusal prevents the first mutation");
	core.fnBeforeInjection = nullptr;
	core.QueuePlay(); core.ProcessPendingGuiCommands();
	core.player.currentTime = .35f;
	Check(core.player.state == PlaybackState::Playing, "trusted mode with capture protection permits playback without network isolation");
	core.injectionPolicy.BeginCapture();
	Check(core.SuspendForCapture() && core.player.state == PlaybackState::Stopped,
		"capture restores native ownership before original capture work");
	core.QueuePlay(); core.QueueSeek(.7f); core.ProcessPendingGuiCommands();
	core.OnLateUpdate(core.currentActor, nullptr, nullptr, .3f);
	Check(core.player.state == PlaybackState::Stopped && core.player.currentTime == .35f,
		"capture await blocks commands and clock advancement");
	core.injectionPolicy.EndCapture(); core.ResumeAfterCapture();
	Check(core.player.state == PlaybackState::Playing && core.player.currentTime == .35f,
		"terminal capture restores the same playback state and time");
	core.player.state = PlaybackState::Paused;
	core.injectionPolicy.BeginCapture(); core.SuspendForCapture();
	core.injectionPolicy.EndCapture(); core.ResumeAfterCapture();
	Check(core.player.state == PlaybackState::Paused && core.player.currentTime == .35f,
		"paused capture resume does not start playback");
	core.injectionPolicy.BeginCapture(); core.SuspendForCapture();
	core.currentActor = reinterpret_cast<void*>(0x5678); // Normal same-frame actor polling.
	core.injectionPolicy.EndCapture(); core.ResumeAfterCapture();
	Check(core.player.state == PlaybackState::Paused, "normal multi-actor polling retains the suspended session");
	core.injectionPolicy.BeginCapture(); core.SuspendForCapture();
	core.OnActorChanged(reinterpret_cast<void*>(0x8765));
	core.injectionPolicy.EndCapture(); core.ResumeAfterCapture();
	Check(core.player.state == PlaybackState::Stopped, "actor replacement cannot inherit a suspended session");
	core.player.state = PlaybackState::Playing;
	core.config.cmdPlay = true;
	core.injectionPolicy.SetContext(injection::Context::Unknown);
	core.RevokeInjection(); core.ConsumeConfigCommands();
	Check(core.player.state == PlaybackState::Stopped && !core.config.cmdPlay,
		"leaving the allowed page restores native ownership and drops commands");
}

#include "test_playback_timing.hpp"
#include "test_track_timing.hpp"
#include "test_actorless_library.hpp"
#include "test_control_api.hpp"

int main(int argc, char** argv)
{
 std::setvbuf(stdout, nullptr, _IONBF, 0);
 const std::string repoRoot = argc > 1 ? argv[1] : ".";
 TestJsonLite();
 TestConfigTemplate();
 TestCommandSeq();
 TestBoneOverride();
 TestBlendshapePlayback();
 TestFaceFamilyOwnership();
 TestCharacterDefaultPriorPlayback();
 TestFacialSemanticBindingV2();
 TestFacialDefaultContinuity();
 TestMikuMouthFit();
 TestRinhaFacialFit();
 TestSceneActorSelection();
 TestEnsembleIntegration(repoRoot);
 TestFaceAnimatorCaptureLifecycle();
 TestFaceRuntimeMetadata();
 TestNativeTalkOwnership();
 TestSecondaryPhysicsAnimatorLifecycle();
 TestSecondaryPhysicsMultiActorRestore();
 TestSecondaryPhysicsConfig();
 TestOrderedPlaybackCommands();
 TestInitialPosition();
 TestInitialYaw();
 TestOverrideInjection();
 TestGkClipWristOverrideSpaces();
 TestWristDiagnosticIsOneShot();
 TestWristTwistRedistribution();
 TestAuxQuaternionSignInvariance();
 TestPlayerSeekSync();
 TestPlaybackSpeedAndClock();
 TestPlaybackTiming();
 TestTrackTimingMetadata();
 TestActorlessLibraryCommands(repoRoot);
 TestPlaylistLifecycle(repoRoot);
 TestLibrarySafety(repoRoot);
 TestHotkeyEdgeGate();
 TestScrubAndPreview();
 TestCameraController(repoRoot);
 TestCameraCoreWorkflow(repoRoot);
 TestWaistSupport();
 TestWaistSupportCore();
 TestWaistSupportIntegration();
 TestSceneNodes();
 TestControlApi();
 TestClipGroundAnchor();
 TestClipSwitchKeepsActorRest();
 TestPreviewWaitsForHumanoid();
 TestSceneIsolation();
 std::printf("Synthetic core: %d passed, %d failed\n", g_pass, g_fail);
 return g_fail == 0 ? 0 : 1;
}
