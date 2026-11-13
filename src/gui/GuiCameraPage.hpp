#pragma once

// =============================================================================
// GuiCameraPage.hpp — 镜头控制页面（模式选择 / 基础 Transform / 跟随参数 / 高级）
// =============================================================================

#include "GuiOverlayCommon.hpp"
#include "GkGuiIcons.hpp"
#include "GuiCameraMode.hpp"
#include "../GkGuiOverlay.hpp"
#include "../GkCamera.hpp"

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	inline void DrawCameraPage(GuiOverlay& overlay, const GuiContext& ctx)
	{
		// 同步草稿状态（保持原有 Unity 线程确认防回弹逻辑）
		if (!overlay.cameraDraftReady)
		{
			overlay.cameraDraft = overlay.cameraLastSnapshot = ctx.cameraSnapshot.settings;
			overlay.cameraDraftReady = true;
		}
		else if (overlay.cameraSettingsPending)
		{
			if (ctx.cameraSnapshot.settings == overlay.cameraExpected)
			{
				overlay.cameraSettingsPending = false;
				overlay.cameraLastSnapshot = ctx.cameraSnapshot.settings;
			}
		}
		else if (!(ctx.cameraSnapshot.settings == overlay.cameraLastSnapshot))
		{
			overlay.cameraDraft = overlay.cameraLastSnapshot = ctx.cameraSnapshot.settings;
		}

		auto& settings = overlay.cameraDraft;
		bool changed = false;

		// 确保 settings.usePerModeTransforms 初始同步（未编辑时展示对应 legacy 偏移）
		if (!settings.usePerModeTransforms)
		{
			settings.transformTrack.position = settings.vmdOffset;
			settings.transformFollowMove.position = settings.followOffset;
			settings.transformLookAt.position = settings.followOffset;
		}

		auto upgradePerMode = [&]()
		{
			if (!settings.usePerModeTransforms)
			{
				settings.transformTrack.position = settings.vmdOffset;
				settings.transformFollowMove.position = settings.followOffset;
				settings.transformLookAt.position = settings.followOffset;
				settings.usePerModeTransforms = true;
			}
		};

		// =========================================================================
		// 1. 镜头模式选择 (Camera Mode)
		// =========================================================================
		int selection = CameraControlModeIndex(settings);
		ImGui::TextUnformatted("镜头模式");
		ImGui::SetNextItemWidth((std::min)(260.0f, ImGui::GetContentRegionAvail().x));
		if (ImGui::Combo("##camera_mode", &selection, kCameraControlModes, IM_ARRAYSIZE(kCameraControlModes)))
		{
			upgradePerMode();
			SetCameraControlMode(settings, selection);
			changed = true;
		}
		// Parameter sections retain their existing Track / FollowMove / LookAt
		// indices; Game has no active GM parameter section.
		const int userMode = selection - 1;
		ImGui::TextWrapped("镜头接管: %s | 运镜轨道: %s",
			ctx.cameraSnapshot.owned ? "已生效" : "未接管",
			ctx.cameraSnapshot.hasTrack ? "已加载" : "无");
		if (!ctx.cameraSnapshot.available)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::WarningColor());
			ImGui::TextWrapped("当前镜头接口未就绪。");
			ImGui::PopStyleColor();
		}
		if (!ctx.cameraSnapshot.error.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::ErrorColor());
			ImGui::TextWrapped("镜头轨道错误: %s", ctx.cameraSnapshot.error.c_str());
			ImGui::PopStyleColor();
		}
		if (!ctx.cameraSnapshot.followTargetError.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::ErrorColor());
			ImGui::TextWrapped("跟随目标错误: %s", ctx.cameraSnapshot.followTargetError.c_str());
			ImGui::PopStyleColor();
		}
		if (overlay.cameraSettingsPending) ImGui::TextDisabled("正在应用镜头设置…");
		ImGui::TextWrapped("选择“游戏”恢复游戏手动镜头，并保留 GakuMotion 镜头参数。");
		ImGui::BeginDisabled(!settings.enabled);
		if (ImGui::Checkbox("镜头随主角色朝向", &settings.followPlacementYaw)) changed = true;
		ImGui::EndDisabled();
		ImGui::TextWrapped("开启后，镜头位置和方向跟随主角色的站位朝向；可通过下方保存镜头设置保留。");
		if (userMode == 0 && !ctx.cameraSnapshot.hasTrack)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::WarningColor());
			ImGui::TextWrapped("当前曲目没有可用运镜轨道，VMD 模式将保持固定机位。");
			ImGui::PopStyleColor();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// =========================================================================
		// 2. 基础 Transform (位置 / 旋转)
		// =========================================================================
		const float dragItemWidth = (std::max)(40.0f, (ImGui::GetContentRegionAvail().x - 36.0f) / 3.0f);
		if (settings.enabled)
		{
			ImGui::TextUnformatted("镜头位置与旋转偏移");
			if (userMode == 0)
			{
				ImGui::SetNextItemWidth((std::min)(240.0f, ImGui::GetContentRegionAvail().x));
				if (ImGui::SliderFloat("VMD 运镜范围", &settings.vmdRangeScale, 0.3f, 1.0f, "%.2f 倍")) changed = true;
				ImGui::TextWrapped("以角色附近为中心缩小轨迹；会改变构图，不能保证避免穿模。点击下方保存镜头设置可保留。");
			}

			CameraModeTransform* activeTf = nullptr;
			if (userMode == 0)
				activeTf = &settings.transformTrack;
			else if (userMode == 1)
				activeTf = &settings.transformFollowMove;
			else
				activeTf = &settings.transformLookAt;

			// 位置 X / Y / Z
			ImGui::TextUnformatted("位置 (米)");
			ImGui::SetNextItemWidth(dragItemWidth);
			if (ImGui::DragFloat("##cam_pos_x", &activeTf->position.x, 0.01f, -50.0f, 50.0f, "X: %.2f"))
			{
				upgradePerMode();
				changed = true;
			}

			ImGui::SameLine();
			ImGui::SetNextItemWidth(dragItemWidth);
			if (ImGui::DragFloat("##cam_pos_y", &activeTf->position.y, 0.01f, -50.0f, 50.0f, "Y: %.2f"))
			{
				upgradePerMode();
				changed = true;
			}

			ImGui::SameLine();
			ImGui::SetNextItemWidth(dragItemWidth);
			if (ImGui::DragFloat("##cam_pos_z", &activeTf->position.z, 0.01f, -50.0f, 50.0f, "Z: %.2f"))
			{
				upgradePerMode();
				changed = true;
			}

			// 旋转 X / Y / Z
			ImGui::TextUnformatted("旋转 (度)");
			ImGui::SetNextItemWidth(dragItemWidth);
			if (ImGui::DragFloat("##cam_rot_x", &activeTf->rotation.x, 0.5f, -180.0f, 180.0f, "X: %.1f°"))
			{
				upgradePerMode();
				changed = true;
			}

			ImGui::SameLine();
			ImGui::SetNextItemWidth(dragItemWidth);
			if (ImGui::DragFloat("##cam_rot_y", &activeTf->rotation.y, 0.5f, -180.0f, 180.0f, "Y: %.1f°"))
			{
				upgradePerMode();
				changed = true;
			}

			ImGui::SameLine();
			ImGui::SetNextItemWidth(dragItemWidth);
			if (ImGui::DragFloat("##cam_rot_z", &activeTf->rotation.z, 0.5f, -180.0f, 180.0f, "Z: %.1f°"))
			{
				upgradePerMode();
				changed = true;
			}

			if (GuiIcons::Button("重置位置与旋转偏移", GuiIcons::Icon::Reset, ImVec2(160, 24)))
			{
				upgradePerMode();
				if (userMode == 0)
				{
					settings.transformTrack = CameraSettings::DefaultTrackTransform();
					settings.vmdOffset = settings.transformTrack.position;
				}
				else if (userMode == 1)
				{
					settings.transformFollowMove = CameraSettings::DefaultFollowMoveTransform();
					settings.followOffset = settings.transformFollowMove.position;
				}
				else
				{
					settings.transformLookAt = CameraSettings::DefaultLookAtTransform();
					settings.followOffset = settings.transformLookAt.position;
				}
				changed = true;
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
		}

		// =========================================================================
		// 3. 跟随响应 (Target Rate)
		// =========================================================================
		if (settings.enabled && (userMode == 1 || userMode == 2 || settings.followActor))
		{
			if (settings.mode == CameraMode::Follow)
			{
				int target = static_cast<int>(settings.followTarget);
				ImGui::SetNextItemWidth((std::min)(260.0f, ImGui::GetContentRegionAvail().x));
				if (ImGui::Combo("跟随部位", &target, "髋部\0胸部\0头部\0"))
				{
					settings.followTarget = static_cast<CameraFollowTarget>(target);
					changed = true;
				}
			}
			ImGui::TextUnformatted("追随速率 (Target Rate)");
			ImGui::TextWrapped("0 停止追随，0.1 平滑追随，1 即时追随。数值越小，追赶越慢。");
			ImGui::TextWrapped("X / Y / Z 为接管时镜头的右 / 上 / 前方向；正速率会持续追近当前目标。");

			ImGui::SetNextItemWidth(dragItemWidth);
			if (ImGui::DragFloat("##rate_x", &settings.followRate.x, 0.01f, 0.0f, 1.0f, "X: %.2f", ImGuiSliderFlags_AlwaysClamp))
				changed = true;

			ImGui::SameLine();
			ImGui::SetNextItemWidth(dragItemWidth);
			if (ImGui::DragFloat("##rate_y", &settings.followRate.y, 0.01f, 0.0f, 1.0f, "Y: %.2f", ImGuiSliderFlags_AlwaysClamp))
				changed = true;

			ImGui::SameLine();
			ImGui::SetNextItemWidth(dragItemWidth);
			if (ImGui::DragFloat("##rate_z", &settings.followRate.z, 0.01f, 0.0f, 1.0f, "Z: %.2f", ImGuiSliderFlags_AlwaysClamp))
				changed = true;

			if (GuiIcons::Button("重置追随速率", GuiIcons::Icon::Reset, ImVec2(120, 24)))
			{
				settings.followRate = Vector3(0.1f, 0.1f, 0.1f);
				changed = true;
			}
			if (settings.followFactor.x != 1.0f || settings.followFactor.y != 1.0f || settings.followFactor.z != 1.0f)
				ImGui::TextWrapped("高级设置中的位移比例未全开，追随目标会按该比例缩放。");

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
		}

		// =========================================================================
		// 4. 高级镜头设置 (折叠)
		// =========================================================================
		ImGui::BeginDisabled(!settings.enabled);
		if (ImGui::CollapsingHeader("高级镜头设置"))
		{
			ImGui::Indent(10.0f);

			// VMD 模式下专属选项
			if (userMode == 0)
			{
				if (ImGui::Checkbox("镜头随角色平移", &settings.followActor))
					changed = true;
			}

			if (userMode == 1 || userMode == 2 || settings.followActor)
			{
				ImGui::TextUnformatted("位移比例（旧版跟随幅度）");
				ImGui::TextWrapped("缩放最终跟随的角色位移：0 不跟随该轴，0.5 跟一半，1 完整跟随。此项决定目标幅度，追随速率决定追赶快慢。");
				ImGui::SetNextItemWidth(dragItemWidth);
				if (ImGui::DragFloat("##ff_x", &settings.followFactor.x, 0.01f, 0.0f, 1.0f, "X: %.2f", ImGuiSliderFlags_AlwaysClamp)) changed = true;
				ImGui::SameLine();
				ImGui::SetNextItemWidth(dragItemWidth);
				if (ImGui::DragFloat("##ff_y", &settings.followFactor.y, 0.01f, 0.0f, 1.0f, "Y: %.2f", ImGuiSliderFlags_AlwaysClamp)) changed = true;
				ImGui::SameLine();
				ImGui::SetNextItemWidth(dragItemWidth);
				if (ImGui::DragFloat("##ff_z", &settings.followFactor.z, 0.01f, 0.0f, 1.0f, "Z: %.2f", ImGuiSliderFlags_AlwaysClamp)) changed = true;
				if (GuiIcons::Button("恢复完整位移", GuiIcons::Icon::Reset, ImVec2(120, 24)))
				{
					settings.followFactor = {1.0f, 1.0f, 1.0f};
					changed = true;
				}
			}

			if (userMode == 1 || userMode == 2)
			{
				// 注视目标偏移
				ImGui::TextUnformatted("注视目标偏移 (Target Offset，相对所选部位)");
				ImGui::SetNextItemWidth(dragItemWidth);
				if (ImGui::DragFloat("##to_x", &settings.targetOffset.x, 0.01f, -5.0f, 5.0f, "X: %.2f")) changed = true;
				ImGui::SameLine();
				ImGui::SetNextItemWidth(dragItemWidth);
				if (ImGui::DragFloat("##to_y", &settings.targetOffset.y, 0.01f, -5.0f, 5.0f, "Y: %.2f")) changed = true;
				ImGui::SameLine();
				ImGui::SetNextItemWidth(dragItemWidth);
				if (ImGui::DragFloat("##to_z", &settings.targetOffset.z, 0.01f, -5.0f, 5.0f, "Z: %.2f")) changed = true;

				// 跟随视野
				ImGui::SetNextItemWidth(120.0f);
				if (ImGui::SliderFloat("镜头 FOV", &settings.followFov, 15.0f, 90.0f, "%.1f°")) changed = true;
			}

			// 场景范围限制
			if (ImGui::Checkbox("限制镜头在场景范围内", &settings.limitToScene)) changed = true;
			if (settings.limitToScene)
			{
				ImGui::TextUnformatted("最小边界 (Bounds Min)");
				ImGui::SetNextItemWidth(dragItemWidth);
				if (ImGui::DragFloat("##bmin_x", &settings.boundsMin.x, 0.1f, -50.0f, 50.0f, "X: %.1f")) changed = true;
				ImGui::SameLine();
				ImGui::SetNextItemWidth(dragItemWidth);
				if (ImGui::DragFloat("##bmin_y", &settings.boundsMin.y, 0.1f, -50.0f, 50.0f, "Y: %.1f")) changed = true;
				ImGui::SameLine();
				ImGui::SetNextItemWidth(dragItemWidth);
				if (ImGui::DragFloat("##bmin_z", &settings.boundsMin.z, 0.1f, -50.0f, 50.0f, "Z: %.1f")) changed = true;

				ImGui::TextUnformatted("最大边界 (Bounds Max)");
				ImGui::SetNextItemWidth(dragItemWidth);
				if (ImGui::DragFloat("##bmax_x", &settings.boundsMax.x, 0.1f, -50.0f, 50.0f, "X: %.1f")) changed = true;
				ImGui::SameLine();
				ImGui::SetNextItemWidth(dragItemWidth);
				if (ImGui::DragFloat("##bmax_y", &settings.boundsMax.y, 0.1f, -50.0f, 50.0f, "Y: %.1f")) changed = true;
				ImGui::SameLine();
				ImGui::SetNextItemWidth(dragItemWidth);
				if (ImGui::DragFloat("##bmax_z", &settings.boundsMax.z, 0.1f, -50.0f, 50.0f, "Z: %.1f")) changed = true;
			}

			ImGui::Unindent(10.0f);
		}

		ImGui::EndDisabled();

		// 提交镜头修改
		if (changed)
		{
			if (userMode == 0)
				settings.vmdOffset = settings.transformTrack.position;
			else if (userMode == 1)
				settings.followOffset = settings.transformFollowMove.position;
			else if (userMode == 2)
				settings.followOffset = settings.transformLookAt.position;

			overlay.cameraExpected = settings;
			overlay.cameraSettingsPending = true;
			if (ctx.fnSetCameraSettings) ctx.fnSetCameraSettings(ctx.self, settings);
		}

		ImGui::Spacing();
		ImGui::BeginDisabled(!settings.IsValid() || !ctx.fnSaveCameraSettings);
		if (GuiIcons::Button("保存镜头设置", GuiIcons::Icon::Save, ImVec2(120, 24))) ctx.fnSaveCameraSettings(ctx.self);
		ImGui::EndDisabled();
		ImGui::TextWrapped("修改立即生效；保存后下次启动自动载入，作用于全部曲目。");
		if (!ctx.cameraNotice.empty())
		{
			const bool failed = ctx.cameraNotice.find("失败") != std::string::npos ||
				ctx.cameraNotice.find("无效") != std::string::npos;
			if (failed) ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::ErrorColor());
			ImGui::TextWrapped("%s", ctx.cameraNotice.c_str());
			if (failed) ImGui::PopStyleColor();
		}
	}
} // namespace GakuMotion
#endif
