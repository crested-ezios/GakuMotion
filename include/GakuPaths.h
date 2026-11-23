// SPDX-License-Identifier: MIT
//
// GakuLoader Public Plugin SDK
// See the applicable MIT license notice.

#pragma once
#include "GakuPlugin.h"

#define GAKU_SERVICE_PATHS "gakuloader.paths"
#define GAKU_PATHS_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t GakuPathKind;
#define GAKU_PATH_USER_DATA   1u
#define GAKU_PATH_CONFIGS     2u
#define GAKU_PATH_PLUGIN_DATA 3u
#define GAKU_PATH_CACHE       4u
#define GAKU_PATH_LOGS        5u
#define GAKU_PATH_TEMP        6u

/* Optional service; all existing GakuPlugin.h v1 layouts remain unchanged.
   The host binds context to the querying owner. No caller-supplied plugin id.
   Roots are absolute UTF-16 Windows paths, fixed for this process, under
   localRoot/<category>/<canonical plugin id>. Querying never creates directories.
   Accepted namespaces are ASCII lowercase, <=64 characters, with nonempty dot
   segments containing a-z, 0-9, _ and -; first character must be a-z or 0-9.
   Windows reserved device stems and trailing dots are rejected. Invalid legacy
   ids may still load/use other services; only this service is unavailable to them.
   Paths organise cooperative plugins; they are not an I/O security sandbox. */
typedef struct GakuPathsV1 {
    uint32_t abiVersion; uint32_t structSize;
    void* context;
    /* capacity/required are wchar_t counts, including the terminating NUL.
       required is mandatory. NULL output with zero capacity measures the root:
       E_SIZE sets only required. Insufficient capacity (including non-NULL/0)
       also sets only required: no partial path is ever written.
       NULL output with nonzero capacity, NULL required or an invalid kind returns
       E_INVALID_ARGUMENT. Invalid namespace and all other failures leave both
       outputs untouched. A retired owner returns E_SHUTTING_DOWN.
       The caller owns the buffer; no allocation crosses the ABI. Thread-safe. */
    GakuStatus (*getRoot)(void* context, GakuPathKind kind,
                          wchar_t* output, uint32_t capacity, uint32_t* required);
    /* Explicit, idempotent creation of only the requested category/owner root.
       A retired owner returns E_SHUTTING_DOWN. I/O errors return E_INTERNAL and
       the host logs owner/category/OS error. No old-root or AppData fallback.
       No cleanup, file migration or creation of unrelated categories. */
    GakuStatus (*ensureRoot)(void* context, GakuPathKind kind);
} GakuPathsV1;

#ifdef __cplusplus
}
#endif
