#pragma once

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
		if (ParseTrackInfoJsonFile(MotionPathFromUtf8(dir) / "info.json", root, err))
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
		int sourceFpsIndex{0}, outputFpsIndex{0};
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
			sourceFpsIndex = outputFpsIndex = 0;
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
} // namespace GakuMotion
#endif
