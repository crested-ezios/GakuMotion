#pragma once

#include "GuiLibraryHelpers.hpp"

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	// 导入进度条与 Spinning 弹窗（与 Core importProgress 实时绑定）
	inline void DrawImportProgressModal(GuiOverlay& overlay, const GuiContext& ctx)
	{
		if (!overlay.isImporting) return;

		ImGui::OpenPopup("导入曲目中##import_progress_modal");
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

		if (ctx.importProgress != nullptr)
		{
			const auto* prog = ctx.importProgress;

			if (prog->finished)
			{
				if (prog->succeeded)
				{
					overlay.isImporting = false;
					ImGui::CloseCurrentPopup();
					return;
				}
				else
				{
					ImGui::SetNextWindowSize(ImVec2(480, 260), ImGuiCond_Appearing);
					if (ImGui::BeginPopupModal("导入曲目中##import_progress_modal", nullptr,
					                           ImGuiWindowFlags_NoResize))
					{
						ImGui::Spacing();
						ImGui::TextColored(GuiTheme::WarningColor(), "导入遇到错误：");
						ImGui::Spacing();

						std::string detail = !prog->errorDetail.empty()
							? prog->errorDetail
							: (!ctx.playlistNotice.empty() ? ctx.playlistNotice : "原生导入遇到未知错误，请查看导入日志");

						ImGui::BeginChild("##import_error_box", ImVec2(-1, 140), true);
						ImGui::TextWrapped("%s", detail.c_str());
						ImGui::EndChild();

						ImGui::Spacing();
						if (ImGui::Button("关闭", ImVec2(90, 26)))
						{
							overlay.isImporting = false;
							ImGui::CloseCurrentPopup();
						}
						ImGui::EndPopup();
					}
					return;
				}
			}

			// 进行中
			ImGui::SetNextWindowSize(ImVec2(400, 150), ImGuiCond_Appearing);
			if (ImGui::BeginPopupModal("导入曲目中##import_progress_modal", nullptr,
			                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
			{
				ImGui::Spacing();
				ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 14.0f);
				DrawSpinner("##import_spinner", 14.0f, 3.0f, GuiTheme::InfoColor());

				ImGui::Spacing();
				std::string st = !prog->statusText.empty()
					? prog->statusText
					: (!prog->stage.empty() ? prog->stage : "正在处理演出数据...");
				ImGui::TextColored(GuiTheme::HighlightColor(), "%s", st.c_str());

				ImGui::Spacing();
				char pctBuf[32];
				std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", prog->progress * 100.0f);
				ImGui::ProgressBar(prog->progress, ImVec2(-1, 18.0f), pctBuf);

				ImGui::EndPopup();
			}
		}
		else
		{
			if (!ctx.importBusy)
			{
				overlay.isImporting = false;
				ImGui::CloseCurrentPopup();
				return;
			}

			ImGui::SetNextWindowSize(ImVec2(380, 140), ImGuiCond_Appearing);
			if (ImGui::BeginPopupModal("导入曲目中##import_progress_modal", nullptr,
			                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
			{
				ImGui::Spacing();
				ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 14.0f);
				DrawSpinner("##import_spinner", 14.0f, 3.0f, GuiTheme::InfoColor());

				ImGui::Spacing();
				ImGui::TextColored(GuiTheme::HighlightColor(), "%s",
					overlay.importStatusText.empty() ? "正在导入演出曲目..." : overlay.importStatusText.c_str());

				ImGui::Spacing();
				ImGui::ProgressBar(0.5f, ImVec2(-1, 18.0f), "处理中...");

				ImGui::EndPopup();
			}
		}
	}
} // namespace GakuMotion
#endif
