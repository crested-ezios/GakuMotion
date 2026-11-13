// Exercise the production resource-name detour at its IL2CPP/host boundary.
// A redirected string is not evidence that a scene finished loading.
#include "GakuPlugin.h"
#include "GkExactMethod.hpp"
#include "GkStageLoadPolicy.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace GakuMotion::host
{
    inline GakuHookService hooks{};
    template<class... Args> void Info(const char*, Args...) {}
    template<class... Args> void Warn(const char*, Args...) {}
}

namespace GakuMotion
{
    struct FakeStageManager
    {
        std::string selected;
        std::string GetLoadReplacementKey() const { return selected; }
    };
    struct Core
    {
        FakeStageManager stageManager;
        std::recursive_mutex stateMutex;
        static Core& Instance() { static Core value; return value; }
    };
}

namespace StageRuntimeHarness
{
    struct Il2CppString
    {
        void* klass{};
        void* monitor{};
        int32_t length{};
        char16_t chars[257]{};
    };
    inline std::vector<std::unique_ptr<Il2CppString>> strings;
    inline int allocations{}, failAllocation{};
    void* Text(std::string_view value)
    {
        auto result = std::make_unique<Il2CppString>();
        result->length = static_cast<int32_t>(value.size());
        for (size_t i = 0; i < value.size() && i < 257; ++i)
            result->chars[i] = static_cast<unsigned char>(value[i]);
        void* address = result.get();
        strings.push_back(std::move(result));
        return address;
    }
    void* NewString(const char* value)
    {
        ++allocations;
        return allocations == failAllocation ? nullptr : Text(value);
    }
    std::string Value(void* value)
    {
        if (!value) return {};
        const auto* str = static_cast<const Il2CppString*>(value);
        std::string result;
        for (int32_t i = 0; i < str->length; ++i) result += static_cast<char>(str->chars[i]);
        return result;
    }

    struct MetadataClass
    {
        const char* name{};
        bool valueType{};
        int size{};
        uint32_t alignment{};
    };
    struct MetadataType { int code{}; void* klass{}; bool byRef{}; };
    struct MetadataMethod
    {
        void* pointer{};
        const char* name{"GetEnvAssetId"};
        void* owner{};
        MetadataType returns{0x0e};
        std::array<MetadataType, 2> parameters{{{0x0e}, {0x11}}};
        uint32_t count{2}, flags{0x96};
        bool generic{}, inflated{};
    };
    inline MetadataClass ruleClass{"ABRule"}, photoClass{"PhotoBackground"},
        enumClass{"PhotoBackgroundTimeType", true, 4, 4}, otherClass{"Other", true, 4, 4};
    inline std::vector<MetadataClass*> nestedClasses;
    inline std::vector<MetadataMethod*> methods;
    const MetadataMethod& Method(const void* value) { return *static_cast<const MetadataMethod*>(value); }
    const void* Methods(void*, void** iterator)
    {
        const auto index = reinterpret_cast<uintptr_t>(*iterator);
        if (index >= methods.size()) return nullptr;
        *iterator = reinterpret_cast<void*>(index + 1);
        return methods[index];
    }
    const char* MethodName(const void* value) { return Method(value).name; }
    uint32_t MethodCount(const void* value) { return Method(value).count; }
    uint32_t MethodFlags(const void* value, uint32_t* implementation)
    { *implementation = 0; return Method(value).flags; }
    bool MethodGeneric(const void* value) { return Method(value).generic; }
    bool MethodInflated(const void* value) { return Method(value).inflated; }
    void* MethodClass(const void* value) { return Method(value).owner; }
    void* MethodParameter(const void* value, uint32_t index)
    { return index < 2 ? const_cast<MetadataType*>(&Method(value).parameters[index]) : nullptr; }
    void* MethodResult(const void* value) { return const_cast<MetadataType*>(&Method(value).returns); }
    int TypeCode(void* value) { return static_cast<MetadataType*>(value)->code; }
    bool TypeByRef(void* value) { return static_cast<MetadataType*>(value)->byRef; }
    void* TypeClass(void* value) { return static_cast<MetadataType*>(value)->klass; }
    const char* ClassName(void* value) { return static_cast<MetadataClass*>(value)->name; }
    bool ClassValueType(void* value) { return static_cast<MetadataClass*>(value)->valueType; }
    int ValueSize(void* value, uint32_t* alignment)
    { *alignment = static_cast<MetadataClass*>(value)->alignment; return static_cast<MetadataClass*>(value)->size; }
    void* NestedTypes(void* owner, void** iterator)
    {
        if (owner != &ruleClass) return nullptr;
        const auto index = reinterpret_cast<uintptr_t>(*iterator);
        if (index >= nestedClasses.size()) return nullptr;
        *iterator = reinterpret_cast<void*>(index + 1);
        return nestedClasses[index];
    }
    void* FindClass(const char* image, const char* namespaze, const char* name)
    {
        if (std::strcmp(image, "Assembly-CSharp.dll")) return nullptr;
        if (!std::strcmp(namespaze, "Campus.Common.AssetNames") && !std::strcmp(name, "ABRule")) return &ruleClass;
        if (!std::strcmp(namespaze, "Campus.Common.Proto.Client.Enums") &&
            !std::strcmp(name, "PhotoBackgroundTimeType")) return &enumClass;
        return nullptr;
    }
    namespace StageLoader
    {
        inline GakuMotion::ExactMethodMetadata metadata{};
        inline void* (*stringNew)(const char*) = NewString;
        inline int (*valueSize)(void*, uint32_t*) = ValueSize;
    }
    inline GakuMotion::injection::OwnerSnapshot g_sceneOwner, observedOwner;
    inline std::atomic<bool> g_runtimeReady{true};
    inline bool g_activeSceneVerified{}, observedVerified{}, unityThread{};
    inline std::string g_verifiedOwnerSceneName, observedPresenter, observedActive;
    inline int refreshCalls{};
    inline bool forcedRefresh{};
    namespace SceneOwnerRuntime { inline std::string cachedActiveScene; }
    bool ProtectionOnUnityThread() { return unityThread; }
    void RefreshProtectedContext(bool force)
    {
        ++refreshCalls;
        forcedRefresh = force;
        g_sceneOwner = observedOwner;
        g_activeSceneVerified = observedVerified;
        g_verifiedOwnerSceneName = observedPresenter;
        SceneOwnerRuntime::cachedActiveScene = observedActive;
    }

#include "GkStageLoadOverride.hpp"

