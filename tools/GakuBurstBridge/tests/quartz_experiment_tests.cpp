#include "../src/GakuBurstBridge.cpp"
#include <cstdlib>
#include <thread>
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static int checks;
namespace gbb {
struct Stream {
    uint32_t version{2}, padding{}; uintptr_t pointers[6]{1,1,1,1,0,1};
    GbbQuaternion q[2]{{0,0,0,1},{0,0,0,1}};
    int writes{}, quartz{}, swing{}; bool fail{};
};
static float Delta(void*) { return .008333f; }
static void Resolve(Handle* h,void* p) { h->skeleton=static_cast<Stream*>(p)->fail ? -1 : h->index; }
static void Read(Handle* h,void* p,GbbQuaternion* q) { *q=static_cast<Stream*>(p)->q[h->index]; }
static void Write(Handle* h,void* p,GbbQuaternion* q) { auto* s=static_cast<Stream*>(p); s->q[h->index]=*q; ++s->writes; }
static void Position(Handle*,void*,GbbVector3*) {}
static void OriginalQuartz(void*,void* p) { ++static_cast<Stream*>(p)->quartz; }
static void OriginalSwing(void*,void* p) { ++static_cast<Stream*>(p)->swing; }
struct BridgeTestAccess {
    static void Run() {
        Bridge b; b.Clock(); b.originalDelta_=&Delta; b.resolve_=&Resolve; b.readRotation_=&Read;
        b.writeRotation_=&Write; b.writePosition_=&Position; b.originalQuartz_=&OriginalQuartz; b.originalSwing_=&OriginalSwing;
        b.experimentReady_=true; b.quartzVariant_=&profile::QuartzVariants[0];
        alignas(16) unsigned char job[1024]{};
        uintptr_t hair=123,arm=456; std::memcpy(job+profile::Hair,&hair,8); std::memcpy(job+profile::Arm,&arm,8);
        Snapshot source; source.count=2; source.hair=hair; source.arm=arm; source.frame=42; source.time=20;
        source.inject=true; source.beforeQuartz=true; source.generation=b.generation_.load(); source.ticks=Ticks();
        source.poses[0].handle.index=0; source.poses[0].hid=0; source.poses[0].rotation={0,0,0,1};
        source.poses[1].handle.index=1; source.poses[1].hid=10; source.poses[1].rotation={0,1,0,0};
        auto reset = [&] { b.Cancel(true); Bridge::pair_.Reset(); Bridge::warm_={}; source.generation=b.generation_.load(); source.ticks=Ticks(); b.current_=source; };
        auto before = [&](Stream& stream) { return b.BeforeQuartz(job,&stream,b.quartzVariant_->quartzReturn); };
        auto finish = [&] { return Bridge::pair_.Finish(reinterpret_cast<uintptr_t>(&b),b.generation_.load()); };
        auto swing = [&](Stream& stream) { return b.BeforeSwing(job+profile::Swing,&stream,b.quartzVariant_->swingReturn); };
        reset(); Stream s;
        CHECK(before(s) && s.writes==2 && Bridge::pair_.source.frame==42);
        // A newer main-thread publication must not change the second injection.
        b.current_.poses[1].rotation={1,0,0,0}; b.current_.frame=43;
        CHECK(finish()); s.q[1]={1,0,0,0}; CHECK(swing(s));
        CHECK(s.writes==4 && s.q[1].y==1 && b.diagnostic_.frame==42 && b.pairedPasses_==1);
        CHECK(Bridge::pair_.phase==experiment::Phase::Empty);
        const auto writes=s.writes; CHECK(swing(s) && !b.fault_ && s.writes==writes); // consume once, missing pair is forwarding-only
        CHECK(b.pairRejects_[static_cast<unsigned>(experiment::Reject::Missing)]==1);
        reset(); s={}; CHECK(swing(s) && !b.fault_ && s.writes==0 && b.warmupSkips_==1); // installed in-flight wrapper
        Bridge::warm_.started=Ticks()-b.frequency_.load(); CHECK(swing(s) && !b.fault_ && s.writes==0); // worker migration must not poison other workers
        CHECK(before(s) && finish() && swing(s) && !b.fault_ && s.writes==4);
        reset(); s={}; CHECK(before(s)); CHECK(!before(s) && b.fault_ && s.writes==2); // duplicate before Swing
        reset(); s={}; CHECK(before(s)); Bridge::pair_.source.ticks=Ticks()-b.frequency_.load();
        CHECK(!before(s) && b.fault_ && s.writes==2); // never retire an active, reentrant Quartz scope
        reset(); s={}; CHECK(before(s)); CHECK(swing(s) && b.fault_ && s.writes==2); // Quartz has not returned
        reset(); s={}; CHECK(before(s) && finish()); b.Cancel(true);
        source.generation=b.generation_; source.ticks=Ticks(); b.current_=source;
        CHECK(swing(s) && !b.fault_ && s.writes==2); // old callback cannot re-fault new Clear epoch
        CHECK(before(s) && finish() && swing(s) && !b.fault_ && s.writes==6);
        reset(); s={}; CHECK(before(s) && finish()); b.current_.ticks=Ticks();
        Bridge::pair_.source.ticks=Ticks()-b.frequency_.load(); CHECK(swing(s) && !b.fault_ && s.writes==2);
        b.current_.ticks=Ticks(); CHECK(before(s) && finish() && swing(s) && !b.fault_ && s.writes==6);
        // Main-thread hitch: Quartz sees an expired publication; the following
        // Swing may see a fresh publication but has no same-source pair.
        reset(); s={}; b.current_.ticks=Ticks()-b.frequency_.load();
        CHECK(!before(s) && !b.fault_ && s.writes==0);
        b.current_.ticks=Ticks(); CHECK(swing(s) && !b.fault_ && s.writes==0);
        CHECK(before(s) && finish() && swing(s) && !b.fault_ && s.writes==4);
        // A worker can retain a completed Quartz pair across a stall without
        // ever reaching Swing. A fresh Quartz must retire that expired pair.
        reset(); s={}; CHECK(before(s) && finish());
        Bridge::pair_.source.ticks=Ticks()-b.frequency_.load(); b.current_.ticks=Ticks();
        CHECK(before(s) && finish() && swing(s) && !b.fault_ && s.writes==6);
        // Repeated stalls do not accumulate a permanent fault; unsafe pairs
        // never perform the second write and need no lifecycle Clear.
        reset(); s={};
        for (int n=0;n<100;++n) {
            b.current_.ticks=Ticks(); CHECK(before(s) && finish());
            Bridge::pair_.source.ticks=Ticks()-b.frequency_.load();
            CHECK(swing(s) && !b.fault_ && s.writes==n*6+2);
            b.current_.ticks=Ticks(); CHECK(before(s) && finish() && swing(s) && !b.fault_ && s.writes==(n+1)*6);
        }
        reset(); s={}; CHECK(before(s) && finish());
        std::thread other([&]{ CHECK(swing(s)); }); other.join(); CHECK(!b.fault_ && s.writes==2); // cold thread forwards, never borrows another pair
        CHECK(swing(s) && !b.fault_ && s.writes==4); // original worker can still consume its own pair
        reset(); s={}; CHECK(before(s) && finish());
        auto* oldJob=Bridge::pair_.job ? job : nullptr; CHECK(oldJob==job);
        alignas(16) unsigned char clone[1024]{}; std::memcpy(clone,job,sizeof(job));
        CHECK(b.BeforeSwing(clone+profile::Swing,&s,b.quartzVariant_->swingReturn) && b.fault_ && s.writes==2);
        reset(); s={}; b.current_.arm=999; CHECK(!before(s) && !b.fault_ && s.writes==0);
        reset(); s={}; CHECK(!b.BeforeQuartz(job,&s,1) && b.fault_ && s.writes==0);
        reset(); s={}; s.fail=true; CHECK(!before(s) && b.fault_ && s.writes==0);
        b.PairRejected(experiment::Reject::Expired,true,source.generation);
        b.PairRejected(experiment::Reject::Missing,true,source.generation);
        CHECK(b.fault_ && s.writes==0); // scheduling skips never clear a real native fault
        reset(); s={}; CHECK(before(s) && finish()); CHECK(b.BeforeSwing(job+profile::Swing,&s,1) && b.fault_ && s.writes==2);
        reset(); s={}; CHECK(before(s) && finish()); CHECK(b.Shutdown()==GBB_OK);
        CHECK(!swing(s) && s.writes==2);
        // Both detours call their natural original once even after shutdown.
        Bridge::QuartzDetour(job,&s); Bridge::SwingDetour(job+profile::Swing,&s);
        CHECK(s.quartz==1 && s.swing==1 && s.writes==2);
        // Unsupported real caller still forwards the natural stages once.
        Bridge wrongCaller; wrongCaller.Clock(); wrongCaller.originalDelta_=&Delta;
        wrongCaller.originalQuartz_=&OriginalQuartz; wrongCaller.originalSwing_=&OriginalSwing;
        wrongCaller.experimentReady_=true; wrongCaller.quartzVariant_=&profile::QuartzVariants[0];
        source.generation=wrongCaller.generation_; source.ticks=Ticks(); wrongCaller.current_=source;
        s={}; Bridge::QuartzDetour(job,&s); Bridge::SwingDetour(job+profile::Swing,&s);
        CHECK(s.quartz==1 && s.swing==1 && s.writes==0 && wrongCaller.fault_);
        CHECK(wrongCaller.pairRejects_[static_cast<unsigned>(experiment::Reject::Caller)]==2);
        Bridge warmup; warmup.Clock(); warmup.originalQuartz_=&OriginalQuartz; warmup.originalSwing_=&OriginalSwing;
        warmup.quartzVariant_=&profile::QuartzVariants[0]; source.generation=warmup.generation_; source.ticks=Ticks(); warmup.current_=source;
        s={}; Bridge::QuartzDetour(job,&s); CHECK(s.quartz==1 && !warmup.fault_); // Quartz passed before Ready
        warmup.experimentReady_=true;
        CHECK(warmup.BeforeSwing(job+profile::Swing,&s,warmup.quartzVariant_->swingReturn) && !warmup.fault_ && s.writes==0);
        CHECK(warmup.AwaitExperimentPair() && warmup.state_==GBB_STATE_DISCOVERING && warmup.result_==GBB_PENDING);
        const auto started=warmup.experimentStarted_.load(); CHECK(warmup.AwaitExperimentPair() && warmup.experimentStarted_==started);
        warmup.experimentStarted_=Ticks()-warmup.frequency_.load()*3;
        CHECK(warmup.AwaitExperimentPair()==GBB_E_UNAVAILABLE && !warmup.failedInitialization_ && warmup.result_==GBB_E_UNAVAILABLE); // optional experiment cannot poison OFF
        // Restoring OFF immediately uses the original Swing path, without a
        // pair, Quartz hook, or matching wrapper caller.
        Bridge off; off.Clock(); off.originalDelta_=&Delta; off.resolve_=&Resolve; off.readRotation_=&Read;
        off.writeRotation_=&Write; off.writePosition_=&Position;
        source.beforeQuartz=false; source.generation=off.generation_; source.ticks=Ticks(); off.current_=source; s={};
        CHECK(!off.BeforeQuartz(job,&s,1) && s.writes==0);
        CHECK(off.BeforeSwing(job+profile::Swing,&s,1) && !off.fault_ && s.writes==2);
        CHECK(off.diagnostic_.frame==42);
        Bridge::self_=&instance;
    }
};
}
static void ProfileGuards() {
    using namespace gbb;
    auto* bytes=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x200000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE)); CHECK(bytes);
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(bytes); dos->e_magic=IMAGE_DOS_SIGNATURE; dos->e_lfanew=0x80;
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes+0x80); nt->Signature=IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64; nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage=0x200000; bytes[0x180000]=1;
    RUNTIME_FUNCTION table[4]{}; unsigned n{};
    for (const auto& v:profile::QuartzVariants) {
        table[n++]={v.wrapper,v.wrapperEnd,0x180000}; table[n++]={v.quartz,v.quartzEnd,0x180000};
        std::memcpy(bytes+v.quartz,v.prefix,sizeof(v.prefix));
        for (auto call : {std::pair{v.quartzReturn,v.quartz},std::pair{v.swingReturn,v.swing}}) {
            bytes[call.first-5]=0xe8; int32_t delta=call.second-call.first; std::memcpy(bytes+call.first-4,&delta,4);
        }
        std::memcpy(bytes+v.quartzReturn-5-v.quartzArgsSize,v.quartzArgs,v.quartzArgsSize);
        std::memcpy(bytes+v.swingReturn-5-v.swingArgsSize,v.swingArgs,v.swingArgsSize);
    }
    CHECK(RtlAddFunctionTable(table,4,reinterpret_cast<DWORD64>(bytes)));
    const auto base=reinterpret_cast<uintptr_t>(bytes);
    for (const auto& v:profile::QuartzVariants) {
        auto verify=[&]{return profile::VerifyQuartz(reinterpret_cast<HMODULE>(bytes),base+v.swing);};
        CHECK(verify()==&v);
        for (auto offset : {v.quartz, v.quartzReturn-1, v.swingReturn-1,
                           v.quartzReturn-5-v.quartzArgsSize, v.swingReturn-5-v.swingArgsSize}) {
            bytes[offset]^=1; CHECK(!verify()); bytes[offset]^=1;
        }
        CHECK(!profile::VerifyQuartz(reinterpret_cast<HMODULE>(bytes),base+v.swing+1));
    }
    nt->OptionalHeader.SizeOfImage=0x80000;
    CHECK(!profile::VerifyQuartz(reinterpret_cast<HMODULE>(bytes),base+profile::QuartzVariants[1].swing));
    CHECK(RtlDeleteFunctionTable(table)); CHECK(VirtualFree(bytes,0,MEM_RELEASE));
}
int main() {
    gbb::BridgeTestAccess::Run(); ProfileGuards();
    using gbb::experiment::TimelineBoundary;
    CHECK(!TimelineBoundary(42,20,43,20.01f)); CHECK(!TimelineBoundary(42,20,42,20));
    CHECK(TimelineBoundary(42,20,43,23)); CHECK(TimelineBoundary(42,20,43,19));
    CHECK(TimelineBoundary(42,20,41,20)); CHECK(TimelineBoundary(42,20,42,20.01f));
    std::printf("Quartz experiment: %d checks passed (offline synthetic gates, not live ABI/visual acceptance).\n",checks);
}
