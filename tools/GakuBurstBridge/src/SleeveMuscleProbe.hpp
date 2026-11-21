#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <intrin.h>
#include "Il2CppAccess.hpp"
#include "QuartzProfile.hpp"
#include "MinHook.h"

namespace gbb {
// Explicit read-only experiment instrumentation. The natural Unity reader is
// called exactly once; only its actual return value is retained. No muscle is
// derived from a Transform and no setter or extra Unity evaluation is called.
class SleeveMuscleProbe {
#if defined(GBB_TESTING)
    friend struct SleeveProbeTestAccess;
#endif
    using Reader = float(*)(void*, void*);
    static constexpr uint32_t Slot = 0x1584b0;
    struct Site { uint32_t begin, end, returned; };
    inline static constexpr Site Sites[] = {
        {0x1faf0,0x20ae5,0x1fc07}, {0xc29b0,0xc3bec,0xc2ac6}
    };
    struct Context {
        SleeveMuscleProbe* owner{};
        uintptr_t actor{}, job{}, stream{};
        uint64_t generation{}, sourceQpc{}, pass{};
        int sourceFrame{-1}; float clipTime{}; bool on{}; uint32_t ordinal{};
    };
    struct Sample {
        uint64_t sequence{}, pass{}, qpc{}, sourceQpc{}, generation{};
        uintptr_t actor{}, job{}, animationStream{}, humanStream{}, caller{};
        int sourceFrame{-1}, humanPartDof{}, dof{};
        float clipTime{}, value{}; uint32_t ordinal{}, thread{}; bool on{};
    };
    inline static SleeveMuscleProbe* self_{};
    inline static thread_local Context context_{};
    static constexpr size_t Capacity = 8192;
    Reader original_{}; void* target_{}; HMODULE burst_{};
    const char* gate_{"not requested"};
    const Site* site_{};
    std::atomic<bool> installed_{};
    std::atomic<uint64_t> passes_{}, rejected_{}, failures_{};
    std::mutex mutex_;
    std::unique_ptr<std::array<Sample, Capacity>> samples_;
    uint64_t captured_{};

