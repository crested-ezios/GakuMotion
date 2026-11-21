#pragma once
#include "GkBurstBridgeMultiClient.hpp"
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
