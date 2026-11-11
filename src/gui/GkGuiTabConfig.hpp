#pragma once

#include "../GkGuiOverlay.hpp"
#include "../GkRuntimeConfig.hpp"
#include "GkGuiThemeColors.hpp"

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	inline void GuiOverlay::DrawConfigTab(const GuiContext& ctx)
	{
		bool waistSupport = ctx.experimentalWaistSupport;
		if (ImGui::Checkbox("腰侧支撑 [Experimental]", &waistSupport) && ctx.fnSetExperimentalWaistSupport)
			ctx.fnSetExperimentalWaistSupport(ctx.self, waistSupport);
		ImGui::TextWrapped("默认关闭。仅恢复腰侧辅助骨的位置和方向；已验收范围限星南指定服装与时刻，大腿扭转支撑尚未恢复。关闭或停止会恢复启用前姿态。");
		ImGui::Separator();
		ImGui::TextUnformatted("物理引擎后端");
		int backend = ctx.physicsBackend;
		if (ImGui::RadioButton("内建 SpringBone", &backend, 1) && ctx.fnSetPhysicsBackend)
			ctx.fnSetPhysicsBackend(ctx.self, backend);
#ifdef GKMOTION_GUI_TEST
		fallbackPhysicsRectMin = ImGui::GetItemRectMin(); fallbackPhysicsRectMax = ImGui::GetItemRectMax();
#endif
		ImGui::TextDisabled("默认与保底方案；轻量弹簧骨，不包含官方碰撞模型。");
		if (ImGui::RadioButton("原生注入 [实验性 / Experimental]", &backend, 2) && ctx.fnSetPhysicsBackend)
			ctx.fnSetPhysicsBackend(ctx.self, backend);
#ifdef GKMOTION_GUI_TEST
		nativePhysicsRectMin = ImGui::GetItemRectMin(); nativePhysicsRectMax = ImGui::GetItemRectMax();
#endif
		ImGui::BeginDisabled(ctx.physicsBackend != 2);
		bool beforeQuartz = ctx.beforeQuartzExperiment;
		if (ImGui::Checkbox("BeforeQuartz 同源输入 [实验性]", &beforeQuartz) && ctx.fnSetBeforeQuartzExperiment)
			ctx.fnSetBeforeQuartzExperiment(ctx.self, beforeQuartz);
#ifdef GKMOTION_GUI_TEST
		beforeQuartzRectMin = ImGui::GetItemRectMin(); beforeQuartzRectMax = ImGui::GetItemRectMax();
#endif
		ImGui::EndDisabled();
		ImGui::TextWrapped("默认关闭。让 Quartz 与 Swing 消费同一份 MMD 身体姿态输入；仅原生注入可启用，切换后端会关闭此实验。");
		ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::WarningColor());
		ImGui::TextWrapped("警告：原生注入存在游戏崩溃风险，运行效果不稳定，依赖官方黑盒物理；游戏更新后可能失效。原生库不可用时会自动回退到内建 SpringBone。");
		ImGui::PopStyleColor();
		if (ctx.physicsBackend == 0)
			ImGui::TextDisabled("配置文件当前关闭物理；选择任一后端即可开启。");
		ImGui::TextWrapped("选择在本次运行中生效；重启或控制文件变更后读取 control.json 的设置。");
		ImGui::Separator();
		if (ctx.config)
		{
			ImGui::TextWrapped("控制文件（热加载）: %s", ctx.config->path.c_str());
			ImGui::TextDisabled("文件存在: %s | 轮询间隔: %.2fs | 已应用命令 seq: %lld",
			                    ctx.config->fileFound ? "是" : "否",
			                    ctx.config->pollIntervalSeconds,
			                    static_cast<long long>(ctx.config->appliedCommandSeq));
			if (!ctx.config->lastChangeLog.empty())
			{
				ImGui::TextWrapped("最近变更: %s", ctx.config->lastChangeLog.c_str());
			}
			if (!ctx.config->lastError.empty())
			{
				ImGui::TextColored(GuiTheme::ErrorColor(), "错误: %s", ctx.config->lastError.c_str());
			}
		}
		ImGui::Separator();
		ImGui::TextUnformatted("快捷键：F9 播放/暂停　F10 停止复位　,/. 上/下一曲　-/= 进度±5s　\\ 实验视线三态　Shift+F11 重载覆盖　Ctrl+F11 覆盖总开关");
	}
} // namespace GakuMotion
#endif