    static uint64_t Qpc() noexcept { LARGE_INTEGER n{}; QueryPerformanceCounter(&n); return n.QuadPart; }
    static bool ReadArguments(void* human, void* muscle, uintptr_t& stream, int& part, int& dof) noexcept {
        __try {
            if (!human || !muscle) return false;
            std::memcpy(&stream,human,8);
            std::memcpy(&part,muscle,4);
            std::memcpy(&dof,static_cast<char*>(muscle)+4,4);
            return stream != 0;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool VerifySite(const Site& site, void* target) const noexcept {
        __try {
            const auto base = reinterpret_cast<uintptr_t>(burst_);
            if (site.end > profile::ImageSize(burst_) || !profile::ExactFunction(base,site.begin,site.end)) return false;
            const auto* call = reinterpret_cast<const unsigned char*>(base+site.returned-6);
            int32_t displacement{}; std::memcpy(&displacement,call+2,4);
            if (call[0]!=0xff || call[1]!=0x15 || base+site.returned+displacement!=base+Slot) return false;
            uintptr_t actual{}; std::memcpy(&actual,reinterpret_cast<void*>(base+Slot),8);
            if (actual != reinterpret_cast<uintptr_t>(target)) return false;
            // This actual Sleeve reader uses the JobBone's measured 0x28
            // forearmTwistMuscleHandle, then passes its eight bytes by address.
            unsigned char args[] = {0x48,0x8b,0x46,0x28,0x48,0x89,0x45,0x40,
                0x48,0x8d,0x4d,0xc8,0x48,0x8d,0x55,0x40};
            if (site.returned==0x1fc07) args[11]=0xe8;
            const uint32_t loadAt = site.returned==0xc2ac6 ? 0xc2ab0 : 0x1fbf1;
            return std::memcmp(reinterpret_cast<void*>(base+loadAt),args,sizeof(args))==0;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool Caller(uintptr_t returned) const noexcept {
        if (!site_ || returned != reinterpret_cast<uintptr_t>(burst_)+site_->returned) return false;
        // The entry may be detoured elsewhere, but the measured natural call
        // site and unwind range must still identify the exact Sleeve body.
        __try {
            const auto base = reinterpret_cast<uintptr_t>(burst_);
            return profile::ExactFunction(base,site_->begin,site_->end);
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void Record(void* human, void* muscle, float value, uintptr_t caller) noexcept {
        const auto context = context_;
        if (context.owner != this) return;
        if (!Caller(caller)) { rejected_.fetch_add(1,std::memory_order_relaxed); return; }
        Sample sample{};
        if (!ReadArguments(human,muscle,sample.humanStream,sample.humanPartDof,sample.dof) || !std::isfinite(value)) {
            failures_.fetch_add(1,std::memory_order_relaxed); return;
        }
        sample.qpc=Qpc(); sample.sourceQpc=context.sourceQpc; sample.pass=context.pass;
        sample.generation=context.generation; sample.actor=context.actor; sample.job=context.job;
        sample.animationStream=context.stream; sample.caller=caller-reinterpret_cast<uintptr_t>(burst_);
        sample.sourceFrame=context.sourceFrame; sample.clipTime=context.clipTime; sample.on=context.on;
        sample.ordinal=context_.ordinal++; sample.thread=GetCurrentThreadId(); sample.value=value;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            sample.sequence=++captured_; (*samples_)[(sample.sequence-1)%Capacity]=sample;
        } catch (...) { failures_.fetch_add(1,std::memory_order_relaxed); }
    }
    static float Detour(void* human, void* muscle) {
        const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
        auto* self=self_;
        // A hook is published only after MinHook supplied the trampoline.
        const float value=self->original_(human,muscle);
        self->Record(human,muscle,value,caller);
        return value;
    }
public:
    bool Install(Il2CppAccess& meta, HMODULE burst, const profile::QuartzVariant* variant) {
        if (installed_.load(std::memory_order_acquire)) return true;
        gate_="module, variant or singleton unavailable";
        if (!burst || !variant || (self_ && self_!=this)) return false;
        burst_=burst;
        site_=variant->swing==profile::Variants[1].begin ? &Sites[1] : &Sites[0];
        auto* human=meta.Class("UnityEngine.AnimationModule.dll","UnityEngine.Animations","AnimationHumanStream");
        auto* muscle=meta.Class("UnityEngine.AnimationModule.dll","UnityEngine.Animations","MuscleHandle");
        auto* sleeve=meta.Class("ActorAnimation.Runtime.dll","ActorAnimation","ActorAnimationQuartzDriverHumanoidSleeveJobBone");
        uint32_t align{};
        gate_="HumanStream, MuscleHandle or Sleeve metadata layout rejected";
        if (!human || !muscle || !sleeve || meta.valueSize(human,&align)!=8 || meta.valueSize(muscle,&align)!=8 ||
            !meta.Offset(human,"stream",0) || !meta.Offset(muscle,"<humanPartDof>k__BackingField",0) ||
            !meta.Offset(muscle,"<dof>k__BackingField",4) || !meta.Offset(sleeve,"forearmTwistMuscleHandle",0x28)) return false;
        target_=meta.icall("UnityEngine.Animations.AnimationHumanStream::InternalGetMuscle_Injected");
        gate_="actual reader icall unavailable";
        if (!profile::Executable(target_)) return false;
        gate_="Sleeve natural caller, unwind, argument bytes or slot rejected";
        if (!VerifySite(*site_,target_)) return false;
        // Allocate only on the main thread during explicit diagnostic setup.
        // Worker detours never allocate, and inert Bridge instances stay small.
        if (!samples_) samples_.reset(new(std::nothrow) std::array<Sample,Capacity>{});
        gate_="bounded sample buffer allocation failed";
        if (!samples_) return false;
        gate_="reader hook creation failed";
        if (MH_CreateHook(target_,reinterpret_cast<void*>(&Detour),reinterpret_cast<void**>(&original_))!=MH_OK) return false;
        self_=this;
        gate_="reader hook enable failed";
        if (MH_EnableHook(target_)!=MH_OK) { MH_RemoveHook(target_); self_=nullptr; original_=nullptr; return false; }
        gate_="verified actual Sleeve reader";
        installed_.store(true,std::memory_order_release); return true;
    }
    bool Installed() const noexcept { return installed_.load(std::memory_order_acquire); }
    void Begin(void* actor, int sourceFrame, float clipTime, uint64_t generation, uint64_t sourceQpc,
               void* job, void* stream, bool experimentOn) noexcept {
        context_={};
        if (!Installed()) return;
        context_.owner=this; context_.actor=reinterpret_cast<uintptr_t>(actor);
        context_.job=reinterpret_cast<uintptr_t>(job); context_.stream=reinterpret_cast<uintptr_t>(stream);
        context_.sourceFrame=sourceFrame; context_.clipTime=clipTime; context_.generation=generation;
        context_.sourceQpc=sourceQpc; context_.on=experimentOn;
        context_.pass=passes_.fetch_add(1,std::memory_order_relaxed)+1;
    }
    void End() noexcept { if (context_.owner==this) context_={}; }
    void AppendJson(std::string& json) {
        std::vector<Sample> copy; uint64_t captured{};
        {
            std::lock_guard<std::mutex> lock(mutex_); captured=captured_;
            const auto count=static_cast<size_t>(captured<Capacity ? captured : Capacity); copy.reserve(count);
            for (size_t n=0;n<count;++n) copy.push_back((*samples_)[(captured-count+n)%Capacity]);
        }
        char row[1200];
        std::snprintf(row,sizeof(row),",\n  \"sleeve_muscle_probe\":{\"installed\":%s,\"gate\":\"%s\",\"capacity\":%zu,\"captured\":%llu,\"overwritten\":%llu,\"passes\":%llu,\"other_reader_calls\":%llu,\"failures\":%llu,\"slot_rva\":\"0x1584b0\",\"semantics\":\"actual InternalGetMuscle_Injected return; natural reader called once\",\"samples\":[",
            Installed()?"true":"false",gate_,Capacity,static_cast<unsigned long long>(captured),
            static_cast<unsigned long long>(captured>Capacity?captured-Capacity:0),
            static_cast<unsigned long long>(passes_.load()),static_cast<unsigned long long>(rejected_.load()),
            static_cast<unsigned long long>(failures_.load())); json+=row;
        bool first=true;
        for (const auto& s:copy) {
            std::snprintf(row,sizeof(row),"%s{\"sequence\":%llu,\"pass\":%llu,\"qpc\":%llu,\"source_qpc\":%llu,\"generation\":%llu,\"actor\":\"0x%llx\",\"job\":\"0x%llx\",\"animation_stream\":\"0x%llx\",\"human_stream\":\"0x%llx\",\"caller_rva\":\"0x%llx\",\"source_frame\":%d,\"clip_time\":%.9g,\"experiment_on\":%s,\"ordinal\":%u,\"thread\":%u,\"human_part_dof\":%d,\"dof\":%d,\"value\":%.9g}",
                first?"":",",static_cast<unsigned long long>(s.sequence),static_cast<unsigned long long>(s.pass),
                static_cast<unsigned long long>(s.qpc),static_cast<unsigned long long>(s.sourceQpc),
                static_cast<unsigned long long>(s.generation),static_cast<unsigned long long>(s.actor),
                static_cast<unsigned long long>(s.job),static_cast<unsigned long long>(s.animationStream),
                static_cast<unsigned long long>(s.humanStream),static_cast<unsigned long long>(s.caller),s.sourceFrame,s.clipTime,
                s.on?"true":"false",s.ordinal,s.thread,s.humanPartDof,s.dof,s.value); json+=row; first=false;
        }
        json+="]}";
    }
};
} // namespace gbb
