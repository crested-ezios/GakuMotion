#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <array>
#include <deque>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <memory>
#include <string>
#include "GakuBurstBridge.h"
#include "MinHook.h"
#include "Il2CppAccess.hpp"
#include "QuartzExperiment.hpp"
#include "QuartzProfile.hpp"
#include "SleeveMuscleProbe.hpp"
#include "NativeListObservation.hpp"

namespace gbb {
static_assert(sizeof(void*) == 8, "Only measured Windows AMD64 is supported");
struct Handle { uint32_t version{}; int index{-1}, skeleton{-1}; };
static_assert(sizeof(Handle) == profile::HandleSize);
struct Pose { Handle handle; GbbQuaternion rotation{}; GbbVector3 position{}; int hid{-1}; };
struct Snapshot {
    void* actor{}; void* animator{}; void* rig{};
    uintptr_t hair{}, arm{}; uint64_t generation{}, ticks{};
    float time{}; int frame{-1}; bool inject{}, worldValid{}, beforeQuartz{}, sleeveDiagnostics{}; uint32_t count{};
    std::array<Pose, GBB_MAX_BONES> poses{};
    std::array<void*, GBB_MAX_BONES> transforms{};
    std::array<GbbWorldPose, 2> anchors{};
};
struct Diagnostic {
    uint64_t sequence{}; void* actor{}; int frame{-1}; float time{}, dt{};
    GbbQuaternion before{}, target{}, after{}; uint32_t writes{}; bool injected{}, worldValid{};
    std::array<GbbWorldPose,2> worldBefore{}, worldTarget{}, worldAfter{};
};
struct DeltaTrace {
    uintptr_t caller{}, begin{}, end{};
    uint64_t calls{};
    float first{}, minimum{}, maximum{};
    DWORD firstThread{}, lastThread{};
};
struct ActorTiming { uint64_t start{}, end{}; DWORD startThread{}, endThread{}; };
struct ActorTimingToken { uint64_t epoch{}; int index{-1}; };
inline uint64_t Ticks() { LARGE_INTEGER n{}; QueryPerformanceCounter(&n); return n.QuadPart; }
struct Timer {
    std::atomic<uint64_t> calls{}, total{}, last{}, maximum{};
    void Add(uint64_t ticks) noexcept {
        calls.fetch_add(1, std::memory_order_relaxed); total.fetch_add(ticks, std::memory_order_relaxed);
        last.store(ticks, std::memory_order_relaxed);
        auto old = maximum.load(std::memory_order_relaxed);
        while (old < ticks && !maximum.compare_exchange_weak(old, ticks, std::memory_order_relaxed)) {}
    }
};
bool ValidQuat(const GbbQuaternion& q) {
    float n = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    return std::isfinite(n) && n >= .5f && n <= 1.5f;
}
bool ValidVec(const GbbVector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool ReadKey(void* raw, int offset, uintptr_t& key) {
    __try {
        if (!raw || offset < 0 || offset > 4096) return false;
        std::memcpy(&key, static_cast<char*>(raw) + offset, sizeof(key)); return key != 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ValidStream(void* stream) {
    __try {
        struct Stream { uint32_t version, padding; uintptr_t pointers[6]; } s;
        static_assert(sizeof(Stream) == profile::StreamSize);
        if (!stream) return false;
        std::memcpy(&s, stream, sizeof(s));
        return s.version >= 2 && s.pointers[0] && s.pointers[1] && s.pointers[2] && s.pointers[3] && s.pointers[5];
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NotifyGuarded(GbbLogFn fn, void* context, int level, const char* message) {
    __try { fn(context, level, message); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string JsonString(const char* value) {
    if (!value) return "null";
    std::string out{"\""};
    for (const auto* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        if (*p == '\"' || *p == '\\') { out += '\\'; out += static_cast<char>(*p); }
        else if (*p < 0x20) {
            char escaped[7]; std::snprintf(escaped, sizeof(escaped), "\\u%04x", *p); out += escaped;
        } else out += static_cast<char>(*p);
    }
    out += '\"'; return out;
}

class Bridge {
#if defined(GBB_TESTING)
    friend struct BridgeTestAccess;
#endif
    using SwingFn = void(*)(void*, void*);
    using DeltaFn = float(*)(void*);
    using BindFn = void(*)(void*, void*, Handle*, const void*);
    using ResolveFn = void(*)(Handle*, void*);
    using QuatFn = void(*)(Handle*, void*, GbbQuaternion*);
    using PosFn = void(*)(Handle*, void*, GbbVector3*);
    using GetRigFn = void*(*)(void*, void*, const void*);
    Il2CppAccess meta_;
    void* rigType_{}; void* jobClass_{};
    const MethodInfo* bindInfo_{}; const MethodInfo* getRigInfo_{};
    BindFn bind_{}; GetRigFn getRig_{}; ResolveFn resolve_{};
    QuatFn readRotation_{}, writeRotation_{}, readWorldRotation_{};
    PosFn writePosition_{}, readWorldPosition_{};
    DeltaFn originalDelta_{}; SwingFn originalSwing_{};
    void* deltaTarget_{}; void* swingTarget_{};
    SwingFn originalQuartz_{};
    void* quartzTarget_{};
    const profile::QuartzVariant* quartzVariant_{};
    std::atomic<bool> experimentReady_{};
    std::atomic<uint64_t> quartzPasses_{}, quartzInjected_{}, pairedPasses_{};
    std::atomic<uint64_t> experimentGeneration_{}, experimentStarted_{}, pairedGeneration_{}, warmupSkips_{};
    Timer quartzNativeTimer_;
    std::array<std::atomic<uint64_t>, static_cast<unsigned>(experiment::Reject::Count)> pairRejects_{};
    static thread_local experiment::Pair<Snapshot> pair_;
    static thread_local experiment::WarmState warm_;
    static thread_local std::deque<experiment::Pair<Snapshot>> contextPairs_;
    static thread_local std::deque<experiment::WarmState> contextWarm_;
    SleeveMuscleProbe sleeveProbe_;
    HMODULE burst_{};
    uint32_t burstSize_{};
    std::atomic<uintptr_t> actualCaller_{};
    std::atomic<bool> observing_{true}, fault_{false}, stopped_{false};
    std::atomic<uint64_t> generation_{1};
    std::atomic<DWORD> mainThread_{};
    bool initialized_{}, failedInitialization_{}, discoveryTimedOut_{}, deltaEnabled_{}, metadataLoaded_{};
    bool readOnlyProbeActive_{}, readOnlyProbeSuspended_{};
    uint64_t discoveryStarted_{};
    std::atomic<uint64_t> frequency_{};
    std::mutex mutex_;
    Snapshot current_{}; Diagnostic diagnostic_{};
    // A bounded, forwarding-only actor observation. The worker never reads
    // a managed object: it compares two copied native job keys only.
    uintptr_t probeHair_{}, probeArm_{};
    void* probeActor_{};
    std::atomic<bool> actorProbeActive_{};
    uint64_t actorProbeMatches_{}, actorProbeOther_{}, actorProbeBadKey_{};
    uint64_t actorTimingEpoch_{};
    unsigned actorTimingCount_{};
    std::array<ActorTiming, 64> actorTimings_{};
    std::array<ListObservation,4> actorLists_{};
    inline static constexpr int ListOffsets[4]{88,96,104,176};
    inline static constexpr const char* ListNames[4]{"dynamicBones","staticBones","breastBones","chainBones"};
    std::array<ListObservation,4> PrepareListLayouts() {
        std::array<ListObservation,4> layouts{};
        if (!meta_.classFromType) return layouts;
        void* header = meta_.Class("Unity.Collections.dll", "Unity.Collections.LowLevel.Unsafe", "UntypedUnsafeList");
        void* swing = meta_.Class("ActorAnimation.Runtime.dll", "ActorAnimation", "ActorAnimationSwingJobSkeleton");
        uint32_t align{};
        if (!header || !swing || meta_.valueSize(header, &align) != sizeof(ListHeader) ||
            !meta_.Offset(header,"Ptr",0) || !meta_.Offset(header,"m_length",8,0x08) || !meta_.Offset(header,"m_capacity",12,0x08)) return layouts;
        const char* elements[]{"ActorSwingJobDynamicBone","ActorSwingJobStaticBone","ActorSwingJobBreastBone","ActorSwingJobChainBone"};
        for (unsigned i=0;i<4;++i) {
            void* f = meta_.Field(swing, ListNames[i], 0x15);
            void* list = f ? meta_.classFromType(meta_.fieldType(f)) : nullptr;
            void* element = meta_.Class("ActorAnimation.Runtime.dll","ActorAnimation",elements[i]);
            const int stride = element ? meta_.valueSize(element,&align) : 0;
            if (f && meta_.fieldOffset(f)-profile::BoxHeader == ListOffsets[i] && list &&
                meta_.valueSize(list,&align)==8 && meta_.Offset(list,"m_ListData",0) && stride>0 && stride<=65536) {
                layouts[i].layoutVerified = true; layouts[i].stride = static_cast<size_t>(stride);
            }
        }
        return layouts;
    }
    static bool ReadListHeader(void* swing, int offset, uintptr_t& container, ListHeader& header) {
        __try {
            std::memcpy(&container, static_cast<char*>(swing)+offset, sizeof(container));
            if (!container) return false;
            std::memcpy(&header, reinterpret_cast<void*>(container), sizeof(header)); return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void ObserveLists(void* swing) {
        for (unsigned i=0;i<4;++i) {
            auto& row=actorLists_[i]; if (!row.layoutVerified || row.samples+row.failures>=64) continue;
            uintptr_t container{}; ListHeader header{};
            if (!ReadListHeader(swing,ListOffsets[i],container,header)) { row.lastError=container?2:1; ++row.failures; continue; }
            if (!ValidListHeader(header,row.stride)) { row.lastError=3; ++row.failures; continue; }
            row.lastError=0;
            if (row.samples && (row.container!=container || row.payload!=header.payload || row.length!=header.length || row.capacity!=header.capacity)) ++row.changes;
            row.container=container; row.payload=header.payload; row.length=header.length; row.capacity=header.capacity; ++row.samples;
        }
    }
    std::array<DeltaTrace,32> traces_{};
    std::atomic<uint64_t> droppedTraces_{};
    uint32_t state_{GBB_STATE_UNINITIALIZED}; int result_{GBB_PENDING};
    char detail_[192]{"Waiting for first main-thread publish"};
    char initializationFailure_[192]{"Native initialization rejected"};
    // The callback pair is protected by mutex_. A host must keep an already
    // borrowed callback's code/context alive while it can still execute;
    // shutdown cancels future callbacks, not a callback already in flight.
    GbbLogFn log_{}; void* logContext_{};
    std::atomic<uint64_t> matches_{}, failures_{}, swingCalls_{}, expired_{}, identitySkips_{}, probeCalls_{};
    Timer publishTimer_, nativeTimer_, solverTimer_;
    static Bridge* self_;
    Bridge* owner_{};
    int contextSlot_{-1};
    void* contextActor_{};
    struct ContextSlot { GbbContext token{}; std::unique_ptr<Bridge> runtime; };
    std::deque<ContextSlot> contexts_{};
    uint64_t nextContext_{1};

    experiment::Pair<Snapshot>& PairState() { if (contextSlot_ < 0) return pair_; if (contextPairs_.size() <= static_cast<size_t>(contextSlot_)) contextPairs_.resize(contextSlot_ + 1); return contextPairs_[contextSlot_]; }
    experiment::WarmState& WarmState() { if (contextSlot_ < 0) return warm_; if (contextWarm_.size() <= static_cast<size_t>(contextSlot_)) contextWarm_.resize(contextSlot_ + 1); return contextWarm_[contextSlot_]; }
    SleeveMuscleProbe& SleeveProbe() { return owner_ ? owner_->sleeveProbe_ : sleeveProbe_; }
    bool Observing() const {
        return observing_.load(std::memory_order_acquire) && !stopped_.load(std::memory_order_acquire) &&
            (!owner_ || (owner_->observing_.load(std::memory_order_acquire) && !owner_->stopped_.load(std::memory_order_acquire)));
    }
    void CallbackFault(uint64_t admittedGeneration) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_.load(std::memory_order_acquire) ||
                (admittedGeneration && generation_.load(std::memory_order_acquire)!=admittedGeneration)) return;
            fault_.store(true,std::memory_order_release); PairState().Reset();
        } catch (...) {}
    }
    void CopyRuntime(const Bridge& source) {
        // Main-thread-only; worker callbacks are installed once by the owner.
        meta_=source.meta_; rigType_=source.rigType_; jobClass_=source.jobClass_;
        bindInfo_=source.bindInfo_; getRigInfo_=source.getRigInfo_; bind_=source.bind_; getRig_=source.getRig_;
        resolve_=source.resolve_; readRotation_=source.readRotation_; writeRotation_=source.writeRotation_;
        readWorldRotation_=source.readWorldRotation_; writePosition_=source.writePosition_; readWorldPosition_=source.readWorldPosition_;
        originalDelta_=source.originalDelta_; burst_=source.burst_; burstSize_=source.burstSize_;
        discoveryStarted_=source.discoveryStarted_;
        frequency_=source.frequency_.load(); mainThread_=source.mainThread_.load();
        quartzVariant_=source.quartzVariant_; experimentReady_=source.experimentReady_.load();
        metadataLoaded_=source.metadataLoaded_;
    }
    Bridge* Context(GbbContext token) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& slot:contexts_) if (token && slot.token==token) return slot.runtime.get();
        return nullptr;
    }
    Bridge* RouteJob(void* job,uint64_t* admittedGeneration=nullptr) {
        if (admittedGeneration) *admittedGeneration=0;
        if (!Observing()) return nullptr;
        uintptr_t hair{},arm{};
        if (!ReadKey(job,profile::Hair,hair) || !ReadKey(job,profile::Arm,arm)) return nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        Bridge* match{};
        auto consider=[&](Bridge* runtime,const Snapshot& source) {
            if (!runtime->Observing() || !source.count || source.hair!=hair || source.arm!=arm) return true;
            if (match) return false;
            match=runtime;
            if (admittedGeneration) *admittedGeneration=source.generation;
            return true;
        };
        if (!consider(this,current_)) return nullptr;
        for (auto& slot:contexts_) if (slot.token) {
            std::lock_guard<std::mutex> contextLock(slot.runtime->mutex_);
            if (!consider(slot.runtime.get(),slot.runtime->current_)) {
                identitySkips_.fetch_add(1,std::memory_order_relaxed); return nullptr;
            }
        }
        return match;
    }
    bool UniqueSnapshot(const Snapshot& next) {
        Bridge* root=owner_ ? owner_ : this;
        std::lock_guard<std::mutex> lock(root->mutex_);
        auto aliases=[&](const Snapshot& old) { return old.count && old.hair==next.hair && old.arm==next.arm; };
        if (root!=this && aliases(root->current_)) return false;
        for (auto& slot:root->contexts_) if (slot.token && slot.runtime.get()!=this) {
            std::lock_guard<std::mutex> contextLock(slot.runtime->mutex_);
            if (aliases(slot.runtime->current_)) return false;
        }
        return true;
    }

    bool MainThread(bool claim) {
        DWORD actual = GetCurrentThreadId(), expected = mainThread_.load(std::memory_order_acquire);
        if (!expected && claim) {
            mainThread_.compare_exchange_strong(expected, actual, std::memory_order_acq_rel);
            expected = mainThread_.load(std::memory_order_acquire);
        }
        return !expected || actual == expected;
    }
    void Clock() {
        if (frequency_) return;
        LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); frequency_ = f.QuadPart;
    }
    void State(uint32_t state, int result, const char* detail) {
        bool changed;
        GbbLogFn log{}; void* context{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_.load(std::memory_order_acquire)) return;
            changed = state_ != state || result_ != result || std::strcmp(detail_, detail);
            state_ = state; result_ = result;
            std::snprintf(detail_, sizeof(detail_), "%s", detail);
            log = log_; context = logContext_;
        }
        // Logging is main-thread-only and occurs outside the snapshot mutex.
        if (changed && log && !stopped_.load(std::memory_order_acquire)) {
            bool delivered{};
            try { delivered = NotifyGuarded(log, context, result < 0 ? 2 : 1, detail); }
            catch (...) {}
            if (!delivered) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (log_ == log && logContext_ == context) { log_ = nullptr; logContext_ = nullptr; }
            }
        }
    }
    void Cancel(bool resetFault) {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_.fetch_add(1, std::memory_order_release);
        if (resetFault) fault_.store(false, std::memory_order_release);
        current_ = {}; diagnostic_ = {};
    }
    bool LoadMetadata() {
        auto unavailable = [&](const char* reason) {
            std::snprintf(initializationFailure_, sizeof(initializationFailure_), "%s", reason); return false;
        };
        metadataLoaded_ = meta_.Load();
        if (!metadataLoaded_) return unavailable("Required GameAssembly IL2CPP exports unavailable");
        auto* stream = meta_.Class("UnityEngine.AnimationModule.dll", "UnityEngine.Animations", "AnimationStream");
        auto* handle = meta_.Class("UnityEngine.AnimationModule.dll", "UnityEngine.Animations", "TransformStreamHandle");
        auto* extensions = meta_.Class("UnityEngine.AnimationModule.dll", "UnityEngine.Animations", "AnimatorJobExtensions");
        auto* rig = meta_.Class("campus-submodule.Runtime.dll", "ActorAnimation", "CampusActorAnimationRig");
        auto* swing = meta_.Class("ActorAnimation.Runtime.dll", "ActorAnimation", "ActorAnimationSwingJobSkeleton");
        auto* component = meta_.Class("UnityEngine.CoreModule.dll", "UnityEngine", "Component");
        jobClass_ = meta_.Class("campus-submodule.Runtime.dll", "ActorAnimation", "CampusActorAnimationJob");
        if (!stream || !handle || !extensions || !rig || !swing || !jobClass_ || !component)
            return unavailable("Required AnimationStream, handle, rig or job metadata class missing");
        uint32_t align{};
        if (meta_.valueSize(stream, &align) != profile::StreamSize ||
            meta_.valueSize(handle, &align) != profile::HandleSize)
            return unavailable("AnimationStream or TransformStreamHandle value size changed");
        for (const auto& field : profile::StreamFields) if (!meta_.Offset(stream, field.name, field.offset)) {
            std::snprintf(initializationFailure_, sizeof(initializationFailure_), "AnimationStream field rejected: %s", field.name); return false;
        }
        for (const auto& field : profile::HandleFields) if (!meta_.Offset(handle, field.name, field.offset)) {
            std::snprintf(initializationFailure_, sizeof(initializationFailure_), "TransformStreamHandle field rejected: %s", field.name); return false;
        }
        if (!meta_.Offset(jobClass_, "_quartzDriverHairJobBones", profile::Hair, 0x15) ||
            !meta_.Offset(jobClass_, "_quartzDriverHumanoidArmJobBones", profile::Arm, 0x15) ||
            !meta_.Offset(jobClass_, "_swingJobSkeleton", profile::Swing, 0x11) ||
            !meta_.Offset(swing, "_time", profile::SwingTime, 0x0c))
            return unavailable("Campus job identity, Swing subobject or Swing time offset changed");
        rigType_ = meta_.typeObject(meta_.classType(rig));
        bindInfo_ = meta_.methodFind(extensions, "InternalBindStreamTransform", 3);
        getRigInfo_ = meta_.methodFind(component, "GetComponentInChildren", 1);
        bind_ = bindInfo_ ? reinterpret_cast<BindFn>(bindInfo_->pointer) : nullptr;
        getRig_ = getRigInfo_ ? reinterpret_cast<GetRigFn>(getRigInfo_->pointer) : nullptr;
        resolve_ = reinterpret_cast<ResolveFn>(meta_.icall("UnityEngine.Animations.TransformStreamHandle::ResolveInternal"));
        readRotation_ = reinterpret_cast<QuatFn>(meta_.icall("UnityEngine.Animations.TransformStreamHandle::GetLocalRotationInternal_Injected"));
        writeRotation_ = reinterpret_cast<QuatFn>(meta_.icall("UnityEngine.Animations.TransformStreamHandle::SetLocalRotationInternal_Injected"));
        writePosition_ = reinterpret_cast<PosFn>(meta_.icall("UnityEngine.Animations.TransformStreamHandle::SetLocalPositionInternal_Injected"));
        readWorldRotation_ = reinterpret_cast<QuatFn>(meta_.icall("UnityEngine.Animations.TransformStreamHandle::GetRotationInternal_Injected"));
        readWorldPosition_ = reinterpret_cast<PosFn>(meta_.icall("UnityEngine.Animations.TransformStreamHandle::GetPositionInternal_Injected"));
        deltaTarget_ = meta_.icall("UnityEngine.Animations.AnimationStream::GetDeltaTime");
        const void* required[] = {reinterpret_cast<void*>(bind_), reinterpret_cast<void*>(getRig_),
            reinterpret_cast<void*>(resolve_), reinterpret_cast<void*>(readRotation_),
            reinterpret_cast<void*>(writeRotation_), reinterpret_cast<void*>(writePosition_), deltaTarget_};
        for (auto* fn : required) if (!profile::Executable(fn)) return unavailable("Required bind, component method or native stream icall unavailable");
        if (!profile::Executable(reinterpret_cast<void*>(readWorldRotation_))) readWorldRotation_ = nullptr;
        if (!profile::Executable(reinterpret_cast<void*>(readWorldPosition_))) readWorldPosition_ = nullptr;
        return rigType_ != nullptr || unavailable("Rig managed System.Type unavailable");
    }
    bool Initialize() {
        if (owner_) {
            if (owner_->failedInitialization_ || (!owner_->initialized_ && !owner_->Initialize())) {
                owner_->failedInitialization_=true;
                std::snprintf(initializationFailure_,sizeof(initializationFailure_),"%s",owner_->initializationFailure_);
                return false;
            }
            CopyRuntime(*owner_); initialized_=true; return true;
        }
        Clock();
        if (!frequency_ || !LoadMetadata()) return false;
        burst_ = GetModuleHandleW(L"lib_burst_generated.dll");
        burstSize_ = profile::ImageSize(burst_);
        if (!burstSize_) { std::snprintf(initializationFailure_, sizeof(initializationFailure_), "lib_burst_generated.dll not loaded or PE image invalid"); return false; }
        std::snprintf(initializationFailure_, sizeof(initializationFailure_), "MinHook initialization, module pin or Delta hook failed");
        const auto status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
        // Keep trampolines and this module alive even if the optional client
        // unloads. Shutdown leaves hooks inert and does not reclaim worker code.
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&DeltaDetour), &pinned)) return false;
        if (MH_CreateHook(deltaTarget_, reinterpret_cast<void*>(&DeltaDetour), reinterpret_cast<void**>(&originalDelta_)) != MH_OK) return false;
        if (MH_EnableHook(deltaTarget_) != MH_OK) {
            MH_RemoveHook(deltaTarget_); originalDelta_ = nullptr; return false;
        }
        deltaEnabled_ = true; discoveryStarted_ = Ticks(); initialized_ = true;
        State(GBB_STATE_DISCOVERING, GBB_PENDING, "Metadata accepted; waiting for measured native Swing caller");
        return true;
    }
    static float DeltaDetour(void* stream) {
        auto* self = self_;
        const float result = self->originalDelta_(stream);
        if (self->observing_.load(std::memory_order_acquire)) {
            self->probeCalls_.fetch_add(1, std::memory_order_relaxed);
            const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
            CaptureTraceGuarded(self, caller, result);
            if (!self->actualCaller_.load(std::memory_order_relaxed) &&
                profile::VerifyCaller(caller, self->burst_, self->deltaTarget_)) {
                uintptr_t zero{};
                self->actualCaller_.compare_exchange_strong(zero, caller, std::memory_order_release);
            }
        }
        return result;
    }
    void CaptureTrace(uintptr_t caller, float dt) {
        const auto base = reinterpret_cast<uintptr_t>(burst_);
        if (!burstSize_ || caller < base || caller - base >= burstSize_) return;
        DWORD64 image{};
        const auto* function = RtlLookupFunctionEntry(caller, &image, nullptr);
        const uintptr_t begin = function && image == base ? function->BeginAddress : 0;
        const uintptr_t end = function && image == base ? function->EndAddress : 0;
        const uintptr_t rva = caller - base;
        const DWORD thread = GetCurrentThreadId();
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_.load(std::memory_order_acquire)) return;
        for (auto& trace : traces_) {
            if (trace.calls && trace.caller != rva) continue;
            if (!trace.calls) {
                trace.caller = rva; trace.begin = begin; trace.end = end;
                trace.first = trace.minimum = trace.maximum = dt; trace.firstThread = thread;
            }
            ++trace.calls; trace.lastThread = thread;
            if (std::isfinite(dt)) {
                if (!std::isfinite(trace.minimum) || dt < trace.minimum) trace.minimum = dt;
                if (!std::isfinite(trace.maximum) || dt > trace.maximum) trace.maximum = dt;
            }
            return;
        }
        droppedTraces_.fetch_add(1, std::memory_order_relaxed);
    }
    static void CaptureTraceCpp(Bridge* self, uintptr_t caller, float dt) noexcept {
        try { self->CaptureTrace(caller, dt); }
        catch (...) { self->droppedTraces_.fetch_add(1, std::memory_order_relaxed); }
    }
    static void CaptureTraceGuarded(Bridge* self, uintptr_t caller, float dt) {
        __try { CaptureTraceCpp(self, caller, dt); }
        __except(EXCEPTION_EXECUTE_HANDLER) { self->droppedTraces_.fetch_add(1, std::memory_order_relaxed); }
    }
    bool Discover() {
        if (owner_) {
            const bool discovered=owner_->Discover();
            if (discovered) swingTarget_=owner_->swingTarget_;
            return discovered;
        }
        if (swingTarget_) return true;
        const auto caller = actualCaller_.load(std::memory_order_acquire);
        if (!caller) return false;
        const auto verified = profile::VerifyCaller(caller, burst_, deltaTarget_);
        if (!verified) { failedInitialization_ = true; return false; }
        auto* target = reinterpret_cast<void*>(verified);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&SwingDetour), reinterpret_cast<void**>(&originalSwing_)) != MH_OK) {
            failedInitialization_ = true; return false;
        }
        if (MH_EnableHook(target) != MH_OK) {
            MH_RemoveHook(target); originalSwing_ = nullptr;
            failedInitialization_ = true; return false;
        }
        swingTarget_ = target;
        // Discovery is complete: remove the global GetDeltaTime overhead.
        // Its retained trampoline still serves NativePass's real stream read.
        if (deltaEnabled_ && MH_DisableHook(deltaTarget_) == MH_OK) deltaEnabled_ = false;
        State(GBB_STATE_READY, GBB_OK, "Verified native Swing hook ready; BeforeQuartz experiment OFF");
        return true;
    }
    // Only a discovery timeout is retryable. Metadata, hook installation,
    // validation and injection faults remain fail-closed.
    bool RetryTimedOutDiscovery(MH_STATUS (WINAPI *enableHook)(LPVOID)) {
        if (!discoveryTimedOut_ || !initialized_ || swingTarget_ || !deltaTarget_ ||
            !originalDelta_ || !enableHook || !frequency_) return false;
        const auto result = deltaEnabled_ ? MH_OK : enableHook(deltaTarget_);
        if (result != MH_OK && result != MH_ERROR_ENABLED) return false;
        deltaEnabled_ = true;
        discoveryStarted_ = Ticks();
        discoveryTimedOut_ = false;
        failedInitialization_ = false;
        return true;
    }
    bool EnableQuartzExperiment() {
        if (experimentReady_.load(std::memory_order_acquire)) return true;
        if (owner_) {
            if (!owner_->EnableQuartzExperiment()) return false;
            quartzVariant_=owner_->quartzVariant_;
            experimentReady_.store(true,std::memory_order_release); return true;
        }
        // This optional gate cannot disable the proven Swing-only OFF path.
        quartzVariant_ = profile::VerifyQuartz(burst_,reinterpret_cast<uintptr_t>(swingTarget_));
        if (!quartzVariant_) return false;
        if (!quartzTarget_) {
            quartzTarget_ = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(burst_) + quartzVariant_->quartz);
            if (MH_CreateHook(quartzTarget_,reinterpret_cast<void*>(&QuartzDetour),reinterpret_cast<void**>(&originalQuartz_)) != MH_OK) {
                quartzTarget_=nullptr; return false;
            }
        }
        if (MH_EnableHook(quartzTarget_) != MH_OK) return false;
        experimentReady_.store(true,std::memory_order_release);
        return true;
    }
    static bool BindGuarded(BindFn fn, void* animator, void* transform, Handle* handle, const void* info) {
        __try { fn(animator, transform, handle, info); return handle->version && handle->index >= 0; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool BuildSnapshot(const GbbFrame& frame, Snapshot& next) {
        void* rig = getRig_(frame.actor, rigType_, getRigInfo_);
        if (!rig) return false;
        void* klass = meta_.objectClass(rig);
        void* jf = meta_.Field(klass, "_job", 0x12), *af = meta_.Field(klass, "_animator", 0x12);
        if (!jf || !af) return false;
        void* boxed{}, *animator{};
        meta_.fieldRead(rig, jf, &boxed); meta_.fieldRead(rig, af, &animator);
        if (!boxed || animator != frame.animator || meta_.objectClass(boxed) != jobClass_) return false;
        next.actor = frame.actor; next.animator = animator; next.rig = rig;
        void* raw = meta_.unbox(boxed);
        if (!ReadKey(raw, profile::Hair, next.hair) || !ReadKey(raw, profile::Arm, next.arm)) return false;
        Snapshot old;
        { std::lock_guard<std::mutex> lock(mutex_); old = current_; }
        const bool same = old.actor == next.actor && old.animator == next.animator && old.rig == rig &&
                          old.hair == next.hair && old.arm == next.arm;
        unsigned anchors{};
        for (uint32_t i = 0; i < frame.bone_count; ++i) {
            const auto& b = frame.bones[i]; auto& p = next.poses[i]; p.hid = b.humanoid_id;
            if (same && i < old.count && old.transforms[i] == b.transform && old.poses[i].hid == p.hid) p.handle = old.poses[i].handle;
            else if (!BindGuarded(bind_, animator, b.transform, &p.handle, bindInfo_)) return false;
            p.rotation = b.local_rotation; p.position = b.local_position; next.transforms[i] = b.transform;
            if ((frame.flags & GBB_FRAME_WORLD_DIAGNOSTICS) && (b.flags & GBB_BONE_WORLD_VALID) &&
                (p.hid == 10 || p.hid == 0) && ValidQuat(b.world_rotation) && ValidVec(b.world_position)) {
                const int index = p.hid == 10 ? 0 : 1;
                next.anchors[index] = {b.world_rotation, b.world_position}; anchors |= 1u << index;
            }
        }
        next.count = frame.bone_count; next.worldValid = anchors == 3;
        next.time = frame.clip_time; next.frame = frame.unity_frame;
        next.inject = (frame.flags & GBB_FRAME_INJECT) != 0;
        next.beforeQuartz = (frame.flags & GBB_FRAME_BEFORE_QUARTZ) != 0; next.ticks = Ticks();
        next.sleeveDiagnostics = (frame.flags & GBB_FRAME_SLEEVE_DIAGNOSTICS) != 0;
        return true;
    }
    bool NativePass(Snapshot& s, void* stream, Diagnostic& d) {
        __try {
            if (!Observing() || !ValidStream(stream) ||
                generation_.load(std::memory_order_acquire) != s.generation) return false;
            d.dt = originalDelta_(stream);
            int head = -1, hips = -1;
            // All resolutions must succeed before the first write.
            for (uint32_t i = 0; i < s.count; ++i) {
                auto& p = s.poses[i]; resolve_(&p.handle, stream);
                if (p.handle.skeleton < 0) return false;
                if (p.hid == 10) head = static_cast<int>(i);
                if (p.hid == 0) hips = static_cast<int>(i);
            }
            if (head < 0 || hips < 0) return false;
            auto& h = s.poses[head]; readRotation_(&h.handle, stream, &d.before); d.target = h.rotation;
            d.worldValid = s.worldValid && readWorldRotation_ && readWorldPosition_;
            d.worldTarget = s.anchors;
            if (d.worldValid) {
                const int indices[] = {head, hips};
                for (int n = 0; n < 2; ++n) {
                    auto* handle = &s.poses[indices[n]].handle;
                    readWorldRotation_(handle, stream, &d.worldBefore[n].rotation);
                    readWorldPosition_(handle, stream, &d.worldBefore[n].position);
                }
            }
            d.injected = s.inject && !fault_.load(std::memory_order_acquire);
            if (!Observing() ||
                generation_.load(std::memory_order_acquire) != s.generation) { d.injected = false; return false; }
            if (d.injected) {
                for (uint32_t i = 0; i < s.count; ++i) {
                    auto& p = s.poses[i]; writeRotation_(&p.handle, stream, &p.rotation);
                    if (p.hid == 0) writePosition_(&p.handle, stream, &p.position);
                    ++d.writes;
                }
            }
            readRotation_(&h.handle, stream, &d.after);
            if (d.worldValid) {
                const int indices[] = {head, hips};
                for (int n = 0; n < 2; ++n) {
                    auto* handle = &s.poses[indices[n]].handle;
                    readWorldRotation_(handle, stream, &d.worldAfter[n].rotation);
                    readWorldPosition_(handle, stream, &d.worldAfter[n].position);
                }
            }
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void PairRejected(experiment::Reject reason, bool inject, uint64_t expectedGeneration) {
        PairState().Reset(); pairRejects_[static_cast<unsigned>(reason)].fetch_add(1,std::memory_order_relaxed);
        // A stalled main thread or a worker resuming without its old pair is
        // not a stream write fault. Forward this pass untouched and let the
        // next complete same-source pair recover, without clearing real faults.
        if (reason == experiment::Reject::Expired) expired_.fetch_add(1,std::memory_order_relaxed);
        if (reason == experiment::Reject::Expired || reason == experiment::Reject::Missing ||
            reason == experiment::Reject::Generation) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopped_.load(std::memory_order_acquire) && generation_.load(std::memory_order_acquire)==expectedGeneration) {
            failures_.fetch_add(1,std::memory_order_relaxed);
            if (inject) fault_.store(true,std::memory_order_release);
        }
    }
    bool BeforeQuartz(void* job, void* stream, uintptr_t caller,uint64_t admittedGeneration=0) {
        const auto bridgeStart=Ticks();
        if (!experimentReady_.load(std::memory_order_acquire) || !Observing()) return false;
        Snapshot s; { std::lock_guard<std::mutex> lock(mutex_); s = current_; }
        if (admittedGeneration && s.generation!=admittedGeneration) return false;
        if (!s.count || !s.beforeQuartz) { PairState().Reset(); return false; }
        uintptr_t hair{}, arm{};
        if (!ReadKey(job,profile::Hair,hair) || !ReadKey(job,profile::Arm,arm) || hair != s.hair || arm != s.arm) return false;
        struct Timing { Timer& timer; uint64_t start; ~Timing() { timer.Add(Ticks()-start); } } timing{quartzNativeTimer_,bridgeStart};
        if (!quartzVariant_ || caller != reinterpret_cast<uintptr_t>(burst_) + quartzVariant_->quartzReturn) {
            PairRejected(experiment::Reject::Caller,s.inject,s.generation); return false;
        }
        const auto now = Ticks();
        if (s.generation != generation_.load(std::memory_order_acquire)) { PairRejected(experiment::Reject::Generation,s.inject,s.generation); return false; }
        if (now < s.ticks || now - s.ticks >= frequency_ / 2) { PairRejected(experiment::Reject::Expired,s.inject,s.generation); return false; }
        // A worker may have completed Quartz before a stall but never reached
        // Swing. Retire only an expired pair; a fresh duplicate still faults.
        if (PairState().owner == reinterpret_cast<uintptr_t>(this) && PairState().phase == experiment::Phase::Ready &&
            PairState().source.generation == s.generation && PairState().Expired(now,frequency_ / 2))
            PairRejected(experiment::Reject::Expired,false,s.generation);
        if (!PairState().Begin(reinterpret_cast<uintptr_t>(this),reinterpret_cast<uintptr_t>(job),s,now)) {
            PairRejected(experiment::Reject::Duplicate,s.inject,s.generation); return false;
        }
        Diagnostic d; d.actor=s.actor; d.frame=s.frame; d.time=s.time;
        if (!NativePass(PairState().source,stream,d)) { PairRejected(experiment::Reject::Native,s.inject,s.generation); return false; }
        quartzPasses_.fetch_add(1,std::memory_order_relaxed);
        if (d.injected) quartzInjected_.fetch_add(1,std::memory_order_relaxed);
        return true;
    }
    static bool BeforeQuartzCpp(Bridge* target, void* job, void* stream, uintptr_t caller,uint64_t admittedGeneration) noexcept {
        try { return target && target->BeforeQuartz(job,stream,caller,admittedGeneration); }
        catch (...) { if (target) target->CallbackFault(admittedGeneration); return false; }
    }
    static bool BeforeQuartzGuarded(Bridge* target, void* job, void* stream, uintptr_t caller,uint64_t admittedGeneration) {
        __try { return BeforeQuartzCpp(target,job,stream,caller,admittedGeneration); }
        __except(EXCEPTION_EXECUTE_HANDLER) { if (target) target->CallbackFault(admittedGeneration); return false; }
    }
    bool BeginSleeveScope(void* job,void* stream,uintptr_t caller,bool paired,uint64_t admittedGeneration=0) {
        if (!Observing() || stopped_.load(std::memory_order_acquire)) return false;
        Snapshot source;
        if (paired) source=PairState().source;
        else { std::lock_guard<std::mutex> lock(mutex_); source=current_; }
        if (admittedGeneration && source.generation!=admittedGeneration) return false;
        if (!source.count || !source.sleeveDiagnostics || (source.beforeQuartz && !paired) ||
            !quartzVariant_ || caller != reinterpret_cast<uintptr_t>(burst_)+quartzVariant_->quartzReturn || !ValidStream(stream)) return false;
        const auto now=Ticks(); uintptr_t hair{},arm{};
        if (source.generation != generation_.load(std::memory_order_acquire) || now<source.ticks || now-source.ticks>=frequency_/2 ||
            !ReadKey(job,profile::Hair,hair) || !ReadKey(job,profile::Arm,arm) || hair!=source.hair || arm!=source.arm) return false;
        SleeveProbe().Begin(source.actor,source.frame,source.time,source.generation,source.ticks,job,stream,
            source.beforeQuartz && source.inject && !fault_.load(std::memory_order_acquire));
        return true;
    }
    static bool BeginSleeveScopeCpp(Bridge* target,void* job,void* stream,uintptr_t caller,bool paired,uint64_t admittedGeneration) noexcept {
        try { return target && target->BeginSleeveScope(job,stream,caller,paired,admittedGeneration); }
        catch (...) { return false; }
    }
    static bool BeginSleeveScopeGuarded(Bridge* target,void* job,void* stream,uintptr_t caller,bool paired,uint64_t admittedGeneration) {
        __try { return BeginSleeveScopeCpp(target,job,stream,caller,paired,admittedGeneration); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static void QuartzDetour(void* job, void* stream) {
        auto* self = self_;
        const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        uint64_t admittedGeneration{};
        auto* target=self->RouteJob(job,&admittedGeneration);
        const bool matched = BeforeQuartzGuarded(target,job,stream,caller,admittedGeneration);
        const auto generation = matched ? target->PairState().source.generation : 0;
        const bool probe=BeginSleeveScopeGuarded(target,job,stream,caller,matched,admittedGeneration);
        // Exactly one natural call; never move the engine stage under SEH.
        self->originalQuartz_(job,stream);
        if (probe) target->SleeveProbe().End();
        if (matched && !target->PairState().Finish(reinterpret_cast<uintptr_t>(target),target->generation_.load(std::memory_order_acquire)))
            target->PairRejected(experiment::Reject::Generation,false,generation);
    }
    bool BeforeSwing(void* swing, void* stream, uintptr_t caller = 0,uint64_t admittedGeneration=0) {
        const auto start = Ticks();
        swingCalls_.fetch_add(1, std::memory_order_relaxed);
        if (!Observing()) return false;
        Snapshot s;
        { std::lock_guard<std::mutex> lock(mutex_); s = current_; }
        if (admittedGeneration && s.generation!=admittedGeneration) return false;
        if (!s.count) return false;
        uintptr_t hair{}, arm{};
        void* job = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(swing) - profile::Swing);
        if (!ReadKey(job, profile::Hair, hair) || !ReadKey(job, profile::Arm, arm) || hair != s.hair || arm != s.arm) {
            identitySkips_.fetch_add(1, std::memory_order_relaxed); return false;
        }
        if (s.beforeQuartz) {
            if (!experimentReady_.load(std::memory_order_acquire)) return false;
            if (!quartzVariant_ || caller != reinterpret_cast<uintptr_t>(burst_) + quartzVariant_->swingReturn) {
                PairRejected(experiment::Reject::Caller,s.inject,s.generation); return true;
            }
            bool paired{};
            const auto reason = PairState().Check(reinterpret_cast<uintptr_t>(this),reinterpret_cast<uintptr_t>(job),
                generation_.load(std::memory_order_acquire),hair,arm,start,frequency_ / 2,paired);
            if (!paired) {
                WarmState().Prepare(reinterpret_cast<uintptr_t>(this),generation_.load(std::memory_order_acquire),start);
                // Enabling two detours can catch a wrapper already past Quartz.
                // Each worker gets a bounded forwarding-only first-pair window.
                if (reason == experiment::Reject::Missing && WarmState().MaySkipMissing(start,frequency_ / 2)) {
                    warmupSkips_.fetch_add(1,std::memory_order_relaxed); return true;
                }
                const auto rejectedGeneration = reason==experiment::Reject::Generation ? PairState().source.generation : s.generation;
                PairRejected(reason,s.inject,rejectedGeneration); return true;
            }
            s = PairState().source; PairState().Reset(); // Consume once before the first write.
        } else PairState().Reset();
        if (start < s.ticks || start - s.ticks >= frequency_ / 2) {
            expired_.fetch_add(1, std::memory_order_relaxed);
        } else {
            Diagnostic d; d.actor = s.actor; d.frame = s.frame; d.time = s.time;
            const bool ok = NativePass(s, stream, d);
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopped_.load(std::memory_order_acquire) &&
                generation_.load(std::memory_order_relaxed) == s.generation) {
                if (ok) { d.sequence = matches_.fetch_add(1, std::memory_order_relaxed) + 1; diagnostic_ = d;
                    if (s.beforeQuartz) {
                        pairedPasses_.fetch_add(1,std::memory_order_relaxed);
                        pairedGeneration_.store(s.generation,std::memory_order_release);
                        WarmState().Prepare(reinterpret_cast<uintptr_t>(this),s.generation,start); WarmState().paired=true;
                    }
                }
                else {
                    failures_.fetch_add(1, std::memory_order_relaxed);
                    if (s.inject) fault_.store(true, std::memory_order_release);
                }
            }
        }
        nativeTimer_.Add(Ticks() - start);
        return true; // Time original solver only for the two-key Actor match.
    }
    static bool BeforeSwingCpp(Bridge* target,void* swing, void* stream, uintptr_t caller,uint64_t admittedGeneration) noexcept {
        try { return target && target->BeforeSwing(swing, stream, caller,admittedGeneration); }
        catch (...) { if (target) target->CallbackFault(admittedGeneration); return false; }
    }
    static bool BeforeSwingGuarded(Bridge* target,void* swing, void* stream, uintptr_t caller,uint64_t admittedGeneration) {
        __try { return BeforeSwingCpp(target,swing, stream, caller,admittedGeneration); }
        __except(EXCEPTION_EXECUTE_HANDLER) { if (target) target->CallbackFault(admittedGeneration); return false; }
    }
    static void SwingDetour(void* swing, void* stream) {
        auto* self = self_;
        if (!self->observing_.load(std::memory_order_acquire)) {
            self->originalSwing_(swing, stream); return;
        }
        const auto actorTiming = ObserveActorSwingGuarded(self, swing);
        void* job=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(swing)-profile::Swing);
        uint64_t admittedGeneration{};
        auto* target=self->RouteJob(job,&admittedGeneration);
        const bool matched = BeforeSwingGuarded(target,swing, stream, reinterpret_cast<uintptr_t>(_ReturnAddress()),admittedGeneration);
        const uint64_t start = matched ? Ticks() : 0;
        // Never manually execute or skip the engine solver. Its own behavior
        // (including an engine fault) is outside the bridge exception boundary.
        self->originalSwing_(swing, stream);
        FinishActorTimingGuarded(self, actorTiming);
        if (matched) target->solverTimer_.Add(Ticks() - start);
    }
    int Reject(int result, const char* why) {
        Cancel(false);
        State(result == GBB_E_FAULT ? GBB_STATE_FAULTED : GBB_STATE_UNAVAILABLE, result, why);
        return result;
    }
public:
    explicit Bridge(bool detourOwner=true) { if (detourOwner) self_ = this; }
    int CreateContext(GbbContext* out) {
        if (!out) return GBB_E_ARGUMENT;
        *out=0;
        if (!MainThread(true)) return GBB_E_THREAD;
        if (stopped_.load(std::memory_order_acquire)) return GBB_E_SHUTDOWN;
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_.load(std::memory_order_acquire)) return GBB_E_SHUTDOWN;
        size_t available=0; while (available<contexts_.size() && contexts_[available].token) ++available;
        if (available==contexts_.size()) contexts_.emplace_back();
        for (size_t i=available;i<contexts_.size();++i) if (!contexts_[i].token) {
            auto& slot=contexts_[i];
            if (!nextContext_) return GBB_E_CAPACITY;
            if (!slot.runtime) {
                slot.runtime=std::make_unique<Bridge>(false);
                slot.runtime->owner_=this; slot.runtime->contextSlot_=static_cast<int>(i);
            }
            auto& runtime=*slot.runtime;
            runtime.Cancel(true); runtime.contextActor_=nullptr;
            runtime.observing_.store(true,std::memory_order_release);
            runtime.State(runtime.initialized_ ? GBB_STATE_READY : GBB_STATE_UNINITIALIZED,
                GBB_PENDING,"Actor context created; waiting for publication");
            slot.token=nextContext_++; *out=slot.token; return GBB_OK;
        }
        return GBB_E_CAPACITY;
    }
    int DestroyContext(GbbContext token) {
        if (!MainThread(false)) return GBB_E_THREAD;
        if (stopped_.load(std::memory_order_acquire)) return GBB_E_SHUTDOWN;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& slot:contexts_) if (token && slot.token==token) {
            slot.runtime->observing_.store(false,std::memory_order_release);
            slot.runtime->Cancel(false); slot.token=0; return GBB_OK;
        }
        return GBB_E_CONTEXT;
    }
    int PublishContext(GbbContext token,const GbbFrame* frame) {
        if (!MainThread(false)) return GBB_E_THREAD;
        if (stopped_.load(std::memory_order_acquire)) return GBB_E_SHUTDOWN;
        auto* runtime=Context(token); if (!runtime) return GBB_E_CONTEXT;
        if (frame && frame->size==sizeof(*frame) && frame->version==GBB_API_VERSION && ValidFrame(*frame)) {
            if (runtime->contextActor_ && runtime->contextActor_!=frame->actor) return GBB_E_CONTEXT;
            runtime->contextActor_=frame->actor;
        }
        const auto start=Ticks(); const int result=runtime->Publish(frame);
        runtime->PublishTiming(Ticks()-start); return result;
    }
    int ClearContext(GbbContext token,uint32_t flags) {
        if (!MainThread(false)) return GBB_E_THREAD;
        if (stopped_.load(std::memory_order_acquire)) return GBB_E_SHUTDOWN;
        auto* runtime=Context(token); return runtime ? runtime->Clear(flags) : GBB_E_CONTEXT;
    }
    int ContextStatus(GbbContext token,GbbStatus* out) {
        // Read-only callers may race a main-thread destroy/recreate. Keep token
        // admission and the copied result in one owner-lock interval.
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& slot:contexts_) if (token && slot.token==token) return slot.runtime->Status(out);
        return GBB_E_CONTEXT;
    }
    int ContextPerf(GbbContext token,GbbPerf* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& slot:contexts_) if (token && slot.token==token) return slot.runtime->Perf(out);
        return GBB_E_CONTEXT;
    }
    void ContextBoundaryFault(GbbContext token) noexcept {
        try { if (auto* runtime=Context(token)) runtime->BoundaryFault(); } catch (...) {}
    }
    ActorTimingToken ObserveActorSwing(void* swing) {
        if (!actorProbeActive_.load(std::memory_order_acquire)) return {};
        uintptr_t hair{}, arm{};
        void* job = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(swing) - profile::Swing);
        const bool readable = ReadKey(job, profile::Hair, hair) && ReadKey(job, profile::Arm, arm);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!probeActor_) return {};
        if (!readable) ++actorProbeBadKey_;
        else if (hair == probeHair_ && arm == probeArm_) {
            ++actorProbeMatches_;
            ObserveLists(swing);
            if (actorTimingCount_ < actorTimings_.size()) {
                const int index = static_cast<int>(actorTimingCount_++);
                actorTimings_[index] = {Ticks(), 0, GetCurrentThreadId(), 0};
                return {actorTimingEpoch_, index};
            }
        }
        else ++actorProbeOther_;
        return {};
    }
    void FinishActorTiming(ActorTimingToken token) {
        if (token.index < 0) return;
        const auto end = Ticks(); const auto thread = GetCurrentThreadId();
        std::lock_guard<std::mutex> lock(mutex_);
        if (token.epoch == actorTimingEpoch_ && static_cast<unsigned>(token.index) < actorTimingCount_) {
            auto& row = actorTimings_[token.index]; row.end = end; row.endThread = thread;
        }
    }
    static ActorTimingToken ObserveActorSwingCpp(Bridge* self, void* swing) noexcept {
        try { return self->ObserveActorSwing(swing); } catch (...) { return {}; }
    }
    static ActorTimingToken ObserveActorSwingGuarded(Bridge* self, void* swing) {
        __try { return ObserveActorSwingCpp(self, swing); } __except(EXCEPTION_EXECUTE_HANDLER) { return {}; }
    }
    static void FinishActorTimingCpp(Bridge* self, ActorTimingToken token) noexcept {
        try { self->FinishActorTiming(token); } catch (...) {}
    }
    static void FinishActorTimingGuarded(Bridge* self, ActorTimingToken token) {
        __try { FinishActorTimingCpp(self, token); } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    int ObserveActor(void* actor, void* animator) {
        if (!MainThread(false)) return GBB_E_THREAD;
        if (!readOnlyProbeActive_ || !actor || !animator) return GBB_E_UNAVAILABLE;
        { std::lock_guard<std::mutex> lock(mutex_); if (probeActor_) return probeActor_ == actor ? GBB_OK : GBB_E_BINDING; }
        if (!actualCaller_.load(std::memory_order_acquire)) return GBB_PENDING;
        void* rig = getRig_(actor, rigType_, getRigInfo_);
        if (!rig) return GBB_E_BINDING;
        void* klass = meta_.objectClass(rig);
        void* jf = meta_.Field(klass, "_job", 0x12), *af = meta_.Field(klass, "_animator", 0x12);
        if (!jf || !af) return GBB_E_BINDING;
        void* boxed{}, *rigAnimator{};
        meta_.fieldRead(rig, jf, &boxed); meta_.fieldRead(rig, af, &rigAnimator);
        if (!boxed || rigAnimator != animator || meta_.objectClass(boxed) != jobClass_) return GBB_E_BINDING;
        void* raw = meta_.unbox(boxed); uintptr_t hair{}, arm{};
        if (!ReadKey(raw, profile::Hair, hair) || !ReadKey(raw, profile::Arm, arm)) return GBB_E_BINDING;
        if (!Discover()) return failedInitialization_ ? GBB_E_UNAVAILABLE : GBB_PENDING;
        const auto layouts=PrepareListLayouts(); // main-thread metadata before publishing worker observation
        { std::lock_guard<std::mutex> lock(mutex_); probeActor_ = actor; probeHair_ = hair; probeArm_ = arm;
          actorProbeMatches_ = actorProbeOther_ = actorProbeBadKey_ = 0; actorLists_=layouts; }
        actorProbeActive_.store(true, std::memory_order_release);
        return GBB_OK;
    }
    int ReadActorObservation(void** actor, uint64_t* matched, uint64_t* other, uint64_t* bad) {
        if (!actor || !matched || !other || !bad) return GBB_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex_);
        *actor = probeActor_; *matched = actorProbeMatches_; *other = actorProbeOther_; *bad = actorProbeBadKey_;
        return probeActor_ ? GBB_OK : GBB_PENDING;
    }
    // Explicit short observation window: no actor snapshot or injection is
    // created, and the original GetDeltaTime result is always forwarded.
    int BeginReadOnlyProbe() {
        if (!MainThread(true)) return GBB_E_THREAD;
        if (stopped_) return GBB_E_SHUTDOWN;
        if (fault_.load(std::memory_order_acquire) || failedInitialization_ || readOnlyProbeActive_)
            return GBB_E_UNAVAILABLE;
        { std::lock_guard<std::mutex> lock(mutex_); if (current_.count) return GBB_E_UNAVAILABLE; }
        if (!initialized_ && !Initialize()) return GBB_E_UNAVAILABLE;
        if (!swingTarget_ && !deltaEnabled_) {
            const auto status = MH_EnableHook(deltaTarget_);
            if (status != MH_OK && status != MH_ERROR_ENABLED) return GBB_E_UNAVAILABLE;
            deltaEnabled_ = true;
        }
        { std::lock_guard<std::mutex> lock(mutex_); ++actorTimingEpoch_; actorTimingCount_ = 0; actorTimings_ = {}; actorLists_={}; }
        readOnlyProbeSuspended_ = false;
        readOnlyProbeActive_ = true;
        discoveryStarted_ = Ticks();
        State(GBB_STATE_DISCOVERING, GBB_PENDING, "Read-only caller observation active; no motion input");
        return GBB_OK;
    }
    int EndReadOnlyProbe() {
        if (!MainThread(false)) return GBB_E_THREAD;
        if (stopped_) return GBB_E_SHUTDOWN;
        if (!readOnlyProbeActive_) return GBB_OK;
        if (deltaEnabled_ && !swingTarget_) {
            const auto status = MH_DisableHook(deltaTarget_);
            if (status != MH_OK && status != MH_ERROR_DISABLED) return GBB_E_UNAVAILABLE;
            deltaEnabled_ = false;
            readOnlyProbeSuspended_ = true;
        }
        actorProbeActive_.store(false, std::memory_order_release);
        { std::lock_guard<std::mutex> lock(mutex_); probeActor_ = nullptr; probeHair_ = probeArm_ = 0; }
        readOnlyProbeActive_ = false;
        State(GBB_STATE_DISCOVERING, GBB_PENDING, "Read-only caller observation complete");
        return GBB_OK;
    }
    int Configure(const GbbConfig* config) {
        if (!config || config->size != sizeof(*config)) return GBB_E_ARGUMENT;
        if (config->version != GBB_API_VERSION) return GBB_E_VERSION;
        for (auto reserved : config->reserved) if (reserved) return GBB_E_ARGUMENT;
        if (!MainThread(false)) return GBB_E_THREAD;
        if (stopped_) return GBB_E_SHUTDOWN;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_.load(std::memory_order_acquire)) return GBB_E_SHUTDOWN;
            log_ = config->log; logContext_ = config->log_context;
        }
        Clock(); return GBB_OK;
    }
    static bool ValidFrame(const GbbFrame& f) {
        if (f.size != sizeof(f) || !f.actor || !f.animator || !f.bone_count || f.bone_count > GBB_MAX_BONES ||
            f.flags & ~(GBB_FRAME_INJECT | GBB_FRAME_WORLD_DIAGNOSTICS | GBB_FRAME_BEFORE_QUARTZ | GBB_FRAME_SLEEVE_DIAGNOSTICS) || !std::isfinite(f.clip_time)) return false;
        bool head{}, hips{};
        for (uint32_t i = 0; i < f.bone_count; ++i) {
            const auto& b = f.bones[i];
            if (!b.transform || b.humanoid_id < 0 || b.humanoid_id >= 64 ||
                b.flags & ~GBB_BONE_WORLD_VALID || !ValidQuat(b.local_rotation) || !ValidVec(b.local_position)) return false;
            for (uint32_t j = 0; j < i; ++j)
                if (f.bones[j].transform == b.transform || f.bones[j].humanoid_id == b.humanoid_id) return false;
            head |= b.humanoid_id == 10; hips |= b.humanoid_id == 0;
        }
        return head && hips;
    }
    int Publish(const GbbFrame* frame) {
        if (!MainThread(true)) return GBB_E_THREAD;
        if (stopped_) return GBB_E_SHUTDOWN;
        if (!frame || frame->size != sizeof(*frame)) return Reject(GBB_E_ARGUMENT, "Publish rejected: invalid structure size");
        if (frame->version != GBB_API_VERSION) return Reject(GBB_E_VERSION, "Publish rejected: unsupported API version");
        if (!ValidFrame(*frame)) return Reject(GBB_E_ARGUMENT, "Publish rejected: invalid bone pose or frame");
        if (failedInitialization_) return GBB_E_UNAVAILABLE;
        if (fault_.load(std::memory_order_acquire)) return Reject(GBB_E_FAULT, "Native stream fault latched; injection disabled until explicit reset");
        if (!initialized_ && !Initialize()) {
            failedInitialization_ = true;
            return Reject(GBB_E_UNAVAILABLE, initializationFailure_);
        }
        if (readOnlyProbeActive_) return GBB_PENDING;
        if (readOnlyProbeSuspended_) {
            if (!actualCaller_.load(std::memory_order_acquire)) {
                const auto status = MH_EnableHook(deltaTarget_);
                if (status != MH_OK && status != MH_ERROR_ENABLED)
                    return Reject(GBB_E_UNAVAILABLE, "Native observation hook could not resume");
                deltaEnabled_ = true;
            }
            readOnlyProbeSuspended_ = false;
            discoveryStarted_ = Ticks();
        }
        if (stopped_.load(std::memory_order_acquire)) return GBB_E_SHUTDOWN;
        Snapshot next;
        if (!BuildSnapshot(*frame, next)) return Reject(GBB_E_BINDING, "Actor rig or transform binding unavailable");
        if (!UniqueSnapshot(next)) return Reject(GBB_E_BINDING, "Actor double fingerprint aliases another context");
        // Managed calls above run on the main thread without this mutex.
        { std::lock_guard<std::mutex> lock(mutex_);
          if (stopped_.load(std::memory_order_acquire)) return GBB_E_SHUTDOWN;
          // Rebinding invalidates in-flight snapshots from the prior Actor or
          // hierarchy even if a host forgot an explicit Clear at the boundary.
          const bool changed = current_.count && (current_.actor != next.actor || current_.animator != next.animator ||
              current_.rig != next.rig || current_.hair != next.hair || current_.arm != next.arm ||
              current_.count != next.count || current_.transforms != next.transforms
              || current_.inject != next.inject || current_.beforeQuartz != next.beforeQuartz || current_.sleeveDiagnostics != next.sleeveDiagnostics ||
              (next.beforeQuartz && experiment::TimelineBoundary(current_.frame,current_.time,next.frame,next.time))
              );
          if (changed) { generation_.fetch_add(1, std::memory_order_release); diagnostic_ = {}; }
          next.generation = generation_.load(std::memory_order_relaxed); current_ = next; }
        if (!Discover()) {
            const bool timedOut = !failedInitialization_ && Ticks() - discoveryStarted_ > frequency_ * 10;
            if (failedInitialization_ || timedOut) {
                discoveryTimedOut_ = timedOut;
                failedInitialization_ = true;
                if (deltaEnabled_ && MH_DisableHook(deltaTarget_) == MH_OK) deltaEnabled_ = false;
                return Reject(GBB_E_UNAVAILABLE, "Native bridge unavailable: verified Swing caller or hook not available");
            }
            return GBB_PENDING;
        }
        const bool inject = (frame->flags & GBB_FRAME_INJECT) != 0;
        if (next.beforeQuartz || next.sleeveDiagnostics) {
            if (!EnableQuartzExperiment()) return Reject(GBB_E_UNAVAILABLE,"BeforeQuartz experiment rejected: verified entry unavailable; OFF path remains available");
            if (next.sleeveDiagnostics && !SleeveProbe().Install(meta_,burst_,quartzVariant_))
                return Reject(GBB_E_UNAVAILABLE,"Sleeve muscle diagnostics rejected: actual reader entry unavailable; diagnostic OFF remains available");
        }
        if (next.beforeQuartz) {
            const int awaiting=AwaitExperimentPair();
            if (awaiting != GBB_OK) return fault_.load(std::memory_order_acquire) ? GBB_E_FAULT : awaiting;
        }
        State(inject ? GBB_STATE_INJECTING : GBB_STATE_OBSERVING, GBB_OK,
              next.beforeQuartz ? "EXPERIMENTAL BeforeQuartz + Swing same-source injection enabled" :
              inject ? "Native stream injection enabled; BeforeQuartz experiment OFF" : "Native stream observation enabled");
        return GBB_OK;
    }
    int AwaitExperimentPair() {
        const auto generation=generation_.load(std::memory_order_acquire), now=Ticks();
        if (experimentGeneration_.load(std::memory_order_acquire) != generation) {
            experimentStarted_.store(now,std::memory_order_release);
            experimentGeneration_.store(generation,std::memory_order_release);
        }
        if (pairedGeneration_.load(std::memory_order_acquire) == generation) return GBB_OK;
        if (now-experimentStarted_.load(std::memory_order_acquire) >= frequency_*2) {
            return Reject(GBB_E_UNAVAILABLE,"EXPERIMENTAL before-Quartz did not form a complete pair within 2 seconds; fallback required");
        } else State(GBB_STATE_DISCOVERING,GBB_PENDING,"EXPERIMENTAL before-Quartz awaiting first complete pair; forwarding warm-up is not native success");
        return GBB_PENDING;
    }
    int Clear(uint32_t flags) {
        if (flags & ~GBB_CLEAR_RESET_FAULT) return GBB_E_ARGUMENT;
        if (!MainThread(false)) return GBB_E_THREAD;
        if (stopped_) return GBB_E_SHUTDOWN;
        Cancel((flags & GBB_CLEAR_RESET_FAULT) != 0);
        if ((flags & GBB_CLEAR_RESET_FAULT) && discoveryTimedOut_ && !fault_.load(std::memory_order_acquire))
            RetryTimedOutDiscovery(&MH_EnableHook);
        if (fault_.load(std::memory_order_acquire)) State(GBB_STATE_FAULTED, GBB_E_FAULT, "Native stream fault remains latched");
        else if (failedInitialization_) State(GBB_STATE_UNAVAILABLE, GBB_E_UNAVAILABLE, "Native profile unavailable for this process");
        else State(swingTarget_ ? GBB_STATE_READY : initialized_ ? GBB_STATE_DISCOVERING : GBB_STATE_UNINITIALIZED,
                   swingTarget_ ? GBB_OK : GBB_PENDING, "Published actor cleared");
        return GBB_OK;
    }
    int Shutdown() {
        // Host shutdown can run on a QuitGuard worker or under DLL detach.
        // Never call Unity, a host callback, or MinHook's thread suspension here.
        // Retained detours become forwarding-only until process exit.
        stopped_.store(true, std::memory_order_release);
        observing_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == GBB_STATE_SHUTDOWN) return GBB_OK;
            generation_.fetch_add(1, std::memory_order_release);
            current_ = {}; diagnostic_ = {};
            log_ = nullptr; logContext_ = nullptr;
            state_ = GBB_STATE_SHUTDOWN; result_ = GBB_E_SHUTDOWN;
            std::snprintf(detail_, sizeof(detail_), "%s", "Native bridge shut down; inert hooks and trampolines retained");
            for (auto& slot:contexts_) if (slot.runtime) slot.runtime->Shutdown();
        }
        return GBB_OK;
    }
    int Status(GbbStatus* out) {
        if (!out || out->size != sizeof(*out)) return GBB_E_ARGUMENT;
        if (out->version != GBB_API_VERSION) return GBB_E_VERSION;
        GbbStatus s{}; s.size = sizeof(s); s.version = GBB_API_VERSION;
        { std::lock_guard<std::mutex> lock(mutex_);
          s.state = state_; s.last_result = result_; const auto& d = diagnostic_;
          s.sequence = d.sequence; s.actor = d.actor; s.source_frame = d.frame; s.clip_time = d.time;
          s.delta_time = d.dt; s.writes = d.writes; s.injected = d.injected; s.world_valid = d.worldValid;
          s.head_before = d.before; s.head_target = d.target; s.head_after = d.after;
          std::memcpy(s.world_before, d.worldBefore.data(), sizeof(s.world_before));
          std::memcpy(s.world_target, d.worldTarget.data(), sizeof(s.world_target));
          std::memcpy(s.world_after, d.worldAfter.data(), sizeof(s.world_after));
          std::memcpy(s.detail, detail_, sizeof(s.detail)); }
        s.main_thread_id = mainThread_.load(); s.generation = generation_.load();
        s.matched_passes = matches_.load(); s.failed_passes = failures_.load(); s.swing_calls = swingCalls_.load();
        s.injection_fault = fault_.load(std::memory_order_acquire);
        if (s.injection_fault && s.state != GBB_STATE_SHUTDOWN) { s.state = GBB_STATE_FAULTED; s.last_result = GBB_E_FAULT; }
        std::snprintf(s.profile, sizeof(s.profile), "%s", profile::Name);
        *out = s; return GBB_OK;
    }
    uint64_t Ns(uint64_t ticks) const {
        const auto frequency = frequency_.load(std::memory_order_relaxed);
        return frequency ? (ticks / frequency) * 1000000000ull + (ticks % frequency) * 1000000000ull / frequency : 0;
    }
    void AppendAbiReport(std::string& json) {
        // Explicit DumpTraces only, on the Unity main thread. Initialization
        // may have rejected a layout: report real metadata anyway, without
        // enabling a hook, binding a Transform, or changing approval state.
        if (!metadataLoaded_) metadataLoaded_ = meta_.Load();
        const bool available = metadataLoaded_ && meta_.classFields && meta_.fieldName;
        json += ",\n  \"metadata_report_available\": "; json += available ? "true" : "false";
        json += ",\n  \"observation_source\": \"Explicit main-thread IL2CPP metadata enumeration; offsets are raw boxed offsets\"";
        json += ",\n  \"profile_assumptions\": {\"boxed_header_size\":" + std::to_string(profile::BoxHeader) + "}";
        json += ",\n  \"field_offsets_basis\": \"Unboxed field maps subtract the profile boxed-header assumption; this report does not independently prove that assumption\"";
        std::string observed = "{\"pointer_size\":" + std::to_string(sizeof(void*));
        std::string classes = "[";
        if (available) {
            struct Target { const char* assembly; const char* name; const char* space; const char* group; const char* sizeKey; };
            const Target targets[] = {
                {"UnityEngine.AnimationModule.dll","AnimationStream","UnityEngine.Animations","stream_fields_unboxed","stream_value_size"},
                {"UnityEngine.AnimationModule.dll","TransformStreamHandle","UnityEngine.Animations","handle_fields_unboxed","handle_value_size"},
                {"campus-submodule.Runtime.dll","CampusActorAnimationJob","ActorAnimation","job_fields_unboxed",nullptr},
                {"ActorAnimation.Runtime.dll","ActorAnimationSwingJobSkeleton","ActorAnimation","swing_fields_unboxed",nullptr},
                {"ActorAnimation.Runtime.dll","ActorSwingJobDynamicBone","ActorAnimation","dynamic_bone_fields_unboxed","dynamic_bone_value_size"},
                {"ActorAnimation.Runtime.dll","ActorSwingJobDynamicBoneCash","ActorAnimation","dynamic_cache_fields_unboxed","dynamic_cache_value_size"},
                {"ActorAnimation.Runtime.dll","ActorSwingJobStaticBone","ActorAnimation","static_bone_fields_unboxed","static_bone_value_size"},
                {"ActorAnimation.Runtime.dll","ActorSwingJobChainBone","ActorAnimation","chain_bone_fields_unboxed","chain_bone_value_size"},
                {"ActorAnimation.Runtime.dll","ActorSwingJobBreastBone","ActorAnimation","breast_bone_fields_unboxed","breast_bone_value_size"},
                {"ActorAnimation.Runtime.dll","ActorSwingJobBreastBoneSetting","ActorAnimation","breast_setting_fields_unboxed","breast_setting_value_size"},
                {"Unity.Collections.dll","UntypedUnsafeList","Unity.Collections.LowLevel.Unsafe","unsafe_list_fields_unboxed","unsafe_list_value_size"},
                {"UnityEngine.CoreModule.dll","JobHandle","Unity.Jobs","job_handle_core_fields_unboxed","job_handle_core_value_size"},
                {"Unity.Jobs.dll","JobHandle","Unity.Jobs","job_handle_jobs_fields_unboxed","job_handle_jobs_value_size"}
            };
            bool firstClass = true;
            for (const auto& target : targets) {
                void* klass = meta_.Class(target.assembly, target.space, target.name);
                if (!firstClass) classes += ','; firstClass = false;
                const std::string fullName = std::string(target.space) + "." + target.name;
                classes += "\n    {\"full_name\":" + JsonString(fullName.c_str()) + ",\"assembly\":" + JsonString(target.assembly);
                classes += ",\"available\":"; classes += klass ? "true" : "false";
                if (!klass) { classes += ",\"fields\":[]}"; continue; }
                uint32_t alignment{}; const int size = meta_.valueSize(klass, &alignment);
                classes += ",\"value_size\":" + std::to_string(size) + ",\"alignment\":" + std::to_string(alignment);
                if (target.sizeKey) observed += ',' + JsonString(target.sizeKey) + ':' + std::to_string(size);
                std::string offsets = "{";
                classes += ",\"fields\":[";
                void* iterator{}; uint32_t count{}; bool firstOffset = true;
                for (; count < 64; ++count) {
                    void* field = meta_.classFields(klass, &iterator);
                    if (!field) break;
                    const char* name = meta_.fieldName(field);
                    const int offset = meta_.fieldOffset(field);
                    const uint32_t flags = meta_.fieldFlags(field);
                    void* type = meta_.fieldType(field);
                    const int code = type ? meta_.typeCode(type) : -1;
                    std::unique_ptr<char, decltype(meta_.freeName)> typeName(
                        type && meta_.typeName ? meta_.typeName(type) : nullptr, meta_.freeName);
                    if (count) classes += ',';
                    classes += "{\"name\":" + JsonString(name) + ",\"type\":" + JsonString(typeName.get()) +
                        ",\"type_code\":" + std::to_string(code) + ",\"offset\":" + std::to_string(offset) +
                        ",\"raw_boxed_offset\":" + std::to_string(offset) + ",\"flags\":" + std::to_string(flags) +
                        ",\"static\":" + ((flags & 0x10) ? "true" : "false") + "}";
                    if (name && !(flags & 0x10) && offset >= profile::BoxHeader) {
                        if (!firstOffset) offsets += ','; firstOffset = false;
                        offsets += JsonString(name) + ':' + std::to_string(offset - profile::BoxHeader);
                    }
                }
                const bool truncated = count == 64 && meta_.classFields(klass, &iterator) != nullptr;
                classes += "],\"field_limit\":64,\"fields_truncated\":";
                classes += truncated ? "true}" : "false}";
                offsets += '}'; observed += ',' + JsonString(target.group) + ':' + offsets;
            }
        }
        observed += '}'; classes += "\n  ]";
        json += ",\n  \"observed\": " + observed + ",\n  \"abi_classes\": " + classes;
    }
    int Perf(GbbPerf* out) {
        if (!out || out->size != sizeof(*out)) return GBB_E_ARGUMENT;
        if (out->version != GBB_API_VERSION) return GBB_E_VERSION;
        GbbPerf p{}; p.size = sizeof(p); p.version = GBB_API_VERSION; p.qpc_frequency = frequency_;
#define GBB_TIMER(prefix, timer) \
        p.prefix##_calls = timer.calls.load(); p.prefix##_total_ns = Ns(timer.total.load()); \
        p.prefix##_last_ns = Ns(timer.last.load()); p.prefix##_max_ns = Ns(timer.maximum.load())
        GBB_TIMER(publish, publishTimer_);
        p.native_passes = nativeTimer_.calls.load(); p.native_total_ns = Ns(nativeTimer_.total.load());
        p.native_last_ns = Ns(nativeTimer_.last.load()); p.native_max_ns = Ns(nativeTimer_.maximum.load());
        GBB_TIMER(solver, solverTimer_);
#undef GBB_TIMER
        p.failed_passes = failures_.load(); p.expired_skips = expired_.load();
        p.identity_skips = identitySkips_.load(); p.delta_probe_calls = probeCalls_.load();
        *out = p; return GBB_OK;
    }
    int DumpTraces(const wchar_t* path) {
        if (!MainThread(false)) return GBB_E_THREAD;
        if (!path) return GBB_E_ARGUMENT;
        const auto length = std::wcslen(path);
        const bool drive = length >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
            (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
        const bool unc = length >= 5 && path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' && path[2] != L'.';
        if ((!drive && !unc) || length > 32700) return GBB_E_ARGUMENT;
        std::array<DeltaTrace,32> traces;
        std::array<ActorTiming,64> actorTimings{}; unsigned actorTimingCount{};
        std::array<ListObservation,4> actorLists{};
        Snapshot source;
        uint32_t state; int result;
        { std::lock_guard<std::mutex> lock(mutex_); traces = traces_; source = current_; state = state_; result = result_;
          actorTimings = actorTimings_; actorTimingCount = actorTimingCount_; actorLists=actorLists_; }
        std::string json;
        json.reserve(12000);
        char buffer[1024];
        std::snprintf(buffer, sizeof(buffer),
            "{\n  \"schema_version\": 1,\n  \"api_version\": %u,\n  \"profile\": \"%s\",\n"
            "  \"burst_image_base\": \"0x%llx\",\n  \"burst_image_size\": \"0x%x\",\n"
            "  \"state\": %u,\n  \"last_result\": %d,\n  \"trace_capacity\": 32,\n"
            "  \"dropped_traces\": %llu,\n  \"delta_probe_calls\": %llu,\n  \"traces\": [",
            GBB_API_VERSION, profile::Name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(burst_)),
            burstSize_, state, result, static_cast<unsigned long long>(droppedTraces_.load()),
            static_cast<unsigned long long>(probeCalls_.load()));
        json += buffer;
        bool first = true;
        for (const auto& trace : traces) {
            if (!trace.calls) continue;
            char values[3][48];
            const float floats[] = {trace.first, trace.minimum, trace.maximum};
            for (int i=0;i<3;++i) {
                if (std::isfinite(floats[i])) std::snprintf(values[i],sizeof(values[i]),"%.9g",floats[i]);
                else std::snprintf(values[i],sizeof(values[i]),"null");
            }
            std::snprintf(buffer,sizeof(buffer),
                "%s\n    {\"caller_rva\":\"0x%llx\",\"function_begin_rva\":\"0x%llx\",\"function_end_rva\":\"0x%llx\","
                "\"calls\":%llu,\"delta_first\":%s,\"delta_min\":%s,\"delta_max\":%s,\"first_thread\":%lu,\"last_thread\":%lu}",
                first ? "" : ",", static_cast<unsigned long long>(trace.caller), static_cast<unsigned long long>(trace.begin),
                static_cast<unsigned long long>(trace.end), static_cast<unsigned long long>(trace.calls),
                values[0],values[1],values[2],trace.firstThread,trace.lastThread);
            json += buffer; first = false;
        }
        json += "\n  ]";
        json += ",\n  \"actor_swing_timing\": {\"qpc_frequency\":" + std::to_string(frequency_.load()) + ",\"capacity\":64,\"samples\":[";
        for (unsigned i = 0; i < actorTimingCount; ++i) {
            const auto& row = actorTimings[i];
            std::snprintf(buffer, sizeof(buffer), "%s{\"start_qpc\":%llu,\"end_qpc\":%llu,\"start_thread\":%lu,\"end_thread\":%lu}",
                i ? "," : "", static_cast<unsigned long long>(row.start), static_cast<unsigned long long>(row.end), row.startThread, row.endThread);
            json += buffer;
        }
        json += "]}";
        json += ",\n  \"actor_swing_lists\":{\"scope\":\"matched natural Swing job before solver; headers only, no payload reads or writes\",\"jobs_complete\":false,\"lists\":[";
        for (unsigned i=0;i<4;++i) {
            const auto& r=actorLists[i];
            std::snprintf(buffer,sizeof(buffer),"%s{\"name\":\"%s\",\"layout_verified\":%s,\"container\":\"0x%llx\",\"payload\":\"0x%llx\",\"length\":%d,\"capacity\":%d,\"stride\":%llu,\"samples\":%llu,\"failures\":%llu,\"changes\":%llu,\"last_error\":%d}",
                i?",":"",ListNames[i],r.layoutVerified?"true":"false",static_cast<unsigned long long>(r.container),static_cast<unsigned long long>(r.payload),r.length,r.capacity,
                static_cast<unsigned long long>(r.stride),static_cast<unsigned long long>(r.samples),static_cast<unsigned long long>(r.failures),static_cast<unsigned long long>(r.changes),r.lastError);
            json+=buffer;
        }
        json+="]}";
        json += ",\n  \"before_quartz_experiment\": {\"compiled\":true,\"ready\":";
        json += experimentReady_.load() ? "true" : "false";
        json += ",\"enabled\":"; json += source.beforeQuartz ? "true" : "false";
        json += ",\"qpc_frequency\":" + std::to_string(frequency_.load()) +
            ",\"quartz_passes\":" + std::to_string(quartzPasses_.load()) +
            ",\"quartz_injected_passes\":" + std::to_string(quartzInjected_.load()) +
            ",\"paired_swing_passes\":" + std::to_string(pairedPasses_.load()) +
            ",\"warmup_skips\":" + std::to_string(warmupSkips_.load()) +
            ",\"quartz_bridge_calls\":" + std::to_string(quartzNativeTimer_.calls.load()) +
            ",\"quartz_bridge_total_ns\":" + std::to_string(Ns(quartzNativeTimer_.total.load())) +
            ",\"quartz_bridge_max_ns\":" + std::to_string(Ns(quartzNativeTimer_.maximum.load())) + ",\"rejected\":{";
        const char* reasons[] = {"missing","duplicate","phase","generation","identity","expired","caller","native"};
        for (unsigned n=0;n<static_cast<unsigned>(experiment::Reject::Count);++n) {
            if (n) json += ',';
            json += JsonString(reasons[n]) + ':' + std::to_string(pairRejects_[n].load());
        }
        json += "}}";
        sleeveProbe_.AppendJson(json);
        json += ",\n  \"actor_contexts\": [";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool firstContext=true;
            for (auto& slot:contexts_) if (slot.token) {
                auto& runtime=*slot.runtime;
                GbbStatus actorStatus{}; actorStatus.size=sizeof(actorStatus); actorStatus.version=GBB_API_VERSION;
                runtime.Status(&actorStatus);
                Snapshot actorSource;
                { std::lock_guard<std::mutex> contextLock(runtime.mutex_); actorSource=runtime.current_; }
                std::snprintf(buffer,sizeof(buffer),
                    "%s{\"token\":%llu,\"actor\":\"0x%llx\",\"generation\":%llu,\"state\":%u,\"injection_fault\":%u,"
                    "\"source_frame\":%d,\"hair_fingerprint\":\"0x%llx\",\"arm_fingerprint\":\"0x%llx\","
                    "\"matched_passes\":%llu,\"writes\":%u,\"before_quartz\":%s,\"quartz_passes\":%llu,\"paired_swing_passes\":%llu}",
                    firstContext ? "" : ",",static_cast<unsigned long long>(slot.token),
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(actorSource.actor)),
                    static_cast<unsigned long long>(actorStatus.generation),actorStatus.state,actorStatus.injection_fault,
                    actorStatus.source_frame,static_cast<unsigned long long>(actorSource.hair),
                    static_cast<unsigned long long>(actorSource.arm),static_cast<unsigned long long>(actorStatus.matched_passes),
                    actorStatus.writes,actorSource.beforeQuartz ? "true" : "false",
                    static_cast<unsigned long long>(runtime.quartzPasses_.load()),
                    static_cast<unsigned long long>(runtime.pairedPasses_.load()));
                json+=buffer; firstContext=false;
            }
        }
        json += "]";
        AppendAbiReport(json);
        json += "\n}\n";
        // Explicit main-thread export; no file I/O ever runs in a detour.
        HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return GBB_E_IO;
        DWORD written{};
        const bool ok = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) && written == json.size();
        const bool closed = CloseHandle(file) != 0;
        return ok && closed ? GBB_OK : GBB_E_IO;
    }
    void PublishTiming(uint64_t elapsed) noexcept { publishTimer_.Add(elapsed); }
    void BoundaryFault() noexcept {
        if (!stopped_.load(std::memory_order_acquire)) {
            fault_.store(true, std::memory_order_release); generation_.fetch_add(1, std::memory_order_release);
        }
    }
};
Bridge* Bridge::self_{};
thread_local experiment::Pair<Snapshot> Bridge::pair_{};
thread_local experiment::WarmState Bridge::warm_{};
thread_local std::deque<experiment::Pair<Snapshot>> Bridge::contextPairs_{};
thread_local std::deque<experiment::WarmState> Bridge::contextWarm_{};
Bridge instance;

