#pragma once

// =============================================================================
// GuiStagePage.hpp — 舞台与场景页面（光影效果 / 场景背景 / 纯色背景 / 高级场景节点）
// =============================================================================

#include "GuiOverlayCommon.hpp"
#include "../GkGuiOverlay.hpp"
#include "../GkStageManager.hpp"

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	inline void DrawStageCandidateNode(int idx, bool ancestorInactive, const std::vector<StageCandidate>& list, const GuiContext& ctx)
	{
		if (idx < 0 || idx >= static_cast<int>(list.size())) return;
		const auto& cand = list[idx];

		const char* badgeText = "[节点]";
		ImVec4 badgeColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
		if (cand.isColliderLocked)
		{
			badgeText = "[碰撞 (锁定)]";
			badgeColor = GuiTheme::WarningColor();
		}
		else if (cand.isLightingRoot || cand.isLightingNode)
		{
			badgeText = "[光影]";
			badgeColor = GuiTheme::InfoColor();
		}
		else if (cand.isIdentifiedBackground)
		{
			badgeText = "[背景]";
			badgeColor = GuiTheme::SuccessColor();
		}

		const std::string& nameToShow = cand.displayName.empty() ? cand.name : cand.displayName;
		const bool disabled = ancestorInactive || cand.isColliderLocked;
		bool displayActive = ancestorInactive ? false : cand.currentlyActive;

		char treeId[32];
		std::snprintf(treeId, sizeof(treeId), "##tree_%d", idx);
		char checkId[32];
		std::snprintf(checkId, sizeof(checkId), "##chk_%d", idx);

		if (cand.hasChildren)
		{
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
			if (cand.depth <= 1) flags |= ImGuiTreeNodeFlags_DefaultOpen;

			bool nodeOpen = ImGui::TreeNodeEx(treeId, flags);

			ImGui::SameLine();
			ImGui::BeginDisabled(disabled);
			if (ImGui::Checkbox(checkId, &displayActive))
			{
				if (ctx.fnToggleStageCandidate)
					ctx.fnToggleStageCandidate(ctx.self, idx, displayActive);
			}
			ImGui::EndDisabled();

			ImGui::SameLine();
			ImGui::TextColored(badgeColor, "%s", badgeText);
			ImGui::SameLine();
			ImGui::TextUnformatted(nameToShow.c_str());

			if (nodeOpen)
			{
				for (int i = 0; i < static_cast<int>(list.size()); ++i)
				{
					if (list[i].parentIndex == idx)
					{
						DrawStageCandidateNode(i, ancestorInactive || !cand.currentlyActive, list, ctx);
					}
				}
				ImGui::TreePop();
			}
		}
		else
		{
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			ImGui::TreeNodeEx(treeId, flags);

			ImGui::SameLine();
			ImGui::BeginDisabled(disabled);
			if (ImGui::Checkbox(checkId, &displayActive))
			{
				if (ctx.fnToggleStageCandidate)
					ctx.fnToggleStageCandidate(ctx.self, idx, displayActive);
			}
			ImGui::EndDisabled();

			ImGui::SameLine();
			ImGui::TextColored(badgeColor, "%s", badgeText);
			ImGui::SameLine();
			ImGui::TextUnformatted(nameToShow.c_str());
		}
	}

	inline void DrawStagePage(GuiOverlay& overlay, const GuiContext& ctx)
	{
		StageSettings settings = ctx.stageSettings;
		bool stageChanged = false;
		if (ImGui::Checkbox("屏蔽游戏音效", &settings.muteNativeSoundEffects)) stageChanged = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("屏蔽支持的预览场景中的游戏音效（含界面音效），不影响动作歌曲；关闭或离开支持场景后恢复。");
		ImGui::Spacing();
		if (!ctx.stageStatusNotice.empty())
		{
			const bool failed = ctx.stageStatusNotice.find("失败") != std::string::npos ||
				ctx.stageStatusNotice.find("错误") != std::string::npos;
			if (failed) ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::ErrorColor());
			ImGui::TextWrapped("舞台状态: %s", ctx.stageStatusNotice.c_str());
			if (failed) ImGui::PopStyleColor();
			ImGui::Spacing();
		}

		if (!settings.loadReplacementKey.empty())
		{
			ImGui::TextWrapped("已保留实验舞台选择；请在 [开发] 页的“摄影舞台替换”中修改或恢复原生背景。");
			ImGui::Spacing();
		}

		// =========================================================================
		// 1. 普通模式：基础视觉与环境控制
		// =========================================================================
		ImGui::TextUnformatted("舞台视觉控制");

		// 自定义节点控制开启时，自动管理被冻结
		const bool customNodeActive = settings.customNodeControl;
		overlay.customNodeControl = customNodeActive;
		if (customNodeActive)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::WarningColor());
			ImGui::TextWrapped("高级自定义节点已启用，以下自动管理选项暂时锁定。");
			ImGui::PopStyleColor();
		}

		ImGui::BeginDisabled(customNodeActive);

		// 合并后的“光影效果”开关
		bool lightingEnabled = settings.lightingEffectsEnabled;
		if (ImGui::Checkbox("光影效果", &lightingEnabled))
		{
			settings.lightingEffectsEnabled = lightingEnabled;
			overlay.environmentShadowEnabled = lightingEnabled;
			stageChanged = true;
			CameraSettings camSettings = ctx.cameraSnapshot.settings;
			camSettings.actorShadowExperimentMode = lightingEnabled ? 1 : 2;
			if (ctx.fnSetCameraSettings) ctx.fnSetCameraSettings(ctx.self, camSettings);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("控制场景 Lighting&PostEffects 光影节点与角色阴影渲染；关闭场景背景时不会关闭此项");

		ImGui::Spacing();

		// 场景背景开关
		// 开启：显示游戏原生场景背景
		// 关闭：隐藏原生场景背景，启用纯色背景与相机清除色
		bool sceneBgEnabled = !settings.hideBackground;
		if (ImGui::Checkbox("场景背景", &sceneBgEnabled))
		{
			settings.hideBackground = !sceneBgEnabled;
			if (!sceneBgEnabled)
			{
				settings.greenScreenActive = true;
				settings.overrideCameraClear = true;
			}
			else
			{
				settings.greenScreenActive = false;
				settings.overrideCameraClear = false;
				if (ctx.fnRestoreCameraSkybox) ctx.fnRestoreCameraSkybox(ctx.self);
			}
			stageChanged = true;
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("关闭时自动隐藏场景网格并应用纯色背景");

		// 背景颜色（当场景背景关闭时可用）
		ImGui::BeginDisabled(sceneBgEnabled);
		if (ImGui::ColorEdit3("##bg_col", settings.greenScreenColor, ImGuiColorEditFlags_NoInputs))
		{
			stageChanged = true;
		}
		ImGui::SameLine();
		ImGui::TextUnformatted("背景颜色");

		// 预置颜色快速选择（白/灰/黑/绿/蓝）
		ImGui::TextDisabled("预置:");
		ImGui::SameLine();

		auto AddPreset = [&](const char* id, const char* label, float r, float g, float b)
		{
			if (ImGui::ColorButton(id, ImVec4(r, g, b, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(22, 20)))
			{
				settings.greenScreenColor[0] = r;
				settings.greenScreenColor[1] = g;
				settings.greenScreenColor[2] = b;
				settings.greenScreenColor[3] = 1.0f;
				stageChanged = true;
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%.1f, %.1f, %.1f)", label, r, g, b);
			ImGui::SameLine();
		};

		AddPreset("##preset_white", "纯白", 1.0f, 1.0f, 1.0f);
		AddPreset("##preset_gray",  "中灰", 0.5f, 0.5f, 0.5f);
		AddPreset("##preset_black", "纯黑", 0.0f, 0.0f, 0.0f);
		AddPreset("##preset_green", "绿幕", 0.0f, 1.0f, 0.0f);
		AddPreset("##preset_blue",  "蓝幕", 0.0f, 0.35f, 0.85f);
		ImGui::NewLine();

		ImGui::EndDisabled();

		ImGui::EndDisabled(); // !customNodeActive

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// =========================================================================
		// 2. 高级场景编辑 (折叠)
		// =========================================================================
		if (ImGui::CollapsingHeader("高级场景编辑"))
		{
			ImGui::Indent(10.0f);

			bool customControl = settings.customNodeControl;
			if (ImGui::Checkbox("自定义场景节点 [开关]", &customControl))
			{
				settings.customNodeControl = customControl;
				overlay.customNodeControl = customControl;
				stageChanged = true;
				if (customControl && ctx.stageCandidates.empty() && ctx.fnScanStageCandidates)
					ctx.fnScanStageCandidates(ctx.self);
			}

			if (settings.customNodeControl)
			{
				ImGui::TextColored(GuiTheme::WarningColor(),
				                   "启用后将停止 GakuMotion 对场景节点的自动管理。\n"
				                   "仅建议在需要手工定制场景时使用。");
				ImGui::Spacing();

				if (ImGui::Button("扫描场景节点##scan", ImVec2(120, 24)))
				{
					if (ctx.fnScanStageCandidates) ctx.fnScanStageCandidates(ctx.self);
				}

				if (!ctx.stageCandidates.empty())
				{
					ImGui::Spacing();
					ImGui::TextDisabled("已探测场景节点 (%zu 个):", ctx.stageCandidates.size());

					const float treeHeight = (std::max)(1.0f, std::floor(ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y));
					ImGui::BeginChild("##custom_node_tree", ImVec2(0, treeHeight), true);
					for (size_t i = 0; i < ctx.stageCandidates.size(); ++i)
					{
						if (ctx.stageCandidates[i].parentIndex < 0 ||
							ctx.stageCandidates[i].parentIndex >= static_cast<int>(ctx.stageCandidates.size()))
						{
							DrawStageCandidateNode(static_cast<int>(i), false, ctx.stageCandidates, ctx);
						}
					}
					ImGui::EndChild();
				}
				else
				{
					ImGui::TextDisabled("暂无场景节点信息（进入 3D 舞台后点击“扫描场景节点”获取）");
				}
			}
			else
			{
				ImGui::TextDisabled("开启自定义场景节点后，可手工逐项勾选/隐藏场景里的特定 Mesh 或物件。");
			}

			ImGui::Unindent(10.0f);
		}

		if (stageChanged && ctx.fnSetStageSettings)
		{
			ctx.fnSetStageSettings(ctx.self, settings);
		}
	}
} // namespace GakuMotion
#endif
