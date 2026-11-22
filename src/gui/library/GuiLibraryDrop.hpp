#pragma once

#include "GuiLibraryHelpers.hpp"

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	inline bool CanAcceptLibraryDrop(const GuiContext& ctx)
	{
		return !ctx.importBusy && !ctx.libraryRefreshing && !g_asyncFileDialog.IsBusy() &&
			!g_libraryModal.isOpen && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
	}

	// 拖放视觉的有效性判定必须和真正导入时的过滤同源，否则会出现"亮起却导不进去"
	// 或"警告却成功"的矛盾外观。
	inline bool IsSupportedLibraryFile(const std::string& file)
	{
		const std::filesystem::path path(Utf8ToWide(file));
		std::error_code error;
		std::string ext = WideToUtf8(path.extension().wstring());
		for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return std::filesystem::is_regular_file(path, error) &&
			(ext == ".vmd" || ext == ".gkcamera" || ext == ".wav" || ext == ".mp3" ||
			 ext == ".ogg" || ext == ".flac" || ext == ".m4a");
	}

	// 拖放目标区域的视觉状态：Idle 保持原样，其余三态由宿主提供的悬停信息驱动。
	enum class DropVisual { Idle, Armed, Hover, Invalid };
	struct DropVisualState
	{
		DropVisual visual = DropVisual::Idle;
		uint32_t fileCount = 0;
		size_t accepted = 0;
		// True while the host itself cannot observe shell drags (it runs above
		// Explorer's integrity level, so UIPI drops every OLE call). The preview
		// stays dark and the drop still works, so the page has to say why.
		bool hostBlocked = false;
		// True when elevation is the reason. Without it the host simply has no
		// hover transport, and claiming "administrator" would be a lie.
		bool hostElevated = false;
	};

	// 宿主只在本窗口被拖动时提供悬停状态；旧宿主没有该服务，返回值恒为 Idle，
	// 面板回到改动前的静态提示。文件类型校验按拖拽批次（generation）缓存，
	// 同一批次内不重复访问磁盘。
	inline const DropVisualState& QueryDropVisual()
	{
		static DropVisualState result;
		static uint32_t trackedGeneration = 0, trackedCount = 0;
		static size_t trackedAccepted = 0;
		result = DropVisualState{};
		const auto& service = host::fileDrag;
		if (!service.peek) return result;
		GakuFileDragState state{};
		if (service.peek(service.context, sizeof(state), &state, 0, nullptr) != GAKU_OK) return result;
		// Host capability, not drag state: report it before the drag-only guards
		// below so the hint can be shown when nothing is being dragged.
		result.hostBlocked = (state.flags & GAKU_FILE_DRAG_HOST_BLOCKED) != 0;
		result.hostElevated = (state.flags & GAKU_FILE_DRAG_HOST_ELEVATED) != 0;
		if (!(state.flags & GAKU_FILE_DRAG_ACTIVE) || !state.fileCount) return result;
		result.fileCount = state.fileCount;
		if (state.generation != trackedGeneration || state.fileCount != trackedCount)
		{
			trackedGeneration = state.generation;
			trackedCount = state.fileCount;
			trackedAccepted = 0;
			if (state.requiredBytes && state.requiredBytes <= GAKU_FILE_DROP_MAX_BYTES)
			{
				std::vector<char> packed(state.requiredBytes);
				GakuFileDragState full{};
				if (service.peek(service.context, sizeof(full), &full,
						static_cast<uint32_t>(packed.size()), packed.data()) == GAKU_OK &&
					full.requiredBytes <= packed.size())
				{
					size_t offset = 0;
					for (uint32_t i = 0; i < full.fileCount; ++i)
					{
						if (offset >= full.requiredBytes) break;
						const auto end = std::find(packed.begin() + offset,
							packed.begin() + full.requiredBytes, '\0');
						if (end == packed.begin() + full.requiredBytes || end == packed.begin() + offset) break;
						if (IsSupportedLibraryFile(std::string(packed.data() + offset,
								static_cast<size_t>(end - (packed.begin() + offset)))))
							++trackedAccepted;
						offset = static_cast<size_t>(end - packed.begin()) + 1;
					}
				}
			}
		}
		result.accepted = trackedAccepted;
		// 无效文件优先于位置：拖进来的东西根本收不了，边框先变警告色更有用。
		if (trackedAccepted == 0) result.visual = DropVisual::Invalid;
		else result.visual = (state.flags & GAKU_FILE_DRAG_INSIDE_TARGET) ? DropVisual::Hover : DropVisual::Armed;
		return result;
	}

	// 曲库用整块宿主客户区接收文件：命中不再要求精确落在表格矩形内，文件丢到
	// 窗口任何位置都应导入。ImGui 主视口就是宿主客户区，也正是 GakuFileDropTarget
	// 的坐标空间，因此矩形每帧直接可得，不必跨帧缓存——也就没有"上一帧矩形"与
	// "本帧实际投放"错位的机会。
	inline void ClientDropRect(ImVec2& minimum, ImVec2& maximum)
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		minimum = viewport->Pos;
		maximum = ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y);
	}

	// 目标每个完成帧失效，因此每帧都要重新登记；未就绪时登记 NULL 让宿主清空目标，
	// 这样非曲库页、模态打开或导入进行中都不会留下可投放的窗口。
	inline void RegisterDropTarget(const GuiContext& ctx, ImVec2 minimum, ImVec2 maximum)
	{
		const auto& service = host::fileDrop;
		if (!service.setTarget || !service.poll) return;
		const bool ready = CanAcceptLibraryDrop(ctx) && minimum.x < maximum.x && minimum.y < maximum.y;
		const GakuFileDropTarget target{GAKU_FILE_DROP_VERSION, sizeof(GakuFileDropTarget),
			minimum.x, minimum.y, maximum.x, maximum.y};
		// [GakuMotion][FileDrop] Runs every UI frame: emit only on a registered
		// <-> unregistered flip; the static remembers the last state so repeated
		// setTarget calls never flood the host log. Owner/rect aid Loader correlation.
		static bool s_dropTargetActive = false;
		const auto targetStatus = service.setTarget(service.context, ready ? &target : nullptr);
		if (targetStatus == GAKU_OK && ready != s_dropTargetActive)
		{
			s_dropTargetActive = ready;
			host::Info("[GakuMotion][FileDrop] owner=LibraryPage target=%s rect=(%.0f,%.0f,%.0f,%.0f)",
				ready ? "enabled" : "disabled", target.left, target.top, target.right, target.bottom);
		}
	}

	// 三段视觉：浅灰=检测到拖拽、亮色+淡填充=进入可投放区、警告色=文件无效。
	// 命中区已放宽到整块客户区，这里仍画在表格上：它是"曲库会接收"的提示，
	// 不是可投放边界，因此不需要跟着命中区一起放大。
	inline void DrawLibraryDropVisual(ImVec2 minimum, ImVec2 maximum, const DropVisualState& state)
	{
		if (state.visual == DropVisual::Idle) return;
		if (minimum.x >= maximum.x || minimum.y >= maximum.y) return;
		ImDrawList* draw = ImGui::GetWindowDrawList();
		if (!draw) return;
		const bool light = GuiTheme::IsLight();
		const float rounding = ImGui::GetStyle().FrameRounding > 0.0f ? ImGui::GetStyle().FrameRounding : 4.0f;
		ImU32 border = 0, fill = 0;
		float thickness = 1.5f;
		switch (state.visual)
		{
		case DropVisual::Armed:
			border = ImGui::GetColorU32(light ? ImVec4(0.55f, 0.56f, 0.58f, 0.90f)
			                                  : ImVec4(0.72f, 0.74f, 0.78f, 0.85f));
			break;
		case DropVisual::Hover:
		{
			const ImVec4 accent = GuiTheme::InfoColor();
			border = ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 1.0f));
			fill = ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, light ? 0.07f : 0.12f));
			thickness = 2.0f;
			break;
		}
		case DropVisual::Invalid:
		{
			const ImVec4 warning = GuiTheme::WarningColor();
			border = ImGui::GetColorU32(ImVec4(warning.x, warning.y, warning.z, 1.0f));
			fill = ImGui::GetColorU32(ImVec4(warning.x, warning.y, warning.z, light ? 0.08f : 0.12f));
			thickness = 2.0f;
			break;
		}
		default:
			return;
		}
		if (fill) draw->AddRectFilled(minimum, maximum, fill, rounding);
		draw->AddRect(minimum, maximum, border, rounding, 0, thickness);
	}

	inline void OpenLibraryDrop(GuiOverlay& overlay, const std::vector<std::string>& files)
	{
		std::vector<std::string> accepted;
		size_t ignored = 0;
		for (const auto& file : files)
		{
			if (!IsSupportedLibraryFile(file)) { ++ignored; continue; }
			if (std::find(accepted.begin(), accepted.end(), file) == accepted.end()) accepted.push_back(file);
		}
		// [GakuMotion][FileDrop] One event-level summary for filtered-out entries
		// (unsupported extension / directory / is_regular_file failure); never list paths.
		if (ignored > 0)
			host::Info("[GakuMotion][FileDrop] filter ignored=%zu", ignored);
		if (accepted.empty())
		{
			// 静默拒绝会让用户以为文件没投递成功，从而反复拖拽重试。因此保留一行
			// 页面内非模态说明，仍然不弹任何窗口，符合"拖放后不出警告框"的约定。
			host::Warn("[GakuMotion][FileDrop] drop rejected reason=no-accepted-files ignored=%zu notice=inline",
				ignored);
			overlay.libraryUiNotice = "未找到可添加的文件。请拖入 VMD、GKCamera 或 WAV 文件；不支持拖入文件夹。";
			return;
		}
		g_libraryModal.SetupForNew(accepted);
		overlay.libraryUiNotice = ignored ? "已忽略 " + std::to_string(ignored) + " 个不支持或无法读取的文件。" : "";
	}

	inline void ReceiveLibraryDrop(GuiOverlay& overlay, const GuiContext& ctx, ImVec2 minimum, ImVec2 maximum)
	{
		const auto& service = host::fileDrop;
		if (!service.setTarget || !service.poll) return;
		RegisterDropTarget(ctx, minimum, maximum);
		const bool ready = CanAcceptLibraryDrop(ctx) && minimum.x < maximum.x && minimum.y < maximum.y;
		if (!ready) return;
		GakuFileDropResult result{};
		const auto peekStatus = service.poll(service.context, sizeof(result), &result, 0, nullptr);
		if (peekStatus == GAKU_OK && result.fileCount > 0)
			host::Info("[GakuMotion][FileDrop] peek fileCount=%u requiredBytes=%u",
				result.fileCount, result.requiredBytes);
		if (peekStatus != GAKU_OK || !result.fileCount ||
			result.fileCount > GAKU_FILE_DROP_MAX_FILES || result.requiredBytes > GAKU_FILE_DROP_MAX_BYTES || !result.requiredBytes) return;
		std::vector<char> packed(result.requiredBytes);
		const auto consumeStatus = service.poll(service.context, sizeof(result), &result,
			static_cast<uint32_t>(packed.size()), packed.data());
		if (consumeStatus != GAKU_OK)
		{
			host::Warn("[GakuMotion][FileDrop] consume failed reason=second-poll status=%d", static_cast<int>(consumeStatus));
			return;
		}
		if (result.requiredBytes > packed.size() || result.fileCount > GAKU_FILE_DROP_MAX_FILES) return;
		std::vector<std::string> files;
		size_t offset = 0;
		for (uint32_t i = 0; i < result.fileCount; ++i)
		{
			const auto end = std::find(packed.begin() + offset, packed.begin() + result.requiredBytes, '\0');
			if (end == packed.begin() + result.requiredBytes || end == packed.begin() + offset)
			{
				host::Warn("[GakuMotion][FileDrop] parse failed reason=empty-or-unterminated-path index=%u offset=%zu requiredBytes=%u",
					i, offset, result.requiredBytes);
				return;
			}
			files.emplace_back(packed.data() + offset, static_cast<size_t>(end - (packed.begin() + offset)));
			offset = static_cast<size_t>(end - packed.begin()) + 1;
		}
		if (offset == result.requiredBytes) OpenLibraryDrop(overlay, files);
		else host::Warn("[GakuMotion][FileDrop] parse failed reason=offset-mismatch offset=%zu requiredBytes=%u",
			offset, result.requiredBytes);
		if (g_libraryModal.isOpen) service.setTarget(service.context, nullptr);
	}

	inline void SyncLibraryEntriesFromMotions(GuiOverlay& overlay, const GuiContext& ctx)
	{
		if (!ctx.motions) return;
		if (overlay.librarySyncedRevision == ctx.libraryRevision) return;
		if (overlay.selectedLibraryKey.empty() && overlay.selectedLibraryIndex >= 0 &&
			overlay.selectedLibraryIndex < static_cast<int>(overlay.libraryEntries.size()))
			overlay.selectedLibraryKey = overlay.libraryEntries[overlay.selectedLibraryIndex].id;

		overlay.libraryEntries.clear();
		for (const auto& m : *ctx.motions)
		{
			LibraryEntry e;
			e.id = m.name;
			e.title = m.title.empty() ? m.name : m.title;
			e.durationSeconds = m.durationSeconds;
			e.totalFrames = m.totalFrames;
			e.boneCount = m.boneCount;
			e.hasAudio = m.hasAudio;
			e.hasOverrides = m.hasOverrides;
			e.timing = m.timing;

			e.motionFile = !m.motionFile.empty() ? m.motionFile : (!m.dir.empty() ? m.dir : m.name) + "/motion.gkclip";

			if (m.hasAudio)
			{
				const std::string audioName = !m.audioFile.empty() ? m.audioFile : "song.wav";
				e.audioFile = (!m.dir.empty() ? m.dir : m.name) + "/" + audioName;
			}

			e.hasCamera = m.hasCamera;
			e.cameraFile = e.hasCamera ? ((!m.dir.empty() ? m.dir : m.name) + "/camera.gkcamera") : "";

			e.hasFacial = m.hasFacial;
			if (e.hasFacial)
				e.facialFile = e.motionFile;

			overlay.libraryEntries.push_back(e);
		}
		overlay.selectedLibraryIndex = FindMotionKeyIndex(*ctx.motions, overlay.selectedLibraryKey);
		overlay.librarySyncedRevision = ctx.libraryRevision;
	}
} // namespace GakuMotion
#endif
