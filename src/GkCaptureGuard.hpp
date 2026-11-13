#pragma once

#include "GkExactMethod.hpp"
#include "GkGmlClient.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdint>

// This header is included at translation-unit scope. It owns no game objects;
// all object reads and callbacks happen on the game's calling Unity thread.
namespace GakuMotion::capture
{
    using FindClassFn = void* (*)(const char*, const char*, const char*);
    struct Event
    {
        uint64_t id{};
        const char* type{};
        const char* phase{};
        const char* reason{};
        unsigned depth{};
        int state{-1};
        bool gmActive{};
        uint64_t parentId{};
    };
    struct Callbacks
    {
        bool (*customActive)() = nullptr;
        bool (*beginSuspend)(const char*) = nullptr;
        void (*endSuspend)(const char*) = nullptr;
        void (*fault)(const char*) = nullptr;
        void (*pageChanged)(bool) = nullptr;
        void (*observe)(const Event&) = nullptr;
        bool (*continueCapture)(const char*) = nullptr;
        bool (*liveTakeoverActive)() = nullptr;
        bool (*onUnityThread)() = nullptr;
    };

    // Async state machines move from the entry method's stack to a UniTask
    // runner at their first await. The stable identity is owner + operation,
    // with a depth for overlapping tasks, never the address of MoveNext.self.
    struct LeaseLedger
    {
        struct Entry { void* owner{}; unsigned operation{}, depth{}, suspendedDepth{}; };
        std::array<Entry, 64> entries{};
        enum class BeginResult { Existing, Acquired, Full };
        BeginResult Begin(void* owner, unsigned operation, bool first)
        {
            Entry* empty = nullptr;
            for (auto& e : entries)
            {
                if (!e.depth && !empty) empty = &e;
                if (e.depth && e.owner == owner && e.operation == operation)
                {
                    if (first) { ++e.depth; return BeginResult::Acquired; }
                    return BeginResult::Existing;
                }
            }
            if (!empty) return BeginResult::Full;
            *empty = {owner, operation, 1, 0};
            return BeginResult::Acquired;
        }
        bool End(void* owner, unsigned operation)
        {
            for (auto& e : entries)
                if (e.depth && e.owner == owner && e.operation == operation)
                {
                    if (!--e.depth) e = {};
                    return true;
                }
            return false;
        }
        unsigned Count() const
        {
            unsigned n = 0;
            for (const auto& e : entries) n += e.depth;
            return n;
        }
        Entry* Find(void* owner, unsigned operation)
        { for (auto& e : entries) if (e.depth && e.owner == owner && e.operation == operation) return &e; return nullptr; }
    };

