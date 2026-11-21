#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/SleeveMuscleProbe.hpp"
#include <cstdlib>
#include <limits>
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static int checks, readerCalls;
static float Reader(void*,void*) { ++readerCalls; return .375f; }
namespace gbb {
struct SleeveProbeTestAccess {
    static void Run() {
        SleeveMuscleProbe p;
        // Real PE/unwind/call-site gate exercised on controlled memory; this
        // does not assert compatibility with the live game's metadata.
        auto* bytes=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x200000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE)); CHECK(bytes);
        auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(bytes); dos->e_magic=IMAGE_DOS_SIGNATURE; dos->e_lfanew=0x80;
        auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes+0x80); nt->Signature=IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64; nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.SizeOfImage=0x200000; bytes[0x180000]=1;
        RUNTIME_FUNCTION table[2]{};
        for (unsigned n=0;n<2;++n) {
            const auto& site=SleeveMuscleProbe::Sites[n]; table[n]={site.begin,site.end,0x180000};
            bytes[site.returned-6]=0xff; bytes[site.returned-5]=0x15;
            int32_t displacement=SleeveMuscleProbe::Slot-site.returned; std::memcpy(bytes+site.returned-4,&displacement,4);
            unsigned char args[]={0x48,0x8b,0x46,0x28,0x48,0x89,0x45,0x40,
                0x48,0x8d,0x4d,0xc8,0x48,0x8d,0x55,0x40};
            if (!n) args[11]=0xe8;
            std::memcpy(bytes+(n ? 0xc2ab0:0x1fbf1),args,sizeof(args));
        }
        auto* target=reinterpret_cast<void*>(&Reader); std::memcpy(bytes+SleeveMuscleProbe::Slot,&target,8);
        const auto base=reinterpret_cast<uintptr_t>(bytes); CHECK(RtlAddFunctionTable(table,2,base));
        p.burst_=reinterpret_cast<HMODULE>(bytes);
        for (const auto& site:SleeveMuscleProbe::Sites) {
            CHECK(p.VerifySite(site,target));
            bytes[site.returned-6]^=1; CHECK(!p.VerifySite(site,target)); bytes[site.returned-6]^=1;
            CHECK(!p.VerifySite(site,reinterpret_cast<void*>(1)));
        }
        p.site_=&SleeveMuscleProbe::Sites[1]; p.installed_=true;
        p.samples_.reset(new std::array<SleeveMuscleProbe::Sample,SleeveMuscleProbe::Capacity>);
        const auto caller=base+p.site_->returned;
        uintptr_t human=12345; int muscle[2]={4,2};
        p.Record(&human,muscle,.25f,caller); CHECK(p.captured_==0); // no natural Quartz scope
        p.Begin(reinterpret_cast<void*>(17),42,20,3,77,reinterpret_cast<void*>(19),reinterpret_cast<void*>(21),false);
        p.Record(&human,muscle,.25f,caller);
        CHECK(p.captured_==1 && p.samples_->at(0).value==.25f && !p.samples_->at(0).on);
        CHECK(p.samples_->at(0).actor==17 && p.samples_->at(0).sourceFrame==42 && p.samples_->at(0).sourceQpc==77);
        CHECK(p.samples_->at(0).humanPartDof==4 && p.samples_->at(0).dof==2 && p.samples_->at(0).ordinal==0);
        p.Record(&human,muscle,.5f,caller+1); CHECK(p.captured_==1 && p.rejected_==1);
        p.Record(&human,muscle,std::numeric_limits<float>::quiet_NaN(),caller); CHECK(p.failures_==1 && p.captured_==1);
        p.Record(nullptr,muscle,.25f,caller); CHECK(p.failures_==2 && p.captured_==1);
        p.End(); p.Record(&human,muscle,.25f,caller); CHECK(p.captured_==1);
        p.Begin(reinterpret_cast<void*>(17),43,20.01f,4,88,reinterpret_cast<void*>(19),reinterpret_cast<void*>(21),true);
        p.Record(&human,muscle,-.5f,caller);
        CHECK(p.captured_==2 && p.samples_->at(1).on && p.samples_->at(1).ordinal==0 && p.samples_->at(1).generation==4);
        // The hook forwards the original reader once, including unrelated
        // callers. No extra getter invocation is made to obtain diagnostics.
        SleeveMuscleProbe::self_=&p; p.original_=&Reader;
        CHECK(SleeveMuscleProbe::Detour(&human,muscle)==.375f && readerCalls==1 && p.captured_==2);
        p.End(); CHECK(SleeveMuscleProbe::Detour(&human,muscle)==.375f && readerCalls==2);
        p.Begin(reinterpret_cast<void*>(17),44,20.02f,5,99,reinterpret_cast<void*>(19),reinterpret_cast<void*>(21),false);
        for (size_t n=0;n<SleeveMuscleProbe::Capacity+1;++n) p.Record(&human,muscle,static_cast<float>(n),caller);
        p.End(); CHECK(p.captured_==SleeveMuscleProbe::Capacity+3);
        std::string json; p.AppendJson(json);
        CHECK(json.find("\"overwritten\":3")!=std::string::npos);
        CHECK(json.find("\"experiment_on\":false")!=std::string::npos);
        CHECK(json.find("\"source_frame\":44")!=std::string::npos && json.find("\"value\":8192")!=std::string::npos);
        SleeveMuscleProbe::self_=nullptr;
        CHECK(RtlDeleteFunctionTable(table)); CHECK(VirtualFree(bytes,0,MEM_RELEASE));
    }
};
}
int main() {
    gbb::SleeveProbeTestAccess::Run();
    std::printf("Sleeve probe: %d checks passed (controlled reader/PE gates, not live input evidence).\n",checks);
}
