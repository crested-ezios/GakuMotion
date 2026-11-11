#pragma once
#include "GakuPlugin.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAKU_SERVICE_FILE_DROP "gakuloader.file-drop"
#define GAKU_FILE_DROP_VERSION 1u
#define GAKU_FILE_DROP_MAX_FILES 256u
#define GAKU_FILE_DROP_MAX_BYTES (1024u * 1024u)

typedef struct GakuFileDropTarget {
    uint32_t abiVersion; uint32_t structSize;
    float left, top, right, bottom; /* ImGui viewport coordinates: host client pixels (single viewport). */
} GakuFileDropTarget;

typedef struct GakuFileDropResult {
    uint32_t abiVersion; uint32_t structSize;
    uint32_t fileCount; uint32_t requiredBytes;
} GakuFileDropResult;

/* Optional extension; frozen GakuPlugin.h service layouts stay unchanged.
   Both calls are valid only inside this owner's onDesktopUi callback.
   Targets expire each completed UI frame; NULL disables the target and clears
   its pending batch. The host routes drops only to the selected panel's target.
   poll(0,NULL) peeks metadata. A sufficient buffer copies fileCount consecutive
   NUL-terminated UTF-8 paths and consumes the batch exactly once. Empty returns
   GAKU_OK with zero counts. Short buffers return GAKU_E_SIZE without consuming.
   One batch, at most MAX_FILES / MAX_BYTES, is retained; overflow is rejected. */
typedef struct GakuFileDropService {
    uint32_t abiVersion; uint32_t structSize;
    void* context;
    GakuStatus (*setTarget)(void* context, const GakuFileDropTarget* target);
    GakuStatus (*poll)(void* context, uint32_t resultSize, GakuFileDropResult* result,
        uint32_t pathCapacity, char* utf8Paths);
} GakuFileDropService;

/* Live shell-drag state, published as a separate optional service so the
   frozen GakuFileDropService layout above keeps its exact size and existing
   plugins keep loading. Valid only inside this owner's onDesktopUi callback;
   the host reports the drag that hovers its control window, independent of the
   registered target, so a plugin can render hover feedback the drop-time
   service can never provide. */
#define GAKU_SERVICE_FILE_DRAG "gakuloader.file-drag"
#define GAKU_FILE_DRAG_VERSION 1u
#define GAKU_FILE_DRAG_ACTIVE 1u
#define GAKU_FILE_DRAG_INSIDE_TARGET 2u
/* Permanently set while the host runs above Explorer's integrity level. UIPI
   then blocks every OLE drop call, so no drag state can ever arrive and the
   shell shows its no-drop cursor; drops still land through gakuloader.file-drop
   (WM_DROPFILES). Plugins should explain the missing preview instead of
   waiting for hover events that cannot be delivered. */
#define GAKU_FILE_DRAG_HOST_BLOCKED 4u
/* Set together with HOST_BLOCKED when the reason is elevation: the host runs
   above Explorer's integrity level. A blocked host without this bit simply has
   no hover transport at all (older host, or OLE unavailable), so a plugin can
   tell "this is expected because you launched elevated" apart from "this host
   never had a preview". */
#define GAKU_FILE_DRAG_HOST_ELEVATED 8u

typedef struct GakuFileDragState {
    uint32_t abiVersion; uint32_t structSize;
    uint32_t flags;         /* GAKU_FILE_DRAG_* bits                  */
    uint32_t generation;    /* increments once per new OS drag        */
    uint32_t fileCount;     /* paths currently dragged, 0 when unknown */
    uint32_t requiredBytes; /* packed UTF-8 bytes, NUL terminated each */
    float x, y;             /* latest drag point, host client pixels;  */
                            /* -1,-1 when the point is unknown         */
} GakuFileDragState;

typedef struct GakuFileDragService {
    uint32_t abiVersion; uint32_t structSize;
    void* context;
    /* Non-consuming peek: paths never leave the host and the drop batch is
       untouched. capacity == 0 measures (state is filled, E_SIZE when
       capacity < requiredBytes). The state is empty with only abiVersion and
       structSize meaningful when this owner has no registered target.
       Dragged paths are copied once per OS drag, not per frame. */
    GakuStatus (*peek)(void* context, uint32_t stateSize, GakuFileDragState* state,
        uint32_t pathCapacity, char* utf8Paths);
} GakuFileDragService;

#ifdef __cplusplus
}
#endif
