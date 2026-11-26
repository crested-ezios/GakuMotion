#pragma once

#include "GuiOverlayCommon.hpp"
#include "../GkGuiOverlay.hpp"
#include "../GkIdolController.hpp"

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	inline const char* MikuFitStatusText(MikuFitDecision decision)
	{
		switch (decision)
		{
		case MikuFitDecision::Applied: return "已应用";
		case MikuFitDecision::ExternalApplied: return "已应用（Rinha 发布配置）";
		case MikuFitDecision::ExternalUnverified: return "已回退：该配置未经实测验收";
		case MikuFitDecision::MissingRawSource: return "已回退：旧曲目缺原始五元音";
		case MikuFitDecision::MissingRawEyeSource: return "已回退：曲目缺原始眼眉源（需重新导入）";
		case MikuFitDecision::InvalidRawSource: return "已回退：源数据不完整";
		case MikuFitDecision::UnsupportedSource: return "已回退：源嘴形未覆盖";
		case MikuFitDecision::UnsupportedEyeSource: return "已回退：源眼眉未覆盖";
		case MikuFitDecision::ExternalSoftA: return "已应用：麟羽轻开口";
		case MikuFitDecision::NativeAuthored: return "已回退：保留显式原生通道";
		case MikuFitDecision::UnknownFace: return "已回退：实际脸型未匹配";
		case MikuFitDecision::CharacterMismatch: return "已回退：角色身份不一致";
		case MikuFitDecision::MissingOutput: return "已回退：目标通道不完整";
		case MikuFitDecision::OutsideDomain: return "已回退：源混合超出拟合范围";
		case MikuFitDecision::InvalidModel: return "已回退：拟合数据无效";
		case MikuFitDecision::Disabled: return "已关闭";
		}
		return "未知状态";
	}

	inline void DrawMotionExperiments(const GuiContext& ctx)
	{
		if (!ctx.player)
		{
			ImGui::TextDisabled("播放器尚未就绪");
			return;
		}
		const MotionPlayer& player = *ctx.player;
		const IdolController& controller = player.controller;
		bool mikuMouthFit = ctx.config && ctx.config->facialMikuMouthFit >= 0
			? ctx.config->facialMikuMouthFit != 0 : controller.facialMikuMouthFit;
		bool mikuEyeFit = ctx.config && ctx.config->facialMikuEyeFit >= 0
			? ctx.config->facialMikuEyeFit != 0 : controller.facialMikuEyeFit;
		bool allowUnverified = ctx.config && ctx.config->facialMikuFitAllowUnverified >= 0
			? ctx.config->facialMikuFitAllowUnverified != 0 : controller.facialMikuFitAllowUnverified;
		ImGui::BeginDisabled(!ctx.config);
		if (ImGui::Checkbox("实验：Miku 嘴形拟合", &mikuMouthFit) && ctx.config)
			ctx.config->facialMikuMouthFit = mikuMouthFit ? 1 : 0;
		if (ImGui::Checkbox("实验：Miku 眼眉拟合", &mikuEyeFit) && ctx.config)
			ctx.config->facialMikuEyeFit = mikuEyeFit ? 1 : 0;
		if (ImGui::Checkbox("允许未经实测验收的拟合分量", &allowUnverified) && ctx.config)
			ctx.config->facialMikuFitAllowUnverified = allowUnverified ? 1 : 0;
		ImGui::EndDisabled();
		ImGui::TextWrapped("嘴形：默认启用，自动识别千奈、麻央、莉波、燕以及 Rinha 发布的 krnh 配置。眼眉：仅当曲目由新版导入器重新导出、带上原始眼眉源时生效；旧曲目自动沿用原适配。krnh 的嘴形映射留出验证为 0/28，只有打开上面的「允许未验收分量」才会使用。切换会重新绑定表情，尚未完成实机视觉验收。");

		const unsigned fittedSnapshot = controller.guiMikuFitSnapshot.load(std::memory_order_relaxed);
		const unsigned fittedRole = (fittedSnapshot >> 8) & 0xff;
		const auto fittedDecision = static_cast<MikuFitDecision>(fittedSnapshot & 0xff);
		const auto eyeDecision = static_cast<MikuFitDecision>((fittedSnapshot >> 16) & 0xff);
		const bool playing = player.state != PlaybackState::Stopped;
		ImGui::TextWrapped("嘴形拟合状态：%s",
			!mikuMouthFit ? "已关闭" : playing ? MikuFitStatusText(fittedDecision) : "尚未接管");
		ImGui::TextWrapped("眼眉拟合状态：%s",
			!mikuEyeFit ? "已关闭" : playing ? MikuFitStatusText(eyeDecision) : "尚未接管");
		if (fittedRole > 0 && fittedRole <= kMikuMouthFits.size())
			ImGui::TextWrapped("最近识别脸型：%s（base-0000）",
				FormatIdolDisplayName(kMikuMouthFits[fittedRole - 1].character).c_str());
		else if (fittedRole == kMikuMouthFits.size() + 1)
			ImGui::TextWrapped("最近识别脸型：发布配置（RinhaReplacement）");
	}
} // namespace GakuMotion
#endif