    using MoveNextFn = void (*)(void*, void*);
    using SetExceptionFn = void (*)(void*, void*, const void*);
    struct Machine
    {
        const char* label{};
        bool manual{};
        unsigned operation{};
        int stateOffset{-1}, ownerOffset{-1}, builderOffset{-1};
        MoveNextFn original{};
        SetExceptionFn setException{};
        const void* setExceptionMetadata{};
        void* target{};
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
        int runnerOffset{-1}; // Validated builder.runnerPromise reference.
#endif
    };
    inline Callbacks callbacks{};
    inline LeaseLedger ledger{};
    inline std::atomic<bool> ready{false}, pageStable{true};
    inline std::atomic<bool> liveReady{false}, liveAutoUnknown{false};
    inline bool attempted = false;
    inline std::array<void*, 16> hookTargets{};
    inline unsigned hookCount = 0;
    inline Machine autoPhoto{"Live.AutoPhoto", false, 1};
    inline Machine captureTexture{"Photography.CaptureTexture", false, 2};
    inline Machine stockCapture{"Photography.StockCapture", false, 3};
    inline Machine photographyButton{"Photography.ManualPhoto", true, 4};
    inline Machine liveButton{"Live.ManualPhoto", true, 5};
    inline bool (*canTakeOriginal)(void*, void*) = nullptr;
    inline void (*buttonClickOriginal)(void*, void*) = nullptr;
    inline void (*buttonPressOriginal)(void*, bool, void*) = nullptr;
    inline void (*willChangeOriginal)(void*, void*) = nullptr;
    inline void (*didChangeOriginal)(void*, void*) = nullptr;
    inline void (*outGameDestroyOriginal)(void*, void*) = nullptr;
    inline const void* topScreenMetadata = nullptr;
    inline void* costumeClass = nullptr;
    inline void* (*objectClass)(void*) = nullptr;
    inline void* (*exceptionFromName)(void*, const char*, const char*, const char*) = nullptr;
    inline void* corlib = nullptr;
    inline ExactMethodMetadata metadata{};
    // Tracks synchronous native work separately from an across-await lease.
    // Shared native eligibility may also be polled by PlayerLoop awaiters.
    inline thread_local unsigned automaticCallDepth = 0;
    inline thread_local uint64_t currentTransactionId = 0;
    inline thread_local const char* currentTransactionType = "none";
    inline uint64_t CurrentTransactionId() noexcept { return currentTransactionId; }
    inline const char* CurrentTransactionType() noexcept { return currentTransactionType; }

#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
    inline std::atomic<uint64_t> nextTransactionId{1};
    struct TransactionLedger
    {
        struct Entry { uint64_t id{}; void* owner{}; unsigned operation{}; void* runner{}; bool gmActive{}; };
        std::array<Entry, 64> entries{};
        Entry* Find(uint64_t id)
        { for (auto& e : entries) if (id && e.id == id) return &e; return nullptr; }
        Entry* Find(void* owner, unsigned operation, void* runner)
        {
            if (!runner) return nullptr;
            for (auto& e : entries)
                if (e.id && e.owner == owner && e.operation == operation && e.runner == runner) return &e;
            return nullptr;
        }
        Entry* Begin(void* owner, unsigned operation, bool active)
        {
            for (auto& e : entries) if (!e.id)
            { e = {nextTransactionId.fetch_add(1, std::memory_order_relaxed), owner, operation, nullptr, active}; return &e; }
            return nullptr;
        }
        void End(uint64_t id) { if (auto* e = Find(id)) *e = {}; }
    };
    inline TransactionLedger transactions{};
#endif

    struct Invocation
    {
        Invocation* previous{};
        Machine& machine;
        void* self{};
        void* owner{};
        uint64_t id{}, parentId{};
        bool gmActive{};
        inline static thread_local Invocation* current = nullptr;
        Invocation(Machine& m, void* value, void* actor, uint64_t captureId, bool active)
            : previous(current), machine(m), self(value), owner(actor), id(captureId),
              parentId(currentTransactionId), gmActive(active)
        { current = this; currentTransactionId = id; currentTransactionType = m.label; }
        ~Invocation()
        {
            current = previous;
            currentTransactionId = previous ? previous->id : 0;
            currentTransactionType = previous ? previous->machine.label : "none";
        }
    };
    inline void Observe(const char* phase, const char* reason, int state)
    {
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
        if (callbacks.observe && Invocation::current)
        {
            const auto& i = *Invocation::current;
            callbacks.observe({i.id, i.machine.label, phase, reason, ledger.Count(), state, i.gmActive, i.parentId});
        }
#else
        (void)phase; (void)reason; (void)state;
#endif
    }
    inline void ObserveRejected(Machine& machine, const char* reason)
    {
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
        Invocation invocation(machine, nullptr, nullptr,
            nextTransactionId.fetch_add(1, std::memory_order_relaxed),
            callbacks.customActive && callbacks.customActive());
        Observe("capture_cancelled", reason, -2);
#else
        (void)machine; (void)reason;
#endif
    }

    inline void Fault(const char* reason)
    {
        ready.store(false, std::memory_order_release);
        if (callbacks.fault) callbacks.fault(reason);
        host::Error("[GakuMotion][CaptureGuard] disabled: %s", reason);
    }
    inline bool Ready() { return ready.load(std::memory_order_acquire); }
    inline bool LiveReady() { return liveReady.load(std::memory_order_acquire) && !liveAutoUnknown.load(std::memory_order_acquire); }
    inline bool LiveAutoPhotoIdle() { return LiveReady() && ledger.Count() == 0; }
    inline bool IsPageStable() { return pageStable.load(std::memory_order_acquire); }
    inline bool IsCurrentCostumePage(void* currentTopClass)
    {
        return IsPageStable() && currentTopClass && currentTopClass == costumeClass;
    }
    inline bool Active() { return callbacks.customActive && callbacks.customActive(); }
    inline bool LiveActive() { return callbacks.liveTakeoverActive && callbacks.liveTakeoverActive(); }
    inline void SetPageStable(bool value)
    {
        const bool old = pageStable.exchange(value, std::memory_order_acq_rel);
        if (old != value && callbacks.pageChanged) callbacks.pageChanged(value);
    }

