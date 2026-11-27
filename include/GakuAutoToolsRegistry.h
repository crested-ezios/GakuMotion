// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "GakuPlugin.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAKU_SERVICE_AUTOTOOLS_REGISTRY "gakuautotools.registry/v1"
#define GAKU_AUTOTOOLS_REGISTRY_VERSION 1u

#define GAKU_AUTOTOOLS_THREAD_ANY  0u
#define GAKU_AUTOTOOLS_THREAD_MAIN 1u

/* Invoked by GakuAutoTools when POST /call runs a registered tool.
   argsJson is a non-NULL UTF-8 JSON object string ("{}" when omitted).
   Writes a UTF-8 JSON object string into responseJson (NUL-terminated).
   If responseCapacity is smaller than requiredBytes (including NUL), set
   *requiredBytes and return GAKU_E_SIZE so GakuAutoTools retries once. */
typedef GakuStatus (*GakuAutoToolsInvokeFn)(
    void* userData,
    const char* argsJson,
    uint32_t responseCapacity,
    char* responseJson,
    uint32_t* requiredBytes);

typedef struct GakuAutoToolsToolDescV1 {
    uint32_t abiVersion;         /* GAKU_AUTOTOOLS_REGISTRY_VERSION */
    uint32_t structSize;         /* sizeof(GakuAutoToolsToolDescV1) */
    const char* ownerPluginId;   /* e.g. "gakumotion", "screenshotmanager", "videoexporter" */
    const char* name;            /* e.g. "motion.load" */
    const char* description;     /* UTF-8 summary */
    const char* inputSchemaJson; /* UTF-8 JSON Schema object string; NULL or "{}" -> empty object */
    uint32_t executionThread;    /* GAKU_AUTOTOOLS_THREAD_ANY or GAKU_AUTOTOOLS_THREAD_MAIN */
    uint32_t reserved;
    GakuAutoToolsInvokeFn invoke;
    void* userData;
} GakuAutoToolsToolDescV1;

/* Optional Present backbuffer provider (registered by ScreenshotManager when loaded).
   Produces tightly packed top-down BGR24 pixels (width * height * 3 bytes).
   If outPixels is NULL or capacity < requiredBytes, set *requiredBytes and return GAKU_E_SIZE. */
typedef GakuStatus (*GakuAutoToolsCaptureFrameFn)(
    void* userData,
    uint32_t requestedWidth,
    uint32_t requestedHeight,
    uint32_t* outWidth,
    uint32_t* outHeight,
    uint64_t* outUnityFrame,
    void* outPixels,
    size_t capacity,
    size_t* requiredBytes,
    char* errBuf,
    size_t errCapacity);

typedef struct GakuAutoToolsFrameProviderV1 {
    uint32_t abiVersion;         /* GAKU_AUTOTOOLS_REGISTRY_VERSION */
    uint32_t structSize;         /* sizeof(GakuAutoToolsFrameProviderV1) */
    const char* ownerPluginId;
    GakuAutoToolsCaptureFrameFn captureBgr24;
    void* userData;
} GakuAutoToolsFrameProviderV1;

typedef struct GakuAutoToolsRegistryV1 {
    uint32_t abiVersion;
    uint32_t structSize;
    void* context;
    GakuStatus (*registerTool)(void* context, const GakuAutoToolsToolDescV1* desc);
    GakuStatus (*unregisterTool)(void* context, const char* name);
    GakuStatus (*unregisterOwner)(void* context, const char* ownerPluginId);
    GakuStatus (*registerFrameProvider)(void* context, const GakuAutoToolsFrameProviderV1* provider);
    GakuStatus (*unregisterFrameProvider)(void* context, const char* ownerPluginId);
} GakuAutoToolsRegistryV1;

#ifdef __cplusplus
}
#endif
