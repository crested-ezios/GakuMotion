#include "../src/GakuBurstBridge.cpp"
#include <cstdlib>
#include <thread>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
namespace gbb {
struct TestStream {
    uint32_t version{2},padding{}; uintptr_t pointers[6]{1,1,1,1,0,1};
    GbbQuaternion q[2]{{0,0,0,1},{0,0,0,1}};
    int writes{},positions{},solver{}; bool fail{};
};
struct TestRig {
    alignas(16) unsigned char job[1024]{};
    void* animator{};
};
static TestRig rigs[3];
static float Delta(void*) { return 1.f/60; }
static void Resolve(Handle* h,void* stream) { h->skeleton=static_cast<TestStream*>(stream)->fail ? -1 : h->index; }
static void Read(Handle* h,void* stream,GbbQuaternion* q) { *q=static_cast<TestStream*>(stream)->q[h->index]; }
static void Write(Handle* h,void* stream,GbbQuaternion* q) { auto& s=*static_cast<TestStream*>(stream); s.q[h->index]=*q; ++s.writes; }
static void Position(Handle*,void* stream,GbbVector3*) { ++static_cast<TestStream*>(stream)->positions; }
static void Solver(void*,void* stream) { ++static_cast<TestStream*>(stream)->solver; }
static void Bind(void*,void* transform,Handle* h,const void*) { h->version=1; h->index=static_cast<int>(reinterpret_cast<uintptr_t>(transform)-10); }
static void* Rig(void* actor,void*,const void*) { return &rigs[reinterpret_cast<uintptr_t>(actor)-1]; }
static void* ObjectClass(void* object) {
    for (auto& rig:rigs) if (object==rig.job) return reinterpret_cast<void*>(2);
    return reinterpret_cast<void*>(1);
}
static void* Field(void*,const char* name) { return reinterpret_cast<void*>(uintptr_t(!std::strcmp(name,"_job") ? 1 : 2)); }
static uint32_t Flags(void*) { return 0; }
static void* Type(void*) { return reinterpret_cast<void*>(1); }
static int TypeCode(void*) { return 0x12; }
static void FieldRead(void* object,void* field,void* out) {
    auto& rig=*static_cast<TestRig*>(object);
    *static_cast<void**>(out)=reinterpret_cast<uintptr_t>(field)==1 ? rig.job : rig.animator;
}
static void* Unbox(void* object) { return object; }
static GbbFrame Frame(unsigned i,bool quartz=false) {
    GbbFrame f{}; f.size=sizeof(f); f.version=GBB_API_VERSION;
    f.actor=reinterpret_cast<void*>(static_cast<uintptr_t>(i+1)); f.animator=rigs[i].animator;
    f.flags=GBB_FRAME_INJECT | (quartz ? GBB_FRAME_BEFORE_QUARTZ : 0);
    f.bone_count=2; f.unity_frame=42; f.clip_time=1.f+i;
    for (int b=0;b<2;++b) {
        f.bones[b].transform=reinterpret_cast<void*>(static_cast<uintptr_t>(10+b));
        f.bones[b].humanoid_id=b ? 10 : 0; f.bones[b].local_rotation.w=1;
    }
    f.bones[1].local_rotation={0,0,0,0};
    (&f.bones[1].local_rotation.x)[i]=1;
    return f;
}
struct BridgeTestAccess {
    static void Setup(Bridge& b) {
        b.Clock(); b.initialized_=true; b.swingTarget_=reinterpret_cast<void*>(1);
        b.originalDelta_=&Delta; b.resolve_=&Resolve; b.readRotation_=&Read;
        b.writeRotation_=&Write; b.writePosition_=&Position; b.bind_=&Bind; b.getRig_=&Rig;
        b.jobClass_=reinterpret_cast<void*>(2); b.originalSwing_=&Solver;
        b.experimentReady_=true; b.quartzVariant_=&profile::QuartzVariants[0];
        b.meta_.objectClass=&ObjectClass; b.meta_.fieldFind=&Field; b.meta_.fieldFlags=&Flags;
        b.meta_.fieldType=&Type; b.meta_.typeCode=&TypeCode; b.meta_.fieldRead=&FieldRead; b.meta_.unbox=&Unbox;
        for (unsigned i=0;i<3;++i) {
            rigs[i]={}; rigs[i].animator=reinterpret_cast<void*>(static_cast<uintptr_t>(100+i));
            uintptr_t hair=200+i,arm=300+i;
            std::memcpy(rigs[i].job+profile::Hair,&hair,8); std::memcpy(rigs[i].job+profile::Arm,&arm,8);
        }
    }
    static void Run() {
        Bridge b; Setup(b);
        GbbContext tokens[3]{};
        for (auto& token:tokens) CHECK(b.CreateContext(&token)==GBB_OK && token);
        auto* stable=b.Context(tokens[0]);
        GbbContext extraTokens[5]{};
        for (auto& token:extraTokens) CHECK(b.CreateContext(&token)==GBB_OK && token && b.Context(tokens[0])==stable);
        for (auto token:extraTokens) CHECK(b.DestroyContext(token)==GBB_OK && !b.Context(token));
        GbbContext extra=0;
        auto* a=b.Context(tokens[0]); auto* c=b.Context(tokens[2]);
        auto fa=Frame(0),fb=Frame(1),fc=Frame(2);
        CHECK(b.PublishContext(tokens[0],&fa)==GBB_OK);
        const auto generation=a->generation_.load();
        CHECK(b.PublishContext(tokens[1],&fb)==GBB_OK && a->generation_==generation);
        CHECK(b.PublishContext(tokens[2],&fc)==GBB_OK && a->generation_==generation);
        for (unsigned i=0;i<3;++i) {
            auto* target=b.Context(tokens[i]); CHECK(b.RouteJob(rigs[i].job)==target);
            TestStream stream; Bridge::SwingDetour(rigs[i].job+profile::Swing,&stream);
            CHECK(stream.solver==1 && stream.writes==2 && (&stream.q[1].x)[i]==1);
            GbbStatus status{}; status.size=sizeof(status); status.version=GBB_API_VERSION;
            CHECK(b.ContextStatus(tokens[i],&status)==GBB_OK && status.injected && status.actor==Frame(i).actor && status.source_frame==42);
            GbbPerf perf{}; perf.size=sizeof(perf); perf.version=GBB_API_VERSION;
            CHECK(b.ContextPerf(tokens[i],&perf)==GBB_OK && perf.publish_calls==1 && perf.solver_calls==1);
        }
        // Foreign jobs forward without consuming another actor's pose.
        unsigned char unknown[1024]{}; TestStream stream;
        Bridge::SwingDetour(unknown+profile::Swing,&stream); CHECK(stream.solver==1 && !stream.writes);
        // One actor's actual stream failure does not quarantine the others.
        stream={}; stream.fail=true; Bridge::SwingDetour(rigs[1].job+profile::Swing,&stream);
        CHECK(b.Context(tokens[1])->fault_ && !a->fault_ && !c->fault_ && !stream.writes);
        stream={}; Bridge::SwingDetour(rigs[0].job+profile::Swing,&stream); CHECK(stream.writes==2);
        CHECK(b.ClearContext(tokens[1],GBB_CLEAR_RESET_FAULT)==GBB_OK && !b.Context(tokens[1])->fault_);
        CHECK(b.PublishContext(tokens[1],&fb)==GBB_OK);
        // The same two fingerprints cannot be published under two contexts.
        uintptr_t originalHair{},originalArm{};
        std::memcpy(&originalHair,rigs[2].job+profile::Hair,8); std::memcpy(&originalArm,rigs[2].job+profile::Arm,8);
        std::memcpy(rigs[2].job+profile::Hair,rigs[0].job+profile::Hair,8);
        std::memcpy(rigs[2].job+profile::Arm,rigs[0].job+profile::Arm,8);
        CHECK(b.PublishContext(tokens[2],&fc)==GBB_E_BINDING && !c->current_.count && a->current_.count);
        std::memcpy(rigs[2].job+profile::Hair,&originalHair,8); std::memcpy(rigs[2].job+profile::Arm,&originalArm,8);
        CHECK(b.PublishContext(tokens[2],&fc)==GBB_OK);
        // Explicit actor lifetimes: stale tokens and address recycling cannot
        // reuse a context. Destroy does not free a worker-visible runtime.
        const auto oldToken=tokens[1]; auto* old=b.Context(oldToken);
        const auto oldGeneration=old->generation_.load();
        uint64_t admitted{}; CHECK(b.RouteJob(rigs[1].job,&admitted)==old && admitted==oldGeneration);
        CHECK(b.DestroyContext(oldToken)==GBB_OK && !old->Observing() && old->generation_>oldGeneration);
        CHECK(!b.RouteJob(rigs[1].job) && b.ClearContext(oldToken,0)==GBB_E_CONTEXT);
        CHECK(b.CreateContext(&tokens[1])==GBB_OK && tokens[1]!=oldToken && b.Context(tokens[1])==old);
        CHECK(b.PublishContext(oldToken,&fb)==GBB_E_CONTEXT && b.PublishContext(tokens[1],&fb)==GBB_OK);
        stream={};
        CHECK(!old->BeforeSwing(rigs[1].job+profile::Swing,&stream,0,admitted) && !stream.writes && !old->fault_);
        CHECK(b.PublishContext(tokens[0],&fb)==GBB_E_CONTEXT && a->current_.actor==fa.actor);
        int threadResult{}; std::thread wrong([&] { threadResult=b.ClearContext(tokens[0],0); }); wrong.join();
        CHECK(threadResult==GBB_E_THREAD && a->current_.count);
        // Quartz pairs for A/B may coexist on the same worker. Publishing C
        // or clearing B cannot invalidate A's in-flight same-source pair.
        fa.flags|=GBB_FRAME_BEFORE_QUARTZ; fb.flags|=GBB_FRAME_BEFORE_QUARTZ;
        CHECK(b.PublishContext(tokens[0],&fa)==GBB_PENDING && b.PublishContext(tokens[1],&fb)==GBB_PENDING);
        auto* second=b.Context(tokens[1]);
        TestStream sa,sb;
        CHECK(!second->BeforeQuartz(rigs[1].job,&sb,second->quartzVariant_->quartzReturn,admitted) && !sb.writes && !second->fault_);
        CHECK(a->BeforeQuartz(rigs[0].job,&sa,a->quartzVariant_->quartzReturn));
        CHECK(second->BeforeQuartz(rigs[1].job,&sb,second->quartzVariant_->quartzReturn));
        CHECK(a->PairState().Finish(reinterpret_cast<uintptr_t>(a),a->generation_));
        CHECK(second->PairState().Finish(reinterpret_cast<uintptr_t>(second),second->generation_));
        CHECK(b.PublishContext(tokens[2],&fc)==GBB_OK);
        CHECK(a->BeforeSwing(rigs[0].job+profile::Swing,&sa,a->quartzVariant_->swingReturn) && sa.writes==4 && !a->fault_);
        CHECK(second->BeforeSwing(rigs[1].job+profile::Swing,&sb,second->quartzVariant_->swingReturn) && sb.writes==4 && !second->fault_);
        CHECK(a->PairState().phase==experiment::Phase::Empty && second->PairState().phase==experiment::Phase::Empty);
        CHECK(a->BeforeQuartz(rigs[0].job,&sa,a->quartzVariant_->quartzReturn));
        CHECK(a->PairState().Finish(reinterpret_cast<uintptr_t>(a),a->generation_));
        CHECK(b.ClearContext(tokens[1],0)==GBB_OK);
        CHECK(a->BeforeSwing(rigs[0].job+profile::Swing,&sa,a->quartzVariant_->swingReturn) && sa.writes==8);
        // Missing pair on another worker is forwarding-only and cannot borrow.
        CHECK(a->BeforeQuartz(rigs[0].job,&sa,a->quartzVariant_->quartzReturn));
        CHECK(a->PairState().Finish(reinterpret_cast<uintptr_t>(a),a->generation_));
        const auto before=sa.writes;
        std::thread worker([&] { CHECK(a->BeforeSwing(rigs[0].job+profile::Swing,&sa,a->quartzVariant_->swingReturn)); }); worker.join();
        CHECK(sa.writes==before && !a->fault_);
        CHECK(a->BeforeSwing(rigs[0].job+profile::Swing,&sa,a->quartzVariant_->swingReturn) && sa.writes==before+2);
        // A stale copied packet never writes after its own actor is cleared.
        auto snapshot=c->current_; CHECK(b.ClearContext(tokens[2],0)==GBB_OK);
        Diagnostic diagnostic; stream={}; CHECK(!c->NativePass(snapshot,&stream,diagnostic) && !stream.writes);
        CHECK(b.PublishContext(tokens[2],&fc)==GBB_OK);

        // The installed Motion keeps its diagnostic-compatible v2 master and
        // uses v3 for its two followers. Both APIs share the same detour owner.
        CHECK(b.DestroyContext(tokens[0])==GBB_OK);
        fa.flags=fb.flags=fc.flags=GBB_FRAME_INJECT;
        CHECK(b.Publish(&fa)==GBB_OK && b.PublishContext(tokens[1],&fb)==GBB_OK && b.PublishContext(tokens[2],&fc)==GBB_OK);
        CHECK(b.RouteJob(rigs[0].job)==&b && b.RouteJob(rigs[1].job)==second && b.RouteJob(rigs[2].job)==c);
        for (unsigned i=0;i<3;++i) {
            TestStream mixed; Bridge::SwingDetour(rigs[i].job+profile::Swing,&mixed);
            CHECK(mixed.solver==1 && mixed.writes==2 && (&mixed.q[1].x)[i]==1);
        }
        CHECK(b.Clear(0)==GBB_OK && !b.RouteJob(rigs[0].job) && b.RouteJob(rigs[1].job)==second && b.RouteJob(rigs[2].job)==c);
        CHECK(b.CreateContext(&tokens[0])==GBB_OK && b.PublishContext(tokens[0],&fa)==GBB_OK);
        a=b.Context(tokens[0]);
        snapshot=c->current_;
        std::thread stop([&] { CHECK(b.Shutdown()==GBB_OK); }); stop.join();
        CHECK(!c->NativePass(snapshot,&stream,diagnostic) && !stream.writes && !a->Observing());
        for (auto token:tokens) {
            GbbStatus status{}; status.size=sizeof(status); status.version=GBB_API_VERSION;
            CHECK(b.ContextStatus(token,&status)==GBB_OK && status.state==GBB_STATE_SHUTDOWN);
        }
        CHECK(b.CreateContext(&extra)==GBB_E_SHUTDOWN && b.ClearContext(tokens[0],0)==GBB_E_SHUTDOWN);
        Bridge::self_=&instance;
    }
};
}
int main() {
    gbb::BridgeTestAccess::Run();
    std::printf("Multi-context: %u checks passed (synthetic publish, worker routing and lifecycle; no live acceptance).\n",checks);
}
