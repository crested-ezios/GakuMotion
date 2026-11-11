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

#ifdef GKMOTION_GUI_ACTIVE
namespace GakuMotion
{
	// 导入弹窗下拉框里的占位条目。它们是哨兵值而不是磁盘路径，所有"是否选中了
	// 真实文件"的判断都必须把它们排除掉。集中在一处，避免各分支各写一份字符串
	// 比较（漏一处就会把"未指定"当成"显式关闭"）。
	namespace ImportSlot
	{
		inline constexpr const char* kEmpty = "（留空）";
		inline constexpr const char* kClear = "（留空 / 清除）";
		inline constexpr const char* kKeepTrack = "[保持当前已导入轨道]";
		inline constexpr const char* kKeepMotion = "[保持当前动作]";
		// 新增导入时的"自动"：表情沿用动作 VMD 内自带的表情，音乐按同名 WAV /
		// song.wav 自动发现。两者都对应 Python CLI 的默认行为（不传对应开关）。
		inline constexpr const char* kAutoFacial = "（自动：沿用动作 VMD 内表情）";
		inline constexpr const char* kAutoAudio = "（自动：同名 WAV 或 song.wav）";
		// 显式关闭：与"自动"区分开，传 --no-morph / --clear-audio。
		inline constexpr const char* kSkip = "（不导入）";
	}

	inline bool IsSlotPlaceholder(const std::string& path)
	{
		return path.empty() ||
			path == ImportSlot::kEmpty || path == ImportSlot::kClear ||
			path == ImportSlot::kKeepTrack || path == ImportSlot::kKeepMotion ||
			path == ImportSlot::kAutoFacial || path == ImportSlot::kAutoAudio ||
			path == ImportSlot::kSkip;
	}

	// 旋转圆弧加载动画 (Spinner) — 纯公开 ImGui API，无私有头文件依赖
	inline void DrawSpinner(const char* label, float radius, float thickness, const ImVec4& color)
	{
		(void)label;
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float size = radius * 2.0f;
		ImGui::Dummy(ImVec2(size, size));

		const float time = static_cast<float>(ImGui::GetTime());
		const int num_segments = 30;
		const float start = std::fmod(time * 4.0f, 3.14159265f * 2.0f);
		const float a_min = start;
		const float a_max = start + 3.14159265f * 1.4f;

		const ImVec2 centre(pos.x + radius, pos.y + radius);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->PathClear();
		for (int i = 0; i <= num_segments; ++i)
		{
			const float a = a_min + (static_cast<float>(i) / static_cast<float>(num_segments)) * (a_max - a_min);
			drawList->PathLineTo(ImVec2(centre.x + std::cos(a) * radius, centre.y + std::sin(a) * radius));
		}
		drawList->PathStroke(ImGui::GetColorU32(color), 0, thickness);
	}

	// 快速检测已导入目录是否包含表情轨道
	inline bool DetectMotionHasFacial(const std::string& dir)
	{
		// 1. 尝试从 info.json 中读取 "has_facial" 字段
		std::string err;
		jsonlite::Value root;
		if (jsonlite::ParseFile(dir + "/info.json", root, err))
		{
			if (const auto* v = root.Find("has_facial"))
			{
				return v->AsBool(false);
			}
		}

		// 2. 检查是否有 motion.morph_report.json 伴随文件
		std::error_code ec;
		if (std::filesystem::exists(std::filesystem::path(Utf8ToWide(dir)) / "motion.morph_report.json", ec))
		{
			return true;
		}

		// 3. 探查 motion.gkclip 头部（若版本 >= 4 且包含 morph 权重通道）
		const auto clipPath = std::filesystem::path(Utf8ToWide(dir)) / "motion.gkclip";
		if (std::filesystem::exists(clipPath, ec))
		{
			std::ifstream f(clipPath, std::ios::binary);
			if (f.is_open())
			{
				char magic[8]{};
				f.read(magic, 8);
				if (f.gcount() == 8 &&
					(std::memcmp(magic, "GKCLIPCL", 8) == 0 || std::memcmp(magic, "GKCLIP\0\0", 8) == 0))
				{
					uint32_t version = 0, total_frames = 0, bone_count = 0;
					float fps = 0.0f, duration_seconds = 0.0f, root_scale = 0.0f;
					f.read(reinterpret_cast<char*>(&version), 4);
					f.read(reinterpret_cast<char*>(&fps), 4);
					f.read(reinterpret_cast<char*>(&total_frames), 4);
					f.read(reinterpret_cast<char*>(&duration_seconds), 4);
					f.read(reinterpret_cast<char*>(&bone_count), 4);
					f.read(reinterpret_cast<char*>(&root_scale), 4);
					if (f && version >= 4 && bone_count <= 256 && total_frames > 0 && total_frames <= 1000000)
					{
						bool ok = true;
						for (uint32_t b = 0; b < bone_count && ok; ++b)
						{
							uint16_t hid = 0;
							uint8_t nlen = 0, hasPos = 0;
							f.read(reinterpret_cast<char*>(&hid), 2);
							f.read(reinterpret_cast<char*>(&nlen), 1);
							f.read(reinterpret_cast<char*>(&hasPos), 1);
							if (!f || !nlen) { ok = false; break; }
							f.seekg(nlen, std::ios::cur);
						}
						if (ok)
						{
							const uint64_t rotBytes = static_cast<uint64_t>(total_frames) * (sizeof(Vector3) + sizeof(Quaternion) * bone_count);
							f.seekg(rotBytes, std::ios::cur);
							uint8_t hasCam = 0;
							f.read(reinterpret_cast<char*>(&hasCam), 1);
							if (f && hasCam)
							{
								f.seekg(static_cast<uint64_t>(total_frames) * 28ull, std::ios::cur);
							}
							if (f)
							{
								uint32_t morphCount = 0;
								f.read(reinterpret_cast<char*>(&morphCount), 4);
								if (f && morphCount > 0)
								{
									return true;
								}
							}
						}
					}
				}
			}
		}
		return false;
	}

