#pragma once
#include "GkBurstBridgeMultiClient.hpp"
#include "GkBurstBridgeClient.hpp"
#include "GkFallbackPhysics.hpp"
#include "GkGmlClient.hpp"
#include "GakuPhysicsDiagnostics.h"
namespace MultiBridgeFixture {
    struct Context { GbbContext token{}; GbbStatus status{}; unsigned publishes{},clears{},destroys{}; };
    inline std::array<Context,8> contexts;
    inline uint64_t next=1;
    inline int fail=-1, stale=-1;
    inline bool refuseClear=false,refuseDestroy=false;
    inline unsigned shutdowns=0;
    inline Context* Find(GbbContext token) { for (auto& c:contexts) if (token && c.token==token) return &c; return nullptr; }
    inline int GBB_CALL Create(GbbContext* token) {
        for (auto& c:contexts) if (!c.token) {
            c={}; c.token=next++; c.status.size=sizeof(GbbStatus); c.status.version=GBB_API_VERSION;
            c.status.generation=1; c.status.state=GBB_STATE_READY; *token=c.token; return GBB_OK;
        }
        return GBB_E_CAPACITY;
    }
    inline int GBB_CALL Destroy(GbbContext token) { auto* c=Find(token); if (!c) return GBB_E_CONTEXT; ++c->destroys; if (refuseDestroy) return GBB_E_FAULT; c->token=0; return GBB_OK; }
    inline int GBB_CALL Publish(GbbContext token,const GbbFrame* frame) {
        auto* c=Find(token); if (!c) return GBB_E_CONTEXT;
        const auto index=static_cast<int>(c-contexts.data()); ++c->publishes;
        if (index==fail) return GBB_E_FAULT;
        for (const auto& other : contexts) {
            if (&other == c || !other.token || other.status.actor != frame->actor) continue;
            c->status.last_result = GBB_E_BINDING;
            std::snprintf(c->status.detail, sizeof(c->status.detail), "Actor double fingerprint aliases another context");
            return GBB_E_BINDING;
        }
        if (index!=stale) {
            c->status.actor=frame->actor; c->status.injected=1; c->status.writes=frame->bone_count;
            c->status.state=GBB_STATE_INJECTING; c->status.source_frame=frame->unity_frame;
            c->status.clip_time=frame->clip_time; ++c->status.sequence;
        }
        return GBB_OK;
    }
    inline int GBB_CALL Clear(GbbContext token,uint32_t) {
        auto* c=Find(token); if (!c) return GBB_E_CONTEXT; ++c->clears;
        if (refuseClear) return GBB_OK;
        const auto generation=c->status.generation+1; c->status={}; c->status.size=sizeof(GbbStatus);
        c->status.version=GBB_API_VERSION; c->status.generation=generation; c->status.state=GBB_STATE_READY; return GBB_OK;
    }
    inline int GBB_CALL Status(GbbContext token,GbbStatus* out) { auto* c=Find(token); if (!c) return GBB_E_CONTEXT; *out=c->status; return GBB_OK; }
    inline int GBB_CALL Perf(GbbContext token,GbbPerf* out) {
        auto* c=Find(token); if (!c) return GBB_E_CONTEXT; out->publish_calls=c->publishes; return GBB_OK;
    }
    inline int GBB_CALL Shutdown() { ++shutdowns; return GBB_OK; }
}
namespace GakuMotion {
    struct BurstBridgeMultiClientTestAccess {
        static void Install(BurstBridgeMultiClient& client) {
            using namespace MultiBridgeFixture;
            contexts={}; next=1; fail=stale=-1; refuseClear=refuseDestroy=false; shutdowns=0;
            client.api_={}; client.api_.size=sizeof(GbbApiV3); client.api_.version=GBB_API_VERSION_V3;
            client.api_.create_context=Create; client.api_.destroy_context=Destroy;
            client.api_.publish_context=Publish; client.api_.clear_context=Clear;
            client.api_.get_context_status=Status; client.api_.get_context_perf=Perf; client.api_.shutdown=Shutdown;
        }
    };
}
static void TestBurstBridgeMultiClient() {
    using namespace GakuMotion;
    using namespace MultiBridgeFixture;
    BeginTest("ensemble bridge client context isolation and confirmed cancellation");
    BurstBridgeMultiClient client; BurstBridgeMultiClientTestAccess::Install(client);
    const BurstBridgeMultiClient& constClient = client;
    Check(!constClient.State(4).token && !constClient.State(4).outstanding,
        "const State() returns an empty slot safely when out of range");
    IdolController ctrl;
    ctrl.fnGetLocalRotation=[](void*,float* q) { q[0]=q[1]=q[2]=0; q[3]=1; };
    ctrl.fnGetLocalPosition=[](void*,float* p) { p[0]=p[1]=p[2]=0; };
    std::array<IdolBoneBinding,5> bindings;
    for (size_t i=0;i<bindings.size();++i) {
        auto& b=bindings[i]; b.actor=reinterpret_cast<void*>(uintptr_t(i+1));
        b.cachedAnimator=reinterpret_cast<void*>(uintptr_t(i+11));
        b.boneTransforms={reinterpret_cast<void*>(0x20),reinterpret_cast<void*>(0x21)}; b.boneHids={0,10};
        Check(client.Publish(i,b,ctrl,1.f,60),"each actor consumes only its own confirmed native context");
    }
    const auto a=client.State(0).token,b=client.State(1).token,c=client.State(2).token;
    Check(a && b && c && a!=b && b!=c && a!=c,"three actors have distinct opaque context tokens");
    Check(!client.Publish(5,bindings[2],ctrl,1.f,60) && client.State(5).faulted &&
        client.State(5).reason.find("Actor double fingerprint aliases another context") != std::string::npos,
        "duplicate native ownership fails closed and keeps the bridge rejection detail");
    client.Clear(5);
    Check(client.State(3).token && client.State(4).token &&
        client.State(3).token != client.State(4).token && client.State(3).token != a && client.State(4).token != c,
        "additional actors acquire separate native contexts beyond the legacy three slots");
    fail=1;
    Check(!client.Publish(1,bindings[1],ctrl,1.1f,61) && client.State(1).faulted,"native failure latches only the matching actor");
    Check(!client.State(0).faulted && !client.State(2).faulted && Find(a)->status.actor==bindings[0].actor && Find(c)->status.actor==bindings[2].actor,
        "one actor's failure leaves the other stream inputs intact");
    Check(client.Publish(0,bindings[0],ctrl,1.1f,61) && client.Publish(2,bindings[2],ctrl,1.1f,61),"unaffected actors keep consuming native data");
    client.Clear(1); fail=-1;
    Check(!Find(b) && client.State(0).token==a && client.State(2).token==c,"clearing a slot destroys only its own token");
    Check(client.Publish(1,bindings[1],ctrl,1.2f,62) && client.State(1).token!=b,"a lifecycle reset negotiates a fresh context");
    stale=0;
    Check(!client.Publish(0,bindings[0],ctrl,2.f,120) && client.State(0).reason=="native_pending_stale" && !client.State(0).faulted,
        "stale native consumption falls back without resetting another context");
    stale=-1;
    Check(client.Publish(0,bindings[0],ctrl,2.f,121),"fresh native consumption recovers after a scheduling skip");
    refuseDestroy=true;
    Check(!client.Clear(2) && client.State(2).token==c && client.State(2).outstanding && Find(c),
        "failed destroy retains the exact context token and unconfirmed input evidence");
    refuseDestroy=false;
    Check(client.Clear(2) && !Find(c) && client.Publish(2,bindings[2],ctrl,2.f,121),
        "successful destroy retires the token before a new actor context can publish");
    refuseClear=true;
    Check(!client.ClearForCapture(0) && client.State(0).outstanding,"silent cancellation failure retains capture ownership");
    refuseClear=false;
    Check(client.ClearForCapture() && !client.HasCaptureInput(),"capture checks cancellation generation for every actor");
    Check(client.Shutdown() && shutdowns==1,"one module shutdown covers all contexts");
    Check(!client.Shutdown() && shutdowns==1,"repeated client shutdown never repeats the module call");
}

