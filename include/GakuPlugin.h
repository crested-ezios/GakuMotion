// SPDX-License-Identifier: MIT
//
// GakuLoader Public Plugin SDK
// See the applicable MIT license notice.

#pragma once
/* GakuLoader plugin ABI v1.
   Fixed-size, C-compatible negotiation; no STL, no ownership transfer, no
   exceptions across the boundary. Service tables are requested by exact name
   and exact version. On failure the output buffer is untouched.

   Lifecycle is split into three distinct phases so a plugin can attach before
   Unity exists:
     1. activate        - early native activation outside the Windows loader
                          lock, before this host's runtime-ready notification.
                          The game may already be loaded: handle both cases.
     2. onRuntimeReady   - GameAssembly/IL2CPP became resolvable. May be called
                          on the loader's worker thread, never the Unity main
                          thread. Metadata/export resolution and hook
                          installation only.
     3. onTick/onActorTick - Unity main thread. onTick runs at most once per
                          frame with or without game actors; onActorTick runs
                          once per live actor.
   Modules, trampolines and host-owned strings live until process exit; hot
   unloading is deliberately unsupported. */
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#if defined(_WIN32)
#define GAKU_PLUGIN_EXPORT __declspec(dllexport)
#else
#define GAKU_PLUGIN_EXPORT
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define GAKU_ABI_VERSION 1u
#define GAKU_SERVICE_VERSION 1u

#define GAKU_SERVICE_LOG "gakuloader.log"
#define GAKU_SERVICE_RUNTIME "gakuloader.runtime"
#define GAKU_SERVICE_MAIN_THREAD "gakuloader.main-thread"
#define GAKU_SERVICE_HOOKS "gakuloader.hooks"
#define GAKU_SERVICE_DESKTOP_UI "gakuloader.desktop-ui"
#define GAKU_SERVICE_APPEARANCE "gakuloader.appearance"
#define GAKU_SERVICE_DESKTOP_WINDOW "gakuloader.desktop-window"
#define GAKU_SERVICE_FILE_DIALOG "gakuloader.file-dialog"
#define GAKU_SERVICE_HOTKEYS "gakuloader.hotkeys"
#define GAKU_SERVICE_ACTOR_ENTRY "gakuloader.actor-entry"
#define GAKU_SERVICE_RENDER_FRAME "gakuloader.render-frame"
#define GAKU_SERVICE_RENDER_FRAME_STATUS "gakuloader.render-frame-status"

typedef int32_t GakuStatus;
#define GAKU_OK ((GakuStatus)0)
#define GAKU_E_INVALID_ARGUMENT ((GakuStatus)-1)
#define GAKU_E_VERSION ((GakuStatus)-2)
#define GAKU_E_SIZE ((GakuStatus)-3)
#define GAKU_E_UNAVAILABLE ((GakuStatus)-4)
#define GAKU_E_CONFLICT ((GakuStatus)-5)
#define GAKU_E_SHUTTING_DOWN ((GakuStatus)-6)
#define GAKU_E_LIMIT ((GakuStatus)-7)
#define GAKU_E_INTERNAL ((GakuStatus)-8)

typedef struct GakuServiceHeader { uint32_t abiVersion; uint32_t structSize; } GakuServiceHeader;
typedef GakuStatus (*GakuQueryServiceFn)(void* hostContext, const char* name,
    uint32_t requestedVersion, uint32_t outputSize, void* output);
/* Copies a table. Service names are unique process-wide; hostContext binds
   ownership to the calling plugin. Provider tables begin with GakuServiceHeader;
   their function pointers stay valid until process exit. The provider owns its
   domain semantics and threading. Names in the "gakuloader." namespace are
   reserved for the loader. */
typedef GakuStatus (*GakuRegisterServiceFn)(void* hostContext, const char* name,
    uint32_t tableSize, const void* table);
typedef struct GakuHostApi {
    uint32_t abiVersion; uint32_t structSize;
    void* hostContext;
    GakuQueryServiceFn queryService;
    GakuRegisterServiceFn registerService;
} GakuHostApi;

typedef enum GakuLogLevel { GAKU_LOG_INFO = 0, GAKU_LOG_WARNING = 1, GAKU_LOG_ERROR = 2 } GakuLogLevel;
typedef struct GakuLogService {
    uint32_t abiVersion; uint32_t structSize;
    void* context;
    /* UTF-8 plain text, never a printf format string, never Unity's logger. */
    void (*write)(void* context, GakuLogLevel level, const char* message);
} GakuLogService;

