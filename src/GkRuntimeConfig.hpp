#pragma once

#include "GkAudioPlayback.hpp"
#include "GkCamera.hpp"
#include "GkTrackTiming.hpp"

// =============================================================================
// GkRuntimeConfig.hpp — 运行时控制文件热加载层（阶段一「无 GUI」控制入口）
// =============================================================================
// 设计目标（对应批准计划的「选曲 / 进度 / 视线 / 角色 / 骨骼微调」五项诉求）：
//   在 ImGui 面板尚未接入（宿主未导出 ImGui 符号，见 ARCHITECTURE.md §6）的
//   阶段，先用「可热加载的 JSON 控制文件 + 快捷键」把全部控制能力跑通，
//   阶段二再用图形面板复用同一套状态机，不需要改动本文件的任何语义。
//
// 文件位置： <localRoot>/GakuMotion/control.json     （首次运行自动生成模板）
//
// schema 分两类字段，语义严格区分：
//   1) 声明式（declarative）—— 文件里写什么就是什么，持续生效：
//        selected_motion    : 目标曲目（文件夹名或 info.json 的 title 都可）
//        autoplay           : 选中曲目后是否立刻开始播放
//        gaze.mode          : 0=Auto 1=ForceOff 2=DesktopOn
//        gaze.desktop_enabled / eye_weight / head_weight / body_weight
//        use_rest_rotations : 是否启用「静息姿态相对校准」（true=现网稳健行为）
//        overrides.enabled  : 骨骼覆盖总开关
//        overrides.motion_file / overrides.global_file : 可选的自定义路径
//        poll_interval_seconds : 文件轮询间隔（默认 0.5s）
//   2) 一次性命令（one-shot）—— 靠单调递增的 seq 去重，执行一次即失效：
//        command.seq            : 必须比上一次大，否则整块命令被忽略
//        command.play / pause / stop
//        command.seek_seconds
//        command.reload_overrides
//        command.toggle_overrides
//     之所以用 seq 而不是「执行后回写清空」，是为了绝不改写用户手写的
//     文件（保留注释与排版），行为可预测、可复现。
// =============================================================================

