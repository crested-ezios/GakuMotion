#pragma once

// =============================================================================
// GuiTransportBar.hpp — 底部全局常驻播放控制器
// =============================================================================

#include "GuiOverlayCommon.hpp"
#include "GuiCameraMode.hpp"
#include "../GkGuiOverlay.hpp"
#include "../GkCamera.hpp"
#include "../GkTransportTime.hpp"
#include <initializer_list>

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	enum class TransportBtnIcon { Prev, Play, Pause, Stop, Next };
	inline constexpr const char* kTransportPlayModes[] = {"播放一次", "单曲循环", "列表循环", "随机播放"};
	inline constexpr const auto& kTransportCameraModes = kCameraControlQuickModes;
	inline constexpr float kTransportPlaybackSpeeds[] = {0.1f, 0.2f, 0.5f, 0.8f, 1.0f, 1.2f, 1.5f, 2.0f, 3.0f};
	inline constexpr const char* kTransportPlaybackSpeedLabels[] = {
		"倍速: 0.1x", "倍速: 0.2x", "倍速: 0.5x", "倍速: 0.8x", "倍速: 1.0x",
		"倍速: 1.2x", "倍速: 1.5x", "倍速: 2.0x", "倍速: 3.0x"};

	inline float TransportTextButtonWidth(const char* text, float minimum = 0.0f)
	{
		return (std::max)(minimum, ImGui::CalcTextSize(text).x + 2.0f * ImGui::GetStyle().FramePadding.x);
	}

	template<size_t Count>
	inline float TransportComboWidth(const char* const (&labels)[Count])
	{
		float textWidth = 0.0f;
		for (const char* label : labels) textWidth = (std::max)(textWidth, ImGui::CalcTextSize(label).x);
		// BeginCombo reserves one frame-height-wide arrow button in addition to
		// the preview's text padding. Measure every choice to avoid resize jitter.
		return (std::max)(105.0f, textWidth + 2.0f * ImGui::GetStyle().FramePadding.x + ImGui::GetFrameHeight());
	}

	inline float TransportTimeInputWidth() { return TransportTextButtonWidth("00:00.000", 126.0f); }
	inline float TransportFrameInputWidth() { return TransportTextButtonWidth("0000000", 92.0f); }
	inline float TransportMusicVolumeWidth() { return 130.0f + ImGui::CalcTextSize("音乐音量").x + ImGui::GetStyle().ItemInnerSpacing.x; }
	inline float TransportPrecisionGroupWidth(const char* label, float inputWidth)
	{
		return ImGui::CalcTextSize(label).x + inputWidth + TransportTextButtonWidth("定位") + 2.0f * ImGui::GetStyle().ItemSpacing.x;
	}

	inline float TransportBarHeight(float availableWidth, const GuiOverlay* overlay = nullptr)
	{
		const float fontSize = ImGui::GetFontSize();
		if (overlay && overlay->transportMeasuredHeight > 0.0f &&
			std::fabs(overlay->transportMeasuredWidth - availableWidth) < 1.0f &&
			std::fabs(overlay->transportMeasuredFontSize - fontSize) < 0.01f)
			return overlay->transportMeasuredHeight;

		// A compact first-frame estimate. Once drawn, use actual content height;
		// do not reserve hypothetical warning/error rows in every normal frame.
		const auto& style = ImGui::GetStyle();
		const float frameHeight = ImGui::GetFrameHeight();
		const float width = (std::max)(1.0f, availableWidth);
		const auto flow = [&](std::initializer_list<float> blocks, float gap, float height) {
			float used = 0.0f, rows = 1.0f;
			for (float block : blocks)
			{
				if (used > 0.0f && used + gap + block > width) { ++rows; used = 0.0f; }
				used += (used > 0.0f ? gap : 0.0f) + block;
			}
			return rows * (height + style.ItemSpacing.y);
		};
		return std::ceil(2.0f * (fontSize + style.ItemSpacing.y) + frameHeight + style.ItemSpacing.y +
			flow({4.0f * (std::max)(34.0f, frameHeight) + 3.0f * style.ItemSpacing.x,
				TransportComboWidth(kTransportPlayModes), TransportComboWidth(kTransportPlaybackSpeedLabels),
				TransportMusicVolumeWidth(),
				TransportComboWidth(kTransportCameraModes)}, 18.0f,
				(std::max)(24.0f, frameHeight)) +
			flow({2.0f * TransportTextButtonWidth("上一帧", 68.0f) + style.ItemSpacing.x,
				TransportPrecisionGroupWidth("时间", TransportTimeInputWidth()),
				TransportPrecisionGroupWidth("动作帧", TransportFrameInputWidth())}, 12.0f, frameHeight));
	}

	inline void TransportSameLineIfRoom(float width, float gap = 12.0f)
	{
		const float previousRight = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
		if (ImGui::GetWindowContentRegionMax().x - previousRight >= width + gap)
			ImGui::SameLine(0.0f, gap);
	}

	// 纯矢量绘制的媒体控制按钮，100% 避免字符集缺失导致的问号乱码
	inline bool TransportIconButton(const char* strId, TransportBtnIcon icon, ImVec2 size)
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const bool isLight = GuiTheme::IsLight();

		if (isLight)
		{
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.78f, 0.90f, 1.00f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.58f, 0.68f, 0.82f, 1.00f));
		}
		else
		{
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.32f, 0.44f, 1.00f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.40f, 0.55f, 1.00f));
		}

		const bool pressed = ImGui::Button(strId, size);
		const bool hovered = ImGui::IsItemHovered();
		const bool active = ImGui::IsItemActive();
		ImGui::PopStyleColor(2);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec4 colVec = GuiTheme::TransportIconColor(hovered, active);
		const ImU32 col = ImGui::GetColorU32(colVec);

		const float midX = pos.x + size.x * 0.5f;
		const float midY = pos.y + size.y * 0.5f;

		switch (icon)
		{
		case TransportBtnIcon::Prev: // |<
			drawList->AddRectFilled(ImVec2(midX - 7.0f, midY - 6.0f), ImVec2(midX - 5.0f, midY + 6.0f), col);
			drawList->AddTriangleFilled(ImVec2(midX - 5.0f, midY), ImVec2(midX + 6.0f, midY - 6.0f), ImVec2(midX + 6.0f, midY + 6.0f), col);
			break;
		case TransportBtnIcon::Play: // >
			drawList->AddTriangleFilled(ImVec2(midX + 6.0f, midY), ImVec2(midX - 5.0f, midY - 6.0f), ImVec2(midX - 5.0f, midY + 6.0f), col);
			break;
		case TransportBtnIcon::Pause: // ||
			drawList->AddRectFilled(ImVec2(midX - 5.0f, midY - 6.0f), ImVec2(midX - 2.0f, midY + 6.0f), col);
			drawList->AddRectFilled(ImVec2(midX + 2.0f, midY - 6.0f), ImVec2(midX + 5.0f, midY + 6.0f), col);
			break;
		case TransportBtnIcon::Stop: // []
			drawList->AddRectFilled(ImVec2(midX - 5.0f, midY - 5.0f), ImVec2(midX + 5.0f, midY + 5.0f), col);
			break;
		case TransportBtnIcon::Next: // >|
			drawList->AddTriangleFilled(ImVec2(midX + 4.0f, midY), ImVec2(midX - 6.0f, midY - 6.0f), ImVec2(midX - 6.0f, midY + 6.0f), col);
			drawList->AddRectFilled(ImVec2(midX + 5.0f, midY - 6.0f), ImVec2(midX + 7.0f, midY + 6.0f), col);
			break;
		}

		return pressed;
	}

	inline void DrawTransportBar(GuiOverlay& overlay, const GuiContext& ctx)
	{
		const float availWidth = ImGui::GetContentRegionAvail().x;
		const float barHeight = TransportBarHeight(availWidth, &overlay);

		// 底部常驻区域分隔线与子窗口
		ImGui::Separator();
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_ChildBg));

		if (ImGui::BeginChild("##gkmotion_global_transport", ImVec2(availWidth, barHeight), false,
			                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			// 1. 获取当前播放条目名称与时长
			std::string playingTitle = "未加载曲目";
			float currentTime = 0.0f;
			float totalDuration = 0.0f;
			bool isPlaying = false;
			bool isPaused = false;
			bool isPreview = false;
			bool hasClip = false;

			if (ctx.player)
			{
				isPlaying = (ctx.player->state == PlaybackState::Playing);
				isPaused = (ctx.player->state == PlaybackState::Paused);
				isPreview = (ctx.player->state == PlaybackState::Preview);
				hasClip = ValidTransportTimeline(ctx.player->clip.total_frames, ctx.player->clip.fps);
				currentTime = ctx.player->currentTime;
				totalDuration = ctx.player->TimelineDuration();

				if (!ctx.player->loadedMotionFile.empty())
				{
					std::filesystem::path p(ctx.player->loadedMotionFile);
					playingTitle = p.parent_path().filename().string();
					if (playingTitle.empty()) playingTitle = p.stem().string();
				}
				else if (ctx.motions && ctx.selectedMotionIndex && *ctx.selectedMotionIndex >= 0 &&
				         *ctx.selectedMotionIndex < static_cast<int>(ctx.motions->size()))
				{
					playingTitle = (*ctx.motions)[*ctx.selectedMotionIndex].title;
				}
			}

			// 第一行：曲目名称（左） + 时间进度 01:24 / 03:20（右）
			char curTimeStr[32], totalTimeStr[32];
			FormatTransportTime(currentTime, curTimeStr, sizeof(curTimeStr));
			FormatTransportTime(totalDuration, totalTimeStr, sizeof(totalTimeStr));
			char timeLabel[64];
			std::snprintf(timeLabel, sizeof(timeLabel), "%s / %s", curTimeStr, totalTimeStr);

			const float timeTextWidth = ImGui::CalcTextSize(timeLabel).x;

			// 播放状态文本与标题（杜绝无法识别的特殊字符）
			if (isPlaying)
				ImGui::TextColored(GuiTheme::SuccessColor(), "[播放] %s", playingTitle.c_str());
			else if (isPaused)
				ImGui::TextColored(GuiTheme::WarningColor(), "[暂停] %s", playingTitle.c_str());
			else if (isPreview)
				ImGui::TextColored(GuiTheme::InfoColor(), "[预览] %s", playingTitle.c_str());
			else
				ImGui::TextDisabled("[停止] %s", playingTitle.c_str());

			if (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + timeTextWidth + 24.0f <
				ImGui::GetWindowContentRegionMax().x)
				ImGui::SameLine(ImGui::GetWindowWidth() - timeTextWidth - 16.0f);
			ImGui::TextDisabled("%s", timeLabel);

			auto& navigation = overlay.transportNavigation;
			const std::string motionFile = ctx.player ? ctx.player->loadedMotionFile : "";
			if (!navigation.bound || navigation.motionFile != motionFile || navigation.generation != ctx.loadedMotionGeneration ||
				navigation.injectionSession != ctx.injectionSession)
			{
				navigation = {};
				navigation.bound = true;
				navigation.motionFile = motionFile;
				navigation.generation = ctx.loadedMotionGeneration;
				navigation.injectionSession = ctx.injectionSession;
				overlay.scrub.active = false;
			}

			// 第二行：Scrub 进度滑条（严格支持三阶段防回弹状态机）
			const float maxDur = (totalDuration > 0.0f) ? totalDuration : 0.0f;
			float scrubVal = overlay.scrub.active ? overlay.scrub.value : currentTime;
			if (scrubVal < 0.0f) scrubVal = 0.0f;
			if (scrubVal > maxDur) scrubVal = maxDur;

			ImGui::SetNextItemWidth(-1.0f);
			ImGui::BeginDisabled(!hasClip || maxDur <= 0.0f);
			if (ImGui::SliderFloat("##global_scrub", &scrubVal, 0.0f, maxDur, ""))
			{
				if (!overlay.scrub.active)
				{
					overlay.scrub.active = true;
					overlay.scrub.value = scrubVal;
					++overlay.probeScrubBegin;
					overlay.ProbeScrub(ctx, "begin", scrubVal, 1);
					if (ctx.fnScrubBegin) ctx.fnScrubBegin(ctx.self, scrubVal);
					else if (ctx.fnSeek) ctx.fnSeek(ctx.self, scrubVal);
				}
				else
				{
					overlay.scrub.value = scrubVal;
					++overlay.probeScrubUpdate;
					if (ctx.fnScrubUpdate) ctx.fnScrubUpdate(ctx.self, scrubVal);
					else if (ctx.fnSeek) ctx.fnSeek(ctx.self, scrubVal);
				}
			}
			if (ImGui::IsItemDeactivated())
			{
				overlay.CommitScrub(ctx);
			}
			ImGui::EndDisabled();

			// 第三行：控制按钮 + 播放模式 + 镜头快捷切换
			const ImVec2 btnSize((std::max)(34.0f, ImGui::GetFrameHeight()), (std::max)(24.0f, ImGui::GetFrameHeight()));

			// 上一曲 (|<)
			ImGui::BeginDisabled(!ctx.motions || ctx.motions->empty());
			if (TransportIconButton("##prev", TransportBtnIcon::Prev, btnSize))
			{
				if (ctx.fnCycleMotion) ctx.fnCycleMotion(ctx.self, -1);
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("上一曲");
			ImGui::EndDisabled();

			ImGui::SameLine();

			// 播放 / 暂停切换 (> / ||)
			ImGui::BeginDisabled(!hasClip);
			if (isPlaying)
			{
				if (TransportIconButton("##pause", TransportBtnIcon::Pause, btnSize))
				{
					if (ctx.fnPause) ctx.fnPause(ctx.self);
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("暂停");
			}
			else
			{
				if (TransportIconButton("##play", TransportBtnIcon::Play, btnSize))
				{
					if (isPaused && ctx.fnResume) ctx.fnResume(ctx.self);
					else if (ctx.fnPlay) ctx.fnPlay(ctx.self);
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(isPaused ? "继续（从当前位置续播）" : "播放");
			}

			ImGui::SameLine();

			// 停止 ([])
			if (TransportIconButton("##stop", TransportBtnIcon::Stop, btnSize))
			{
				if (ctx.fnStop) ctx.fnStop(ctx.self);
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("停止：归零并恢复游戏动作");
			ImGui::EndDisabled();

			ImGui::SameLine();

			// 下一曲 (>|)
			ImGui::BeginDisabled(!ctx.motions || ctx.motions->empty());
			if (TransportIconButton("##next", TransportBtnIcon::Next, btnSize))
			{
				if (ctx.fnCycleMotion) ctx.fnCycleMotion(ctx.self, 1);
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("下一曲");
			ImGui::EndDisabled();

			const float playModeWidth = TransportComboWidth(kTransportPlayModes);
			TransportSameLineIfRoom(playModeWidth, 18.0f);

			// 播放模式切换 (0:播放一次, 1:单曲循环, 2:列表循环, 3:随机播放)
			int currentPlayMode = ctx.playlistMode;
			if (currentPlayMode < 0 || currentPlayMode > 3) currentPlayMode = 1;
			ImGui::SetNextItemWidth(playModeWidth);
			if (ImGui::Combo("##play_mode", &currentPlayMode, kTransportPlayModes, IM_ARRAYSIZE(kTransportPlayModes)))
			{
				overlay.PlaylistAction(ctx, GuiPlaylistAction::SetMode, "", currentPlayMode);
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("播放列表循环模式");

			const float playbackSpeedWidth = TransportComboWidth(kTransportPlaybackSpeedLabels);
			TransportSameLineIfRoom(playbackSpeedWidth, 18.0f);
			const float playbackSpeed = ctx.player ? ctx.player->playbackSpeedMultiplier : 1.0f;
			char playbackSpeedPreview[48]{};
			std::snprintf(playbackSpeedPreview, sizeof(playbackSpeedPreview), "倍速: %.1fx", playbackSpeed);
			ImGui::BeginDisabled(!ctx.fnSetSpeed);
			ImGui::SetNextItemWidth(playbackSpeedWidth);
			if (ImGui::BeginCombo("##playback_speed", playbackSpeedPreview))
			{
				for (int i = 0; i < IM_ARRAYSIZE(kTransportPlaybackSpeeds); ++i)
				{
					const bool selected = std::fabs(playbackSpeed - kTransportPlaybackSpeeds[i]) < 0.0001f;
					if (ImGui::Selectable(kTransportPlaybackSpeedLabels[i], selected) && ctx.fnSetSpeed)
						ctx.fnSetSpeed(ctx.self, kTransportPlaybackSpeeds[i]);
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("与各文件播放速度相乘；音频无法拉伸时自动静音。");

			TransportSameLineIfRoom(TransportMusicVolumeWidth(), 18.0f);
			int musicVolume = ctx.player ? static_cast<int>(std::lround(ctx.player->musicVolume * 100.0f)) : 100;
			ImGui::BeginDisabled(!ctx.player || !ctx.fnSetMusicVolume);
			ImGui::SetNextItemWidth(130.0f);
			if (ImGui::SliderInt("音乐音量", &musicVolume, 0, 100, "%d%%") && ctx.fnSetMusicVolume)
				ctx.fnSetMusicVolume(ctx.self, static_cast<float>(musicVolume) / 100.0f);
			if (ImGui::IsItemDeactivatedAfterEdit() && ctx.fnSaveMusicVolume) ctx.fnSaveMusicVolume(ctx.self);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("只调整 GakuMotion 音乐；松开后自动保存。");
			ImGui::EndDisabled();

			const float cameraModeWidth = TransportComboWidth(kTransportCameraModes);
			TransportSameLineIfRoom(cameraModeWidth, 18.0f);

			// 镜头模式快捷切换（仅切换模式，不暴露参数）
			if (overlay.cameraSettingsPending && ctx.cameraSnapshot.settings == overlay.cameraExpected)
			{
				overlay.cameraSettingsPending = false;
				overlay.cameraLastSnapshot = ctx.cameraSnapshot.settings;
			}
			const CameraSettings& selectionSettings = overlay.cameraSettingsPending
				? overlay.cameraDraft : ctx.cameraSnapshot.settings;
			int currentCamIndex = CameraControlModeIndex(selectionSettings);

			ImGui::SetNextItemWidth(cameraModeWidth);
			if (ImGui::Combo("##camera_quick_mode", &currentCamIndex, kTransportCameraModes, IM_ARRAYSIZE(kTransportCameraModes)))
			{
				CameraSettings next = selectionSettings;
				SetCameraControlMode(next, currentCamIndex);
				if (ctx.fnSetCameraSettings)
				{
					ctx.fnSetCameraSettings(ctx.self, next);
					overlay.cameraDraft = overlay.cameraExpected = next;
					overlay.cameraDraftReady = overlay.cameraSettingsPending = true;
				}
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("快速切换镜头模式；游戏镜头会释放接管并保留 GakuMotion 参数");

			// Precision controls bind their drafts to the actual loaded generation.
			// Enter or the adjacent button commits; Escape cancels. Drafts survive
			// focus moving to the button, so its mouse-up cannot submit live time.
			// Bind ImGui's internal text-edit identity too: resetting our buffers
			// alone would let a still-active old session restore its internal draft.
			const std::string navigationId = navigation.motionFile + ":" + std::to_string(navigation.generation) + ":" +
				std::to_string(navigation.injectionSession);
			ImGui::PushID(navigationId.c_str());
			ImGui::BeginDisabled(!hasClip);
			ImGui::BeginGroup();
			const float stepWidth = (std::max)(TransportTextButtonWidth("上一帧", 68.0f), TransportTextButtonWidth("下一帧", 68.0f));
			if (ImGui::Button("上一帧", ImVec2(stepWidth, 0.0f)) && ctx.fnStepFrame) ctx.fnStepFrame(ctx.self, -1);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("暂停并检查前一个动作采样帧；不切换曲目");
			ImGui::SameLine();
			if (ImGui::Button("下一帧", ImVec2(stepWidth, 0.0f)) && ctx.fnStepFrame) ctx.fnStepFrame(ctx.self, 1);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("暂停并检查后一个动作采样帧；不切换曲目");
			ImGui::EndGroup();

			const float timeInputWidth = TransportTimeInputWidth();
			TransportSameLineIfRoom(TransportPrecisionGroupWidth("时间", timeInputWidth));
			ImGui::BeginGroup();
			ImGui::TextUnformatted("时间");
			ImGui::SameLine();
			if (!navigation.editingTime) FormatTransportTime(currentTime, navigation.time, sizeof(navigation.time));
			ImGui::SetNextItemWidth(timeInputWidth);
			const bool timeEnter = ImGui::InputText("##transport_time", navigation.time, sizeof(navigation.time), ImGuiInputTextFlags_EnterReturnsTrue);
			if (ImGui::IsItemActivated()) navigation.editingTime = true;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("秒数或 mm:ss.mmm；Enter 定位，Esc 取消。时间可位于两个采样帧之间。");
			if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) navigation.editingTime = false;
			ImGui::SameLine();
			const bool timeClick = ImGui::Button("定位##transport_time");
			if (timeEnter || timeClick)
			{
				float seconds = 0.0f;
				if (!ParseTransportTime(navigation.time, seconds))
					navigation.error = "时间格式无效：请输入非负秒数或 mm:ss.mmm。";
				else if (seconds > totalDuration)
					navigation.error = "时间超出演出时长；可用动作帧输入检查末采样。";
				else if (ctx.fnSeek)
				{
					ctx.fnSeek(ctx.self, seconds);
					navigation.error.clear();
				}
				navigation.editingTime = false;
			}
			ImGui::EndGroup();

			const float frameInputWidth = TransportFrameInputWidth();
			TransportSameLineIfRoom(TransportPrecisionGroupWidth("动作帧", frameInputWidth));
			ImGui::BeginGroup();
			ImGui::TextUnformatted("动作帧");
			ImGui::SameLine();
			const double framePosition = hasClip
				? (ctx.player->navigationFrame >= 0 && currentTime == ctx.player->navigationFrameTime
					? static_cast<double>(ctx.player->navigationFrame)
					: TransportFramePosition(ctx.player->MotionSourceTime(), ctx.player->clip.fps, ctx.player->clip.total_frames))
				: 0.0;
			if (!navigation.editingFrame)
				std::snprintf(navigation.frame, sizeof(navigation.frame), "%lld", static_cast<long long>(std::floor(framePosition)));
			ImGui::SetNextItemWidth(frameInputWidth);
			const bool frameEnter = ImGui::InputText("##transport_frame", navigation.frame, sizeof(navigation.frame), ImGuiInputTextFlags_EnterReturnsTrue);
			if (ImGui::IsItemActivated()) navigation.editingFrame = true;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("按本动作的实际采样帧率换算，首帧为 0。Enter 定位并暂停，Esc 取消。");
			if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) navigation.editingFrame = false;
			ImGui::SameLine();
			const bool frameClick = ImGui::Button("定位##transport_frame");
			if (frameEnter || frameClick)
			{
				int64_t frame = 0;
				if (!ParseTransportFrame(navigation.frame, frame))
					navigation.error = "帧号格式无效：请输入从 0 开始的非负整数。";
				else if (!hasClip || frame >= static_cast<int64_t>(ctx.player->clip.total_frames))
					navigation.error = "帧号超出范围；末帧为总采样帧数减 1。";
				else if (ctx.fnSeekFrame)
				{
					ctx.fnSeekFrame(ctx.self, frame);
					navigation.error.clear();
				}
				navigation.editingFrame = false;
			}
			ImGui::EndGroup();
			ImGui::EndDisabled();
			ImGui::PopID();

			if (hasClip)
			{
				if (framePosition == std::floor(framePosition))
					ImGui::TextDisabled("当前位置：动作帧 %lld / %u · %.6g fps", static_cast<long long>(framePosition),
						ctx.player->clip.total_frames - 1, ctx.player->clip.fps);
				else
					ImGui::TextDisabled("当前位置：帧间 %.3f / %u · %.6g fps", framePosition,
						ctx.player->clip.total_frames - 1, ctx.player->clip.fps);
				if (TransportDurationMismatch(ctx.player->clip.total_frames, ctx.player->clip.fps, ctx.player->clip.duration_seconds))
				{
					ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::WarningColor());
					ImGui::TextWrapped("源动作头部时长与采样帧不一致：头部 %.3f 秒；末采样 %.3f 秒。",
						ctx.player->clip.duration_seconds, TransportLastFrameTime(ctx.player->clip.total_frames, ctx.player->clip.fps));
					ImGui::PopStyleColor();
				}
			}
			if (ctx.loadedSourceChanged || ctx.loadedSourceMissing)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::WarningColor());
				ImGui::TextWrapped("%s", ctx.loadedSourceMissing
					? "曲目源文件已移除：请先恢复文件，再到曲库重新加载当前曲目。"
					: "曲目文件已更新：请到曲库重新加载当前曲目；音乐更新后需要重新加载才能开播。");
				ImGui::PopStyleColor();
			}
			if (ctx.musicVolumeSupport < 0 || !ctx.musicVolumeNotice.empty())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::WarningColor());
				ImGui::TextWrapped("%s", ctx.musicVolumeSupport < 0
					? "当前音乐后端不支持独立音量，音乐仍以原音量播放。" : ctx.musicVolumeNotice.c_str());
				ImGui::PopStyleColor();
			}
			if (!navigation.error.empty())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::ErrorColor());
				ImGui::TextWrapped("%s", navigation.error.c_str());
				ImGui::PopStyleColor();
			}

			// Cursor Y includes the real wrapped rows and the final item spacing.
			// The shell consumes this same result next frame, so extra messages grow
			// the transport only while present instead of leaving permanent blank rows.
			overlay.transportMeasuredHeight = std::ceil(ImGui::GetCursorPosY());
			overlay.transportMeasuredWidth = availWidth;
			overlay.transportMeasuredFontSize = ImGui::GetFontSize();
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
	}
} // namespace GakuMotion
#endif
