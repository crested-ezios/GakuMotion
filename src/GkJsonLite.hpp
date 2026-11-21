#pragma once

// =============================================================================
// GkJsonLite.hpp — GakuMotion 运行时控制层专用的「零依赖最小 JSON 解析器」
// =============================================================================
// 设计约束（与 AGENT_WORKFLOW.md 的工具链契约保持一致）：
//   1. 不引入任何第三方库：build.ps1 以「单 TU + 静态链接」方式编译
//      src/GakuMotionPlugin.cpp，所有新增能力必须是纯头文件实现；
//   2. 只读解析：控制层只需要「读配置」，不做序列化回写，
//      因此不实现 writer，避免无谓复杂度；
//   3. 宽容但严格：允许 UTF-8 BOM（\xEF\xBB\xBF）、行内 // 与块级 /* */ 注释、
//      对象/数组尾随逗号；一旦出现真正的语法错误，返回 false 并给出
//      <行>:<列> 定位信息，绝不抛异常、绝不中断游戏主线程；
//   4. 对象成员「保序」保存；同名键以最后一个为准（Find 从后向前扫描），
//      这样用户可以用「后写的覆盖先写的」这种直觉方式打补丁；
//   5. 认识 \uXXXX 转义（含代理对），输出 UTF-8，
//      以便骨骼名里的日文/中文（如「左ひじ」「頭」）能被正确读入。
//
// 典型用法：
//   GakuMotion::jsonlite::Value root;
//   std::string err;
//   if (!GakuMotion::jsonlite::ParseFile(path, root, err)) { /* 记日志并跳过 */ }
//   const Value* gaze = root.FindPath("gaze.mode");
//   int mode = gaze ? gaze->AsInt(-1) : -1;
// =============================================================================

