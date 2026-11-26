#pragma once
#include <array>
#include <cstddef>

namespace GakuMotion::mouth_source
{
    enum ExtraChannel : std::size_t { ExtraPresent, Smirk, CornerDown, ExtraUnsupported };
    inline constexpr std::array<const char*, 4> ExtraNames{
        "miku.rawmouth.extra.v1.present", "miku.rawmouth.extra.v1.にやり2",
        "miku.rawmouth.extra.v1.口角下げ", "miku.rawmouth.extra.v1.unsupported"};
}
