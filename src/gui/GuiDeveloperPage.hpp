#pragma once

// =============================================================================
// GuiDeveloperPage.hpp — 开发者页面（仅开发者模式显示：遥测/诊断/鞋底测量/Pitch/摄影舞台）
// =============================================================================

#include "GuiOverlayCommon.hpp"
#include "../GkGuiOverlay.hpp"
#include "GuiMotionExperiments.hpp"

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	inline void DrawDeveloperPage(GuiOverlay& overlay, const GuiContext& ctx)
	{
		ImGui::TextColored(GuiTheme::WarningColor(), "【开发者模式已激活】当前包含低频维护工具、实验性功能与内部遥测");
		ImGui::Separator();

		if (ImGui::BeginTabBar("##developer_sub_tabs"))
		{
			if (ImGui::BeginTabItem("动作"))
			{
				DrawMotionExperiments(ctx);
				bool muteSe = ctx.muteNativeSoundEffects;
				if (ImGui::Checkbox("播放 MMD 时屏蔽游戏音效（默认开启）", &muteSe))
				{
					if (ctx.fnSetMuteNativeSoundEffects) ctx.fnSetMuteNativeSoundEffects(ctx.self, muteSe);
				}
				ImGui::TextWrapped("仅在 MMD 表演期间静音游戏 Se 音效（含界面音效），不影响动作歌曲；停止播放、离开场景或受保护拍照时自动恢复，修改会持久化保存。");
				ImGui::Spacing();
				ImGui::Separator();
				ImGui::Spacing();
				overlay.DrawGazeTab(ctx);
				ImGui::EndTabItem();
			}

			// ---- 子页面 1：运行时诊断与遥测 ----
			if (ImGui::BeginTabItem("遥测与诊断"))
			{
				ImGui::Spacing();
				ImGui::TextUnformatted("内部状态诊断");
				if (!ctx.statusLine.empty())
				{
					ImGui::TextColored(GuiTheme::SuccessColor(), "%s", ctx.statusLine.c_str());
				}

				ImGui::Spacing();
				ImGui::Separator();
				ImGui::Spacing();

				// 播放器内部遥测
				if (ctx.player)
				{
					const auto& player = *ctx.player;
					ImGui::Text("播放器状态:");
					ImGui::BulletText("当前状态: %d (%s) | 当前时间: %.3fs",
					                  static_cast<int>(player.state),
					                  GuiOverlay::Core_PlaybackStateName(player.state),
					                  player.currentTime);
					ImGui::BulletText("剪辑信息: 时长=%.3fs | 总帧数=%u | FPS=%.1f | 骨骼数=%u",
					                  player.clip.duration_seconds,
					                  player.clip.total_frames,
					                  player.clip.fps,
					                  player.clip.bone_count);
					ImGui::BulletText("已载入文件: %s", player.loadedMotionFile.c_str());
					ImGui::BulletText("驱动 Actor: %p | 绑定骨骼: %zu",
					                  player.controller.currentBinding.actor,
					                  player.controller.currentBinding.boundBoneCount);
				}

				ImGui::Spacing();
				ImGui::Separator();
				ImGui::Spacing();

				// Scrub 探针遥测
				ImGui::Text("Scrub 拖动链路探针 (PROBE S1):");
				ImGui::BulletText("Begin 触发: %d 次 | Update 触发: %d 次 | End 触发: %d 次",
				                  overlay.probeScrubBegin, overlay.probeScrubUpdate, overlay.probeScrubEnd);
				ImGui::BulletText("最后拖动值: %.3f | active: %d",
				                  overlay.probeLastScrubValue, overlay.scrub.active ? 1 : 0);

				ImGui::Spacing();
				ImGui::Separator();
				ImGui::Spacing();

				ImGui::EndTabItem();
			}

			// ---- 子页面 2：鞋底测量 v2 ----
			if (ImGui::BeginTabItem("鞋底测量 v2"))
			{
				overlay.DrawSoleTab(ctx);
				ImGui::EndTabItem();
			}

			// ---- 子页面 3：Pitch Demo ----
			if (ImGui::BeginTabItem("Pitch Demo [Experimental]"))
			{
				overlay.DrawExperimentalSolePitchTab(ctx);
				ImGui::EndTabItem();
			}

			// ---- 子页面 4：尚未完成的载入替换设置，仅开发者可见 ----
			if (ImGui::BeginTabItem("摄影舞台替换"))
			{
				ImGui::Spacing();
				DrawStageLoadReplacementControl(ctx);
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("布光与视频"))
			{
				DrawStageStudio(overlay, ctx);
				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}
	}
} // namespace GakuMotion
#endif