#include "GkJsonLite.hpp"
#include "GkBoneOverride.hpp"
#include "config/GkMotionCatalog.hpp"
#include "config/GkTrackInfoJson.hpp"
#include "config/GkTrackMetadataWrite.hpp"
#include "config/GkLibraryScanner.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace GakuMotion
{
	// -----------------------------------------------------------------------------
	// 3. 运行时控制配置
	// -----------------------------------------------------------------------------
	class RuntimeConfig
	{
	public:
		// ---- 位置与节奏 ----
		std::string path; // control.json 全路径
		std::string rootDir; // <localRoot>/GakuMotion
		float pollIntervalSeconds{0.5f}; // 轮询间隔
		float pollInterval{0.5f}; // 兼容别名（外部可直接改）

		// ---- 播放控制 ----
		// 全局播放倍速（默认 1.0x），与每个文件的速度相乘。
		// 音频有效速度非 1 时静音并用本地时钟；配置兼容范围 [0.1, 5.0]。
		float playbackSpeedMultiplier{1.0f};
		CameraSettings camera;
		bool cameraDeclared{false};

		// ---- 声明式状态（-1 / 空 表示「未设置」，不覆盖运行时既有值） ----
		std::string selectedMotion;
		// 兼容旧配置键；产品运行时始终使用 C# NativeAOT，不启动 Python。
		std::string importBackend{"csharp"};
		bool autoPlayOnSelect{false};
		int gazeMode{-1}; // 0=Auto 1=ForceOff 2=DesktopOn
		int desktopLookAtEnabled{-1}; // 三态
		float eyeWeight{-1.0f};
		float headWeight{-1.0f};
		float bodyWeight{-1.0f};
		int useRestRotations{-1}; // 三态
		// gkclip 腿部静息基准开关（2026-10-07 A/B 实验用，见 GkIdolController 调用点）。
		// 三态：-1 未设置（走默认值 false）、0 关、1 开。
		int legRestBasis{-1};
		// gkclip 眼骨按角色实测几何响应缩放（#150 §8）。三态：-1 未设置（默认开）、
		// 0 关（raw 1:1 对照）、1 开。这是眼骨自身的角度缩放，不是 LookAt、也不是晚写。
		int eyePoseRelativeGain{-1};
		// Opt-in experiment: preserve the Animator graph for native hair/cloth jobs.
		// Missing keeps the existing runtime setting; default remains legacy mute.
		int preserveAnimatorForSecondaryPhysics{-1};
		// Native is preferred by default. It requires the optional
		// companion DLL and otherwise uses fallback; no private ABI lives here.
		int secondaryPhysicsBackend{2}; // 0=off, 1=fallback, 2=native
		bool beforeQuartzExperiment{false}; // Explicit opt-in; omission returns to OFF.
		bool sleeveMuscleDiagnostics{false}; // Bounded read-only native-reader samples.
		int overridesEnabled{-1}; // 三态
		int auxMode{-1}; // 三态：辅助骨驱动模式 0=normal 1=silence 2=identity 3=replay
		int experimentalWaistSupport{1}; // default-on Waist_H geometry; missing preserves session state
		int muteNativeSoundEffects{-1}; // 三态：-1 未设置(默认开启), 0 关, 1 开
		std::string overrideGlobalFile; // 可选自定义全局覆盖文件
		std::string overrideMotionFile; // 可选自定义曲目覆盖文件

		// ---- 面部表情默认基线与动态让位 ----
		int facialEnableBaseline{-1}; // 三态：-1 未设置(默认开启), 0 关, 1 开
		int facialMikuMouthFit{-1}; // Missing preserves session; fresh controller/config default is on.
		int facialMikuEyeFit{-1}; // Published eye/brow profile; missing preserves session, fresh default is on.
		int facialMikuFitAllowUnverified{-1}; // 0 refuses every component the bundle did not accept.
		float facialBaselineMultiplier{-1.0f}; // 默认 1.0f (基线强度倍率)
		std::string facialCharacterOverride{"auto"}; // Legacy field: identity always comes from the bound actor.

		// ---- 一次性命令（由 command.seq 去重） ----
		bool cmdPlay{false};
		bool cmdPause{false};
		bool cmdStop{false};
		bool cmdSeek{false};
		float cmdSeekSeconds{0.0f};
		bool cmdReloadOverrides{false};
		bool cmdToggleOverrides{false};

		struct SolePitchCheckCommand
		{
			float initialYM{0}, planeOffsetM{0}, maxPitchDegrees{20};
			bool enabled{false}, hasPlane{false}, useMeshEnvelope{false};
		} cmdSolePitchSettings;

		bool cmdSolePitchCheck{false}, cmdSolePitchSnapshot{false};
		bool cmdBurstTrace{false};

		// ---- 状态与诊断 ----
		bool fileFound{false};
		bool loadedOnce{false};
		std::string lastError;
		std::string lastChangeLog;
		FileSignature signature;
		long long appliedCommandSeq{0};
		long long lastSeenCommandSeq{0};
		uint64_t revision{0};

		bool HasPendingCommands() const noexcept
		{
			return cmdPlay || cmdPause || cmdStop || cmdSeek ||
				cmdReloadOverrides || cmdToggleOverrides || cmdSolePitchCheck || cmdSolePitchSnapshot || cmdBurstTrace;
		}

		// 命令消费后必须调用：一次性命令只允许执行一次
		void ClearCommands() noexcept
		{
			cmdPlay = cmdPause = cmdStop = cmdSeek = false;
			cmdReloadOverrides = cmdToggleOverrides = false;
			cmdSolePitchCheck = cmdSolePitchSnapshot = false;
			cmdBurstTrace = false;
			cmdSeekSeconds = 0.0f;
		}

		void ForceReload() noexcept { forceReload_ = true; }
		bool ReloadRequested() const noexcept { return forceReload_; }

		// ---- 模板生成：首次运行自动落盘，便于用户直接编辑 ----
		static std::string DefaultTemplate();

		// 首次运行时生成模板；已存在则不动
		bool EnsureFileExists();

		// -------------------------------------------------------------------------
		// 轮询：低频检查文件签名，变化时重新解析
		// forceReload = true 时立即重读（快捷键 F11 等场景）
		// -------------------------------------------------------------------------
		bool Poll(float deltaTime, bool forceReload = false);

		// -------------------------------------------------------------------------
		// 立即解析并应用声明式字段（命令块按 seq 去重后挂起到 HasPendingCommands）
		// -------------------------------------------------------------------------
		bool Reload();

	private:
		// "normal"/"silence"/"identity"/"replay" <-> 0/1/2/3；无法识别时返回 -1（表示不改动）
		static int ParseAuxMode(const jsonlite::Value* v) noexcept
		{
			if (!v) return -1;
			if (v->IsNumber())
			{
				int n = v->AsInt(-1);
				return (n >= 0 && n <= 3) ? n : -1;
			}
			if (v->IsBool()) return v->AsBool(true) ? 0 : 1;
			if (!v->IsString()) return -1;
			const std::string& s = v->stringValue;
			if (s == "normal" || s == "quartz") return 0;
			if (s == "silence" || s == "rest" || s == "lock") return 1;
			if (s == "identity" || s == "zero") return 2;
			if (s == "replay" || s == "gt") return 3;
			return -1;
		}

		float timer_{0.0f};
		bool forceReload_{false};

		static float Clamp01(float v) noexcept
		{
			if (v < 0.0f) return 0.0f;
			if (v > 1.0f) return 1.0f;
			return v;
		}

		static void AppendChange(std::string& log, const char* name,
		                         const std::string& before, const std::string& after)
		{
			if (before == after) return;
			if (!log.empty()) log += " | ";
			log += std::string(name) + ":'" + before + "'->'" + after + "'";
		}

		static void AppendChange(std::string& log, const char* name, int before, int after)
		{
			if (before == after) return;
			if (!log.empty()) log += " | ";
			log += std::string(name) + ":" + std::to_string(before) + "->" + std::to_string(after);
		}

		static void AppendChange(std::string& log, const char* name, float before, float after)
		{
			if (before == after) return;
			if (!log.empty()) log += " | ";
			char buf[128]{};
			std::snprintf(buf, sizeof(buf), "%s:%.3f->%.3f", name, before, after);
			log += buf;
		}
	};
} // namespace GakuMotion

#include "config/GkRuntimeConfigLifecycle.inl"
#include "config/GkRuntimeConfigReload.inl"
