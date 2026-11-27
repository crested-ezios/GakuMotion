#pragma once
#include <array>
#include <cstddef>

namespace GakuMotion::mouth_source
{
    enum ExtraChannel : std::size_t { ExtraPresent, Smirk, CornerDown, ExtraUnsupported };
    inline constexpr std::array<const char*, 4> ExtraNames{
        "miku.rawmouth.extra.v1.present", "miku.rawmouth.extra.v1.にやり2",
        "miku.rawmouth.extra.v1.口角下げ", "miku.rawmouth.extra.v1.unsupported"};
    enum V2Channel : std::size_t {
        V2A, V2I, V2U, V2E, V2O, V2Close, V2CornerUp, V2Smirk, V2CornerDown, V2Smile, V2WideOpen,
        V2Present, V2Unsupported, V2Authored
    };
    inline constexpr char V2Prefix[] = "miku.rawmouth.v2.";
    inline constexpr std::array<const char*, 14> V2Names{
        "miku.rawmouth.v2.a", "miku.rawmouth.v2.i", "miku.rawmouth.v2.u",
        "miku.rawmouth.v2.e", "miku.rawmouth.v2.o", "miku.rawmouth.v2.∧",
        "miku.rawmouth.v2.口角上げ", "miku.rawmouth.v2.smirk", "miku.rawmouth.v2.corner_down",
        "miku.rawmouth.v2.smile", "miku.rawmouth.v2.wide_open", "miku.rawmouth.v2.present",
        "miku.rawmouth.v2.unsupported", "miku.rawmouth.v2.native_authored"};
}