namespace EnsemblePhysicsRuntimeFixture {
#include "plugin/GkPluginContextAndPhysics.inl"

    static void TestSwappingNativeSlots() {
        using namespace GakuMotion;
        using namespace MultiBridgeFixture;
        BeginTest("ensemble native physics ownership follows formation slot changes");
        Core core;
        std::array<int, 4> actors{}, animators{};
        const std::vector<SceneActorObservation> roster{
            {&actors[0], 1}, {&actors[1], 2}, {&actors[2], 3}, {&actors[3], 4},
        };
        core.player.controller.fnGetLocalRotation = [](void*, float* q) {
            q[0] = q[1] = q[2] = 0.f; q[3] = 1.f;
        };
        core.player.controller.fnGetLocalPosition = [](void*, float* p) {
            p[0] = p[1] = p[2] = 0.f;
        };
        core.PublishSceneActors(roster);
        core.config.secondaryPhysicsBackend = 2;
        core.player.state = PlaybackState::Paused;
        for (size_t i = 0; i < actors.size(); ++i) {
            const auto& slot = core.formation.slots[i];
            core.ConfigureFormationTrackForSlot(slot, nullptr, false);
            auto& binding = core.TrackForSlot(static_cast<int>(i))->controller.bindings[&actors[i]];
            binding.actor = &actors[i];
            binding.cachedAnimator = &animators[i];
            binding.boneTransforms = {&actors[i]};
            binding.boneHids = {0};
            binding.bindPoseCaptured = true;
        }
        BurstBridgeMultiClientTestAccess::Install(g_multiBurstBridge);
        const auto publish = [&](size_t actorIndex, int frame) {
            DispatchEnsemblePhysics(core, &actors[actorIndex], frame, 1.f / 60.f, std::chrono::steady_clock::now());
        };
        for (size_t i = 0; i < actors.size(); ++i) publish(i, 60);
        const auto slot1 = g_multiBurstBridge.State(0).token;
        const auto slot2 = g_multiBurstBridge.State(1).token;
        const auto slot3 = g_multiBurstBridge.State(2).token;
        const auto slot4 = g_multiBurstBridge.State(3).token;
        Check(slot1 && slot2 && slot3 && slot4, "every occupied slot publishes a native context");

        Check(core.SwapSlots(1, 2), "swap the second and third actors while paused");
        publish(2, 61);
        Check(!Find(slot2) && !Find(slot3) && g_ensemblePhysics[1].actual == 2,
            "both old contexts retire before the first displaced actor can publish");
        publish(1, 61);
        Check(g_ensemblePhysics[2].actual == 2 && g_multiBurstBridge.State(0).token == slot1 &&
            g_multiBurstBridge.State(3).token == slot4 && !g_capturePhysicsUnconfirmed,
            "both moved actors retain native physics and unaffected actors retain their contexts");

        core.PublishSceneActors({roster[0], roster[1], roster[2]});
        Check(core.formation.slots.size() == 3 && ReconcileEnsemblePhysicsSlots(core) && !Find(slot4),
            "roster reconciliation retires a removed overflow slot without its callback");

        Check(core.SwapSlots(1, 2), "restore the original formation");
        const auto priorToken = g_multiBurstBridge.State(1).token;
        refuseDestroy = true;
        publish(1, 62);
        Check(g_multiBurstBridge.State(1).token == priorToken && g_capturePhysicsUnconfirmed,
            "failed retirement keeps the old context and prevents an ambiguous publication");
        publish(0, 62);
        Check(g_multiBurstBridge.State(0).token == slot1 && g_multiBurstBridge.State(0).status.source_frame == 62,
            "an unchanged slot continues publishing while a displaced context awaits retirement");
        refuseDestroy = false;
        publish(1, 63);
        publish(2, 63);
        Check(!Find(priorToken) && g_ensemblePhysics[1].actual == 2 && g_ensemblePhysics[2].actual == 2,
            "successful retirement retry admits both actors");
        ClearSecondaryPhysics(true);
    }
}
