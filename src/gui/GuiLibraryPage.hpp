#pragma once

// =============================================================================
// GuiLibraryPage.hpp — 曲库页面、添加/管理模态弹窗与导入进度
// =============================================================================

#include "GuiOverlayCommon.hpp"
#include "GkGuiIcons.hpp"
namespace GakuMotion
{
	class GuiOverlay;
	struct GuiContext;
	void DrawLibraryPage(GuiOverlay& overlay, const GuiContext& ctx);
}

#include "../GkGuiOverlay.hpp"
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <shellapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#endif

#include "library/GuiLibraryHelpers.hpp"
#include "library/GuiLibraryDrop.hpp"
#include "library/GuiLibraryAddEditModal.hpp"
#include "library/GuiLibraryProgressModal.hpp"

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	inline void DrawLibraryPage(GuiOverlay& overlay, const GuiContext& ctx)
	{
		SyncLibraryEntriesFromMotions(overlay, ctx);

		// 命中区是整块客户区，与表格布局无关，因此帧首即可登记：悬停状态、提示
		// 文案、边框和稍后的实际投放都来自这一份目标定义，不会互相错位。
		ImVec2 dropMinimum{}, dropMaximum{};
		ClientDropRect(dropMinimum, dropMaximum);
		RegisterDropTarget(ctx, dropMinimum, dropMaximum);
		const DropVisualState dropVisual = QueryDropVisual();

		// 处理后台异步文件选择器回调结果
		std::vector<std::string> pickedFiles;
		int pickedSlot = -1;
		if (g_asyncFileDialog.Poll(pickedFiles, pickedSlot))
		{
			if (!pickedFiles.empty())
			{
				if (pickedSlot == -1)
				{
					if (g_libraryModal.isOpen)
						g_libraryModal.AppendFiles(pickedFiles);
					else
						g_libraryModal.SetupForNew(pickedFiles);
				}
				else if (pickedSlot == 0)
				{
					g_libraryModal.SelectSingleMotion(pickedFiles.front());
				}
				else if (pickedSlot == 1)
				{
					g_libraryModal.SelectSingleFacial(pickedFiles.front());
				}
				else if (pickedSlot == 2)
				{
					g_libraryModal.SelectSingleCamera(pickedFiles.front());
				}
				else if (pickedSlot == 3)
				{
					g_libraryModal.SelectSingleAudio(pickedFiles.front());
				}
			}
		}

		// ---- 顶部操作栏：空间不足时换行，保持刷新与管理入口可见 ----
		auto ContinueToolbar = [](const char* nextLabel, float minimumWidth = 0.0f) {
			GuiIcons::SameLineIfRoom(nextLabel, ImVec2(minimumWidth, 26));
		};
		ImGui::BeginDisabled(ctx.libraryRefreshing || ctx.importBusy || !ctx.fnRefreshMotions);
		if (GuiIcons::Button(ctx.libraryRefreshing ? "正在刷新..." : "刷新曲库", GuiIcons::Icon::Refresh, ImVec2(100, 26)))
		{
			if (ctx.fnRefreshMotions && ctx.fnRefreshMotions(ctx.self)) overlay.libraryUiNotice.clear();
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("重新扫描外部新增、删除和修改的曲目；不重新加载当前表演");
		ContinueToolbar("打开曲库文件夹", 140);
		if (GuiIcons::Button("打开曲库文件夹", GuiIcons::Icon::Folder, ImVec2(140, 26)))
		{
			const auto folder = Utf8ToWide(overlay.motionsRoot);
			const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
			overlay.libraryUiNotice = result <= 32 ? "无法打开曲库文件夹，请检查目录是否存在（错误 " + std::to_string(result) + "）" : "";
		}
		if (ctx.fnReloadMotion)
		{
			ContinueToolbar("重新加载当前曲目", 160);
			ImGui::BeginDisabled(ctx.loadedMotionKey.empty() || ctx.loadedSourceMissing || ctx.importBusy || ctx.libraryRefreshing);
			if (GuiIcons::Button("重新加载当前曲目", GuiIcons::Icon::Refresh, ImVec2(160, 26))) ctx.fnReloadMotion(ctx.self);
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("载入当前曲目的磁盘更新；保留时间并暂停，点击继续后播放");
		}
		if (!ctx.libraryNotice.empty()) ImGui::TextWrapped("%s", ctx.libraryNotice.c_str());
		if (!ctx.playlistNotice.empty() && ctx.playlistNotice != ctx.libraryNotice)
		{
			const bool failed = ctx.playlistNotice.find("失败") != std::string::npos || ctx.playlistNotice.find("错误") != std::string::npos;
			ImGui::PushStyleColor(ImGuiCol_Text, failed ? GuiTheme::ErrorColor() : GuiTheme::InfoColor());
			ImGui::TextWrapped("%s", ctx.playlistNotice.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::WarningColor());
		if (!overlay.libraryUiNotice.empty()) ImGui::TextWrapped("%s", overlay.libraryUiNotice.c_str());
		if (ctx.loadedSourceMissing) ImGui::TextWrapped("当前表演的源文件已移除；已加载内容保留，无法重新加载。");
		else if (ctx.loadedSourceChanged) ImGui::TextWrapped("当前曲目的文件已更新，待重新加载。");
		ImGui::PopStyleColor();
		ImGui::Spacing();

		ImGui::BeginDisabled(g_asyncFileDialog.IsBusy() || ctx.importBusy);
		if (GuiIcons::Button("添加到曲库", GuiIcons::Icon::Add, ImVec2(100, 26)))
		{
			g_asyncFileDialog.Open(true, kAllImportFilter, -1);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("选择 .vmd 动作/表情/镜头或音频文件添加到曲库");

		ContinueToolbar("管理选中", 85);

		const bool hasSelection = (overlay.selectedLibraryIndex >= 0 &&
		                           overlay.selectedLibraryIndex < static_cast<int>(overlay.libraryEntries.size()));

		ImGui::BeginDisabled(!hasSelection || ctx.importBusy || ctx.libraryRefreshing);
		if (GuiIcons::Button("管理选中", GuiIcons::Icon::Edit, ImVec2(85, 26)))
		{
			if (hasSelection)
			{
				g_libraryModal.SetupForEdit(overlay.libraryEntries[overlay.selectedLibraryIndex]);
			}
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("修改选中的曲目配置与文件映射");

		ContinueToolbar("删除选中", 120);
		ImGui::BeginDisabled(!ctx.fnPlaylistAction);
		if (GuiIcons::Button("删除选中", GuiIcons::Icon::Trash, ImVec2(120, 26)) && hasSelection)
			overlay.RequestMotionDelete(ctx, overlay.libraryEntries[overlay.selectedLibraryIndex].id.c_str());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("确认后永久删除选中曲目的目录及其中全部文件，无法撤销");
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		overlay.DrawMotionDeleteConfirmation(ctx);

		// 搜索独占一行，避免窄窗口把操作按钮挤出内容区域。
		overlay.libraryFilter.Draw("搜索曲目", (std::max)(80.0f, ImGui::GetContentRegionAvail().x - 75.0f));
		if (ImGui::IsItemHovered() && overlay.libraryFilter.InputBuf[0] == '\0')
			ImGui::SetTooltip("输入关键字过滤曲库列表");
		if (overlay.libraryFilter.IsActive())
		{
			ContinueToolbar("清空", 55);
			if (ImGui::Button("清空")) overlay.libraryFilter.Clear();
		}
		if (hasSelection)
		{
			const auto& selected = overlay.libraryEntries[overlay.selectedLibraryIndex];
			ImGui::TextWrapped("选中：%s", selected.title.c_str());
			ImGui::BeginDisabled(ctx.importBusy || ctx.libraryRefreshing || !ctx.fnSelectMotion);
			if (GuiIcons::Button("加载预览", GuiIcons::Icon::Preview)) ctx.fnSelectMotion(ctx.self, overlay.selectedLibraryIndex, false);
			ContinueToolbar("播放选中", 85);
			if (GuiIcons::Button("播放选中", GuiIcons::Icon::Play)) ctx.fnSelectMotion(ctx.self, overlay.selectedLibraryIndex, true);
			ImGui::EndDisabled();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// ---- 曲库表格 ----
		// 严格区分三个状态：
		//   Selected: 曲库当前选中条目 (overlay.selectedLibraryIndex)
		//   Loaded:   当前实际加载到 runtime 的稳定曲目 key
		//   Playing:  当前真正正在播放的条目 (ctx.player->state == PlaybackState::Playing)
		const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
		                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
		std::vector<int> visibleIndices;
		for (size_t i = 0; i < overlay.libraryEntries.size(); ++i)
		{
			const auto& entry = overlay.libraryEntries[i];
			if (overlay.libraryFilter.PassFilter(entry.title.c_str()) || overlay.libraryFilter.PassFilter(entry.id.c_str()))
				visibleIndices.push_back(static_cast<int>(i));
		}
		ImGui::TextDisabled("显示 %zu / %zu 个曲目", visibleIndices.size(), overlay.libraryEntries.size());
		if (host::fileDrop.setTarget)
		{
			switch (dropVisual.visual)
			{
			case DropVisual::Hover:
				ImGui::TextColored(GuiTheme::InfoColor(), "松开即可把 %zu 个文件添加到曲库", dropVisual.accepted);
				break;
			case DropVisual::Invalid:
				ImGui::TextColored(GuiTheme::WarningColor(),
					"拖入的内容无法导入，仅接受 VMD / GKCamera / WAV 等单个文件");
				break;
			case DropVisual::Armed:
				ImGui::TextColored(GuiTheme::InfoColor(), "将文件拖入本窗口任意位置…");
				break;
			default:
				// 宿主提权时系统会丢掉所有跨完整性级别的拖拽回调，禁止光标是必然现象，
				// 但松开鼠标后仍会正常导入。与其让用户以为拖放坏了，不如直接说明。
				if (dropVisual.hostElevated)
					ImGui::TextColored(GuiTheme::WarningColor(),
						"本窗口以管理员权限运行：拖拽时可能出现禁止光标且没有悬停高亮，这属正常现象；把文件拖到本窗口任意位置松开仍可正常导入");
				else if (dropVisual.hostBlocked)
					ImGui::TextColored(GuiTheme::WarningColor(),
						"当前宿主不提供拖拽悬停预览；把文件拖到本窗口任意位置松开，仍可正常导入");
				else
					ImGui::TextDisabled("将 VMD / GKCamera / WAV 文件拖入本窗口任意位置，即可添加到曲库");
				break;
			}
		}
		if (overlay.libraryEntries.empty()) ImGui::TextWrapped("曲库为空。添加动作，或将动作文件夹放入曲库后点击“刷新曲库”。");
		else if (visibleIndices.empty()) ImGui::TextWrapped("没有匹配的曲目，可清空搜索后查看全部曲目。");

		const float tableHeight = ImGui::GetContentRegionAvail().y - 8.0f;
		ImVec2 tableMinimum{}, tableMaximum{};
		if (ImGui::BeginTable("##gkmotion_library_table", 4, tableFlags, ImVec2(0.0f, (std::max)(100.0f, tableHeight))))
		{
			ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 42.0f);
			ImGui::TableSetupColumn("演出曲目", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("包含内容", ImGuiTableColumnFlags_WidthFixed, 140.0f);
			ImGui::TableSetupColumn("时长", ImGuiTableColumnFlags_WidthFixed, 55.0f);
			ImGui::TableHeadersRow();

			// 使用 ImGuiListClipper 优化大列表虚拟渲染
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(visibleIndices.size()));

			while (clipper.Step())
			{
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
				{
					const int idx = visibleIndices[row];
					const auto& entry = overlay.libraryEntries[idx];

					const bool isSelected = (overlay.selectedLibraryIndex == idx);

					bool isLoaded = false;
					bool isPlaying = false;
					if (ctx.player)
					{
						isLoaded = !ctx.loadedMotionKey.empty() ? ctx.loadedMotionKey == entry.id
							: MotionPathEqualsUtf8(ctx.player->loadedMotionFile, entry.motionFile);
						isPlaying = isLoaded && (ctx.player->state == PlaybackState::Playing);
					}

					ImGui::TableNextRow();

					// 列 0：播放状态
					ImGui::TableSetColumnIndex(0);
					if (isPlaying)
						ImGui::TextColored(GuiTheme::SuccessColor(), "[播]");
					else if (isLoaded && ctx.player && ctx.player->state == PlaybackState::Paused)
						ImGui::TextColored(GuiTheme::WarningColor(), "[暂]");
					else if (isLoaded && ctx.player && ctx.player->state == PlaybackState::Preview)
						ImGui::TextColored(GuiTheme::InfoColor(), "[预]");
					else if (isLoaded)
						ImGui::TextDisabled("[载]");
					else
						ImGui::TextUnformatted("");

					// 列 1：演出曲目（支持单击选中，双击播放）
					ImGui::TableSetColumnIndex(1);
					ImGui::PushID(entry.id.c_str());

					const ImGuiSelectableFlags selectFlags = ImGuiSelectableFlags_SpanAllColumns |
					                                         ImGuiSelectableFlags_AllowDoubleClick;

					if (ImGui::Selectable("##row", isSelected, selectFlags))
					{
						overlay.selectedLibraryIndex = idx;
						overlay.selectedLibraryKey = entry.id;

						if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						{
							if (ctx.fnSelectMotion && !ctx.importBusy && !ctx.libraryRefreshing)
							{
								ctx.fnSelectMotion(ctx.self, idx, true /* autoplay */);
							}
						}
					}
					ImGui::PopID();

					ImGui::SameLine(0.0f, 0.0f);
					if (isPlaying)
						ImGui::TextColored(GuiTheme::SuccessColor(), "%s", entry.title.c_str());
					else
						ImGui::TextUnformatted(entry.title.c_str());

					// 列 2：包含标签
					ImGui::TableSetColumnIndex(2);
					if (entry.HasMotion())
					{
						ImGui::TextColored(GuiTheme::TagMotionColor(), "[动作]");
						ImGui::SameLine();
					}
					if (entry.HasFacial())
					{
						ImGui::TextColored(GuiTheme::TagFacialColor(), "[表情]");
						ImGui::SameLine();
					}
					if (entry.HasCamera())
					{
						ImGui::TextColored(GuiTheme::TagCameraColor(), "[镜头]");
						ImGui::SameLine();
					}
					if (entry.HasAudio())
					{
						ImGui::TextColored(GuiTheme::TagAudioColor(), "[音乐]");
						ImGui::SameLine();
					}

					// 列 3：时长
					ImGui::TableSetColumnIndex(3);
					char durBuf[16];
					FormatTimeMmSs(entry.durationSeconds, durBuf, sizeof(durBuf));
					ImGui::TextDisabled("%s", durBuf);
				}
			}
			clipper.End();
			ImGui::EndTable();
			tableMinimum = ImGui::GetItemRectMin();
			tableMaximum = ImGui::GetItemRectMax();
			const ImVec2 clipMin = ImGui::GetWindowDrawList()->GetClipRectMin();
			const ImVec2 clipMax = ImGui::GetWindowDrawList()->GetClipRectMax();
			tableMinimum.x = (std::max)(tableMinimum.x, clipMin.x);
			tableMinimum.y = (std::max)(tableMinimum.y, clipMin.y);
			tableMaximum.x = (std::min)(tableMaximum.x, clipMax.x);
			tableMaximum.y = (std::min)(tableMaximum.y, clipMax.y);
		}
		// 投放按整块客户区的目标判定；表格矩形只用来画提示边框。
		ReceiveLibraryDrop(overlay, ctx, dropMinimum, dropMaximum);
		DrawLibraryDropVisual(tableMinimum, tableMaximum, dropVisual);

		// 模态弹窗与导入动画渲染
		DrawAddEditModal(overlay, ctx);
		DrawImportProgressModal(overlay, ctx);
	}
} // namespace GakuMotion
#endif
