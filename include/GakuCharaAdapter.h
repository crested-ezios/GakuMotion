#pragma once
#include "GakuPlugin.h"
#include "RinhaFacialFit.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAKU_CHARA_ADAPTER_SERVICE "chara-adapter"
#define GAKU_CHARA_ADAPTER_VERSION 1u

typedef struct GakuCharaIdentity {
    char character[32];    /* NUL-terminated character ID. */
    char displayName[96];  /* NUL-terminated UTF-8 display name. */
} GakuCharaIdentity;

typedef struct GakuCharaAdapterService {
    uint32_t abiVersion;
    uint32_t structSize;
    void* context;
    /* Look up the exact face prefab name, with Unity's clone suffix removed.
       Both callbacks are read-only, require no Unity access, and leave
       output untouched on failure. GAKU_E_UNAVAILABLE means no match or
       no configured fit. The returned fit uses process-lifetime data. */
    GakuStatus (*resolveFace)(void* context, const char* faceName, GakuCharaIdentity* output);
    GakuStatus (*getFacialFit)(void* context, RinhaFacialFitService* output);
} GakuCharaAdapterService;

#ifdef __cplusplus
}
#endif