// Separate C++ and SEH boundaries keep host ABI calls exception-free without
// depending on /EHa or putting destructible C++ objects in an __try function.
template<class F> int CppBoundary(F fn) noexcept {
    try { return fn(); }
    catch (...) { instance.BoundaryFault(); return GBB_E_FAULT; }
}
int GBB_CALL Configure(const GbbConfig* config) {
    __try { return CppBoundary([=] { return instance.Configure(config); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { instance.BoundaryFault(); return GBB_E_FAULT; }
}
int PublishInner(const GbbFrame* frame) {
    __try { return CppBoundary([=] { return instance.Publish(frame); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { instance.BoundaryFault(); return GBB_E_FAULT; }
}
int GBB_CALL Publish(const GbbFrame* frame) {
    const auto start = Ticks(); const int result = PublishInner(frame);
    instance.PublishTiming(Ticks() - start); return result;
}
int GBB_CALL Clear(uint32_t flags) {
    __try { return CppBoundary([=] { return instance.Clear(flags); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { instance.BoundaryFault(); return GBB_E_FAULT; }
}
int GBB_CALL Status(GbbStatus* status) {
    __try { return CppBoundary([=] { return instance.Status(status); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_ARGUMENT; }
}
int GBB_CALL Perf(GbbPerf* perf) {
    __try { return CppBoundary([=] { return instance.Perf(perf); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_ARGUMENT; }
}
int GBB_CALL Shutdown() {
    __try { return CppBoundary([] { return instance.Shutdown(); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { instance.BoundaryFault(); return GBB_E_FAULT; }
}
int DumpTracesCpp(const wchar_t* path) noexcept {
    try { return instance.DumpTraces(path); }
    catch (...) { return GBB_E_IO; }
}
int GBB_CALL DumpTraces(const wchar_t* path) {
    __try { return DumpTracesCpp(path); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_ARGUMENT; }
}
int GBB_CALL CreateContext(GbbContext* out) {
    __try { return CppBoundary([=] { return instance.CreateContext(out); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_ARGUMENT; }
}
template<class F> int ContextBoundary(GbbContext token,F fn) noexcept {
    try { return fn(); }
    catch (...) { instance.ContextBoundaryFault(token); return GBB_E_FAULT; }
}
template<class F> int ReadContextBoundary(F fn) noexcept {
    try { return fn(); } catch (...) { return GBB_E_ARGUMENT; }
}
int GBB_CALL DestroyContext(GbbContext token) {
    __try { return ContextBoundary(token,[=] { return instance.DestroyContext(token); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { instance.ContextBoundaryFault(token); return GBB_E_FAULT; }
}
int GBB_CALL PublishContext(GbbContext token,const GbbFrame* frame) {
    __try { return ContextBoundary(token,[=] { return instance.PublishContext(token,frame); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { instance.ContextBoundaryFault(token); return GBB_E_FAULT; }
}
int GBB_CALL ClearContext(GbbContext token,uint32_t flags) {
    __try { return ContextBoundary(token,[=] { return instance.ClearContext(token,flags); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { instance.ContextBoundaryFault(token); return GBB_E_FAULT; }
}
int GBB_CALL ContextStatus(GbbContext token,GbbStatus* out) {
    __try { return ReadContextBoundary([=] { return instance.ContextStatus(token,out); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_ARGUMENT; }
}
int GBB_CALL ContextPerf(GbbContext token,GbbPerf* out) {
    __try { return ReadContextBoundary([=] { return instance.ContextPerf(token,out); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_ARGUMENT; }
}
} // namespace gbb

extern "C" GBB_EXPORT int32_t GBB_CALL GakuBurstBridge_GetApi(uint32_t version, void* raw) {
    __try {
        if (!raw) return GBB_E_ARGUMENT;
        auto* api=static_cast<GbbApi*>(raw);
        if (version==GBB_API_VERSION_V3) {
            auto* api3=static_cast<GbbApiV3*>(raw);
            if (api3->size!=sizeof(*api3)) return GBB_E_ARGUMENT;
            if (api3->version!=GBB_API_VERSION_V3) return GBB_E_VERSION;
            const GbbApiV3 result{sizeof(GbbApiV3),GBB_API_VERSION_V3,&gbb::Configure,
                &gbb::CreateContext,&gbb::DestroyContext,&gbb::PublishContext,&gbb::ClearContext,
                &gbb::ContextStatus,&gbb::ContextPerf,&gbb::Shutdown,&gbb::DumpTraces};
            *api3=result; return GBB_OK;
        }
        if (api->size != sizeof(*api)) return GBB_E_ARGUMENT;
        if (version != GBB_API_VERSION || api->version != GBB_API_VERSION) return GBB_E_VERSION;
        const GbbApi result{sizeof(GbbApi), GBB_API_VERSION, &gbb::Configure, &gbb::Publish,
            &gbb::Clear, &gbb::Status, &gbb::Perf, &gbb::Shutdown, &gbb::DumpTraces};
        *api = result; return GBB_OK;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_ARGUMENT; }
}
extern "C" GBB_EXPORT int32_t GBB_CALL GakuBurstBridge_BeginReadOnlyProbe() {
    __try { return gbb::CppBoundary([] { return gbb::instance.BeginReadOnlyProbe(); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
}
extern "C" GBB_EXPORT int32_t GBB_CALL GakuBurstBridge_EndReadOnlyProbe() {
    __try { return gbb::CppBoundary([] { return gbb::instance.EndReadOnlyProbe(); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
}
extern "C" GBB_EXPORT int32_t GBB_CALL GakuBurstBridge_ObserveActor(void* actor, void* animator) {
    __try { return gbb::CppBoundary([&] { return gbb::instance.ObserveActor(actor, animator); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
}
extern "C" GBB_EXPORT int32_t GBB_CALL GakuBurstBridge_ReadActorObservation(void** actor, uint64_t* matched, uint64_t* other, uint64_t* bad) {
    __try { return gbb::CppBoundary([&] { return gbb::instance.ReadActorObservation(actor, matched, other, bad); }); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
}