typedef struct GakuRuntimeService {
    uint32_t abiVersion; uint32_t structSize;
    /* Absolute paths; valid until process exit. */
    const wchar_t* gameRoot; const wchar_t* localRoot; const wchar_t* pluginsRoot;
    /* NULL until the game assembly is loaded. */
    void* (*getGameAssembly)(void);
} GakuRuntimeService;

typedef void (*GakuTaskFn)(void* userData);
typedef struct GakuMainThreadService {
    uint32_t abiVersion; uint32_t structSize;
    void* context;
    /* Bounded FIFO drained on the Unity main thread before onTick; also drained
       while no actor exists. Tasks never run synchronously; canceled at
       shutdown. Callbacks must not throw across the ABI. */
    GakuStatus (*enqueue)(void* context, GakuTaskFn callback, void* userData);
    /* 1 only on the Unity main thread once it has been observed. */
    uint32_t (*isMainThread)(void* context);
} GakuMainThreadService;

typedef struct GakuHookRequest {
    void* target; void* detour; void** original;
} GakuHookRequest;

typedef struct GakuHookService {
    uint32_t abiVersion; uint32_t structSize;
    void* context;
    /* An exclusive detour lease on a process-wide target address; enabled
       before success and refused on collision with another owner. Reservations
       and trampolines live until process exit; the same owner+detour may
       acquire again after release. No global hook operations, no cross-provider
       takeover. */
    GakuStatus (*acquire)(void* context, void* target, void* detour, void** original);
    GakuStatus (*release)(void* context, void* target);
    /* 1..256 distinct targets/output slots. All originals are published before
       any detour can run. Validation/create failures leave outputs untouched.
       An enable failure disables this batch but retains originals/trampolines;
       do not free detour state on the assumption that no call was in flight. */
    GakuStatus (*acquireBatch)(void* context, uint32_t count, const GakuHookRequest* requests);
} GakuHookService;

typedef struct GakuImGuiAbi {
    uint32_t versionNum; uint32_t ioSize; uint32_t styleSize;
    uint32_t drawVertSize; uint32_t drawIdxSize; uint32_t wcharSize;
} GakuImGuiAbi;
typedef void* (*GakuImGuiAllocFn)(size_t bytes, void* userData);
typedef void (*GakuImGuiFreeFn)(void* pointer, void* userData);
typedef struct GakuDesktopUiBinding {
    uint32_t abiVersion; uint32_t structSize;
    void* imguiContext;
    GakuImGuiAllocFn alloc; GakuImGuiFreeFn free; void* allocatorUserData;
} GakuDesktopUiBinding;
typedef struct GakuDesktopUiService {
    uint32_t abiVersion; uint32_t structSize;
    /* Available inside the descriptor's onDesktopUi callback. Validate the exact
       ABI fingerprint before copying the context or allocator pointers. */
    GakuStatus (*bind)(const GakuImGuiAbi* consumerAbi, uint32_t outputSize,
        GakuDesktopUiBinding* output);
} GakuDesktopUiService;

typedef enum GakuUiTheme {
    GAKU_UI_THEME_DARK = 0,
    GAKU_UI_THEME_SAPPHIRE = 1,
    GAKU_UI_THEME_PAPER_AND_INK = 2,
    GAKU_UI_THEME_APPLE_LIGHT = 3,
    GAKU_UI_THEME_APPLE_DARK = 4,
} GakuUiTheme;

typedef struct GakuUiAppearanceV1 {
    uint32_t apiVersion; uint32_t structSize;
    GakuUiTheme theme;
    float uiScale;
} GakuUiAppearanceV1;

typedef struct GakuAppearanceService {
    uint32_t abiVersion; uint32_t structSize;
    /* Copies the host's current appearance snapshot. On success returns GAKU_OK. */
    GakuStatus (*getAppearance)(uint32_t outputSize, GakuUiAppearanceV1* output);
} GakuAppearanceService;