	// 模态弹窗表单状态（严格按文件类型内部拦截，支持缓存探针）
	struct LibraryModalState
	{
		bool isOpen{false};
		bool isEditMode{false}; // false = 添加, true = 管理修改已有
		std::string editTargetKey;

		char titleInput[256]{};
		PerformanceTiming timing;
		std::vector<std::string> motionCandidates;
		std::vector<std::string> facialCandidates;
		std::vector<std::string> cameraCandidates;
		std::vector<std::string> audioCandidates;
		std::vector<std::string> vmdCandidates; // 保持兼容

		int motionSelection{0};
		int facialSelection{0};
		int cameraSelection{0};
		int audioSelection{0};
		// 本次挑文件中被忽略的非 WAV 音频数量（导入链只接受 PCM WAV）。
		int unsupportedAudio{0};

		std::unordered_map<std::string, VmdProbeInfo> probeCache;

		const VmdProbeInfo& GetOrProbe(const std::string& path)
		{
			static const VmdProbeInfo emptyInfo{};
			if (IsSlotPlaceholder(path))
			{
				return emptyInfo;
			}
			auto it = probeCache.find(path);
			if (it != probeCache.end()) return it->second;
			VmdProbeInfo info = ProbeCameraOrVmdFile(path);
			probeCache[path] = info;
			return probeCache[path];
		}

		void Reset()
		{
			isOpen = false;
			isEditMode = false;
			editTargetKey.clear();
			timing = {};
			titleInput[0] = '\0';
			motionCandidates.clear();
			facialCandidates.clear();
			cameraCandidates.clear();
			audioCandidates.clear();
			vmdCandidates.clear();
			motionSelection = 0;
			facialSelection = 0;
			cameraSelection = 0;
			audioSelection = 0;
			unsupportedAudio = 0;
			probeCache.clear();
		}

		void AppendFiles(const std::vector<std::string>& files)
		{
			auto AddUnique = [](std::vector<std::string>& list, const std::string& item)
			{
				if (std::find(list.begin(), list.end(), item) == list.end())
					list.push_back(item);
			};

			for (const auto& f : files)
			{
				std::filesystem::path p(Utf8ToWide(f));
				std::string ext = p.extension().string();
				for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

				if (ext == ".vmd")
				{
					GetOrProbe(f);
					AddUnique(motionCandidates, f);
					AddUnique(facialCandidates, f);
					AddUnique(cameraCandidates, f);
					AddUnique(vmdCandidates, f);
				}
				else if (ext == ".gkcamera")
				{
					GetOrProbe(f);
					AddUnique(cameraCandidates, f);
				}
				else if (ext == ".wav")
				{
					// 只有 PCM WAV 能被导入；把 mp3/ogg/flac/m4a 列进来只会让用户
					// 选到一个必然被拒绝的文件。
					AddUnique(audioCandidates, f);
				}
			}
		}

		void SelectSingleMotion(const std::string& path)
		{
			GetOrProbe(path);
			auto it = std::find(motionCandidates.begin(), motionCandidates.end(), path);
			if (it == motionCandidates.end())
			{
				motionCandidates.push_back(path);
				motionSelection = static_cast<int>(motionCandidates.size()) - 1;
			}
			else
			{
				motionSelection = static_cast<int>(it - motionCandidates.begin());
			}
		}

