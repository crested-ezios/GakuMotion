#pragma once

// =============================================================================
// GuiMotionPage.hpp — 动作控制页面（场景驱动角色 / 初始位置 / 高级动作）
// =============================================================================

#include "GuiOverlayCommon.hpp"
#include "GkGuiIcons.hpp"
#include "../GkGuiOverlay.hpp"
#include "../GkBoneOverride.hpp"
#include "../GkIdolController.hpp"

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	inline void DrawEnsembleControls(GuiOverlay& overlay, const GuiContext& ctx)
	{
		const auto& snapshot = ctx.ensembleSnapshot;
		if (!overlay.ensembleDraftReady || overlay.ensembleDraftRevision != snapshot.revision)
		{
			overlay.ensembleDraft = snapshot.settings;
			overlay.ensembleDraftRevision = snapshot.revision;
			overlay.ensembleDraftEpoch = snapshot.actorEpoch;
			overlay.ensembleDraftReady = true;
			overlay.ensembleDraftDirty = false;
		}
		else if (overlay.ensembleDraftEpoch != snapshot.actorEpoch)
		{
			for (auto& slot : overlay.ensembleDraft.slots) slot.actorToken = 0;
			overlay.ensembleDraftEpoch = snapshot.actorEpoch;
			overlay.ensembleDraftDirty = true;
		}
		if (!ImGui::CollapsingHeader("多人舞台", snapshot.active ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None))
			return;
		ImGui::PushID("ensemble");
		auto& draft = overlay.ensembleDraft;
		const size_t targetSlotCount = (std::max)(size_t{1}, ctx.sceneActors.size());
		if (!snapshot.active && !overlay.ensembleDraftDirty && draft.slots.size() != targetSlotCount)
		{
			draft.slots.resize(targetSlotCount);
			if (draft.masterSlot >= targetSlotCount) draft.masterSlot = 0;
			if (draft.cameraSlot >= targetSlotCount) draft.cameraSlot = 0;
		}
		const bool playing = snapshot.active && ctx.player && ctx.player->state == PlaybackState::Playing;
		ImGui::TextWrapped("为摄影场景中的角色分别指定动作，共享播放进度、音频与镜头。未绑定的槽位不会参与播放；预设载入后请重新绑定角色并应用。");
		if (!snapshot.notice.empty()) ImGui::TextWrapped("%s", snapshot.notice.c_str());
		if (!snapshot.available) ImGui::TextDisabled("进入有效摄影场景后可应用；现在可编辑和保存预设。");
		else if (!snapshot.verified) ImGui::TextDisabled("尚未完成摄影槽位验证；请开启探针确认后再应用。");
		bool probe = snapshot.probeEnabled;
		ImGui::BeginDisabled(!ctx.fnEnsembleProbe);
		if (ImGui::Checkbox("摄影槽位验证探针", &probe)) ctx.fnEnsembleProbe(ctx.self, probe);
		ImGui::EndDisabled();
		if (playing)
		{
			ImGui::BeginDisabled(!ctx.fnPause);
			if (ImGui::Button("暂停并配置舞台")) ctx.fnPause(ctx.self);
			ImGui::EndDisabled();
		}
		ImGui::BeginDisabled(playing);
		bool changed = ImGui::Checkbox("启用多人舞台", &draft.enabled);
		std::vector<std::string> names;
		names.reserve(draft.slots.size());
		for (size_t i = 0; i < draft.slots.size(); ++i) names.push_back("演员" + std::to_string(i + 1));
		std::vector<const char*> slotNames;
		slotNames.reserve(names.size());
		for (const auto& name : names) slotNames.push_back(name.c_str());
		int master = static_cast<int>(draft.masterSlot), follow = static_cast<int>(draft.cameraSlot);
		if (ImGui::Combo("音频与运镜主槽", &master, slotNames.data(), static_cast<int>(slotNames.size())))
		{ draft.masterSlot = static_cast<uint32_t>(master); changed = true; }
		if (ImGui::Combo("镜头跟随目标槽", &follow, slotNames.data(), static_cast<int>(slotNames.size())))
		{ draft.cameraSlot = static_cast<uint32_t>(follow); changed = true; }
		for (size_t i = 0; i < draft.slots.size(); ++i)
		{
			ImGui::PushID(static_cast<int>(i));
			auto& slot = draft.slots[i];
			if (ImGui::TreeNodeEx(slotNames[i], ImGuiTreeNodeFlags_DefaultOpen))
			{
				changed |= ImGui::Checkbox("使用此槽", &slot.enabled);
				std::string actorLabel = slot.actorHint.empty() ? "请选择当前场景角色" : slot.actorHint + "（未绑定）";
				bool bound = false;
				for (const auto& actor : ctx.sceneActors)
					if (actor.token == slot.actorToken) { actorLabel = actor.label; bound = true; }
				if (ImGui::BeginCombo("角色", actorLabel.c_str()))
				{
					if (ImGui::Selectable("不绑定角色", slot.actorToken == 0))
					{ slot.actorToken = 0; changed = true; }
					for (const auto& actor : ctx.sceneActors)
					{
						bool occupied = false;
						for (size_t other = 0; other < draft.slots.size(); ++other)
							if (other != i && draft.slots[other].actorToken == actor.token) occupied = true;
						ImGui::BeginDisabled(occupied);
						const std::string label = actor.label + "##ensemble_actor_" + std::to_string(actor.token);
						if (ImGui::Selectable(label.c_str(), actor.token == slot.actorToken))
						{ slot.actorToken = actor.token; slot.actorHint = actor.label; changed = true; }
						ImGui::EndDisabled();
					}
					ImGui::EndCombo();
				}
				if (slot.enabled && !bound) ImGui::TextDisabled("此槽尚未绑定当前摄影角色。");
				std::string motionLabel = slot.motionKey.empty() ? "请选择曲库动作" : slot.motionKey + "（未找到）";
				if (ctx.motions)
					for (const auto& motion : *ctx.motions) if (motion.name == slot.motionKey) motionLabel = motion.DisplayLabel();
				if (ImGui::BeginCombo("动作", motionLabel.c_str()))
				{
					if (ctx.motions) for (const auto& motion : *ctx.motions)
					{
						const auto label = motion.DisplayLabel() + "##ensemble_motion_" + motion.name;
						if (ImGui::Selectable(label.c_str(), slot.motionKey == motion.name))
						{ slot.motionKey = motion.name; changed = true; }
					}
					ImGui::EndCombo();
				}
				float offset[]{slot.offset.x, slot.offset.y, slot.offset.z};
				if (ImGui::DragFloat3("站位偏移（米）", offset, .01f, -256, 256, "%.2f", ImGuiSliderFlags_AlwaysClamp))
				{ slot.offset = {offset[0], offset[1], offset[2]}; changed = true; }
				changed |= ImGui::DragFloat("朝向（度）", &slot.yawDegrees, 1, -180, 180, "%.1f");
				if (i < snapshot.physics.size() && !snapshot.physics[i].empty()) ImGui::TextWrapped("物理：%s", snapshot.physics[i].c_str());
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		const bool masterHasMotion = draft.masterSlot < draft.slots.size() && !draft.slots[draft.masterSlot].motionKey.empty();
		ImGui::BeginDisabled(!masterHasMotion);
		if (ImGui::Button("复制主槽动作到全部槽"))
		{
			const auto motionKey = draft.slots[draft.masterSlot].motionKey;
			for (auto& slot : draft.slots) slot.motionKey = motionKey;
			changed = true;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("重置各槽站位"))
		{
			for (auto& slot : draft.slots) { slot.offset = {}; slot.yawDegrees = 0; }
			changed = true;
		}
		if (changed) overlay.ensembleDraftDirty = true;
		auto activeDraft = draft;
		for (auto& slot : activeDraft.slots)
			if (!slot.actorToken || slot.motionKey.empty()) slot.enabled = false;
		std::string error;
		bool valid = ensemble::Validate(activeDraft, error);
		if (valid && activeDraft.enabled)
		{
			if (!activeDraft.slots[activeDraft.masterSlot].enabled || !activeDraft.slots[activeDraft.cameraSlot].enabled)
			{ valid = false; error = "主槽与镜头跟随槽必须启用。"; }
			for (const auto& slot : activeDraft.slots) if (slot.enabled)
			{
				bool found = false;
				for (const auto& actor : ctx.sceneActors) if (actor.token == slot.actorToken) found = true;
				if (!found) { valid = false; error = "请为每个启用槽绑定当前摄影角色。"; break; }
			}
		}
		if (!valid) ImGui::TextWrapped("%s", error.c_str());
		const bool ready = !activeDraft.enabled || (snapshot.available && snapshot.verified);
		ImGui::BeginDisabled(!ctx.fnSetEnsemble || !valid || !ready);
		if (ImGui::Button("应用多人舞台配置"))
		{
			ctx.fnSetEnsemble(ctx.self, activeDraft, overlay.ensembleDraftRevision, overlay.ensembleDraftEpoch);
			overlay.ensembleDraftDirty = false;
		}
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!ctx.fnSetEnsemble || !snapshot.active);
		if (ImGui::Button("停用多人舞台"))
		{
			auto next = snapshot.settings;
			next.enabled = false;
			ctx.fnSetEnsemble(ctx.self, next, snapshot.revision, snapshot.actorEpoch);
		}
		ImGui::EndDisabled();
		ImGui::InputText("舞台预设名称", overlay.ensemblePresetName, sizeof(overlay.ensemblePresetName));
		std::string presetError;
		const bool canSave = ensemble::Validate(draft, presetError);
		ImGui::BeginDisabled(!ctx.fnEnsemblePreset || !overlay.ensemblePresetName[0] || !canSave);
		if (ImGui::Button("保存舞台预设")) ctx.fnEnsemblePreset(ctx.self, true, overlay.ensemblePresetName, draft);
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!ctx.fnEnsemblePreset || !overlay.ensemblePresetName[0] || playing);
		if (ImGui::Button("载入舞台预设")) ctx.fnEnsemblePreset(ctx.self, false, overlay.ensemblePresetName, draft);
		ImGui::EndDisabled();
		ImGui::TextDisabled("预设保存动作与布局；角色提示不自动绑定场景角色。");
		ImGui::PopID();
	}

	inline void DrawMotionPage(GuiOverlay& overlay, const GuiContext& ctx)
	{
		if (!ctx.player)
		{
			ImGui::TextDisabled("播放器尚未就绪");
			return;
		}

		MotionPlayer& player = *ctx.player;
		IdolController& controller = player.controller;
		controller.useRestRotations = true; // 静息姿态对齐自动保持启用
		DrawEnsembleControls(overlay, ctx);
		if (!ctx.ensembleSnapshot.active)
		{

		ImGui::TextUnformatted("驱动角色");
		const std::string targetLabel = ctx.sceneActors.empty() ? "当前场景没有可驱动角色" :
			(ctx.targetActorLabel.empty() ? "正在识别角色" : ctx.targetActorLabel);
		ImGui::SetNextItemWidth((std::min)(360.0f, ImGui::GetContentRegionAvail().x));
		ImGui::BeginDisabled(ctx.sceneActors.empty() || !ctx.fnSelectTargetActor);
		if (ImGui::BeginCombo("##drive_actor_combo", targetLabel.c_str()))
		{
			for (const auto& actor : ctx.sceneActors)
			{
				const std::string label = actor.label + "##actor_" + std::to_string(actor.token);
				if (ImGui::Selectable(label.c_str(), actor.selected) && ctx.fnSelectTargetActor)
					ctx.fnSelectTargetActor(ctx.self, actor.token, ctx.actorRosterEpoch);
				if (actor.selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();
		ImGui::TextWrapped("表情随驱动角色自动适配。");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextUnformatted("表情幅度");
		auto& face = controller.facialWeights;
		ImGui::SliderFloat("整体##face", &face.overall, 0.0f, 2.0f, "%.2f 倍");
		ImGui::SliderFloat("眉毛##face", &face.brow, 0.0f, 2.0f, "%.2f 倍");
		ImGui::SliderFloat("眼部##face", &face.eye, 0.0f, 2.0f, "%.2f 倍");
		ImGui::SliderFloat("嘴部##face", &face.mouth, 0.0f, 2.0f, "%.2f 倍");
		ImGui::TextWrapped("分项与整体倍率相乘；通常限制在 0–100%%。眼骨视线不受影响。");
		if (ImGui::Button("重置表情幅度")) face = FacialWeights{};
		ImGui::SameLine();
		if (ImGui::Button("保存表情幅度") && ctx.fnSaveFacialWeights) ctx.fnSaveFacialWeights(ctx.self);
		if (!ctx.facialWeightsNotice.empty()) ImGui::TextWrapped("%s", ctx.facialWeightsNotice.c_str());

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// =========================================================================
		// 3. 初始角色位置偏移
		// =========================================================================
		ImGui::TextUnformatted("初始角色位置偏移");

		if (!overlay.initialPositionEditing)
		{
			const auto offset = controller.initialPositionOffset;
			overlay.initialPositionEdit[0] = offset.x;
			overlay.initialPositionEdit[1] = offset.y;
			overlay.initialPositionEdit[2] = offset.z;
		}

		const float dragItemWidth = (std::max)(40.0f, (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f);
		bool posChanged = false;

		// 位置 X / Y / Z
		ImGui::TextUnformatted("世界坐标偏移 (米)");
		ImGui::SetNextItemWidth(dragItemWidth);
		if (ImGui::DragFloat("##pos_x", &overlay.initialPositionEdit[0], 0.01f, -256.0f, 256.0f, "X: %.2f", ImGuiSliderFlags_AlwaysClamp))
			posChanged = true;

		ImGui::SameLine();
		ImGui::SetNextItemWidth(dragItemWidth);
		if (ImGui::DragFloat("##pos_y", &overlay.initialPositionEdit[1], 0.01f, -256.0f, 256.0f, "Y: %.2f", ImGuiSliderFlags_AlwaysClamp))
			posChanged = true;

		ImGui::SameLine();
		ImGui::SetNextItemWidth(dragItemWidth);
		if (ImGui::DragFloat("##pos_z", &overlay.initialPositionEdit[2], 0.01f, -256.0f, 256.0f, "Z: %.2f", ImGuiSliderFlags_AlwaysClamp))
			posChanged = true;

		if (posChanged)
		{
			overlay.initialPositionEditing = true;
			if (ctx.fnSetInitialPosition)
			{
				ctx.fnSetInitialPosition(ctx.self, Vector3(overlay.initialPositionEdit[0],
				                                           overlay.initialPositionEdit[1],
				                                           overlay.initialPositionEdit[2]));
			}
			else
			{
				controller.SetInitialPositionOffset(Vector3(overlay.initialPositionEdit[0],
				                                            overlay.initialPositionEdit[1],
				                                            overlay.initialPositionEdit[2]));
			}
		}

		// 重置与保存按钮
		if (GuiIcons::Button("重置位置偏移", GuiIcons::Icon::Reset, ImVec2(120, 24)))
		{
			overlay.initialPositionEdit[0] = 0.0f;
			overlay.initialPositionEdit[1] = 0.0f;
			overlay.initialPositionEdit[2] = 0.0f;
			overlay.initialPositionEditing = false;
			if (ctx.fnSetInitialPosition)
				ctx.fnSetInitialPosition(ctx.self, Vector3(0.0f, 0.0f, 0.0f));
			else
				controller.SetInitialPositionOffset(Vector3(0.0f, 0.0f, 0.0f));
		}
		GuiIcons::SameLineIfRoom("保存初始位置为默认", ImVec2(150, 24));
		if (GuiIcons::Button("保存初始位置为默认", GuiIcons::Icon::Save, ImVec2(150, 24)))
		{
			if (ctx.fnSaveInitialPosition) ctx.fnSaveInitialPosition(ctx.self);
		}
		if (!ctx.initialPositionNotice.empty())
		{
			const bool failed = ctx.initialPositionNotice.find("失败") != std::string::npos ||
				ctx.initialPositionNotice.find("无效") != std::string::npos;
			if (failed) ImGui::PushStyleColor(ImGuiCol_Text, GuiTheme::ErrorColor());
			ImGui::TextWrapped("%s", ctx.initialPositionNotice.c_str());
			if (failed) ImGui::PopStyleColor();
		}
		ImGui::TextWrapped("修改立即作用于当前驱动对象，切歌保留，停止后恢复场景原位；保存后作为下次启动的默认偏移。");
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// =========================================================================
		// 4. 高级动作设置 (折叠) — 仅保留骨骼微调选单
		// =========================================================================
		if (ImGui::CollapsingHeader("高级动作设置"))
		{
			ImGui::Indent(10.0f);
			overlay.DrawBoneOverridesSection(ctx);
			ImGui::Unindent(10.0f);
		}
	}
} // namespace GakuMotion
#endif