#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <shared_mutex>
#ifdef _WIN32
#include <chrono>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace GakuMotion::jsonlite
{
	// -----------------------------------------------------------------------------
	// 1. 值类型
	// -----------------------------------------------------------------------------
	enum class Type { Null, Bool, Number, String, Array, Object };

	inline const char* TypeName(Type t) noexcept
	{
		switch (t)
		{
		case Type::Null: return "null";
		case Type::Bool: return "bool";
		case Type::Number: return "number";
		case Type::String: return "string";
		case Type::Array: return "array";
		case Type::Object: return "object";
		}
		return "unknown";
	}

	class Value
	{
	public:
		Type type{Type::Null};
		bool boolValue{false};
		double numberValue{0.0};
		std::string stringValue;
		std::vector<Value> arrayItems;
		std::vector<std::pair<std::string, Value>> objectMembers;

		// -------- 类型判定 --------
		bool IsNull() const noexcept { return type == Type::Null; }
		bool IsBool() const noexcept { return type == Type::Bool; }
		bool IsNumber() const noexcept { return type == Type::Number; }
		bool IsString() const noexcept { return type == Type::String; }
		bool IsArray() const noexcept { return type == Type::Array; }
		bool IsObject() const noexcept { return type == Type::Object; }

		size_t Size() const noexcept
		{
			if (type == Type::Array) return arrayItems.size();
			if (type == Type::Object) return objectMembers.size();
			return 0;
		}

		bool Empty() const noexcept { return Size() == 0; }

		// -------- 对象成员查找（同名键取最后一个） --------
		const Value* Find(const std::string& key) const noexcept
		{
			if (type != Type::Object) return nullptr;
			for (size_t i = objectMembers.size(); i-- > 0;)
			{
				if (objectMembers[i].first == key) return &objectMembers[i].second;
			}
			return nullptr;
		}

		// -------- 数组下标访问 --------
		const Value* At(size_t index) const noexcept
		{
			if (type != Type::Array || index >= arrayItems.size()) return nullptr;
			return &arrayItems[index];
		}

		// -------- 路径查找： "a.b.c"，数组段用十进制下标（如 "bones.0.rot"） --------
		const Value* FindPath(const std::string& path) const noexcept
		{
			auto cur = this;
			size_t start = 0;
			while (start <= path.size())
			{
				size_t dot = path.find('.', start);
				std::string seg = (dot == std::string::npos)
					                  ? path.substr(start)
					                  : path.substr(start, dot - start);
				if (cur == nullptr) return nullptr;
				if (seg.empty()) return nullptr;

				bool numeric = cur->type == Type::Array;
				if (numeric)
				{
					for (char c : seg)
					{
						if (c < '0' || c > '9')
						{
							numeric = false;
							break;
						}
					}
				}
				if (numeric)
				{
					cur = cur->At(static_cast<size_t>(std::strtoul(seg.c_str(), nullptr, 10)));
				}
				else
				{
					cur = cur->Find(seg);
				}
				if (dot == std::string::npos) break;
				start = dot + 1;
			}
			return cur;
		}

		// -------- 带默认值的取值（类型不符时返回默认值，不抛异常） --------
		bool AsBool(bool defaultValue) const noexcept
		{
			if (type == Type::Bool) return boolValue;
			if (type == Type::Number) return numberValue != 0.0;
			return defaultValue;
		}

		double AsDouble(double defaultValue) const noexcept
		{
			if (type == Type::Number) return numberValue;
			if (type == Type::Bool) return boolValue ? 1.0 : 0.0;
			return defaultValue;
		}

		float AsFloat(float defaultValue) const noexcept
		{
			if (type == Type::Number) return static_cast<float>(numberValue);
			if (type == Type::Bool) return boolValue ? 1.0f : 0.0f;
			return defaultValue;
		}

		int AsInt(int defaultValue) const noexcept
		{
			if (type == Type::Number) return static_cast<int>(numberValue);
			if (type == Type::Bool) return boolValue ? 1 : 0;
			return defaultValue;
		}

		std::string AsString(const std::string& defaultValue) const noexcept
		{
			if (type == Type::String) return stringValue;
			return defaultValue;
		}

		// 便捷路径取值 ------------------------------------------------------------
		float FloatPath(const std::string& path, float def) const noexcept
		{
			const Value* v = FindPath(path);
			return v ? v->AsFloat(def) : def;
		}

		int IntPath(const std::string& path, int def) const noexcept
		{
			const Value* v = FindPath(path);
			return v ? v->AsInt(def) : def;
		}

		bool BoolPath(const std::string& path, bool def) const noexcept
		{
			const Value* v = FindPath(path);
			return v ? v->AsBool(def) : def;
		}

		std::string StringPath(const std::string& path, const std::string& def) const noexcept
		{
			const Value* v = FindPath(path);
			return v ? v->AsString(def) : def;
		}

		bool HasPath(const std::string& path) const noexcept
		{
			return FindPath(path) != nullptr;
		}
	};

	// -----------------------------------------------------------------------------
	// 2. 递归下降解析器
	// -----------------------------------------------------------------------------
	class Parser
	{
	public:
		static constexpr int kMaxDepth = 64;

		explicit Parser(const std::string& source) : src_(source)
		{
		}

		bool Parse(Value& out, std::string& error)
		{
			SkipProlog();
			if (pos_ >= src_.size()) return Fail(error, "empty input: no JSON value found");
			if (!ParseValue(out, 0, error)) return false;
			SkipTrivia();
			if (pos_ < src_.size())
			{
				return Fail(error, "trailing characters after root value");
			}
			return true;
		}

		size_t Line() const noexcept { return line_; }
		size_t Column() const noexcept { return col_; }

	private:
		const std::string& src_;
		size_t pos_{0};
		size_t line_{1};
		size_t col_{1};

		// -------- 基础字符操作 --------
		bool Eof() const noexcept { return pos_ >= src_.size(); }
		char Peek() const noexcept { return Eof() ? '\0' : src_[pos_]; }

		char PeekAt(size_t off) const noexcept
		{
			return (pos_ + off < src_.size()) ? src_[pos_ + off] : '\0';
		}

		void Advance() noexcept
		{
			if (Eof()) return;
			if (src_[pos_] == '\n')
			{
				++line_;
				col_ = 1;
			}
			else { ++col_; }
			++pos_;
		}

		bool Fail(std::string& error, const std::string& what) const
		{
			std::ostringstream oss;
			oss << what << " (line " << line_ << ", col " << col_ << ")";
			error = oss.str();
			return false;
		}

		// BOM + UTF-8 BOM 跳过
		void SkipProlog() noexcept
		{
			if (src_.size() >= 3 &&
				static_cast<unsigned char>(src_[0]) == 0xEF &&
				static_cast<unsigned char>(src_[1]) == 0xBB &&
				static_cast<unsigned char>(src_[2]) == 0xBF)
			{
				pos_ = 3;
			}
		}

		// 空白 + 注释 + 尾随逗号探测
		void SkipTrivia() noexcept
		{
			for (;;)
			{
				if (Eof()) return;
				char c = Peek();
				if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
				{
					Advance();
					continue;
				}
				if (c == '/' && PeekAt(1) == '/')
				{
					while (!Eof() && Peek() != '\n') Advance();
					continue;
				}
				if (c == '/' && PeekAt(1) == '*')
				{
					Advance();
					Advance();
					while (!Eof() && !(Peek() == '*' && PeekAt(1) == '/')) Advance();
					if (!Eof())
					{
						Advance();
						Advance();
					}
					continue;
				}
				return;
			}
		}

		// -------- 值分派 --------
		bool ParseValue(Value& out, int depth, std::string& error)
		{
			if (depth > kMaxDepth) return Fail(error, "nesting too deep");
			SkipTrivia();
			if (Eof()) return Fail(error, "expected a value but reached end of input");

			char c = Peek();
			switch (c)
			{
			case '{': return ParseObject(out, depth, error);
			case '[': return ParseArray(out, depth, error);
			case '"':
				{
					out.type = Type::String;
					return ParseString(out.stringValue, error);
				}
			case 't':
			case 'f': return ParseLiteral(out, error);
			case 'n': return ParseLiteral(out, error);
			default:
				if (c == '-' || c == '+' || (c >= '0' && c <= '9') || c == '.')
				{
					return ParseNumber(out, error);
				}
				return Fail(error, std::string("unexpected value-start character '") + c + "'");
			}
		}

		bool ParseLiteral(Value& out, std::string& error)
		{
			if (src_.compare(pos_, 4, "true") == 0)
			{
				for (int i = 0; i < 4; ++i) Advance();
				out.type = Type::Bool;
				out.boolValue = true;
				return true;
			}
			if (src_.compare(pos_, 5, "false") == 0)
			{
				for (int i = 0; i < 5; ++i) Advance();
				out.type = Type::Bool;
				out.boolValue = false;
				return true;
			}
			if (src_.compare(pos_, 4, "null") == 0)
			{
				for (int i = 0; i < 4; ++i) Advance();
				out.type = Type::Null;
				return true;
			}
			return Fail(error, "invalid literal (expected true / false / null)");
		}

		bool ParseNumber(Value& out, std::string& error)
		{
			size_t begin = pos_;
			if (Peek() == '-' || Peek() == '+') Advance();
			bool anyDigit = false;
			while (!Eof())
			{
				char c = Peek();
				if ((c >= '0' && c <= '9'))
				{
					anyDigit = true;
					Advance();
					continue;
				}
				if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
				{
					Advance();
					continue;
				}
				break;
			}
			if (!anyDigit) return Fail(error, "invalid number");

			std::string token = src_.substr(begin, pos_ - begin);
			// 严格性检查：拒绝 "--1"、"1.2.3" 这类明显异常的写法
			int dotCount = 0;
			for (char c : token) if (c == '.') ++dotCount;
			if (dotCount > 1 || token.find("--") != std::string::npos)
			{
				return Fail(error, "invalid number format: " + token);
			}

			errno = 0;
			char* end = nullptr;
			double v = std::strtod(token.c_str(), &end);
			if (end == token.c_str() || *end != '\0')
			{
				return Fail(error, "failed to parse number: " + token);
			}
			out.type = Type::Number;
			out.numberValue = v;
			return true;
		}

		// -------- 字符串（含 \uXXXX 与代理对） --------
		bool ParseString(std::string& out, std::string& error)
		{
			if (Peek() != '"') return Fail(error, "expected opening quote for a string");
			Advance();

			out.clear();
			while (!Eof())
			{
				char c = Peek();
				if (c == '"')
				{
					Advance();
					return true;
				}
				if (c == '\\')
				{
					Advance();
					if (Eof()) return Fail(error, "string ended unexpectedly after escape character");
					char esc = Peek();
					Advance();
					switch (esc)
					{
					case '"': out.push_back('"');
						break;
					case '\\': out.push_back('\\');
						break;
					case '/': out.push_back('/');
						break;
					case 'b': out.push_back('\b');
						break;
					case 'f': out.push_back('\f');
						break;
					case 'n': out.push_back('\n');
						break;
					case 'r': out.push_back('\r');
						break;
					case 't': out.push_back('\t');
						break;
					case 'u':
						{
							uint32_t cp = 0;
							if (!ParseHex4(cp, error)) return false;
							// UTF-16 代理对
							if (cp >= 0xD800 && cp <= 0xDBFF)
							{
								if (Peek() == '\\' && PeekAt(1) == 'u')
								{
									Advance();
									Advance();
									uint32_t low = 0;
									if (!ParseHex4(low, error)) return false;
									if (low >= 0xDC00 && low <= 0xDFFF)
									{
										cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
									}
									else
									{
										cp = 0xFFFDu; // 非法代理，降级为替换字符
									}
								}
								else
								{
									cp = 0xFFFDu;
								}
							}
							else if (cp >= 0xDC00 && cp <= 0xDFFF)
							{
								cp = 0xFFFDu; // 孤立低代理
							}
							AppendUtf8(out, cp);
							break;
						}
					default:
						return Fail(error, std::string("unknown escape sequence '\\") + esc + "'");
					}
					continue;
				}
				if (static_cast<unsigned char>(c) < 0x20)
				{
					return Fail(error, "unescaped control character in string");
				}
				out.push_back(c);
				Advance();
			}
			return Fail(error, "unterminated string");
		}

		bool ParseHex4(uint32_t& out, std::string& error)
		{
			uint32_t v = 0;
			for (int i = 0; i < 4; ++i)
			{
				if (Eof()) return Fail(error, "incomplete \\u escape");
				char c = Peek();
				uint32_t d;
				if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
				else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
				else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
				else return Fail(error, "non-hex character in \\u escape");
				v = (v << 4) | d;
				Advance();
			}
			out = v;
			return true;
		}

		static void AppendUtf8(std::string& out, uint32_t cp)
		{
			if (cp <= 0x7F)
			{
				out.push_back(static_cast<char>(cp));
			}
			else if (cp <= 0x7FF)
			{
				out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
			else if (cp <= 0xFFFF)
			{
				out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
				out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
			else
			{
				out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
				out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
		}

		// -------- 数组 --------
		bool ParseArray(Value& out, int depth, std::string& error)
		{
			out.type = Type::Array;
			out.arrayItems.clear();
			Advance(); // '['
			SkipTrivia();
			if (Peek() == ']')
			{
				Advance();
				return true;
			}

			for (;;)
			{
				Value item;
				if (!ParseValue(item, depth + 1, error)) return false;
				out.arrayItems.push_back(std::move(item));

				SkipTrivia();
				if (Peek() == ',')
				{
					Advance();
					SkipTrivia();
					// 容忍尾随逗号
					if (Peek() == ']')
					{
						Advance();
						return true;
					}
					continue;
				}
				if (Peek() == ']')
				{
					Advance();
					return true;
				}
				return Fail(error, "expected ',' or ']' between array elements");
			}
		}

		// -------- 对象 --------
		bool ParseObject(Value& out, int depth, std::string& error)
		{
			out.type = Type::Object;
			out.objectMembers.clear();
			Advance(); // '{'
			SkipTrivia();
			if (Peek() == '}')
			{
				Advance();
				return true;
			}

			for (;;)
			{
				SkipTrivia();
				if (Peek() != '"') return Fail(error, "object keys must be strings");
				std::string key;
				if (!ParseString(key, error)) return false;

				SkipTrivia();
				if (Peek() != ':') return Fail(error, "expected ':' after object key");
				Advance();

				Value member;
				if (!ParseValue(member, depth + 1, error)) return false;
				out.objectMembers.emplace_back(std::move(key), std::move(member));

				SkipTrivia();
				if (Peek() == ',')
				{
					Advance();
					SkipTrivia();
					if (Peek() == '}')
					{
						Advance();
						return true;
					} // 容忍尾随逗号
					continue;
				}
				if (Peek() == '}')
				{
					Advance();
					return true;
				}
				return Fail(error, "expected ',' or '}' between object members");
			}
		}
	};

	// -----------------------------------------------------------------------------
	// 3. 便捷入口
	// -----------------------------------------------------------------------------
	inline std::shared_mutex& JsonFileCommitMutex()
	{
		static std::shared_mutex mutex;
		return mutex;
	}

	inline bool ParseText(const std::string& text, Value& out, std::string& error)
	{
		Parser parser(text);
		return parser.Parse(out, error);
	}

	inline bool ParseFile(const std::string& path, Value& out, std::string& error)
	{
		const std::filesystem::path fsPath(std::u8string(path.begin(), path.end()));
		std::string text;
#ifdef _WIN32
		{
			std::shared_lock<std::shared_mutex> commitReadLock(JsonFileCommitMutex());
			HANDLE file = INVALID_HANDLE_VALUE;
			for (int attempt = 0; attempt < 16; ++attempt)
			{
				file = CreateFileW(
					fsPath.c_str(),
					GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					nullptr,
					OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL,
					nullptr);
				if (file != INVALID_HANDLE_VALUE) break;
				const DWORD lastErr = GetLastError();
				if (lastErr == ERROR_ACCESS_DENIED)
				{
					const DWORD attrs = GetFileAttributesW(fsPath.c_str());
					if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
						break;
				}
				if ((lastErr != ERROR_ACCESS_DENIED && lastErr != ERROR_SHARING_VIOLATION) || attempt == 15)
				{
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (file == INVALID_HANDLE_VALUE)
			{
				error = "cannot open file: " + path;
				return false;
			}
			LARGE_INTEGER fileSize{};
			if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 0 ||
				static_cast<uint64_t>(fileSize.QuadPart) > 64ULL * 1024ULL * 1024ULL)
			{
				CloseHandle(file);
				error = "cannot open file: " + path;
				return false;
			}
			text.resize(static_cast<size_t>(fileSize.QuadPart));
			size_t totalRead = 0;
			while (totalRead < text.size())
			{
				DWORD bytesRead = 0;
				const size_t remaining = text.size() - totalRead;
				const DWORD chunk = static_cast<DWORD>(remaining < size_t{0x40000000} ? remaining : size_t{0x40000000});
				if (!ReadFile(file, text.data() + totalRead, chunk, &bytesRead, nullptr))
				{
					CloseHandle(file);
					error = "cannot open file: " + path;
					return false;
				}
				if (bytesRead == 0) break;
				totalRead += bytesRead;
			}
			CloseHandle(file);
			text.resize(totalRead);
		}
#else
		std::ifstream file(fsPath, std::ios::binary);
		if (!file.is_open())
		{
			error = "cannot open file: " + path;
			return false;
		}
		std::ostringstream buffer;
		buffer << file.rdbuf();
		text = buffer.str();
#endif
		if (text.empty())
		{
			error = "empty file: " + path;
			return false;
		}
		if (!ParseText(text, out, error))
		{
			error = path + " -> " + error;
			return false;
		}
		return true;
	}

	// -----------------------------------------------------------------------------
	// 4. 极简写出口：只用于「首次运行生成模板文件」与「状态回写」两种场景，
	//    不追求通用性；字符串一律做最小必要转义。
	// -----------------------------------------------------------------------------
	inline std::string EscapeString(const std::string& in)
	{
		std::string out;
		out.reserve(in.size() + 2);
		for (char c : in)
		{
			switch (c)
			{
			case '"': out += "\\\"";
				break;
			case '\\': out += "\\\\";
				break;
			case '\n': out += "\\n";
				break;
			case '\r': out += "\\r";
				break;
			case '\t': out += "\\t";
				break;
			default: out.push_back(c);
				break;
			}
		}
		return out;
	}

	inline std::string FormatNumber(double v, int precision = 4)
	{
		char buf[64]{};
		std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
		// 去掉多余的尾随 0
		std::string s(buf);
		if (s.find('.') != std::string::npos)
		{
			while (!s.empty() && s.back() == '0') s.pop_back();
			if (!s.empty() && s.back() == '.') s.pop_back();
		}
		if (s.empty()) s = "0";
		return s;
	}
}
