#pragma once
#include "../GkCharacterNames.hpp"
#include "../GkTrackTiming.hpp"

// =============================================================================
// GuiOverlayCommon.hpp — GakuMotion UI 共享定义与辅助工具
// =============================================================================

#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#undef near
#undef far

#if defined(GKMOTION_ENABLE_IMGUI) || defined(HAS_IMGUI)
#include <imgui.h>
#include "GkGuiThemeColors.hpp"
#endif

namespace GakuMotion
{
	// -----------------------------------------------------------------------------
	// 页面枚举
	// -----------------------------------------------------------------------------
	enum class GuiPage : int
	{
		Library = 0, // 曲库
		Motion = 1,  // 动作
		Camera = 2,  // 镜头
		Stage = 3,   // 舞台
		Settings = 4,// 设置
		Developer = 5// 开发（仅开发者模式显示）
	};

	// -----------------------------------------------------------------------------
	// 性能条目 / 曲库条目（PerformanceEntry / LibraryEntry）
	// -----------------------------------------------------------------------------
	struct LibraryEntry
	{
		std::string id;            // 唯一标识（如文件夹名）
		std::string title;         // 显示标题
		std::string motionFile;    // 动作文件（.vmd, .gkmotion, .gkclip）
		std::string facialFile;    // 表情文件（.vmd 等）
		std::string cameraFile;    // 镜头文件（.vmd, .gkcamera）
		std::string audioFile;     // 音频文件（.wav, .mp3 等）
		float durationSeconds{0.0f};
		uint32_t totalFrames{0};
		uint32_t boneCount{0};
		bool hasAudio{false};
		bool hasCamera{false};
		bool hasFacial{false};
		bool hasOverrides{false};
		PerformanceTiming timing;

		bool HasMotion() const { return !motionFile.empty(); }
		bool HasFacial() const { return !facialFile.empty() || hasFacial; }
		bool HasCamera() const { return !cameraFile.empty() || hasCamera; }
		bool HasAudio() const  { return !audioFile.empty() || hasAudio; }
	};

	// -----------------------------------------------------------------------------
	// 时间格式化 (mm:ss)
	// -----------------------------------------------------------------------------
	inline void FormatTimeMmSs(float seconds, char* out, size_t outSize)
	{
		if (!out || outSize == 0) return;
		if (seconds < 0.0f) seconds = 0.0f;
		const int totalSec = static_cast<int>(seconds);
		const int mm = totalSec / 60;
		const int ss = totalSec % 60;
		std::snprintf(out, outSize, "%02d:%02d", mm, ss);
	}