    namespace seam = StageLoadOverride;
    inline int passed{}, failed{};
    void Check(bool condition, const char* description)
    {
        if (condition) ++passed;
        else { ++failed; std::printf("[FAIL] %s\n", description); }
    }

    struct Call { int count{}; void* background{}; int time{}; const void* method{}; };
    inline Call nativeCall;
    inline int methodToken{}, otherMethodToken{}, targetToken{};
    inline void* nativeResult{};
    inline int acquireCalls{}, releaseCalls{};
    inline bool releaseDisabled{}, publishOriginal{true};
    inline GakuStatus acquireStatus{GAKU_OK}, releaseStatus{GAKU_OK};
    inline void* acquiredTarget{}, *releasedTarget{}, *acquiredDetour{};
    void* NativeGetEnv(void* background, int time, const void* method)
    {
        nativeCall = {nativeCall.count + 1, background, time, method};
        return nativeResult;
    }
    GakuStatus Acquire(void*, uint32_t count, const GakuHookRequest* requests)
    {
        ++acquireCalls;
        Check(count == 1, "install acquires exactly one resource-name hook");
        acquiredTarget = requests[0].target;
        acquiredDetour = requests[0].detour;
        if (publishOriginal) *requests[0].original = reinterpret_cast<void*>(NativeGetEnv);
        return acquireStatus;
    }
    GakuStatus Release(void*, void* value)
    {
        ++releaseCalls;
        releaseDisabled = !seam::enabled.load();
        releasedTarget = value;
        return releaseStatus;
    }
    void Reset()
    {
        allocations = failAllocation = refreshCalls = acquireCalls = releaseCalls = 0;
        forcedRefresh = releaseDisabled = false;
        acquiredTarget = releasedTarget = acquiredDetour = nullptr;
        acquireStatus = releaseStatus = GAKU_OK;
        publishOriginal = true;
        nativeCall = {};
        nativeResult = Text("env_3d_adv_studio-01-00-noon");
        GakuMotion::stage_load::receipt.Clear();
        GakuMotion::Core::Instance().stageManager.selected = "env_3d_live_all001-00-noon";
        g_runtimeReady = true;
        unityThread = observedVerified = g_activeSceneVerified = true;
        observedOwner = {true, 0x1234, 17, true, GakuMotion::injection::OwnerRole::Photography, false};
        g_sceneOwner = observedOwner;
        observedPresenter = g_verifiedOwnerSceneName = "Photography";
        observedActive = SceneOwnerRuntime::cachedActiveScene = "env_3d_adv_studio-01-00-noon";
        StageLoader::stringNew = NewString;
        StageLoader::valueSize = ValueSize;
        GakuMotion::host::hooks = {};
        GakuMotion::host::hooks.acquireBatch = Acquire;
        GakuMotion::host::hooks.release = Release;
        seam::original = NativeGetEnv;
        seam::enabled = true;
        seam::envMethod = &methodToken;
        seam::target = nullptr;
        seam::attempted = true;
        seam::nestedTypes = NestedTypes;
        seam::className = ClassName;
        seam::samples = seam::changes = 0;
        for (auto& entry : seam::entries) entry.store(0);
        enumClass = {"PhotoBackgroundTimeType", true, 4, 4};
        nestedClasses = {&photoClass};
        methods.clear();
        auto& api = StageLoader::metadata;
        api.methods = Methods; api.name = MethodName; api.count = MethodCount;
        api.flags = MethodFlags; api.generic = MethodGeneric; api.inflated = MethodInflated;
        api.declaringClass = MethodClass; api.parameter = MethodParameter; api.result = MethodResult;
        api.typeCode = TypeCode; api.byRef = TypeByRef; api.classFromType = TypeClass;
    }
    bool NativePreserved(void* background, int time, const void* method, void* result)
    {
        return nativeCall.count == 1 && nativeCall.background == background && nativeCall.time == time &&
            nativeCall.method == method && result == nativeResult && allocations == 0;
    }
    void TestDetour()
    {
        void* background = Text("studio-01-00");
        for (int refusal = 0; refusal < 9; ++refusal)
        {
            Reset();
            if (refusal == 0) seam::enabled = false;
            if (refusal == 1) g_runtimeReady = false;
            if (refusal == 2) unityThread = false;
            if (refusal == 3) observedVerified = false;
            if (refusal == 4) observedOwner.role = GakuMotion::injection::OwnerRole::Other;
            if (refusal == 5) observedOwner.confirmed = false;
            if (refusal == 6) observedOwner.sceneLoaded = false;
            if (refusal == 7) observedPresenter = "Live";
            if (refusal == 8) observedActive = "env_3d_live_courtyard-00-00-noon";
            void* result = seam::GetEnvAssetId(background, 7, &methodToken);
            Check(NativePreserved(background, 7, &methodToken, result), "denied detour forwards every input and native return exactly once");
            Check(refusal < 3 ? refreshCalls == 0 : refreshCalls == 1 && forcedRefresh,
                "worker/unready/disabled calls avoid managed refresh while current owner is read on the Unity thread");
            Check(GakuMotion::stage_load::receipt.target.empty(), "denied request cannot create a stage receipt");
        }
        Reset();
        nativeResult = reinterpret_cast<void*>(1);
        void* result = seam::GetEnvAssetId(background, 2, &otherMethodToken);
        Check(NativePreserved(background, 2, &otherMethodToken, result) && refreshCalls == 0,
            "an unrelated folded MethodInfo never reads an arbitrary native result as a string");
        for (const char* key : {"", "unknown", "mdl_chr_ttmr-casl-0000_body", "ENV_3D_STAGE"})
        {
            Reset(); nativeResult = Text(key);
            result = seam::GetEnvAssetId(background, 3, &methodToken);
            Check(NativePreserved(background, 3, &methodToken, result), "non-scene resource names retain native identity");
        }
        Reset(); nativeResult = nullptr;
        result = seam::GetEnvAssetId(background, 3, &methodToken);
        Check(NativePreserved(background, 3, &methodToken, result), "null native result is preserved");
        for (int malformed = 0; malformed < 4; ++malformed)
        {
            Reset(); auto* text = static_cast<Il2CppString*>(nativeResult);
            if (malformed == 0) text->length = 257;
            if (malformed == 1) text->length = -1;
            if (malformed == 2) text->chars[8] = 0;
            if (malformed == 3) text->chars[8] = 0x4e00;
            result = seam::GetEnvAssetId(background, 9, &methodToken);
            Check(NativePreserved(background, 9, &methodToken, result), "oversized, negative-length, NUL and non-ASCII scene results fail closed");
        }
        for (const char* selection : {"", "env_3d_live_hski001-00-noon", "env_3d_live_all001-00-noon_extra"})
        {
            Reset(); GakuMotion::Core::Instance().stageManager.selected = selection;
            result = seam::GetEnvAssetId(background, 1, &methodToken);
            Check(NativePreserved(background, 1, &methodToken, result), "native selection and targets outside the exact standard-stage allowlist remain native");
        }
        Reset(); failAllocation = 1;
        result = seam::GetEnvAssetId(background, 6, &methodToken);
        Check(result == nativeResult && nativeCall.count == 1 && allocations == 1 && GakuMotion::stage_load::receipt.target.empty(),
            "allocation failure preserves native return and creates no receipt");
        for (const char* selection : {"env_3d_live_all001-00-noon", "env_3d_prod_hifstage-00-00-noon", "env_3d_live_courtyard-00-00-noon"})
        {
            Reset(); GakuMotion::Core::Instance().stageManager.selected = selection;
            result = seam::GetEnvAssetId(background, 19, &methodToken);
            Check(Value(result) == selection && result != nativeResult && nativeCall.count == 1 && nativeCall.background == background &&
                nativeCall.time == 19 && nativeCall.method == &methodToken && allocations == 1,
                "allowed request changes only returned key after exactly one native call with original arguments");
            Check(GakuMotion::stage_load::receipt.owner.SameOwner(observedOwner) && GakuMotion::stage_load::receipt.target == selection,
                "receipt binds the request to the fresh exact Photography owner and selected target");
        }
        Reset(); nativeResult = Text("env_3d_live_all001-00-noon");
        result = seam::GetEnvAssetId(background, 0, nullptr);
        Check(result == nativeResult && allocations == 0 && nativeCall.count == 1 && !nativeCall.method &&
            GakuMotion::stage_load::receipt.owner.SameOwner(observedOwner),
            "equal resource key preserves pointer identity and records same owner's request without allocation");
    }
    void TestInstall()
    {
        MetadataMethod valid;
        valid.pointer = &targetToken; valid.owner = &photoClass; valid.parameters[1].klass = &enumClass;
        for (int refusal = 0; refusal < 19; ++refusal)
        {
            Reset(); seam::enabled = false; seam::attempted = false; seam::original = nullptr;
            MetadataMethod candidate = valid, alternate = valid; methods = {&candidate};
            if (refusal == 0) enumClass.size = 8;
            if (refusal == 1) enumClass.alignment = 8;
            if (refusal == 2) StageLoader::valueSize = nullptr;
            if (refusal == 3) seam::nestedTypes = nullptr;
            if (refusal == 4) seam::className = nullptr;
            if (refusal == 5) nestedClasses.clear();
            if (refusal == 6) nestedClasses = {&photoClass, &photoClass};
            if (refusal == 7) candidate.flags &= ~0x10u;
            if (refusal == 8) candidate.parameters[0].byRef = true;
            if (refusal == 9) candidate.parameters[1].klass = &otherClass;
            if (refusal == 10) candidate.parameters[1].byRef = true;
            if (refusal == 11) candidate.returns.code = 0x11;
            if (refusal == 12) candidate.generic = true;
            if (refusal == 13) candidate.inflated = true;
            if (refusal == 14) methods.push_back(&alternate);
            if (refusal == 15) StageLoader::stringNew = nullptr;
            if (refusal == 16) GakuMotion::host::hooks.acquireBatch = nullptr;
            if (refusal == 17) GakuMotion::host::hooks.release = nullptr;
            if (refusal == 18) candidate.owner = &otherClass;
            Check(!seam::Install() && !seam::enabled && acquireCalls == 0,
                "unverified enum layout, nested type, exact metadata or host service prevents acquisition");
        }
        Reset(); seam::enabled = false; seam::attempted = false; seam::original = nullptr; methods = {&valid};
        Check(seam::Install() && seam::enabled && seam::envMethod == &valid && seam::target == &targetToken &&
            seam::original == NativeGetEnv && acquiredDetour == reinterpret_cast<void*>(seam::GetEnvAssetId),
            "exact static string-returning metadata and verified enum layout install the production detour");
        Check(seam::Install() && acquireCalls == 1, "repeated install does not acquire another lease");
        for (bool published : {false, true})
        {
            Reset(); seam::enabled = false; seam::attempted = false; seam::original = nullptr; methods = {&valid};
            acquireStatus = GAKU_E_INTERNAL; publishOriginal = published;
            Check(!seam::Install() && !seam::enabled && acquireCalls == 1 && releaseCalls == (published ? 1 : 0),
                "failed acquisition stays disabled and cleans up any published trampoline lease");
        }
    }
    void TestShutdown()
    {
        void* background = Text("studio-01-00");
        for (auto status : {GAKU_OK, GAKU_E_INTERNAL})
        {
            Reset(); seam::target = &targetToken; releaseStatus = status; seam::Shutdown();
            Check(!seam::enabled && releaseCalls == 1 && releaseDisabled && releasedTarget == &targetToken,
                "shutdown disables redirection before releasing exact target lease");
            Check(status == GAKU_OK ? !seam::target && !seam::attempted : seam::target == &targetToken && seam::attempted,
                "release success clears identity while failure retains identity for cleanup");
            void* result = seam::GetEnvAssetId(background, 43, &methodToken);
            Check(seam::original == NativeGetEnv && NativePreserved(background, 43, &methodToken, result) && refreshCalls == 0,
                "post-shutdown detour retains valid original and forwards unchanged inputs and result");
        }
    }
}

#include "test_live_takeover_session.hpp"
#include "test_live_takeover_driver.hpp"
#include "test_live_api.hpp"
#include "test_live_observation.hpp"

int main()
{
    using namespace StageRuntimeHarness;
    TestDetour(); TestInstall(); TestShutdown(); TestLiveTakeoverSession(); TestLiveTakeoverDriver(); LiveApiHarness::Test(); LiveGateResolveHarness::Test(); LiveObservationHarness::Test();
    std::printf("Stage-load runtime detour: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
