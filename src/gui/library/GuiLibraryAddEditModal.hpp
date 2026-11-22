#pragma once

#include "GuiLibraryHelpers.hpp"

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	// 添加 / 管理条目模态弹窗
	inline void DrawAddEditModal(GuiOverlay& overlay, const GuiContext& ctx)
	{
		if (!g_libraryModal.isOpen) return;

		ImGui::OpenPopup(g_libraryModal.isEditMode ? "管理曲目##modal" : "添加到曲库##modal");

		const ImVec2 center = ImGui::GetMainViewport()->GetWorkCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		const auto& style = ImGui::GetStyle();
		const float typeWidth = (std::max)(100.0f, ImGui::CalcTextSize("动作 (.vmd)").x + 2.0f * style.CellPadding.x);
		const float speedWidth = (std::max)(115.0f, ImGui::CalcTextSize("播放速度").x + 2.0f * style.CellPadding.x);
		const float offsetWidth = (std::max)(155.0f, ImGui::CalcTextSize("初始偏移量（秒）").x + 2.0f * style.CellPadding.x);
		const float tableWidth = (std::max)(850.0f, typeWidth + speedWidth + offsetWidth + 310.0f);
		const float modalWidth = (std::min)((std::max)(980.0f, tableWidth + 2.0f * style.WindowPadding.x),
			(std::max)(1.0f, ImGui::GetMainViewport()->WorkSize.x - 32.0f));
		const float frameHeight = ImGui::GetFrameHeight();
		const float buttonHeight = (std::max)(26.0f, frameHeight);
		const bool tableNeedsScroll = tableWidth > modalWidth - 2.0f * style.WindowPadding.x;
		const float tableHeight = ImGui::GetTextLineHeight() + 2.0f * style.CellPadding.y +
			4.0f * (frameHeight + 2.0f * style.CellPadding.y) + (tableNeedsScroll ? style.ScrollbarSize : 0.0f);
		static float measuredBodyHeight = 0.0f, measuredBodyWidth = 0.0f, measuredFontSize = 0.0f;
		const bool measuredLayoutMatches = std::fabs(measuredBodyWidth - modalWidth) < 0.5f &&
			std::fabs(measuredFontSize - ImGui::GetFontSize()) < 0.01f;
		const float estimatedBodyHeight = tableHeight + (std::max)(24.0f, frameHeight) + frameHeight +
			4.0f * ImGui::GetTextLineHeight() + 10.0f * style.ItemSpacing.y;
		const float bodyTargetHeight = measuredLayoutMatches && measuredBodyHeight > 0.0f
			? measuredBodyHeight : estimatedBodyHeight;
		const float footerHeight = buttonHeight + 1.0f + 2.0f * style.ItemSpacing.y;
		const float modalHeight = (std::min)(bodyTargetHeight + footerHeight + frameHeight + 2.0f * style.WindowPadding.y,
			(std::max)(1.0f, ImGui::GetMainViewport()->WorkSize.y - 32.0f));
		ImGui::SetNextWindowSize(ImVec2(modalWidth, modalHeight));

		if (ImGui::BeginPopupModal(g_libraryModal.isEditMode ? "管理曲目##modal" : "添加到曲库##modal",
		                           &g_libraryModal.isOpen, ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			auto FormatCandidateLabel = [&](const std::string& path) -> std::string
			{
				if (IsSlotPlaceholder(path))
				{
					return path;
				}
				std::filesystem::path p(Utf8ToWide(path));
				std::string filename = WideToUtf8(p.filename().wstring());
				const VmdProbeInfo& info = g_libraryModal.GetOrProbe(path);
				if (info.valid)
				{
					return filename + "  " + info.BadgeText();
				}
				return filename;
			};

			auto DrawCandidateRow = [&](const char* label, const char* comboId, int& selectedIdx,
			                            const std::vector<std::string>& candidates,
			                            int browseSlot, const wchar_t* filter, const char* browseBtnId,
			                            TrackTiming& timing)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(label);
				ImGui::TableSetColumnIndex(1);

				std::string preview = (selectedIdx >= 0 && selectedIdx < static_cast<int>(candidates.size()))
					? FormatCandidateLabel(candidates[selectedIdx])
					: ImportSlot::kEmpty;

				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::BeginCombo(comboId, preview.c_str()))
				{
					for (size_t i = 0; i < candidates.size(); ++i)
					{
						const bool isSelected = (selectedIdx == static_cast<int>(i));
						std::string itemText = FormatCandidateLabel(candidates[i]);
						if (ImGui::Selectable(itemText.c_str(), isSelected))
						{
							selectedIdx = static_cast<int>(i);
						}
						if (isSelected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}

				ImGui::TableSetColumnIndex(2);
				ImGui::BeginDisabled(g_asyncFileDialog.IsBusy());
				if (ImGui::Button(browseBtnId, ImVec2(55, 0)))
				{
					g_asyncFileDialog.Open(false, filter, browseSlot);
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("浏览并指定文件");

				ImGui::PushID(comboId);
				ImGui::TableSetColumnIndex(3);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::InputFloat("##track_speed", &timing.speed, 0.1f, 1.0f, "%.2f"))
					timing.speed = std::isfinite(timing.speed) ? (std::max)(0.0f, timing.speed) : 1.0f;
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("最低为 0；0 停止该轨道的时间推进。与播放栏倍速相乘。");
				ImGui::TableSetColumnIndex(4);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::InputFloat("##track_offset", &timing.offsetSeconds, 0.1f, 1.0f, "%.3f"))
					if (!std::isfinite(timing.offsetSeconds)) timing.offsetSeconds = 0.0f;
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("正数后移，负数前移；单位为秒。");
				ImGui::PopID();
			};

			// Keep the action row visible when the viewport is shorter than the form.
			const float bodyHeight = (std::max)(1.0f, ImGui::GetContentRegionAvail().y - footerHeight);
			ImGui::BeginChild("##library_modal_body", ImVec2(0, bodyHeight));
			ImGui::Spacing();

			// 顶部文件追加工具栏
			ImGui::BeginDisabled(g_asyncFileDialog.IsBusy());
			if (ImGui::Button("+ 追加文件...", ImVec2((std::max)(105.0f,
				ImGui::CalcTextSize("+ 追加文件...").x + 2.0f * style.FramePadding.x), (std::max)(24.0f, frameHeight))))
			{
				g_asyncFileDialog.Open(true, kAllImportFilter, -1);
			}
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("从其他文件夹追加动作、表情、镜头或音频文件");

			if (g_asyncFileDialog.IsBusy())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("（正在等待文件选择…）");
			}

			ImGui::Spacing();
			if (ImGui::BeginTable("##library_file_timing", 5, ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersInnerV,
				ImVec2(0, tableHeight),
				(std::max)(tableWidth, ImGui::GetContentRegionAvail().x)))
			{
				ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, typeWidth);
				ImGui::TableSetupColumn("文件", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60.0f);
				ImGui::TableSetupColumn("播放速度", ImGuiTableColumnFlags_WidthFixed, speedWidth);
				ImGui::TableSetupColumn("初始偏移量（秒）", ImGuiTableColumnFlags_WidthFixed, offsetWidth);
				ImGui::TableHeadersRow();
				DrawCandidateRow("动作 (.vmd)", "##sel_motion", g_libraryModal.motionSelection,
					g_libraryModal.motionCandidates, 0, kMotionFilter, "浏览##b_mot", g_libraryModal.timing.motion);
				DrawCandidateRow("表情 (.vmd)", "##sel_facial", g_libraryModal.facialSelection,
					g_libraryModal.facialCandidates, 1, kFacialFilter, "浏览##b_fac", g_libraryModal.timing.facial);
				DrawCandidateRow("镜头", "##sel_camera", g_libraryModal.cameraSelection,
					g_libraryModal.cameraCandidates, 2, kCameraFilter, "浏览##b_cam", g_libraryModal.timing.camera);
				DrawCandidateRow("音乐 (音频)", "##sel_audio", g_libraryModal.audioSelection,
					g_libraryModal.audioCandidates, 3, kAudioFilter, "浏览##b_aud", g_libraryModal.timing.audio);
				ImGui::EndTable();
			}
			ImGui::TextDisabled("速度与播放栏倍速相乘；偏移正数后移，负数前移。");
			ImGui::SetNextItemWidth(160.0f);
			ImGui::Combo("源动作帧率", &g_libraryModal.sourceFpsIndex, "自动检测\0 30 fps\0 60 fps\0");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("素材的时间基准；动作过快或过慢时可指定。修改已有曲目的源帧率会重新烘焙动作。");
			ImGui::SetNextItemWidth(160.0f);
			ImGui::Combo("输出采样帧率", &g_libraryModal.outputFpsIndex, "自动（保持原有）\0 30 fps\0 60 fps\0");
			ImGui::TextDisabled("自动检测依据身体骨骼与音频，素材长度不一致时请检查结果。");

			if (g_libraryModal.unsupportedAudio > 0)
			{
				ImGui::TextColored(GuiTheme::WarningColor(),
					"已忽略 %d 个非 WAV 音频（当前导入链仅支持 PCM WAV）",
					g_libraryModal.unsupportedAudio);
			}

			ImGui::Spacing();
			ImGui::TextUnformatted("演出曲目名称");
			ImGui::SameLine((std::max)(115.0f, ImGui::CalcTextSize("演出曲目名称").x + style.ItemSpacing.x));
			ImGui::SetNextItemWidth((std::min)(330.0f, (std::max)(1.0f, ImGui::GetContentRegionAvail().x)));
			ImGui::InputText("##title_input", g_libraryModal.titleInput, sizeof(g_libraryModal.titleInput));

			// 智能校验与防呆逻辑
			std::string validationError;
			bool canConfirm = true;
			if (ctx.importBusy)
			{
				canConfirm = false;
				validationError = "正在导入，请等待完成后保存";
			}
			else if (g_libraryModal.isEditMode && (!ctx.motions || FindMotionKeyIndex(*ctx.motions, g_libraryModal.editTargetKey) < 0))
			{
				canConfirm = false;
				validationError = "原曲目已不在曲库中；表单已保留，请刷新后重试";
			}

			auto IsDiskFile = [](const std::string& p) -> bool
			{
				return !IsSlotPlaceholder(p);
			};

			std::string pickedMotion = (g_libraryModal.motionSelection >= 0 &&
				g_libraryModal.motionSelection < static_cast<int>(g_libraryModal.motionCandidates.size()))
				? g_libraryModal.motionCandidates[g_libraryModal.motionSelection] : "";

			std::string pickedFacial = (g_libraryModal.facialSelection >= 0 &&
				g_libraryModal.facialSelection < static_cast<int>(g_libraryModal.facialCandidates.size()))
				? g_libraryModal.facialCandidates[g_libraryModal.facialSelection] : "";

			std::string pickedCamera = (g_libraryModal.cameraSelection >= 0 &&
				g_libraryModal.cameraSelection < static_cast<int>(g_libraryModal.cameraCandidates.size()))
				? g_libraryModal.cameraCandidates[g_libraryModal.cameraSelection] : "";

			std::string pickedAudio = (g_libraryModal.audioSelection >= 0 &&
				g_libraryModal.audioSelection < static_cast<int>(g_libraryModal.audioCandidates.size()))
				? g_libraryModal.audioCandidates[g_libraryModal.audioSelection] : "";

			if (!g_libraryModal.isEditMode)
			{
				if (!IsDiskFile(pickedMotion))
				{
					canConfirm = false;
					validationError = "请选择有效的身体动作 (.vmd) 文件";
				}
				else
				{
					const VmdProbeInfo& info = g_libraryModal.GetOrProbe(pickedMotion);
					if (!info.CanUseAsMotion())
					{
						canConfirm = false;
						validationError = "所选动作文件不包含身体骨骼动作关键帧（不能用纯镜头或纯表情文件作为动作）";
					}
				}
			}
			else
			{
				if (IsDiskFile(pickedMotion))
				{
					const VmdProbeInfo& info = g_libraryModal.GetOrProbe(pickedMotion);
					if (!info.CanUseAsMotion())
					{
						canConfirm = false;
						validationError = "所选动作文件不包含身体骨骼动作关键帧";
					}
				}
			}

			if (canConfirm && IsDiskFile(pickedFacial))
			{
				const VmdProbeInfo& info = g_libraryModal.GetOrProbe(pickedFacial);
				if (!info.CanUseAsFacial())
				{
					canConfirm = false;
					validationError = "所选表情文件不包含表情或眼神关键帧";
				}
			}

			if (canConfirm && IsDiskFile(pickedCamera))
			{
				const VmdProbeInfo& info = g_libraryModal.GetOrProbe(pickedCamera);
				if (!info.CanUseAsCamera())
				{
					canConfirm = false;
					validationError = "所选镜头文件不包含镜头关键帧";
				}
			}

			if (canConfirm && IsDiskFile(pickedAudio))
			{
				std::string ext = WideToUtf8(std::filesystem::path(Utf8ToWide(pickedAudio)).extension().wstring());
				for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				if (ext != ".wav" && ext != ".mp3" && ext != ".flac" && ext != ".ogg" &&
				    ext != ".oga" && ext != ".m4a" && ext != ".aac" && ext != ".opus")
				{
					canConfirm = false;
					validationError = "音频轨道仅支持 .wav/.mp3/.flac/.ogg/.m4a/.aac/.opus 本地文件";
				}
			}

			if (canConfirm)
			{
				bool titleEmpty = true;
				for (size_t i = 0; i < sizeof(g_libraryModal.titleInput) && g_libraryModal.titleInput[i]; ++i)
				{
					if (!std::isspace(static_cast<unsigned char>(g_libraryModal.titleInput[i])))
					{
						titleEmpty = false;
						break;
					}
				}
				if (titleEmpty)
				{
					canConfirm = false;
					validationError = "请输入有效的演出曲目名称";
				}
			}

			ImGui::Spacing();
			if (!validationError.empty())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::WarningColor());
				ImGui::TextWrapped("%s", validationError.c_str());
				ImGui::PopStyleColor();
			}
			else
			{
				ImGui::TextUnformatted("");
			}

			measuredBodyHeight = std::ceil(ImGui::GetCursorPosY());
			measuredBodyWidth = modalWidth;
			measuredFontSize = ImGui::GetFontSize();
			ImGui::EndChild();
			ImGui::Separator();

			// 底部按钮 [取消] [添加 / 保存修改]
			const float actionWidth = (std::max)(90.0f, ImGui::CalcTextSize("保存修改").x + 2.0f * style.FramePadding.x);
			if (ImGui::Button("取消", ImVec2(actionWidth, buttonHeight)))
			{
				g_libraryModal.isOpen = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine((std::max)(actionWidth + style.ItemSpacing.x,
				ImGui::GetWindowWidth() - style.WindowPadding.x - actionWidth));

			const char* confirmBtnText = g_libraryModal.isEditMode ? "保存修改" : "添加";
			ImGui::BeginDisabled(!canConfirm);
			if (ImGui::Button(confirmBtnText, ImVec2(actionWidth, buttonHeight)))
			{
				MotionImportRequest req{};
				req.title = g_libraryModal.titleInput[0] ? g_libraryModal.titleInput : "未命名演出";
				req.fps = g_libraryModal.outputFpsIndex == 1 ? 30.f : g_libraryModal.outputFpsIndex == 2 ? 60.f : 0.f;
				req.rebakeOutput = g_libraryModal.isEditMode && req.fps > 0;
				req.sourceFps = g_libraryModal.sourceFpsIndex == 1 ? 30.f : g_libraryModal.sourceFpsIndex == 2 ? 60.f : 0.f;
				req.timing = g_libraryModal.timing;

				if (g_libraryModal.isEditMode)
				{
					req.editKey = g_libraryModal.editTargetKey;

					if (IsDiskFile(pickedMotion))
						req.vmd = pickedMotion;

					if (pickedFacial == ImportSlot::kClear || pickedFacial == ImportSlot::kEmpty ||
						pickedFacial == ImportSlot::kSkip)
						req.disableMorph = true;
					else if (IsDiskFile(pickedFacial))
						req.morphVmd = pickedFacial;

					if (pickedCamera == ImportSlot::kClear || pickedCamera == ImportSlot::kEmpty)
						req.clearCamera = true;
					else if (IsDiskFile(pickedCamera))
						req.cameraVmd = pickedCamera;

					if (pickedAudio == ImportSlot::kClear || pickedAudio == ImportSlot::kEmpty ||
						pickedAudio == ImportSlot::kSkip)
						req.clearAudio = true;
					else if (IsDiskFile(pickedAudio))
						req.audio = pickedAudio;
				}
				else
				{
					// 三态：自动（不传开关，导入器按默认行为处理）／指定文件／
					// 显式关闭（--no-morph、--clear-audio）。
					req.vmd = pickedMotion;

					if (pickedFacial == ImportSlot::kSkip)
						req.disableMorph = true;
					else if (IsDiskFile(pickedFacial))
						req.morphVmd = pickedFacial;

					if (IsDiskFile(pickedCamera))
						req.cameraVmd = pickedCamera;

					if (pickedAudio == ImportSlot::kSkip)
						req.clearAudio = true;
					else if (IsDiskFile(pickedAudio))
						req.audio = pickedAudio;
				}

				if (ctx.fnImportMotion)
				{
					ctx.fnImportMotion(ctx.self, req);
				}

				overlay.isImporting = true;
				overlay.importProgress = 0.0f;
				overlay.importStartTime = ImGui::GetTime();
				overlay.importStatusText = "正在启动导入流程...";

				g_libraryModal.isOpen = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();

			ImGui::EndPopup();
		}
	}
} // namespace GakuMotion
#endif
