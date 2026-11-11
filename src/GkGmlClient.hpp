#pragma once
#include "GakuPlugin.h"
#include "GakuFileDrop.h"
#include "GakuPaths.hpp"
#include "GakuMotionServices.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <atomic>

namespace GakuMotion::host {
// Versioned GakuLoader tables are copied independently. Optional capabilities
// (desktop UI/window, file dialog, hotkeys) may be absent; required ones are
// validated in Connect().
inline GakuHostApi api{};
inline GakuLogService log{};
inline GakuRuntimeService runtime{};
inline GakuMainThreadService mainThread{};
inline GakuHookService hooks{};
inline GakuDesktopUiService desktop{};
inline GakuDesktopWindowService desktopWindow{};
inline GakuFileDialogService fileDialog{};
inline GakuFileDropService fileDrop{};
inline GakuFileDragService fileDrag{};
inline GakuHotkeyService hotkeys{};
inline GakuAppearanceService appearance{};
inline GakuPathsV1 paths{};
inline bool active = false;
inline std::atomic_bool motionPlaying{false};
using PlaybackService = GakuMotionPlaybackService;
inline uint32_t IsPlaying() { return motionPlaying.load(std::memory_order_acquire) ? 1u : 0u; }
inline void SetPlaying(bool playing) { motionPlaying.store(playing, std::memory_order_release); }
inline bool IsPlayingFlag() { return motionPlaying.load(std::memory_order_acquire); }

template<class T> GakuStatus Query(const char* name, T& table) {
    T candidate{};
    const auto status = api.queryService(api.hostContext, name, GAKU_SERVICE_VERSION,
        sizeof(candidate), &candidate);
    if (status != GAKU_OK) return status;
    if (candidate.abiVersion != GAKU_SERVICE_VERSION) return GAKU_E_VERSION;
    if (candidate.structSize < sizeof(candidate)) return GAKU_E_SIZE;
    table = candidate;
    return GAKU_OK;
}
inline GakuStatus Connect(const GakuHostApi* host) {
    active = false;
    if (!host) return GAKU_E_INVALID_ARGUMENT;
    if (host->abiVersion != GAKU_ABI_VERSION) return GAKU_E_VERSION;
    if (host->structSize < sizeof(GakuHostApi)) return GAKU_E_SIZE;
    if (!host->queryService || !host->registerService) return GAKU_E_INVALID_ARGUMENT;
    api = *host;
    GakuStatus status;
    if ((status = Query(GAKU_SERVICE_LOG, log)) != GAKU_OK ||
        (status = Query(GAKU_SERVICE_RUNTIME, runtime)) != GAKU_OK ||
        (status = Query(GAKU_SERVICE_MAIN_THREAD, mainThread)) != GAKU_OK ||
        (status = Query(GAKU_SERVICE_HOOKS, hooks)) != GAKU_OK) return status;
    if (!log.write || !runtime.getGameAssembly || !runtime.gameRoot || !runtime.localRoot ||
        !runtime.pluginsRoot || !mainThread.enqueue || !mainThread.isMainThread ||
        !hooks.acquire || !hooks.release) return GAKU_E_INVALID_ARGUMENT;
    // Desktop presentation is optional for a headless host. There is no DLL
    // probing fallback on this path, including when the capability is absent.
    desktop = {};
    Query(GAKU_SERVICE_DESKTOP_UI, desktop);
    desktopWindow = {};
    Query(GAKU_SERVICE_DESKTOP_WINDOW, desktopWindow);
    fileDialog = {};
    Query(GAKU_SERVICE_FILE_DIALOG, fileDialog);
    if (!fileDialog.beginOpen || !fileDialog.poll || !fileDialog.release) fileDialog = {};
    fileDrop = {};
    Query(GAKU_SERVICE_FILE_DROP, fileDrop);
    if (!fileDrop.setTarget || !fileDrop.poll) fileDrop = {};
    // Hover feedback is optional: a host without it simply never lights the
    // drop region, and the drop itself keeps working through fileDrop.
    fileDrag = {};
    Query(GAKU_SERVICE_FILE_DRAG, fileDrag);
    if (!fileDrag.peek) fileDrag = {};
    // Hotkeys are optional: older hosts still load, they just cannot open the
    // controls panel with a key.
    hotkeys = {};
    Query(GAKU_SERVICE_HOTKEYS, hotkeys);
    if (!hotkeys.registerHotkey || !hotkeys.unregisterHotkey) hotkeys = {};
    // Appearance service is optional: older hosts still load, default to Dark theme.
    appearance = {};
    Query(GAKU_SERVICE_APPEARANCE, appearance);
    if (!appearance.getAppearance) appearance = {};
    paths = {};
    GakuPaths::Query(host, paths); // Optional; legacy installations keep their layout.
    active = true;
    return GAKU_OK;
}
inline GakuUiTheme GetCurrentTheme() {
    if (appearance.getAppearance) {
        GakuUiAppearanceV1 data{};
        if (appearance.getAppearance(sizeof(data), &data) == GAKU_OK) {
            return data.theme;
        }
    }
    return GAKU_UI_THEME_DARK;
}
inline void Write(GakuLogLevel level, const char* format, va_list args) {
    if (!log.write) return;
    char buffer[2048]{};
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    log.write(log.context, level, buffer);
}
inline void Info(const char* format, ...) {
    va_list args; va_start(args, format); Write(GAKU_LOG_INFO, format, args); va_end(args);
}
inline void Warn(const char* format, ...) {
    va_list args; va_start(args, format); Write(GAKU_LOG_WARNING, format, args); va_end(args);
}
inline void Error(const char* format, ...) {
    va_list args; va_start(args, format); Write(GAKU_LOG_ERROR, format, args); va_end(args);
}
inline GakuStatus PublishPlayback() {
    const PlaybackService service{1u, sizeof(PlaybackService), IsPlaying};
    return api.registerService(api.hostContext, GAKUMOTION_SERVICE_PLAYBACK_STATE, sizeof(service), &service);
}
// A host without the hotkey service reports GAKU_E_UNAVAILABLE instead of
// failing activation; callers only log the miss.
inline GakuStatus RegisterHotkey(const char* actionId, const char* displayName,
    uint32_t virtualKey, uint32_t modifiers, GakuHotkeyFn callback, void* userData = nullptr) {
    if (!hotkeys.registerHotkey) return GAKU_E_UNAVAILABLE;
    const GakuHotkeyBinding binding{GAKU_SERVICE_VERSION, sizeof(GakuHotkeyBinding),
        actionId, displayName, virtualKey, modifiers, callback, userData};
    return hotkeys.registerHotkey(hotkeys.context, &binding);
}
inline GakuStatus UnregisterHotkey(const char* actionId) {
    if (!hotkeys.unregisterHotkey) return GAKU_E_UNAVAILABLE;
    return hotkeys.unregisterHotkey(hotkeys.context, actionId);
}
}
