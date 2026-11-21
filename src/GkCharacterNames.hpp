#pragma once
#include <cctype>
#include <string>

namespace GakuMotion
{
	struct IdolName { const char* code; const char* chinese; const char* shortChinese; };
	inline constexpr IdolName kIdolNames[] = {
		// Codes and names checked against the local Character translation data.
		{"amao", "有村麻央", "麻央"}, {"andk", "蓝井抚子", "抚子"}, {"atbm", "雨夜燕", "燕"},
		{"fktn", "藤田琴音", "琴音"}, {"hkne", "冰渡香名江", "香名江"}, {"hmsz", "秦谷美铃", "美铃"},
		{"hrnm", "姬崎莉波", "莉波"}, {"hski", "花海咲季", "咲季"}, {"hume", "花海佑芽", "佑芽"},
		{"jkno", "十王邦夫", "邦夫"}, {"jsna", "十王星南", "星南"}, {"kcna", "仓本千奈", "千奈"},
		{"kllj", "葛城莉莉娅", "莉莉娅"}, {"krnh", "贺阳燐羽", "燐羽"}, {"ktko", "黑井崇男", "崇男"},
		{"myu0", "真城优", "优"}, {"nasr", "根绪亚纱里", "亚纱里"}, {"sgka", "白草月花", "月花"},
		{"shro", "筱泽广", "广"}, {"ssmk", "紫云清夏", "清夏"}, {"sson", "白草四音", "四音"},
		{"trda", "舞蹈训练员", "舞蹈训练员"}, {"trvi", "视觉训练员", "视觉训练员"}, {"trvo", "歌唱训练员", "歌唱训练员"},
		{"ttmr", "月村手毬", "手毬"}
	};

	inline std::string DetectIdolCharacterCode(std::string name)
	{
		for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		std::string result;
		for (const auto& idol : kIdolNames)
			for (size_t pos = name.find(idol.code); pos != std::string::npos; pos = name.find(idol.code, pos + 1))
			{
				const size_t end = pos + 4;
				if ((pos == 0 || !std::isalnum(static_cast<unsigned char>(name[pos - 1]))) &&
					(end == name.size() || !std::isalnum(static_cast<unsigned char>(name[end]))))
				{
					if (!result.empty() && result != idol.code) return {};
					result = idol.code;
				}
			}
		return result;
	}

	inline const char* LookupIdolChineseName(const char* code)
	{
		if (!code || !code[0]) return nullptr;
		std::string normalized = code;
		for (auto& c : normalized) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		for (const auto& idol : kIdolNames) if (normalized == idol.code) return idol.chinese;
		return nullptr;
	}

	/// Returns localized short given name for known character codes, or nullptr if unknown.
	inline const char* LookupIdolShortName(const char* code)
	{
		if (!code || !code[0]) return nullptr;
		std::string normalized = code;
		for (auto& c : normalized) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		for (const auto& idol : kIdolNames) if (normalized == idol.code) return idol.shortChinese;
		return nullptr;
	}

	inline std::string FormatIdolDisplayName(const char* code)
	{
		if (!code || !code[0] || std::string(code) == "auto") return "当前角色";
		if (const char* name = LookupIdolChineseName(code)) return std::string(name) + " (" + code + ")";
		return code;
	}
}
