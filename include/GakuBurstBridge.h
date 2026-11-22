#pragma once
#include <stdint.h>
#include <stddef.h>

/* Windows x64 C ABI. Every caller-owned structure must set size and version.
 * Configure/Publish/Clear/trace export run on Unity's main thread.
 * Shutdown is any-thread, terminal and callback-free; status/perf are read-only.
 * The DLL is optional and pins itself only after installing native hooks.
 * Shutdown retains inert hooks/trampolines until process exit. It cancels
 * future input/results, not a native write or host callback already in flight.
 * Keep callback code/context resident while a borrowed callback can run.
 * No allocation or exception crosses the ABI. No Unity type is exposed. */
#define GBB_API_VERSION 2u
#define GBB_API_VERSION_V3 3u
/* Legacy v3 source-compatibility constant; dynamic implementations may exceed it. */
#define GBB_MAX_CONTEXTS 3u
#define GBB_MAX_BONES 64u
#if defined(_WIN32)
#define GBB_CALL __cdecl
#else
#define GBB_CALL
#endif
#if defined(GBB_BUILD_DLL)
#define GBB_EXPORT __declspec(dllexport)
#else
#define GBB_EXPORT
#endif
#ifdef __cplusplus
extern "C" {
#endif

typedef struct GbbQuaternion { float x, y, z, w; } GbbQuaternion;
typedef struct GbbVector3 { float x, y, z; } GbbVector3;
typedef struct GbbWorldPose { GbbQuaternion rotation; GbbVector3 position; } GbbWorldPose;
enum GbbResult { GBB_OK=0, GBB_PENDING=1, GBB_E_ARGUMENT=-1, GBB_E_VERSION=-2,
    GBB_E_THREAD=-3, GBB_E_UNAVAILABLE=-4, GBB_E_BINDING=-5, GBB_E_FAULT=-6, GBB_E_SHUTDOWN=-7, GBB_E_IO=-8,
    GBB_E_CONTEXT=-9, GBB_E_CAPACITY=-10 };
enum GbbState { GBB_STATE_UNINITIALIZED=0, GBB_STATE_DISCOVERING=1,
    GBB_STATE_READY=2, GBB_STATE_OBSERVING=3, GBB_STATE_INJECTING=4,
    GBB_STATE_UNAVAILABLE=5, GBB_STATE_FAULTED=6, GBB_STATE_SHUTDOWN=7 };
enum GbbFrameFlags { GBB_FRAME_INJECT=1u, GBB_FRAME_WORLD_DIAGNOSTICS=2u,
    /* Experimental, default off. Inject the same snapshot before Quartz and
     * before Swing. Clearing this flag restores the original Swing-only path. */
    GBB_FRAME_BEFORE_QUARTZ=4u,
    /* Bounded read-only capture of actual HumanoidSleeve muscle reader calls.
     * Independent from injection, so OFF and ON use identical diagnostics. */
    GBB_FRAME_SLEEVE_DIAGNOSTICS=8u,
    /* Optional per-job scene force filter. Negotiate the capability first. */
    GBB_FRAME_NO_SCENE_FORCES=16u };
enum GbbBoneFlags { GBB_BONE_WORLD_VALID=1u };
enum GbbClearFlags { GBB_CLEAR_RESET_FAULT=1u };

typedef void (GBB_CALL *GbbLogFn)(void* context, int32_t level, const char* message);
typedef struct GbbConfig {
    uint32_t size, version;
    GbbLogFn log;
    void* log_context;
    uint32_t reserved[4];
} GbbConfig;
typedef struct GbbBonePose {
    void* transform;
    int32_t humanoid_id;
    uint32_t flags;
    GbbQuaternion local_rotation;
    GbbVector3 local_position;
    GbbQuaternion world_rotation;
    GbbVector3 world_position;
} GbbBonePose;
typedef struct GbbFrame {
    uint32_t size, version, flags, bone_count;
    void* actor;
    void* animator;
    float clip_time;
    int32_t unity_frame;
    GbbBonePose bones[GBB_MAX_BONES];
} GbbFrame;
typedef struct GbbStatus {
    uint32_t size, version, state;
    int32_t last_result;
    uint32_t main_thread_id, injection_fault, world_valid, writes;
    uint64_t generation, sequence, matched_passes, failed_passes, swing_calls;
    void* actor;
    int32_t source_frame;
    float clip_time, delta_time;
    uint32_t injected;
    GbbQuaternion head_before, head_target, head_after;
    GbbWorldPose world_before[2], world_target[2], world_after[2]; /* Head, Hips */
    char profile[64];
    char detail[192];
} GbbStatus;
typedef struct GbbPerf {
    uint32_t size, version;
    uint64_t qpc_frequency, publish_calls, publish_total_ns, publish_last_ns, publish_max_ns;
    uint64_t native_passes, native_total_ns, native_last_ns, native_max_ns;
    uint64_t solver_calls, solver_total_ns, solver_last_ns, solver_max_ns;
    uint64_t failed_passes, expired_skips, identity_skips, delta_probe_calls;
} GbbPerf;
typedef struct GbbApi {
    uint32_t size, version;
    int32_t (GBB_CALL *configure)(const GbbConfig* config);
    int32_t (GBB_CALL *publish)(const GbbFrame* frame);
    int32_t (GBB_CALL *clear)(uint32_t flags);
    int32_t (GBB_CALL *get_status)(GbbStatus* status);
    int32_t (GBB_CALL *get_perf)(GbbPerf* perf);
    int32_t (GBB_CALL *shutdown)(void);
    /* Main thread, explicit absolute UTF-16 file path. Exports bounded,
     * read-only caller and IL2CPP ABI metadata evidence even after a layout
     * rejection; never enables a hook or injection. */
    int32_t (GBB_CALL *dump_traces)(const wchar_t* absolute_file);
} GbbApi;
typedef int32_t (GBB_CALL *GbbGetApiFn)(uint32_t requested_version, GbbApi* api);
/* v3 adds explicit actor-lifetime contexts. Payloads retain their v2 layout and
 * version, including GbbConfig/Frame/Status/Perf. Tokens are opaque, nonzero,
 * process-unique and invalid after destroy. All mutators are main-thread-only;
 * status/perf remain read-only and shutdown retains its any-thread contract. */
typedef uint64_t GbbContext;
typedef struct GbbApiV3 {
    uint32_t size, version;
    int32_t (GBB_CALL *configure)(const GbbConfig* config);
    int32_t (GBB_CALL *create_context)(GbbContext* context);
    int32_t (GBB_CALL *destroy_context)(GbbContext context);
    int32_t (GBB_CALL *publish_context)(GbbContext context, const GbbFrame* frame);
    int32_t (GBB_CALL *clear_context)(GbbContext context, uint32_t flags);
    int32_t (GBB_CALL *get_context_status)(GbbContext context, GbbStatus* status);
    int32_t (GBB_CALL *get_context_perf)(GbbContext context, GbbPerf* perf);
    int32_t (GBB_CALL *shutdown)(void);
    int32_t (GBB_CALL *dump_traces)(const wchar_t* absolute_file);
} GbbApiV3;
typedef int32_t (GBB_CALL *GbbGetApiV3Fn)(uint32_t requested_version, GbbApiV3* api);
GBB_EXPORT int32_t GBB_CALL GakuBurstBridge_GetApi(uint32_t requested_version, void* api);
GBB_EXPORT uint32_t GBB_CALL GakuBurstBridge_SupportsSceneForceFilter(void);

#ifdef __cplusplus
}
#endif