		void SelectSingleFacial(const std::string& path)
		{
			GetOrProbe(path);
			auto it = std::find(facialCandidates.begin(), facialCandidates.end(), path);
			if (it == facialCandidates.end())
			{
				facialCandidates.push_back(path);
				facialSelection = static_cast<int>(facialCandidates.size()) - 1;
			}
			else
			{
				facialSelection = static_cast<int>(it - facialCandidates.begin());
			}
		}

		void SelectSingleCamera(const std::string& path)
		{
			GetOrProbe(path);
			auto it = std::find(cameraCandidates.begin(), cameraCandidates.end(), path);
			if (it == cameraCandidates.end())
			{
				cameraCandidates.push_back(path);
				cameraSelection = static_cast<int>(cameraCandidates.size()) - 1;
			}
			else
			{
				cameraSelection = static_cast<int>(it - cameraCandidates.begin());
			}
		}

		void SelectSingleAudio(const std::string& path)
		{
			auto it = std::find(audioCandidates.begin(), audioCandidates.end(), path);
			if (it == audioCandidates.end())
			{
				audioCandidates.push_back(path);
				audioSelection = static_cast<int>(audioCandidates.size()) - 1;
			}
			else
			{
				audioSelection = static_cast<int>(it - audioCandidates.begin());
			}
		}

		void SetupForNew(const std::vector<std::string>& files)
		{
			Reset();
			isOpen = true;
			isEditMode = false;

			const auto classification = ClassifyImportFiles(files);
			std::snprintf(titleInput, sizeof(titleInput), "%s", classification.suggestedTitle.c_str());

			// 新增导入：0 号项默认是"自动"，与 Python CLI 的默认行为一致（表情取
			// 动作 VMD 自带，音乐按同名 WAV / song.wav 发现）。显式关闭另设一个
			// 尾部条目，避免"没选"被翻译成"关掉"。
			motionCandidates.push_back(ImportSlot::kEmpty);
			facialCandidates.push_back(ImportSlot::kAutoFacial);
			cameraCandidates.push_back(ImportSlot::kEmpty);
			audioCandidates.push_back(ImportSlot::kAutoAudio);
			vmdCandidates.push_back(ImportSlot::kEmpty);

			AppendFiles(files);

			facialCandidates.push_back(ImportSlot::kSkip);
			audioCandidates.push_back(ImportSlot::kSkip);

			unsupportedAudio = classification.unsupportedAudioCount;

			auto FindIndex = [](const std::vector<std::string>& list, const std::string& path) -> int
			{
				if (path.empty()) return 0;
				for (size_t i = 1; i < list.size(); ++i)
				{
					if (list[i] == path) return static_cast<int>(i);
				}
				return 0;
			};

			motionSelection = FindIndex(motionCandidates, classification.motionCandidate);
			facialSelection = FindIndex(facialCandidates, classification.facialCandidate);
			cameraSelection = FindIndex(cameraCandidates, classification.cameraCandidate);
			audioSelection = FindIndex(audioCandidates, classification.audioCandidate);
		}

		void SetupForEdit(const LibraryEntry& entry)
		{
			Reset();
			isOpen = true;
			isEditMode = true;
			editTargetKey = entry.id;
			timing = entry.timing;
			std::snprintf(titleInput, sizeof(titleInput), "%s", entry.title.c_str());

			// 动作槽位：保留当前动作
			motionCandidates.push_back(ImportSlot::kKeepMotion);
			motionSelection = 0;

			// 表情槽位
			facialCandidates.push_back(ImportSlot::kClear);
			if (entry.HasFacial())
			{
				facialCandidates.push_back(ImportSlot::kKeepTrack);
				facialSelection = 1;
			}
			else
			{
				facialSelection = 0;
			}

			// 镜头槽位
			cameraCandidates.push_back(ImportSlot::kClear);
			if (entry.HasCamera())
			{
				cameraCandidates.push_back(ImportSlot::kKeepTrack);
				cameraSelection = 1;
			}
			else
			{
				cameraSelection = 0;
			}

			// 音乐槽位
			audioCandidates.push_back(ImportSlot::kClear);
			if (entry.HasAudio())
			{
				audioCandidates.push_back(ImportSlot::kKeepTrack);
				audioSelection = 1;
			}
			else
			{
				audioSelection = 0;
			}
		}
	};

	inline LibraryModalState g_libraryModal;

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
				e.audioFile = (!m.dir.empty() ? m.dir : m.name) + "/song.wav";

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
				if (ext != ".wav")
				{
					canConfirm = false;
					validationError = "当前音频轨道仅支持 .wav 无损格式";
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
				req.fps = 0.0f;
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
