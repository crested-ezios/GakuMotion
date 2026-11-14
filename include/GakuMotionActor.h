#pragma once
#include "GakuPlugin.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAKU_MOTION_ACTOR_SERVICE "gakumotion.actor/v1"
#define GAKU_MOTION_ACTOR_VERSION 1u

typedef struct GakuMotionActorSnapshot {
    uint32_t abiVersion;
    uint32_t structSize;
    uint64_t rosterEpoch;
    uint64_t actorToken;
    char character[32];
    char displayName[96];
    char faceName[256];
} GakuMotionActorSnapshot;

typedef struct GakuMotionActorService {
    uint32_t abiVersion;
    uint32_t structSize;
    /* Copy the selected target (the primary track during ensemble playback).
       No Unity call or managed object is exposed. Zero actorToken means no
       selected target. The UTF-8 strings may be empty for unrecognized actors.
       Tokens identify only the latest roster epoch, never a Unity pointer.
       After shutdown returns GAKU_E_UNAVAILABLE. Failures leave output intact. */
    GakuStatus (*getSelected)(uint32_t outputSize, GakuMotionActorSnapshot* output);
} GakuMotionActorService;

#ifdef __cplusplus
}
#endif
