#pragma once

// =============================================================================
// GuiSettingsPage.hpp — 设置页面（物理引擎 / 热键一览 / 开发者模式开关）
// =============================================================================

#include "GuiOverlayCommon.hpp"
#include "../GkGuiOverlay.hpp"

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	inline void DrawSettingsPage(GuiOverlay& overlay, const GuiContext& ctx)
	{
		// =========================================================================
		// 1. 物理引擎选择
		// =========================================================================
		ImGui::TextUnformatted("物理引擎");

		// 与 RuntimeConfig / Core 使用相同的后端编号。
		static const char* kPhysicsOptions[] = {
			"关闭",
			"内建物理",
			"原生物理（默认，实验性）"
		};

		int currentChoice = (std::clamp)(ctx.physicsBackend, 0, 2);

		ImGui::SetNextItemWidth((std::min)(240.0f, ImGui::GetContentRegionAvail().x));
		if (ImGui::Combo("##physics_engine_select", &currentChoice, kPhysicsOptions, IM_ARRAYSIZE(kPhysicsOptions)))
		{
			if (ctx.fnSetPhysicsBackend) ctx.fnSetPhysicsBackend(ctx.self, currentChoice);
		}
		ImGui::TextWrapped("选择在本次运行中生效；重启或控制文件变更后读取 control.json 的设置。");
		if (ctx.physicsBackend == 2)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::WarningColor());
			ImGui::TextWrapped("已选择原生物理实验；原生接口不可用时回退到内建物理。选择后端不代表原生物理已实际运行。");
			ImGui::PopStyleColor();
		}
		else if (ctx.physicsBackend == 1)
			ImGui::TextWrapped("内建弹簧骨为保底方案，不包含官方碰撞模型。");

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// =========================================================================
		// 2. 快捷键一览
		// =========================================================================
		ImGui::TextUnformatted("常用快捷键");

		const ImGuiTableFlags hotkeyTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
		                                         ImGuiTableFlags_SizingStretchProp;
		if (ImGui::BeginTable("##hotkeys_table", 3, hotkeyTableFlags))
		{
			ImGui::TableSetupColumn("快捷键", ImGuiTableColumnFlags_WidthFixed, 110.0f);
			ImGui::TableSetupColumn("功能", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			ImGui::TableSetupColumn("说明", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			auto AddRow = [](const char* key, const char* func, const char* desc)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", key);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextWrapped("%s", func);
				ImGui::TableSetColumnIndex(2);
				ImGui::TextWrapped("%s", desc);
			};

			AddRow("Ctrl + M", "唤出控制面板", "打开或唤回 GakuMotion 控制窗口");
			AddRow("F9", "播放 / 暂停 / 继续", "控制当前曲目；暂停后从当前位置继续");
			AddRow("F10", "停止并恢复游戏状态", "停止表演，复位进度并恢复原生角色控制");
			AddRow(", / .", "上一曲 / 下一曲", "切换播放列表中的曲目");
			AddRow("- / =", "后退 / 前进 5 秒", "相对当前位置跳转");
			AddRow("Shift + F11", "重新加载骨骼微调", "从磁盘重新载入骨骼覆盖设置");
			AddRow("Ctrl + F11", "切换骨骼微调启用", "开启或暂时旁路骨骼覆盖");

			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// =========================================================================
		// 3. 开发者模式开关
		// =========================================================================
		ImGui::TextUnformatted("开发者选项");
		if (ImGui::Checkbox("开发者模式", &overlay.developerMode))
		{
			// 开启后侧边栏将显示 [开发] 选项
		}
		ImGui::TextWrapped("开启后在侧边栏显示 [开发] 页，包含诊断、鞋底测量、探针与实验性开关。");
	}
} // namespace GakuMotion
#endif
