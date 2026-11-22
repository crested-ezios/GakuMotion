#pragma once

#include "../GkMotionData.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace GakuMotion::webpanel
{
	// -------------------------------------------------------------------------
	// Minimal JSON writer (jsonlite is parse-only by design).
	// -------------------------------------------------------------------------
	inline void AppendEscaped(std::string& out, const std::string& text)
	{
		out.push_back('"');
		for (const unsigned char ch : text)
		{
			switch (ch)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (ch < 0x20)
				{
					char buffer[8]{};
					std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(ch));
					out += buffer;
				}
				else out.push_back(static_cast<char>(ch));
			}
		}
		out.push_back('"');
	}

	inline std::string JString(const std::string& text)
	{
		std::string out;
		AppendEscaped(out, text);
		return out;
	}

	inline std::string JNum(double value)
	{
		if (!std::isfinite(value)) return "0";
		char buffer[48]{};
		if (value == std::floor(value) && std::fabs(value) < 9.0e15)
			std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
		else
			std::snprintf(buffer, sizeof(buffer), "%.17g", value);
		return buffer;
	}

	inline std::string JBool(bool value) { return value ? "true" : "false"; }

	inline std::string JVec(const Vector3& value)
	{
		return "{\"x\":" + JNum(value.x) + ",\"y\":" + JNum(value.y) + ",\"z\":" + JNum(value.z) + "}";
	}

	inline std::string JQuat(const Quaternion& value)
	{
		return "{\"x\":" + JNum(value.x) + ",\"y\":" + JNum(value.y) +
			",\"z\":" + JNum(value.z) + ",\"w\":" + JNum(value.w) + "}";
	}

	inline std::string JColor3(const std::array<float, 3>& value)
	{
		return "[" + JNum(value[0]) + "," + JNum(value[1]) + "," + JNum(value[2]) + "]";
	}
} // namespace GakuMotion::webpanel
