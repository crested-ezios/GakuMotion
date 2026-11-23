// SPDX-License-Identifier: MIT
//
// GakuLoader Public Plugin SDK
// See the applicable MIT license notice.

#pragma once
/* Optional plugin metadata, dependency declaration, and read-only plugin
   registry extension for GakuLoader ABI v1.

   All existing GakuPlugin.h v1 layouts remain unchanged and older plugins
   without GakuPlugin_QueryMetadata continue to load normally.

   To opt in, a plugin DLL exports:
     GakuStatus GakuPlugin_QueryMetadata(
         uint32_t requestedAbi, uint32_t outputSize, GakuPluginMetadataV1* output);

   Rules for GakuPlugin_QueryMetadata:
     - Called once outside the loader lock immediately after GakuPlugin_Query.
     - Do not initialize IL2CPP, UI, hooks, or worker threads here.
     - Validate requestedAbi == GAKU_METADATA_ABI_VERSION, output != NULL, and
       outputSize >= sizeof(GakuPluginMetadataV1) before writing *output.
     - All string pointers and array pointers must point to static or module-
       lifetime storage; the host copies all strings before activate() or
       FreeLibrary(). */

#include "GakuPlugin.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAKU_METADATA_ABI_VERSION 1u
#define GAKU_LOADER_VERSION "1.1.0"

#define GAKU_SERVICE_PLUGINS "gakuloader.plugins"
#define GAKU_PLUGINS_SERVICE_VERSION 1u

/* Dependency edge kinds. */
typedef uint32_t GakuPluginDependencyKind;
#define GAKU_DEPENDENCY_REQUIRED 1u
#define GAKU_DEPENDENCY_OPTIONAL 2u
#define GAKU_DEPENDENCY_CONFLICT 3u

/* Process-wide plugin load states reported by gakuloader.plugins. */
typedef uint32_t GakuPluginLoadState;
#define GAKU_PLUGIN_STATE_NOT_FOUND        0u
#define GAKU_PLUGIN_STATE_DISABLED         1u
#define GAKU_PLUGIN_STATE_DEPENDENCY_ERROR 2u
#define GAKU_PLUGIN_STATE_INCOMPATIBLE     3u
#define GAKU_PLUGIN_STATE_QUERIED          4u
#define GAKU_PLUGIN_STATE_ACTIVATED        5u
#define GAKU_PLUGIN_STATE_RUNTIME_READY    6u
#define GAKU_PLUGIN_STATE_FAILED           7u

typedef struct GakuPluginLocalizedTextV1 {
    /* Language tag matching GakuLanguage / BCP-47, e.g. "en", "zh-Hans", "zh-Hant", "ja". */
    const char* languageTag;
    /* UTF-8 plain text. */
    const char* text;
} GakuPluginLocalizedTextV1;

typedef struct GakuPluginDependencyV1 {
    /* Canonical unique id of the target plugin (GakuPluginDescriptor.id). */
    const char* pluginId;
    /* Optional human-readable name used in diagnostics and F1 UI. */
    const char* displayName;
    /* SemVer 2.0.0 constraint expression, e.g. ">0.2.0-beta.1", ">=1.0.0 <2.0.0",
       "^0.2.0", "~0.2.1", or "*" / NULL / "" for any version. */
    const char* versionRange;
    GakuPluginDependencyKind kind;
    uint32_t reserved; /* Must be 0. */
} GakuPluginDependencyV1;

typedef struct GakuPluginMetadataV1 {
    uint32_t abiVersion;                  /* GAKU_METADATA_ABI_VERSION */
    uint32_t structSize;                  /* sizeof(GakuPluginMetadataV1) */
    const char* originalAuthor;           /* Original author (UTF-8, optional) */
    uint32_t maintainerCount;
    uint32_t reserved0;
    const char* const* maintainers;       /* Active maintainers (UTF-8, optional) */
    const char* subVersion;               /* Minor/build display tag, e.g. "build.42" (ignored by SemVer comparison) */
    const char* releaseChannel;           /* Channel display label, e.g. "stable", "beta", "dev" */
    const char* summary;                  /* Default one-line summary (UTF-8, optional) */
    uint32_t localizedSummaryCount;
    uint32_t reserved1;
    const GakuPluginLocalizedTextV1* localizedSummaries;
    const char* repositoryUrl;            /* https:// or http:// */
    const char* releaseUrl;               /* https:// or http:// */
    const char* issuesUrl;                /* https:// or http:// */
    const char* license;                  /* SPDX or license identifier, e.g. "MIT" */
    const char* minLoaderVersion;         /* Minimum GakuLoader version or SemVer range (bare version means >=version) */
    uint32_t dependencyCount;
    uint32_t reserved2;
    const GakuPluginDependencyV1* dependencies;
    uint32_t auxiliaryDllCount;
    uint32_t reserved3;
    const char* const* auxiliaryDlls;     /* Native helper DLLs belonging to this plugin */
} GakuPluginMetadataV1;

/* Optional DLL export signature. */
typedef GakuStatus (*FnGakuPlugin_QueryMetadata)(
    uint32_t requestedAbi, uint32_t outputSize, GakuPluginMetadataV1* output);

typedef struct GakuHostInfoV1 {
    uint32_t abiVersion;       /* GAKU_PLUGINS_SERVICE_VERSION */
    uint32_t structSize;       /* sizeof(GakuHostInfoV1) */
    uint32_t pluginAbiVersion; /* GAKU_ABI_VERSION */
    uint32_t pluginCount;      /* Number of discovered plugin records (including disabled/failed) */
    char loaderVersion[64];    /* Host SemVer string, e.g. "1.1.0" */
} GakuHostInfoV1;

typedef struct GakuPluginSummaryV1 {
    uint32_t abiVersion;       /* GAKU_PLUGINS_SERVICE_VERSION */
    uint32_t structSize;       /* sizeof(GakuPluginSummaryV1) */
    GakuPluginLoadState state;
    uint32_t hasMetadata;      /* 1 if GakuPlugin_QueryMetadata succeeded */
    char id[65];
    char name[128];
    char version[64];
    char subVersion[64];
} GakuPluginSummaryV1;

typedef struct GakuPluginsServiceV1 {
    uint32_t abiVersion;       /* GAKU_PLUGINS_SERVICE_VERSION */
    uint32_t structSize;       /* sizeof(GakuPluginsServiceV1) */
    GakuStatus (*getHostInfo)(uint32_t outputSize, GakuHostInfoV1* output);
    GakuStatus (*findPlugin)(const char* pluginId, uint32_t outputSize, GakuPluginSummaryV1* output);
    GakuStatus (*getPluginAt)(uint32_t index, uint32_t outputSize, GakuPluginSummaryV1* output);
} GakuPluginsServiceV1;

#ifdef __cplusplus
}
#endif