#define GAKU_DESKTOP_PRESENTATION_PLUGIN_PANEL 1u
typedef struct GakuDesktopWindowService {
    uint32_t abiVersion; uint32_t structSize;
    void* context; /* Bound to the plugin that queried this table. */
    /* Native host-owned HWND, or NULL while unavailable. Any thread may query
       it; never destroy/subclass it or send it game-close messages. */
    void* (*getWindow)(void);
    /* Asynchronous show/select of the requesting plugin's panel, including
       when another panel is selected. Safe from any plugin thread. */
    GakuStatus (*requestShow)(void* context);
    /* onDesktopUi draws inside the selected host content panel. */
    uint32_t presentationKind;
} GakuDesktopWindowService;

#define GAKU_FILE_DIALOG_PENDING 0u
#define GAKU_FILE_DIALOG_ACCEPTED 1u
#define GAKU_FILE_DIALOG_CANCELLED 2u
#define GAKU_FILE_DIALOG_FAILED 3u
typedef struct GakuFileFilter {
    const wchar_t* name; const wchar_t* pattern;
} GakuFileFilter;
typedef struct GakuFileDialogRequest {
    uint32_t abiVersion; uint32_t structSize;
    const wchar_t* title;
    uint32_t filterCount; const GakuFileFilter* filters;
} GakuFileDialogRequest;
typedef struct GakuFileDialogResult {
    uint32_t abiVersion; uint32_t structSize; uint32_t state;
    int32_t osError; uint32_t requiredBytes;
} GakuFileDialogResult;
typedef struct GakuFileDialogService {
    uint32_t abiVersion; uint32_t structSize;
    void* context;
    /* Copies request strings immediately; never opens a modal inside the plugin
       callback. One unreleased ticket per owner, sixteen process-wide. */
    GakuStatus (*beginOpen)(void* context, const GakuFileDialogRequest* request, uint64_t* ticket);
    /* pathCapacity=0/utf8Path=NULL queries metadata. ACCEPTED requiredBytes
       includes the UTF-8 NUL. Invalid output/buffer arguments change no output. */
    GakuStatus (*poll)(void* context, uint64_t ticket, uint32_t resultSize,
        GakuFileDialogResult* result, uint32_t pathCapacity, char* utf8Path);
    GakuStatus (*release)(void* context, uint64_t ticket);
} GakuFileDialogService;

#define GAKU_HOTKEY_MOD_CTRL 0x1u
#define GAKU_HOTKEY_MOD_SHIFT 0x2u
#define GAKU_HOTKEY_MOD_ALT 0x4u
#define GAKU_HOTKEY_MOD_WIN 0x8u
typedef void (*GakuHotkeyFn)(void* userData);
typedef struct GakuHotkeyBinding {
    uint32_t abiVersion; uint32_t structSize;
    /* Stable, case-sensitive owner-local id, e.g. "show-gui". The manager saves
       exact identities; <pluginId>.<actionId> is a legacy INI alias only. */
    const char* actionId;
    /* Shown in the F1 manager and in conflict diagnostics. */
    const char* displayName;
    uint32_t virtualKey;   /* Win32 virtual-key code */
    uint32_t modifiers;    /* GAKU_HOTKEY_MOD_* */
    GakuHotkeyFn callback; /* Runs on the Unity main thread */
    void* userData;
} GakuHotkeyBinding;
typedef struct GakuHotkeyService {
    uint32_t abiVersion; uint32_t structSize;
    void* context;
    /* Discrete, foreground-scoped hotkeys only (never continuous camera/mouse
       input). The registry owns edge detection: one physical press dispatches
       once, focus loss never replays keys, and no OS-global hotkey is grabbed.
       A combo already claimed by a different owner marks every binding on that
       combo as conflicted: none of them fires until it is changed, and the
       manager shows the conflict. Re-registering the same actionId replaces the
       binding. F1 is reserved by the loader. */
    GakuStatus (*registerHotkey)(void* context, const GakuHotkeyBinding* binding);
    GakuStatus (*unregisterHotkey)(void* context, const char* actionId);
} GakuHotkeyService;

