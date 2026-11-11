// SPDX-License-Identifier: GPL-3.0-only
//
// GakuMotion is licensed under GNU GPL version 3.
// See LICENSE and THIRD_PARTY_NOTICES.md.

#pragma once

// Lightweight severity facade for every GakuMotion diagnostic.
//
// Why this exists instead of adding a level to the host ABI:
//   GakuPlugin.h exposes exactly three wire values (INFO/WARNING/ERROR). Adding
//   GAKU_LOG_DEBUG would be binary compatible, but an old loader receiving an
//   unknown level falls through to INFO -- the message would still be printed,
//   which defeats the point of having a level at all. Debug therefore never
//   crosses the ABI: it is stripped at compile time in release builds, and a
//   development build tags it with "[D] " and sends it as INFO.
//
// Nothing here changes an ABI struct, an export, or an existing call site's
// meaning: an unmigrated g_hostContext.logInfo(...) is INFO by definition.

#ifndef GAKUMOTION_LOG_FLOOR
#  ifdef NDEBUG
#    define GAKUMOTION_LOG_FLOOR 1   /* release: Info and above */
#  else
#    define GAKUMOTION_LOG_FLOOR 0   /* development: everything */
#  endif
#endif

// Opt-in geometry probes (PROBE A1..A13). Off by default; the blocks they guard
// exist to answer a rig question that has already been answered, and they cost
// ~57 lines per clip bind when enabled.
#ifndef GAKUMOTION_ENABLE_GEOMETRY_PROBE
#  define GAKUMOTION_ENABLE_GEOMETRY_PROBE 0
#endif

namespace GakuMotion::Log {

enum class Level : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Formats, filters by the compile-time floor, and forwards to the host log
// service. Safe before activation and on any thread: a null service is a no-op.
void Write(Level level, const char* format, ...);

} // namespace GakuMotion::Log

#define GM_LOG_AT(level, ...) \
    do { if (GAKUMOTION_LOG_FLOOR <= static_cast<int>(level)) \
        ::GakuMotion::Log::Write(level, __VA_ARGS__); } while (0)

#define GM_DEBUG(...) GM_LOG_AT(::GakuMotion::Log::Level::Debug, __VA_ARGS__)
#define GM_INFO(...)  GM_LOG_AT(::GakuMotion::Log::Level::Info,  __VA_ARGS__)
#define GM_WARN(...)  GM_LOG_AT(::GakuMotion::Log::Level::Warn,  __VA_ARGS__)
#define GM_ERROR(...) GM_LOG_AT(::GakuMotion::Log::Level::Error, __VA_ARGS__)