    inline bool ReadState(const Machine& m, void* self, int& state, void*& owner)
    {
        if (!self || m.stateOffset < 0 || m.ownerOffset < 0) return false;
        __try
        {
            state = *reinterpret_cast<int*>(static_cast<char*>(self) + m.stateOffset);
            owner = *reinterpret_cast<void**>(static_cast<char*>(self) + m.ownerOffset);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
    inline bool ReadRunner(const Machine& m, void* self, void*& runner)
    {
        if (!self || m.runnerOffset < 0) return false;
        __try
        {
            runner = *reinterpret_cast<void**>(static_cast<char*>(self) + m.runnerOffset);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    inline TransactionLedger::Entry* FindContinuation(Machine& m, void* self, void* owner)
    {
        void* runner = nullptr;
        if (!ReadRunner(m, self, runner) || !runner) return nullptr;
        if (auto* found = transactions.Find(owner, m.operation, runner)) return found;
        // Await registration can run a continuation inline before the first
        // stack invocation returns. Its stack is still live, so read the same
        // validated runner field there and require exact reference equality.
        for (auto* live = Invocation::current; live; live = live->previous)
        {
            if (live->machine.operation != m.operation || live->owner != owner) continue;
            void* liveRunner = nullptr;
            if (!ReadRunner(live->machine, live->self, liveRunner) || liveRunner != runner) continue;
            if (auto* found = transactions.Find(live->id)) { found->runner = runner; return found; }
        }
        return nullptr;
    }
#endif
    // Equivalent to the compiler-generated catch path: state=-2, then the
    // real builder.SetException. Awaiters see a cancelled/faulted task; no
    // synthetic UniTask representation or permanently pending task is made.
    inline bool Cancel(Machine& m, void* self, const char* reason)
    {
        if (!self || m.stateOffset < 0 || m.builderOffset < 0 || !exceptionFromName || !corlib || !m.setException) return false;
        __try
        {
            void* exception = exceptionFromName(corlib, "System", "OperationCanceledException", reason);
            if (!exception) return false;
            *reinterpret_cast<int*>(static_cast<char*>(self) + m.stateOffset) = -2;
            m.setException(static_cast<char*>(self) + m.builderOffset, exception, m.setExceptionMetadata);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    inline void Run(Machine& m, void* self, void* methodInfo)
    {
        if (&m == &autoPhoto)
        {
            if (!callbacks.onUnityThread || !callbacks.onUnityThread())
            {
                liveAutoUnknown.store(true, std::memory_order_release);
                // Never touch the non-atomic task ledger or call restoration
                // off-thread. In an active Live takeover, reject the task.
                if (LiveActive())
                {
                    if (!Cancel(m, self, "Live capture arrived off the Unity thread"))
                        Fault("off-thread Live capture cancellation failed");
                }
                else if (m.original) m.original(self, methodInfo);
                return;
            }
            auto uncertain = [&](const char* reason)
            {
                if (LiveActive() && Active())
                {
                    // The capture may proceed only after checked restoration;
                    // without a trackable completion, leave GM suspended.
                    const bool restored = callbacks.beginSuspend && callbacks.beginSuspend(m.label);
                    if (!restored)
                    {
                        liveAutoUnknown.store(true, std::memory_order_release);
                        if (!Cancel(m, self, reason)) Fault("untracked Live automatic capture cancellation failed");
                        return;
                    }
                }
                liveAutoUnknown.store(true, std::memory_order_release);
                if (m.original) m.original(self, methodInfo);
            };
            // Observe every automatic task, but never cancel or suspend an
            // inactive session. Pending tasks prevent a new Live takeover.
            int before = 0; void* owner = nullptr;
            if (!ReadState(m, self, before, owner) || !owner)
            {
                uncertain("Live automatic capture state unavailable");
                return;
            }
            if (before == -2) { if (m.original) m.original(self, methodInfo); return; }
            const auto added = ledger.Begin(owner, m.operation, before == -1);
            if (added == LeaseLedger::BeginResult::Full)
            {
                uncertain("Live automatic capture tracker full");
                return;
            }
            auto* entry = ledger.Find(owner, m.operation);
            if (added == LeaseLedger::BeginResult::Acquired &&
                ((LiveActive() && Active()) || (entry && entry->suspendedDepth)))
            {
                if (!callbacks.beginSuspend || !callbacks.beginSuspend(m.label))
                {
                    if (!Cancel(m, self, "GakuMotion could not restore Live before automatic capture"))
                        Fault("Live automatic capture cancellation failed");
                    if (callbacks.endSuspend) callbacks.endSuspend(m.label);
                    ledger.End(owner, m.operation);
                    return;
                }
                entry = ledger.Find(owner, m.operation);
                if (!entry)
                {
                    liveAutoUnknown.store(true, std::memory_order_release);
                    if (!Cancel(m, self, "Live automatic capture tracking changed"))
                        Fault("Live automatic capture tracking changed");
                    return;
                }
                ++entry->suspendedDepth;
            }
            else if (added == LeaseLedger::BeginResult::Existing && entry && entry->suspendedDepth &&
                (!callbacks.continueCapture || !callbacks.continueCapture(m.label)))
            {
                if (!Cancel(m, self, "Live capture continuation could not confirm restoration"))
                    Fault("Live capture continuation cancellation failed");
                --entry->suspendedDepth;
                ledger.End(owner, m.operation);
                if (callbacks.endSuspend) callbacks.endSuspend(m.label);
                return;
            }
            if (m.original) m.original(self, methodInfo);
            int after = 0; void* afterOwner = nullptr;
            if (!ReadState(m, self, after, afterOwner) || (after != -2 && afterOwner != owner))
            {
                liveAutoUnknown.store(true, std::memory_order_release);
                if (LiveActive()) Fault("Live automatic capture completion unavailable");
                return;
            }
            if (after == -2)
            {
                entry = ledger.Find(owner, m.operation);
                const bool suspended = entry && entry->suspendedDepth;
                if (suspended) --entry->suspendedDepth;
                ledger.End(owner, m.operation);
                if (suspended && callbacks.endSuspend) callbacks.endSuspend(m.label);
            }
            return;
        }
        // Automatic/public captures are native-owned. An inactive GM session
        // also leaves manual tasks entirely native, including their awaits.
        // Do not inspect a state machine or touch scene/restore/lease state.
        if (!m.manual || !Active())
        {
            if (m.original) m.original(self, methodInfo);
            return;
        }
        ObserveRejected(m, "manual-gm-active");
        if (!Cancel(m, self, "GakuMotion manual photography is disabled while its session is active"))
            Fault("manual capture cancellation failed");
    }
    inline void AutoPhotoMoveNext(void* s, void* m) { Run(autoPhoto, s, m); }
    inline void CaptureTextureMoveNext(void* s, void* m) { Run(captureTexture, s, m); }
    inline void StockCaptureMoveNext(void* s, void* m) { Run(stockCapture, s, m); }
    inline void PhotographyButtonMoveNext(void* s, void* m) { Run(photographyButton, s, m); }
    inline void LiveButtonMoveNext(void* s, void* m) { Run(liveButton, s, m); }
    inline bool CanTakePhoto(void* self, void* m)
    {
        // Not hooked: retained only for offline compatibility checks.
        return canTakeOriginal && canTakeOriginal(self, m);
    }
    inline void ButtonClick(void* self, void* m)
    {
        if (Active())
        {
            Machine request{"PhotoButton.Click", true};
            ObserveRejected(request, "manual-gm-active");
            return;
        }
        if (buttonClickOriginal) buttonClickOriginal(self, m);
    }
    inline void ButtonPress(void* self, bool pressed, void* m)
    {
        // A release still reaches the game, so an existing rapid-shot task can
        // cancel normally when takeover starts between press and release.
        if (pressed && Active())
        {
            Machine request{"PhotoButton.Press", true};
            ObserveRejected(request, "manual-gm-active");
            return;
        }
        if (buttonPressOriginal) buttonPressOriginal(self, pressed, m);
    }
    inline void WillChange(void* self, void* m)
    {
        SetPageStable(false);
        if (willChangeOriginal) willChangeOriginal(self, m);
    }
    inline void DidChange(void* self, void* m)
    {
        if (didChangeOriginal) didChangeOriginal(self, m);
        // This callback completes the OutGame transition. Its TopScreen may
        // still be an idol list; a child window can select Costume afterwards.
        // Page identity is read fresh by the existing runtime_invoke getter.
        SetPageStable(true);
    }
    inline void OutGameDestroy(void* self, void* m)
    {
        SetPageStable(false);
        if (outGameDestroyOriginal) outGameDestroyOriginal(self, m);
    }

    inline void* Pointer(const void* method)
    {
        return method ? *static_cast<void* const*>(method) : nullptr;
    }
    inline bool Hook(const char* label, const void* method, void* detour, void** original)
    {
        void* target = Pointer(method);
        if (!target || !host::hooks.acquire || hookCount == hookTargets.size()) return false;
        const auto status = host::hooks.acquire(host::hooks.context, target, detour, original);
        host::Info("[GakuMotion][CaptureGuard] hook %s target=%p status=%d", label, target, status);
        if (status != GAKU_OK) return false;
        if (!*original)
        {
            // An acquired lease must never be orphaned on validation failure.
            if (host::hooks.release) host::hooks.release(host::hooks.context, target);
            return false;
        }
        hookTargets[hookCount++] = target;
        return true;
    }
    inline void Shutdown()
    {
        ready.store(false, std::memory_order_release);
        liveReady.store(false, std::memory_order_release);
        liveAutoUnknown.store(false, std::memory_order_release);
        // Host shutdown may run off the Unity thread. The runtime owns native
        // restoration; never call its page-transition callback from teardown.
        pageStable.store(false, std::memory_order_release);
        while (hookCount)
        {
            void* target = hookTargets[--hookCount];
            if (host::hooks.release) host::hooks.release(host::hooks.context, target);
            hookTargets[hookCount] = nullptr;
        }
        // Do not restore an uncertain in-flight capture on teardown. The
        // caller's plugin-deactivation path removes all custom injection.
        ledger = {};
#ifdef GKMOTION_CAPTURE_DIAGNOSTICS
        transactions = {};
#endif
        automaticCallDepth = 0;
        attempted = false;
    }

    inline bool Install(HMODULE module, FindClassFn findClass, Callbacks supplied)
    {
        if (attempted) return Ready();
        attempted = true;
        callbacks = supplied;
        pageStable.store(true, std::memory_order_release);
        if (!module || !findClass || !callbacks.customActive || !callbacks.pageChanged) return false;
        auto resolve = [module](const char* name) { return GetProcAddress(module, name); };
#define GK_CAPTURE_API(member, name) member = reinterpret_cast<decltype(member)>(resolve(name))
        GK_CAPTURE_API(metadata.methods, "il2cpp_class_get_methods");
        GK_CAPTURE_API(metadata.name, "il2cpp_method_get_name");
        GK_CAPTURE_API(metadata.count, "il2cpp_method_get_param_count");
        GK_CAPTURE_API(metadata.flags, "il2cpp_method_get_flags");
        GK_CAPTURE_API(metadata.generic, "il2cpp_method_is_generic");
        GK_CAPTURE_API(metadata.inflated, "il2cpp_method_is_inflated");
        GK_CAPTURE_API(metadata.declaringClass, "il2cpp_method_get_class");
        GK_CAPTURE_API(metadata.parameter, "il2cpp_method_get_param");
        GK_CAPTURE_API(metadata.result, "il2cpp_method_get_return_type");
        GK_CAPTURE_API(metadata.typeCode, "il2cpp_type_get_type");
        GK_CAPTURE_API(metadata.byRef, "il2cpp_type_is_byref");
        GK_CAPTURE_API(metadata.classFromType, "il2cpp_class_from_type");
        GK_CAPTURE_API(objectClass, "il2cpp_object_get_class");
        GK_CAPTURE_API(exceptionFromName, "il2cpp_exception_from_name_msg");
#undef GK_CAPTURE_API
        const auto getCorlib = reinterpret_cast<void* (*)()>(resolve("il2cpp_get_corlib"));
        const auto nestedTypes = reinterpret_cast<void* (*)(void*, void**)>(resolve("il2cpp_class_get_nested_types"));
        const auto className = reinterpret_cast<const char* (*)(void*)>(resolve("il2cpp_class_get_name"));
        const auto valueType = reinterpret_cast<bool (*)(void*)>(resolve("il2cpp_class_is_valuetype"));
        const auto valueSize = reinterpret_cast<int (*)(void*, uint32_t*)>(resolve("il2cpp_class_value_size"));
        const auto findField = reinterpret_cast<void* (*)(void*, const char*)>(resolve("il2cpp_class_get_field_from_name"));
        const auto fieldType = reinterpret_cast<void* (*)(void*)>(resolve("il2cpp_field_get_type"));
        const auto fieldFlags = reinterpret_cast<int (*)(void*)>(resolve("il2cpp_field_get_flags"));
        const auto fieldOffset = reinterpret_cast<size_t (*)(void*)>(resolve("il2cpp_field_get_offset"));
        if (!metadata.methods || !metadata.name || !metadata.count || !metadata.flags || !metadata.generic ||
            !metadata.inflated || !metadata.declaringClass || !metadata.parameter || !metadata.result ||
            !metadata.typeCode || !metadata.byRef || !metadata.classFromType || !objectClass ||
            !exceptionFromName || !getCorlib || !nestedTypes || !className || !valueType || !valueSize ||
            !findField || !fieldType || !fieldFlags || !fieldOffset) return false;
        corlib = getCorlib();
        void* exceptionClass = findClass("mscorlib.dll", "System", "Exception");
        void* photography = findClass("Assembly-CSharp.dll", "Campus.Photography", "PhotographyScenePresenter");
        void* button = findClass("Assembly-CSharp.dll", "Campus.Common.Photography", "PhotoButtonPresenter");
        void* outGame = findClass("Assembly-CSharp.dll", "Campus.OutGame", "OutGameScenePresenter");
        costumeClass = findClass("Assembly-CSharp.dll", "Campus.OutGame", "CostumeSelectScreenPresenter");
        void* window = findClass("quaunity-ui.Runtime.dll", "Qua.UI", "WindowPresenterBase");
        void* screen = findClass("quaunity-ui.Runtime.dll", "Qua.UI", "ScreenPresenterBase");
        if (!corlib || !exceptionClass || !photography || !button ||
            !outGame || !costumeClass || !window || !screen) return false;
        using Type = ExactMethodMetadata::Type;
        const Type voidType{0x01}, boolType{0x02};
        topScreenMetadata = metadata.Find(window, "get_TopScreen", false, {0x12, screen}, {});
        if (!topScreenMetadata) return false;

        auto prepare = [&](Machine& m, void* owner, const char* prefix) -> const void*
        {
            void* machineClass = nullptr;
            void* iter = nullptr;
            while (void* nested = nestedTypes(owner, &iter))
            {
                const char* name = className(nested);
                if (!name || std::strncmp(name, prefix, std::strlen(prefix))) continue;
                if (machineClass) return nullptr;
                machineClass = nested;
            }
            if (!machineClass || !valueType(machineClass)) return nullptr;
            const void* move = metadata.Find(machineClass, "MoveNext", false, voidType, {});
            void* state = findField(machineClass, "<>1__state");
            void* thisField = findField(machineClass, "<>4__this");
            void* builder = findField(machineClass, "<>t__builder");
            if (!move || !state || !thisField || !builder || (fieldFlags(state) & 0x10) ||
                (fieldFlags(thisField) & 0x10) || (fieldFlags(builder) & 0x10) ||
                !metadata.Matches(fieldType(state), {0x08}) ||
                !metadata.Matches(fieldType(thisField), {0x12, owner})) return nullptr;
            void* builderClass = metadata.classFromType(fieldType(builder));
            const char* builderName = builderClass ? className(builderClass) : nullptr;
            if (!builderClass || !valueType(builderClass) || !builderName ||
                std::strncmp(builderName, "AsyncUniTaskMethodBuilder", 25)) return nullptr;
            uint32_t align = 0;
            const int size = valueSize(machineClass, &align);
            const int builderSize = valueSize(builderClass, &align);
            const size_t header = 2 * sizeof(void*);
            const size_t so = fieldOffset(state), oo = fieldOffset(thisField), bo = fieldOffset(builder);
            if (size <= 0 || size > 16384 || builderSize <= 0 || so != header ||
                oo < header + 8 || oo % alignof(void*) || oo + sizeof(void*) > header + size ||
                bo < header + 4 || bo + builderSize > header + size ||
                !(bo + builderSize <= oo || oo + sizeof(void*) <= bo)) return nullptr;
            // Closed generic builders report inflated=true. Validate the exact
            // instantiated declaring class and SetException signature rather
            // than rejecting all inflated methods or guessing its ABI.
            const void* completion = nullptr;
            iter = nullptr;
            while (const void* method = metadata.methods(builderClass, &iter))
            {
                const char* name = metadata.name(method);
                uint32_t impl = 0;
                if (!name || std::strcmp(name, "SetException") || metadata.count(method) != 1 ||
                    metadata.declaringClass(method) != builderClass || metadata.generic(method) ||
                    (metadata.flags(method, &impl) & (0x10 | 0x400)) ||
                    !metadata.Matches(metadata.result(method), voidType) ||
                    !metadata.Matches(metadata.parameter(method, 0), {0x12, exceptionClass})) continue;
                if (completion) return nullptr;
                completion = method;
            }
            if (!Pointer(completion)) return nullptr;
            m.stateOffset = static_cast<int>(so - header);
            m.ownerOffset = static_cast<int>(oo - header);
            m.builderOffset = static_cast<int>(bo - header);
            m.setException = reinterpret_cast<SetExceptionFn>(Pointer(completion));
            m.setExceptionMetadata = completion;
            m.target = Pointer(move);
            host::Info("[GakuMotion][CaptureGuard] metadata %s size=%d state=%d owner=%d builder=%d",
                m.label, size, m.stateOffset, m.ownerOffset, m.builderOffset);
            return move;
        };
        const void* photoMove = prepare(photographyButton, photography, "<OnPhotoButtonAsync>d__");
        const void* click = metadata.Find(button, "OnClick", false, voidType, {});
        const void* press = metadata.Find(button, "OnPress", false, voidType, {boolType});
        const void* will = metadata.Find(outGame, "OnNodeWillChange", false, voidType, {});
        const void* did = metadata.Find(outGame, "OnNodeDidChange", false, voidType, {});
        const void* destroy = metadata.Find(outGame, "OnDestroy", false, voidType, {});
        if (!photoMove || !click || !press || !will || !did || !destroy) return false;
        const bool installed =
            Hook(photographyButton.label, photoMove, reinterpret_cast<void*>(&PhotographyButtonMoveNext), reinterpret_cast<void**>(&photographyButton.original)) &&
            Hook("PhotoButton.OnClick", click, reinterpret_cast<void*>(&ButtonClick), reinterpret_cast<void**>(&buttonClickOriginal)) &&
            Hook("PhotoButton.OnPress", press, reinterpret_cast<void*>(&ButtonPress), reinterpret_cast<void**>(&buttonPressOriginal)) &&
            Hook("OutGame.OnNodeWillChange", will, reinterpret_cast<void*>(&WillChange), reinterpret_cast<void**>(&willChangeOriginal)) &&
            Hook("OutGame.OnNodeDidChange", did, reinterpret_cast<void*>(&DidChange), reinterpret_cast<void**>(&didChangeOriginal)) &&
            Hook("OutGame.OnDestroy", destroy, reinterpret_cast<void*>(&OutGameDestroy), reinterpret_cast<void**>(&outGameDestroyOriginal));
        if (!installed) { Shutdown(); return false; }
        // Live hooks are optional for the existing Photography feature. The
        // Live takeover must fail closed unless both exact hooks are leased.
        void* liveScene = findClass("Assembly-CSharp.dll", "Campus.Live", "LiveScenePresenter");
        const void* liveMove = liveScene ? prepare(liveButton, liveScene, "<OnPhotoButtonAsync>d__") : nullptr;
        const void* autoMove = liveScene ? prepare(autoPhoto, liveScene, "<AutoPhotoAsync>d__") : nullptr;
        const unsigned priorHooks = hookCount;
        const bool liveInstalled = callbacks.onUnityThread && liveMove && autoMove &&
            Hook(liveButton.label, liveMove, reinterpret_cast<void*>(&LiveButtonMoveNext), reinterpret_cast<void**>(&liveButton.original)) &&
            Hook(autoPhoto.label, autoMove, reinterpret_cast<void*>(&AutoPhotoMoveNext), reinterpret_cast<void**>(&autoPhoto.original));
        if (!liveInstalled)
            while (hookCount > priorHooks)
            {
                void* target = hookTargets[--hookCount];
                if (host::hooks.release) host::hooks.release(host::hooks.context, target);
                hookTargets[hookCount] = nullptr;
            }
        liveReady.store(liveInstalled, std::memory_order_release);
        host::Info("[GakuMotion][LiveGuard] manual_metadata=%d auto_metadata=%d installed=%d",
            liveMove != nullptr, autoMove != nullptr, liveInstalled);
        ready.store(true, std::memory_order_release);
        host::Info("[GakuMotion][CaptureGuard] ready; Photography manual shutter blocked only while GM session active; native capture unchanged");
        return true;
    }
}