#define GAKU_ACTOR_ENTRY_BEFORE 1u
#define GAKU_ACTOR_ENTRY_AFTER 2u
typedef struct GakuActorEntryBinding {
    uint32_t abiVersion; uint32_t structSize;
    /* GAKU_ACTOR_ENTRY_BEFORE runs before the game's own actor update; AFTER
       runs after it and before the loader's onTick/onActorTick dispatch. */
    uint32_t phase;
    /* Diagnostic only, copied immediately; may be NULL. */
    const char* name;
    void (*callback)(void* userData, void* actor); /* Unity main thread */
    void* userData;
} GakuActorEntryBinding;
typedef struct GakuActorEntryService {
    uint32_t abiVersion; uint32_t structSize;
    void* context;
    /* CampusActorController::LateUpdate has exactly one detour, owned by the
       loader. A plugin that must run on that entry registers a participant
       here instead of acquiring a colliding hook lease. The game original runs
       exactly once per invocation between the before and after phases, in
       registration order per phase. One binding per owner and phase;
       re-registering replaces it. Never a general hook chain: this exists only
       because the loader itself owns that one shared game entry. */
    GakuStatus (*registerEntry)(void* context, const GakuActorEntryBinding* binding);
    GakuStatus (*unregisterEntry)(void* context, uint32_t phase);
} GakuActorEntryService;

typedef void (*GakuRenderFrameFn)(void* userData, int isNormalOuterFrame);
typedef struct GakuRenderFrameBinding {
    uint32_t abiVersion;
    uint32_t structSize;
    /* Diagnostic only, copied immediately; may be NULL. */
    const char* name;
    GakuRenderFrameFn onBeforeRender; /* Unity main thread, before outer DoRenderLoop_Internal */
    GakuRenderFrameFn onAfterRender;  /* Unity main thread, after outer DoRenderLoop_Internal */
    void* userData;
} GakuRenderFrameBinding;
typedef struct GakuRenderFrameService {
    uint32_t abiVersion;
    uint32_t structSize;
    void* context;
    /* Register an observer on UnityEngine.Rendering.RenderPipelineManager::DoRenderLoop_Internal
       (or a non-conflicting fallback render boundary).
       One binding per owner; re-registering replaces it. Installed on-demand after runtime-ready. */
    GakuStatus (*registerObserver)(void* context, const GakuRenderFrameBinding* binding);
    GakuStatus (*unregisterObserver)(void* context);
} GakuRenderFrameService;

#define GAKU_RENDER_FRAME_HOOK_PENDING 0u
#define GAKU_RENDER_FRAME_HOOK_ACTIVE 1u
#define GAKU_RENDER_FRAME_HOOK_UNAVAILABLE 2u
typedef struct GakuRenderFrameStatusService {
    uint32_t abiVersion;
    uint32_t structSize;
    /* Returns GAKU_RENDER_FRAME_HOOK_* indicating whether the host's
       render-frame boundary detour is pending, active, or unavailable. */
    uint32_t (*getStatus)(void);
} GakuRenderFrameStatusService;

typedef struct GakuPluginDescriptor {
    uint32_t abiVersion; uint32_t structSize;
    const char* id; const char* name; const char* version;
    /* Native activation on the loader's activation thread outside the Windows
       loader lock. Runtime modules may already exist. Connect services,
       read configuration and install process hooks here; no Unity or managed
       calls and no desktop UI. Returning failure disables this plugin only. */
    GakuStatus (*activate)(const GakuHostApi* host, void** pluginState);
    /* IL2CPP/GameAssembly became resolvable. May run on a loader worker thread:
       resolve exports and metadata, install detours, then return. Managed object
       access belongs to onTick/onActorTick. May be NULL. */
    void (*onRuntimeReady)(void* pluginState);
    /* Unity main thread, at most once per frame, with or without actors. Queued
       tasks already ran. Frame-scoped work only. May be NULL. */
    void (*onTick)(void* pluginState, float deltaTime);
    /* Unity main thread, once per live actor after its native LateUpdate.
       Actor-scoped work; never the frame pump. May be NULL. */
    void (*onActorTick)(void* pluginState, void* activeActor, float deltaTime);
    /* Dedicated desktop UI thread inside the host window's content panel.
       Never call Create/DestroyContext, NewFrame, Render or Present, and never
       mix private font objects or allocators. May be NULL. */
    void (*onDesktopUi)(void* pluginState);
    /* Unity main thread during shutdown, in reverse activation order. */
    void (*shutdown)(void* pluginState);
} GakuPluginDescriptor;

/* Export GakuPlugin_Query with this signature. The query itself must not
   perform runtime work; it is called once, outside the loader lock, to copy a
   descriptor. */
typedef GakuStatus (*FnGakuPlugin_Query)(uint32_t requestedAbi, uint32_t outputSize,
    GakuPluginDescriptor* output);

#ifdef __cplusplus
}
#endif