	// -----------------------------------------------------------------------------
	// 字符串编码转换辅助
	// -----------------------------------------------------------------------------
	inline std::string WideToUtf8(const std::wstring& wstr)
	{
		if (wstr.empty()) return {};
		const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
		                                           nullptr, 0, nullptr, nullptr);
		if (sizeNeeded <= 0) return {};
		std::string res(sizeNeeded, 0);
		WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
		                    res.data(), sizeNeeded, nullptr, nullptr);
		return res;
	}

	inline std::wstring Utf8ToWide(const std::string& str)
	{
		if (str.empty()) return {};
		const int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
		if (sizeNeeded <= 0) return {};
		std::wstring res(sizeNeeded, 0);
		MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), res.data(), sizeNeeded);
		return res;
	}

	// -----------------------------------------------------------------------------
	// VMD / GKCAM 二进制内容倾向探针 (Fast C++ Binary VMD Probe)
	// -----------------------------------------------------------------------------
	enum class VmdTendency : int
	{
		Invalid = 0,
		MotionOnly,       // 仅身体骨骼动画，无表情关键帧
		MotionAndFacial,  // 同时包含身体骨骼动画与表情关键帧
		FacialOnly,       // 仅表情（或仅眼神/眼骨）关键帧，无身体关键帧
		CameraOnly,       // 仅镜头关键帧，无骨骼与表情
		Empty             // 有效 VMD 标头，但无任何关键帧
	};

	struct VmdProbeInfo
	{
		bool valid{false};
		bool isGkCamera{false};
		VmdTendency tendency{VmdTendency::Invalid};
		uint32_t boneFrames{0};
		uint32_t bodyBoneFrames{0};
		uint32_t eyeBoneFrames{0};
		uint32_t morphFrames{0};
		uint32_t cameraFrames{0};

		bool CanUseAsMotion() const { return valid && bodyBoneFrames > 0; }
		bool CanUseAsFacial() const { return valid && (morphFrames > 0 || eyeBoneFrames > 0); }
		bool CanUseAsCamera() const { return valid && cameraFrames > 0; }

		const char* BadgeText() const
		{
			if (isGkCamera) return "[烘焙镜头]";
			switch (tendency)
			{
			case VmdTendency::MotionAndFacial: return "[动作+表情]";
			case VmdTendency::MotionOnly:      return "[动作]";
			case VmdTendency::FacialOnly:      return "[表情]";
			case VmdTendency::CameraOnly:      return "[镜头]";
			case VmdTendency::Empty:           return "[空VMD]";
			default:                           return "[无效VMD]";
			}
		}
	};

	inline VmdProbeInfo ProbeVmdFile(const std::string& utf8Path)
	{
		VmdProbeInfo info{};
		std::filesystem::path p(Utf8ToWide(utf8Path));
		std::ifstream file(p, std::ios::binary);
		if (!file.is_open()) return info;

		file.seekg(0, std::ios::end);
		const auto totalSize = static_cast<std::streamoff>(file.tellg());
		if (totalSize < 30) return info;
		file.seekg(0, std::ios::beg);

		char header[30]{};
		file.read(header, 30);
		if (file.gcount() < 30) return info;
		if (std::strncmp(header, "Vocaloid Motion Data ", 21) != 0) return info;

		const bool version2 = (std::strncmp(header, "Vocaloid Motion Data 0002", 25) == 0);
		const size_t modelLen = version2 ? 20 : 10;
		const auto headerOffset = static_cast<std::streamoff>(30 + modelLen);
		if (totalSize < headerOffset + 4) return info;
		file.seekg(headerOffset, std::ios::beg);

		uint32_t boneCount = 0;
		file.read(reinterpret_cast<char*>(&boneCount), 4);
		if (!file) return info;
		info.boneFrames = boneCount;

		const auto posAfterBoneCount = static_cast<std::streamoff>(file.tellg());
		const auto remainingAfterBoneCount = (totalSize > posAfterBoneCount)
			? static_cast<uint64_t>(totalSize - posAfterBoneCount) : 0ull;

		if (static_cast<uint64_t>(boneCount) > remainingAfterBoneCount / 111ull)
		{
			return info; // 骨骼数量异常，超出文件尺寸上限
		}

		auto IsEyeBone = [](const char* name) -> bool
		{
			if ((std::memcmp(name, "\x97\xBC\x96\xDA", 4) == 0 || // Shift-JIS 両目
			     std::memcmp(name, "\x8D\xB6\x96\xDA", 4) == 0 || // Shift-JIS 左目
			     std::memcmp(name, "\x89\x45\x96\xDA", 4) == 0 || // Shift-JIS 右目
			     std::memcmp(name, "\x96\xDA\x96\xDF", 4) == 0)   // Shift-JIS 目戻
			    && (name[4] == '\0' || name[4] == ' '))
			{
				return true;
			}
			if (std::memcmp(name, "\x96\xDA", 2) == 0 && (name[2] == '\0' || name[2] == ' ')) // Shift-JIS 目
			{
				return true;
			}
			return false;
		};

		const uint32_t scanCount = (std::min)(boneCount, 4096u);
		for (uint32_t i = 0; i < scanCount; ++i)
		{
			char bname[16]{};
			file.read(bname, 15);
			if (!file) return info;

			if (IsEyeBone(bname))
			{
				++info.eyeBoneFrames;
			}
			else if (bname[0] != '\0')
			{
				++info.bodyBoneFrames;
				if (boneCount > 4096u && info.bodyBoneFrames > 0)
				{
					// 已明确包含身体动作骨骼，直接跳转跳过剩余骨骼块（< 0.1ms）
					break;
				}
			}
			file.seekg(96, std::ios::cur);
		}

		// 精确跳至骨骼记录块末尾
		const auto boneBlockEnd = headerOffset + 4 + static_cast<std::streamoff>(static_cast<uint64_t>(boneCount) * 111ull);
		file.seekg(boneBlockEnd, std::ios::beg);
		if (!file) return info;

		const auto posAfterBones = static_cast<std::streamoff>(file.tellg());
		const auto remAfterBones = (totalSize > posAfterBones) ? static_cast<uint64_t>(totalSize - posAfterBones) : 0ull;
		if (remAfterBones >= 4)
		{
			uint32_t morphCount = 0;
			file.read(reinterpret_cast<char*>(&morphCount), 4);
			if (file)
			{
				const auto curPos = static_cast<std::streamoff>(file.tellg());
				const auto remAfterMorphCount = (totalSize > curPos) ? static_cast<uint64_t>(totalSize - curPos) : 0ull;
				if (static_cast<uint64_t>(morphCount) <= remAfterMorphCount / 23ull)
				{
					info.morphFrames = morphCount;
					const auto morphBlockEnd = posAfterBones + 4 + static_cast<std::streamoff>(static_cast<uint64_t>(morphCount) * 23ull);
					file.seekg(morphBlockEnd, std::ios::beg);
				}
				else
				{
					return info;
				}
			}
		}

		const auto posAfterMorph = static_cast<std::streamoff>(file.tellg());
		const auto remAfterMorph = (totalSize > posAfterMorph) ? static_cast<uint64_t>(totalSize - posAfterMorph) : 0ull;
		if (remAfterMorph >= 4)
		{
			uint32_t cameraCount = 0;
			file.read(reinterpret_cast<char*>(&cameraCount), 4);
			if (file)
			{
				const auto curPos = static_cast<std::streamoff>(file.tellg());
				const auto remAfterCamCount = (totalSize > curPos) ? static_cast<uint64_t>(totalSize - curPos) : 0ull;
				if (static_cast<uint64_t>(cameraCount) <= remAfterCamCount / 61ull)
				{
					info.cameraFrames = cameraCount;
				}
				else
				{
					return info;
				}
			}
		}

		info.valid = true;
		if (info.cameraFrames > 0 && info.bodyBoneFrames == 0 && info.morphFrames == 0)
			info.tendency = VmdTendency::CameraOnly;
		else if (info.bodyBoneFrames > 0 && info.morphFrames > 0)
			info.tendency = VmdTendency::MotionAndFacial;
		else if (info.bodyBoneFrames > 0)
			info.tendency = VmdTendency::MotionOnly;
		else if (info.morphFrames > 0 || info.eyeBoneFrames > 0)
			info.tendency = VmdTendency::FacialOnly;
		else
			info.tendency = VmdTendency::Empty;

		return info;
	}

	inline VmdProbeInfo ProbeCameraOrVmdFile(const std::string& utf8Path)
	{
		std::filesystem::path p(Utf8ToWide(utf8Path));
		std::string ext = WideToUtf8(p.extension().wstring());
		for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

		if (ext == ".gkcamera")
		{
			VmdProbeInfo info{};
			std::ifstream file(p, std::ios::binary);
			if (!file.is_open()) return info;

			char magic[8]{};
			file.read(magic, 8);
			if (file.gcount() == 8 && std::memcmp(magic, "GKCAM\0\0\0", 8) == 0)
			{
				uint32_t version = 0, count = 0;
				float fps = 0.0f;
				file.read(reinterpret_cast<char*>(&version), 4);
				file.read(reinterpret_cast<char*>(&fps), 4);
				file.read(reinterpret_cast<char*>(&count), 4);
				if (file && version == 1)
				{
					info.valid = true;
					info.isGkCamera = true;
					info.tendency = VmdTendency::CameraOnly;
					info.cameraFrames = count;
					return info;
				}
			}
			return info;
		}

		return ProbeVmdFile(utf8Path);
	}

	// -----------------------------------------------------------------------------
	// 非阻塞异步文件选择器 (Non-blocking Async File Dialog)
	// -----------------------------------------------------------------------------
	static const wchar_t kAllImportFilter[] =
		L"演出文件 (*.vmd;*.gkcamera;*.wav;*.mp3;*.flac;*.ogg;*.m4a;*.aac;*.opus)\0*.vmd;*.gkcamera;*.wav;*.mp3;*.flac;*.ogg;*.oga;*.m4a;*.aac;*.opus\0所有文件 (*.*)\0*.*\0\0";

	static const wchar_t kMotionFilter[] =
		L"动作 VMD (*.vmd)\0*.vmd\0所有文件 (*.*)\0*.*\0\0";

	static const wchar_t kFacialFilter[] =
		L"表情 VMD (*.vmd)\0*.vmd\0所有文件 (*.*)\0*.*\0\0";

	static const wchar_t kCameraFilter[] =
		L"镜头文件 (*.vmd;*.gkcamera)\0*.vmd;*.gkcamera\0所有文件 (*.*)\0*.*\0\0";

	static const wchar_t kAudioFilter[] =
		L"音频文件 (*.wav;*.mp3;*.flac;*.ogg;*.m4a;*.aac;*.opus)\0*.wav;*.mp3;*.flac;*.ogg;*.oga;*.m4a;*.aac;*.opus\0所有文件 (*.*)\0*.*\0\0";

	class AsyncFileDialog
	{
	public:
		AsyncFileDialog() = default;
		~AsyncFileDialog()
		{
			if (worker.joinable())
			{
				if (isRunning.load()) worker.detach();
				else worker.join();
			}
		}

		bool IsBusy() const { return isRunning.load(); }

		bool Open(bool allowMulti, const wchar_t* filter, int slot = -1, HWND hwndOwner = nullptr)
		{
			if (isRunning.load()) return false;
			if (worker.joinable()) worker.join();

			targetSlot = slot;
			allowMultiSelect = allowMulti;
			ownerHwnd = hwndOwner;
			hasCompleted = false;
			pickedFiles.clear();

			filterBuf.clear();
			if (filter)
			{
				const wchar_t* p = filter;
				while (*p != L'\0' || *(p + 1) != L'\0') ++p;
				const size_t count = (p - filter) + 2;
				filterBuf.assign(filter, filter + count);
			}
			else
			{
				const wchar_t defaultFilter[] = L"所有文件 (*.*)\0*.*\0\0";
				filterBuf.assign(defaultFilter, defaultFilter + sizeof(defaultFilter) / sizeof(wchar_t));
			}

			isRunning.store(true);
			worker = std::thread([this]()
			{
				HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

				std::vector<wchar_t> buffer(65536, 0);
				OPENFILENAMEW ofn{};
				ofn.lStructSize = sizeof(ofn);
				ofn.hwndOwner = ownerHwnd;
				ofn.lpstrFilter = filterBuf.data();
				ofn.lpstrFile = buffer.data();
				ofn.nMaxFile = static_cast<DWORD>(buffer.size());
				ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
				if (allowMultiSelect)
				{
					ofn.Flags |= OFN_ALLOWMULTISELECT;
				}

				std::vector<std::string> localResults;
				if (GetOpenFileNameW(&ofn))
				{
					const wchar_t* p = buffer.data();
					std::wstring first = p;
					p += first.length() + 1;

					if (*p == L'\0')
					{
						if (!first.empty())
						{
							localResults.push_back(WideToUtf8(first));
						}
					}
					else
					{
						std::wstring dir = first;
						if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
							dir += L'\\';
						while (*p != L'\0')
						{
							std::wstring fileName = p;
							localResults.push_back(WideToUtf8(dir + fileName));
							p += fileName.length() + 1;
						}
					}
				}

				{
					std::lock_guard<std::mutex> lock(dataMutex);
					pickedFiles = std::move(localResults);
					hasCompleted = true;
				}

				if (SUCCEEDED(hr))
				{
					CoUninitialize();
				}

				isRunning.store(false);
			});

			return true;
		}

		bool Poll(std::vector<std::string>& outFiles, int& outSlot)
		{
			if (!hasCompleted) return false;
			std::lock_guard<std::mutex> lock(dataMutex);
			if (!hasCompleted) return false;

			outFiles = std::move(pickedFiles);
			outSlot = targetSlot;
			hasCompleted = false;

			if (worker.joinable() && !isRunning.load())
			{
				worker.join();
			}
			return true;
		}

	private:
		std::atomic<bool> isRunning{false};
		std::atomic<bool> hasCompleted{false};
		bool allowMultiSelect{false};
		int targetSlot{-1};
		HWND ownerHwnd{nullptr};
		std::vector<wchar_t> filterBuf;
		std::vector<std::string> pickedFiles;
		std::mutex dataMutex;
		std::thread worker;
	};

	inline AsyncFileDialog g_asyncFileDialog;

	// 同步多选文件对话框（保留为兼容接口）
	inline std::vector<std::string> PickMultipleFilesWin32(HWND hwndOwner = nullptr)
	{
		std::vector<std::string> results;
		std::vector<wchar_t> buffer(65536, 0);

		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = hwndOwner;
		ofn.lpstrFilter = kAllImportFilter;
		ofn.lpstrFile = buffer.data();
		ofn.nMaxFile = static_cast<DWORD>(buffer.size());
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;

		if (GetOpenFileNameW(&ofn))
		{
			const wchar_t* p = buffer.data();
			std::wstring dirOrPath = p;
			p += dirOrPath.length() + 1;

			if (*p == L'\0')
			{
				results.push_back(WideToUtf8(dirOrPath));
			}
			else
			{
				while (*p != L'\0')
				{
					std::wstring fileName = p;
					std::wstring fullPath = dirOrPath;
					if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/')
						fullPath += L'\\';
					fullPath += fileName;
					results.push_back(WideToUtf8(fullPath));
					p += fileName.length() + 1;
				}
			}
		}
		return results;
	}

	// -----------------------------------------------------------------------------
	// 导入文件智能自动识别（Heuristics + Binary Tendency Recommendation）
	// -----------------------------------------------------------------------------
	struct ImportClassification
	{
		std::string suggestedTitle;
		std::string motionCandidate;
		std::vector<std::string> motionCandidates;
		std::string facialCandidate;
		std::string cameraCandidate;
		std::string audioCandidate;
		// 拖入/追加的音频里有几个不是 PCM WAV（不支持）。用来在弹窗里给一句
		// 明确的提示，而不是让用户自己发现"选了没反应"。
		int unsupportedAudioCount{0};
	};

	inline ImportClassification ClassifyImportFiles(const std::vector<std::string>& files)
	{
		ImportClassification res;
		std::vector<std::string> wavAudios;
		std::vector<std::string> otherAudios;

		struct CandidateInfo
		{
			std::string path;
			VmdProbeInfo probe;
		};
		std::vector<CandidateInfo> vmdAndCamCandidates;

		for (const auto& path : files)
		{
			std::filesystem::path p(Utf8ToWide(path));
			std::string ext = WideToUtf8(p.extension().wstring());
			for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

			if (ext == ".vmd" || ext == ".gkcamera")
			{
				VmdProbeInfo probe = ProbeCameraOrVmdFile(path);
				vmdAndCamCandidates.push_back({path, probe});
			}
			else if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg" ||
			         ext == ".oga" || ext == ".m4a" || ext == ".aac" || ext == ".opus")
			{
				wavAudios.push_back(path);
			}
		}

		// 优先选择支持的本地音频
		if (!wavAudios.empty())
			res.audioCandidate = wavAudios.front();
		res.unsupportedAudioCount = static_cast<int>(otherAudios.size());

		auto HasCamKeyword = [](const std::string& path) -> bool
		{
			std::filesystem::path p(Utf8ToWide(path));
			std::string stem = WideToUtf8(p.stem().wstring());
			std::string lower = stem;
			for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return (lower.find("camera") != std::string::npos) ||
			       (lower.find("cam") != std::string::npos) ||
			       (stem.find("カメラ") != std::string::npos) ||
			       (stem.find("镜头") != std::string::npos) ||
			       (stem.find("運鏡") != std::string::npos) ||
			       (stem.find("运镜") != std::string::npos);
		};

		auto HasFaceKeyword = [](const std::string& path) -> bool
		{
			std::filesystem::path p(Utf8ToWide(path));
			std::string stem = WideToUtf8(p.stem().wstring());
			std::string lower = stem;
			for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return (lower.find("face") != std::string::npos) ||
			       (lower.find("facial") != std::string::npos) ||
			       (lower.find("morph") != std::string::npos) ||
			       (lower.find("eye") != std::string::npos) ||
			       (stem.find("表情") != std::string::npos) ||
			       (stem.find("口型") != std::string::npos) ||
			       (stem.find("目") != std::string::npos);
		};

		auto HasMotionKeyword = [](const std::string& path) -> bool
		{
			std::filesystem::path p(Utf8ToWide(path));
			std::string stem = WideToUtf8(p.stem().wstring());
			std::string lower = stem;
			for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return (lower.find("motion") != std::string::npos) ||
			       (lower.find("dance") != std::string::npos) ||
			       (lower.find("choreo") != std::string::npos) ||
			       (stem.find("动作") != std::string::npos) ||
			       (stem.find("モーション") != std::string::npos);
		};

		std::vector<std::string> cameraOnlyList;
		std::vector<std::string> facialOnlyList;
		std::vector<std::string> motionAndFacialList;
		std::vector<std::string> motionOnlyList;

		for (const auto& item : vmdAndCamCandidates)
		{
			if (item.probe.tendency == VmdTendency::CameraOnly)
				cameraOnlyList.push_back(item.path);
			else if (item.probe.tendency == VmdTendency::FacialOnly)
				facialOnlyList.push_back(item.path);
			else if (item.probe.tendency == VmdTendency::MotionAndFacial)
				motionAndFacialList.push_back(item.path);
			else if (item.probe.tendency == VmdTendency::MotionOnly)
				motionOnlyList.push_back(item.path);
		}

		auto PickBest = [](const std::vector<std::string>& list, auto keywordPredicate) -> std::string
		{
			if (list.empty()) return {};
			for (const auto& p : list)
			{
				if (keywordPredicate(p)) return p;
			}
			return list.front();
		};

		// 1. 镜头分配：优先 CameraOnly（含 .gkcamera），关键词为平手判定
		if (!cameraOnlyList.empty())
		{
			res.cameraCandidate = PickBest(cameraOnlyList, HasCamKeyword);
		}
		else
		{
			for (const auto& item : vmdAndCamCandidates)
			{
				if (item.probe.CanUseAsCamera())
				{
					res.cameraCandidate = item.path;
					break;
				}
			}
		}

		// 2. 表情分配：优先专属 FacialOnly，关键词为平手判定
		if (!facialOnlyList.empty())
		{
			res.facialCandidate = PickBest(facialOnlyList, HasFaceKeyword);
		}

		// 3. 动作分配：优先从仅动作/动作+表情中挑选，关键词为平手判定
		std::vector<std::string> motionCandidatesPool = motionOnlyList;
		motionCandidatesPool.insert(motionCandidatesPool.end(), motionAndFacialList.begin(), motionAndFacialList.end());
		if (motionCandidatesPool.empty())
		{
			for (const auto& item : vmdAndCamCandidates)
			{
				if (item.probe.CanUseAsMotion()) motionCandidatesPool.push_back(item.path);
			}
		}

		// Deduplicate and sort body motion candidates by filename for deterministic multi-VMD slot mapping.
		for (const auto& path : motionCandidatesPool)
		{
			if (std::find(res.motionCandidates.begin(), res.motionCandidates.end(), path) == res.motionCandidates.end())
			{
				res.motionCandidates.push_back(path);
			}
		}
		auto FileNameLower = [](const std::string& p) -> std::string
		{
			std::string name = WideToUtf8(std::filesystem::path(Utf8ToWide(p)).filename().wstring());
			for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return name;
		};
		std::sort(res.motionCandidates.begin(), res.motionCandidates.end(),
		          [&](const std::string& a, const std::string& b)
		          {
			          const std::string la = FileNameLower(a);
			          const std::string lb = FileNameLower(b);
			          if (la != lb) return la < lb;
			          return a < b;
		          });

		if (!motionOnlyList.empty() || !motionAndFacialList.empty())
		{
			res.motionCandidate = PickBest(motionCandidatesPool, HasMotionKeyword);
		}
		else if (!res.motionCandidates.empty())
		{
			res.motionCandidate = res.motionCandidates.front();
		}

		// 4. 若未匹配到独立表情文件，但动作文件自身携带表情（MotionAndFacial），则表情复用该文件
		if (res.facialCandidate.empty() && !res.motionCandidate.empty())
		{
			auto it = std::find(motionAndFacialList.begin(), motionAndFacialList.end(), res.motionCandidate);
			if (it != motionAndFacialList.end())
			{
				res.facialCandidate = res.motionCandidate;
			}
			else if (!motionAndFacialList.empty())
			{
				res.facialCandidate = motionAndFacialList.front();
			}
		}

		// 5. 提取建议标题
		if (!res.motionCandidate.empty())
		{
			std::filesystem::path p(Utf8ToWide(res.motionCandidate));
			res.suggestedTitle = WideToUtf8(p.stem().wstring());
		}
		else if (!res.audioCandidate.empty())
		{
			std::filesystem::path p(Utf8ToWide(res.audioCandidate));
			res.suggestedTitle = WideToUtf8(p.stem().wstring());
		}
		else if (!files.empty())
		{
			std::filesystem::path p(Utf8ToWide(files.front()));
			res.suggestedTitle = WideToUtf8(p.stem().wstring());
		}
		else
		{
			res.suggestedTitle = "新演出条目";
		}

		// 清理标题中常见的后缀（如 _motion, _vmd 等）
		auto CleanSuffix = [](std::string& s, const std::string& suffix)
		{
			if (s.size() >= suffix.size())
			{
				std::string sub = s.substr(s.size() - suffix.size());
				for (auto& c : sub) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				std::string target = suffix;
				for (auto& c : target) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				if (sub == target) s.resize(s.size() - suffix.size());
			}
		};
		CleanSuffix(res.suggestedTitle, "_motion");
		CleanSuffix(res.suggestedTitle, "_dance");
		CleanSuffix(res.suggestedTitle, "_choreo");
		CleanSuffix(res.suggestedTitle, "_vmd");

		return res;
	}
} // namespace GakuMotion
