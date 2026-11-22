#pragma once

#include "../GkMotionData.hpp"
#include "../GkJsonLite.hpp"
#include <cmath>
#include <string>
#include <vector>

namespace GakuMotion
{
	// =========================================================================
	// 辅助骨（_Roll_H 扭转骨 / _H 半角支撑骨）驱动模式 —— 实机靶向对照实验
	// =========================================================================
	// 背景（见 DevAgents/reverse_engineering/official_quartz_pipeline.md §5）：
	//   离线单测 133/133 PASS 证明公式自洽，但实机袖筒依然塌陷 —— 数学自洽 ≠ 实机正确。
	//   为叫停「闭门推导」，提供运行时可切换的实机对照开关，让肉眼事实裁决：
	//     塌陷是「消失 / 减弱 / 完全不变」，从而判定辅助骨是否真为根因。
	enum AuxDriveMode : int
	{
		// 0 = 出厂默认：由「实机录制真值」标定出来的模型（局部空间单轴写入，见 GkAuxChannelModel.hpp）
		kAuxNormal = 0,
		kAuxSilence = 1, // 绝对静默：硬锁「初始绑定局部姿态」(Local Rest Rotation)，刚性随父跟动，零独立形变
		kAuxIdentity = 2, // 绝对归零：局部旋转强制 Identity(0,0,0,1)
		kAuxReplay = 3, // 真值直灌：把官方录制下来的辅助骨四元数逐帧原样写入内存
		kAuxLegacy = 4, // 旧的自洽解算注入（100% Bend + 50% Roll + 世界空间直写），仅保留作对照
		kAuxModeCount = 5
	};

	inline const char* AuxDriveModeName(int mode) noexcept
	{
		switch (mode)
		{
		case kAuxSilence: return "SILENCE(rest-lock)";
		case kAuxIdentity: return "IDENTITY(zero)";
		case kAuxReplay: return "REPLAY(ground-truth)";
		case kAuxLegacy: return "LEGACY(world-quartz)";
		case kAuxNormal:
		default: return "NORMAL(fitted-model)";
		}
	}

	// =========================================================================
	// AuxGroundTruth —— 辅助骨官方真值直灌数据源
	// =========================================================================
	// 由 GakuRecorder（实机内存读取）导出的 aux_replay.json 喂养。
	// 文件格式（与 gkmotion 同目录）：
	//   { "fps": 60.0,
	//     "tracks": [ { "humanoid_id": 13, "channel": "roll_h"|"joint_h",
	//                   "bone": "LeftArm_Roll_H",
	//                   "quaternions": [[x,y,z,w], ...] }, ... ] }
	// quaternions 一律为「局部空间 (localRotation)」逐帧真值，写入时不再做任何变换。
	struct AuxGroundTruth
	{
		struct Track
		{
			int humanoidId{-1};
			int channel{-1}; // 0 = _Roll_H, 1 = _H
			std::string boneName;
			std::vector<Quaternion> frames;
		};

		float fps{60.0f};
		std::vector<Track> tracks;
		bool loaded{false};
		std::string sourcePath;
		std::string lastError;

		bool Empty() const noexcept { return !loaded || tracks.empty(); }

		bool Load(const std::string& path, std::string& err)
		{
			tracks.clear();
			loaded = false;
			lastError.clear();
			jsonlite::Value root;
			if (!jsonlite::ParseFile(path, root, err))
			{
				lastError = err;
				return false;
			}
			if (!root.IsObject())
			{
				lastError = path + " -> top-level must be an object";
				err = lastError;
				return false;
			}

			fps = static_cast<float>(root.FloatPath("fps", 60.0f));
			if (fps <= 0.0f) fps = 60.0f;

			if (const jsonlite::Value* tr = root.Find("tracks"))
			{
				for (size_t i = 0; i < tr->Size(); ++i)
				{
					const jsonlite::Value* t = tr->At(i);
					if (!t || !t->IsObject()) continue;
					Track tk;
					tk.humanoidId = t->IntPath("humanoid_id", -1);
					const std::string chan = t->StringPath("channel", "");
					tk.channel = (chan == "roll_h") ? 0 : ((chan == "joint_h") ? 1 : -1);
					tk.boneName = t->StringPath("bone", "");
					if (tk.humanoidId < 0 || tk.channel < 0) continue;
					if (const jsonlite::Value* qs = t->Find("quaternions"))
					{
						tk.frames.reserve(qs->Size());
						for (size_t f = 0; f < qs->Size(); ++f)
						{
							const jsonlite::Value* q = qs->At(f);
							if (!q || q->Size() < 4)
							{
								tk.frames.clear();
								break;
							}
							tk.frames.emplace_back(
								static_cast<float>(q->At(0)->AsDouble(0.0)),
								static_cast<float>(q->At(1)->AsDouble(0.0)),
								static_cast<float>(q->At(2)->AsDouble(0.0)),
								static_cast<float>(q->At(3)->AsDouble(1.0)));
						}
					}
					if (!tk.frames.empty()) tracks.push_back(std::move(tk));
				}
			}
			loaded = true;
			sourcePath = path;
			return true;
		}

		// 返回 nullptr 表示该通道无真值数据（调用方降级为静默）
		const Quaternion* Sample(int humanoidId, int channel, float timeSeconds) const noexcept
		{
			if (Empty()) return nullptr;
			for (const Track& t : tracks)
			{
				if (t.humanoidId != humanoidId || t.channel != channel) continue;
				if (t.frames.empty()) return nullptr;
				long long idx = static_cast<long long>(std::floor(timeSeconds * fps + 0.5));
				if (idx < 0) idx = 0;
				if (idx >= static_cast<long long>(t.frames.size()))
				{
					idx = static_cast<long long>(t.frames.size()) - 1;
				}
				return &t.frames[static_cast<size_t>(idx)];
			}
			return nullptr;
		}
	};
} // namespace GakuMotion
