#pragma once

// Include GkPhotographyEnsembleProbe.hpp at translation-unit scope first.
// This read-only adapter is included in the plugin private namespace, after
// StageLoader, SceneOwnerRuntime and ProtectionOnUnityThread are defined.
namespace PhotographyEnsembleProbe
{
    namespace policy = GakuMotion::photography_ensemble_probe;
    inline std::atomic<bool> requested{false}, available{false}, crossFrameAvailable{false};
    inline std::atomic<bool> appliedComparisonAvailable{false}, readyForInjection{false};
    inline std::atomic<uint64_t> requestGeneration{0};
    inline std::atomic<unsigned> observedFrames{0}, populatedSlots{0}, stableActorRoots{0};
    inline std::atomic<const char*> detail{"photography ensemble observation disabled"};
    inline bool active{}, resolved{};
    inline uint64_t activeGeneration{};
    inline policy::Session session;
    inline policy::Frame frame;
    inline policy::ReadinessLatch readiness;
    inline policy::Frame previousFrame;
    inline policy::ExtraActorEvidence extraActors;
    inline uint64_t extraRosterEpoch{};
    inline void *presenterClass{}, *modelClass{}, *threeDClass{}, *settingClass{}, *photoActorClass{}, *actorInterface{}, *actorClass{};
    inline void *modelField{}, *threeDField{};
    inline const void *getSettings{}, *getCapacity{}, *getLayout{}, *getPhotoActor{}, *getActor{}, *getBuilt{}, *getIndex{};
    inline void* (*handleTarget)(void*){};

    inline void SetEnabled(bool enabled) noexcept
    {
        const bool previous = requested.exchange(enabled, std::memory_order_acq_rel);
        if (enabled && !previous) requestGeneration.fetch_add(1, std::memory_order_release);
        if (!enabled) { readyForInjection.store(false); available.store(false); }
    }
    inline bool IsEnabled() noexcept { return requested.load(std::memory_order_acquire); }
    inline bool Verified() noexcept { return false; }
    inline bool Fail(const char* reason) noexcept
    { available.store(false, std::memory_order_release); readyForInjection.store(false); detail.store(reason, std::memory_order_release); return false; }
    inline bool Call(const void* method, void* self, void** args, void*& output)
    {
        output = nullptr;
        if (!method || !g_faceRuntime.invoke) return false;
        void* exception = nullptr;
        __try { output = g_faceRuntime.invoke(method, self, args, &exception); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        return !exception;
    }
    inline bool FieldValue(void* object, void* field, void*& output)
    {
        output = nullptr;
        if (!object || !field || !g_faceRuntime.readField) return false;
        __try { g_faceRuntime.readField(object, field, &output); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    inline bool Unbox(void* object, void* output, size_t bytes)
    {
        if (!object || !g_safeBones.unbox) return false;
        __try
        {
            void* value = g_safeBones.unbox(object);
            if (!value) return false;
            std::memcpy(output, value, bytes); return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    inline bool Int(const void* method, void* self, int& value)
    { void* box{}; return Call(method, self, nullptr, box) && Unbox(box, &value, sizeof(value)); }
    inline bool Built(void* self, bool& value)
    { void* box{}; return Call(getBuilt, self, nullptr, box) && Unbox(box, &value, sizeof(value)); }
    struct Roots
    {
        StageLoader::Handle roots[24]{}; unsigned count{};
        ~Roots() { for (unsigned i = 0; i < count; ++i) StageLoader::freeHandle(roots[i]); }
        void* Keep(void* object)
        {
            if (!object || count == 24) return nullptr;
            auto handle = StageLoader::newHandle(object, false);
            if (!handle) return nullptr;
            roots[count++] = handle;
            return handleTarget(handle);
        }
    };
    inline void* ExactField(void* owner, const char* name, void* type)
    {
        void* field = owner && g_faceRuntime.findField ? g_faceRuntime.findField(owner, name) : nullptr;
        return field && !(g_faceRuntime.fieldFlags(field) & 0x10) &&
            StageLoader::metadata.Matches(g_faceRuntime.fieldType(field), {0x12, type}) ? field : nullptr;
    }
    inline bool Resolve()
    {
        if (resolved) return true;
        if (!ProtectionOnUnityThread() || !g_faceRuntime.findField || !g_faceRuntime.fieldType ||
            !g_faceRuntime.fieldFlags || !g_faceRuntime.parentClass || !g_faceRuntime.objectClass ||
            !StageLoader::metadata.byRef || !StageLoader::metadata.typeCode || !StageLoader::metadata.classFromType ||
            !StageLoader::newHandle || !StageLoader::freeHandle || !StageLoader::componentGo || !StageLoader::goScene ||
            !StageLoader::instanceId || !g_getWorldPosNative || !g_getWorldRotNative || !g_getParentNative)
            return Fail("ensemble observer APIs unavailable");
        handleTarget = reinterpret_cast<decltype(handleTarget)>(GetProcAddress(g_gameAssembly, "il2cpp_gchandle_get_target"));
        if (!handleTarget) return Fail("ensemble observer strong-root API unavailable");
        presenterClass = FindClass("Assembly-CSharp.dll", "Campus.Photography", "PhotographyScenePresenter");
        modelClass = FindClass("Assembly-CSharp.dll", "Campus.Photography", "PhotographySceneModel");
        threeDClass = FindClass("Assembly-CSharp.dll", "Campus.Photography", "Photography3DPresenter");
        settingClass = FindClass("Assembly-CSharp.dll", "Campus.Photography", "PhotographyLayoutSettingData");
        photoActorClass = FindClass("Assembly-CSharp.dll", "Campus.Photography", "PhotographyActor");
        actorInterface = FindClass("campus-submodule.Runtime.dll", "Campus.Common", "ICampusActorController");
        actorClass = FindClass("campus-submodule.Runtime.dll", "Campus.Common", "CampusActorController");
        void* layoutClass = FindClass("campus-submodule.Runtime.dll", "Campus.Common", "CampusPhotographySceneLayout");
        if (!presenterClass || !modelClass || !threeDClass || !settingClass || !photoActorClass ||
            !actorInterface || !actorClass || !layoutClass) return Fail("ensemble observer class contract unavailable");
        modelField = ExactField(presenterClass, "_sceneModel", modelClass);
        threeDField = ExactField(presenterClass, "_3dPresenter", threeDClass);
        auto& methods = StageLoader::metadata;
        getSettings = methods.Find(modelClass, "get_LayoutSettingData", false, {0x12, settingClass}, {});
        getCapacity = methods.Find(settingClass, "get_MaxCharacterCount", false, {0x08}, {});
        getLayout = methods.Find(threeDClass, "get_CurrentSceneLayout", false, {0x12, layoutClass}, {});
        getPhotoActor = methods.Find(threeDClass, "GetPhotographyActor", false, {0x12, photoActorClass}, {{0x08}});
        getActor = methods.Find(photoActorClass, "get_Actor", false, {0x12, actorInterface}, {});
        getBuilt = methods.Find(photoActorClass, "get_IsActorBuilt", false, {0x02}, {});
        // Controller index is diagnostic only and may be inherited. It never
        // defines a slot or authorizes access to an otherwise missing actor.
        void* indexOwner = actorClass;
        for (unsigned depth = 0; indexOwner && !getIndex && depth < 64; ++depth, indexOwner = g_faceRuntime.parentClass(indexOwner))
            getIndex = methods.Find(indexOwner, "get_index", false, {0x08}, {});
        resolved = modelField && threeDField && getSettings && getCapacity && getLayout && getPhotoActor && getActor && getBuilt;
        return resolved || Fail("ensemble observer exact field/getter contract unavailable");
    }
    inline policy::Pose ReadPose(void* transform)
    {
        policy::Pose pose;
        if (!transform || !g_getWorldPosNative || !g_getWorldRotNative) return pose;
        float p[3]{NAN, NAN, NAN}, q[4]{NAN, NAN, NAN, NAN};
        __try { HelperGetWorldPos(transform, p); HelperGetWorldRot(transform, q); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return pose; }
        pose.position = {p[0], p[1], p[2]}; pose.rotation = {q[0], q[1], q[2], q[3]};
        pose.available = true; pose.available = policy::Finite(pose); return pose;
    }
    inline bool SampleExtraActor(void* actor, uint64_t number, policy::ExtraActorSample& sample)
    {
        auto& registry = GakuMotion::Core::Instance().sceneActorRegistry;
        const auto* entry = registry.Find(actor);
        if (!entry || !entry->token || !frame.available || !frame.owner || frame.scene != g_sceneOwner.sceneHandle ||
            !StageLoader::assignable || !actorClass || !g_faceRuntime.objectClass) return false;
        Roots roots;
        void* held = roots.Keep(actor);
        if (!held || held != actor || !StageLoader::assignable(actorClass, g_faceRuntime.objectClass(held))) return false;
        int instance{}; void* go{};
        if (!Int(StageLoader::instanceId, held, instance) || instance != entry->instanceId ||
            !Call(StageLoader::componentGo, held, nullptr, go) || !go ||
            StageLoader::SceneOfFreshGo(go) != frame.scene) return false;
        void* root = HelperGetComponentTransform(held);
        void* hips = HelperGetBoneTransform(held, 0);
        if (!root || !hips || !g_getParentNative) return false;
        sample.actor = reinterpret_cast<uintptr_t>(actor);
        sample.root = reinterpret_cast<uintptr_t>(root);
        sample.hips = reinterpret_cast<uintptr_t>(hips);
        sample.rootParent = reinterpret_cast<uintptr_t>(g_getParentNative(root));
        sample.hipsParent = reinterpret_cast<uintptr_t>(g_getParentNative(hips));
        sample.owner = frame.owner; sample.scene = frame.scene; sample.instance = instance;
        sample.token = entry->token; sample.epoch = registry.Epoch(); sample.frame = number;
        sample.rootPose = ReadPose(root);
        return policy::Finite(sample.rootPose);
    }
    inline bool Eligible()
    {
        return requested.load(std::memory_order_acquire) && ProtectionOnUnityThread() && g_activeSceneVerified &&
            g_sceneOwner.Valid() && g_sceneOwner.role == GakuMotion::injection::OwnerRole::Photography &&
            g_verifiedOwnerSceneName == "Photography";
    }
    inline bool Capture(uint64_t number)
    {
        frame = {}; frame.number = number;
        if (!Eligible()) return Fail("ensemble observer requires current verified Photography owner");
        if (!Resolve()) return false;
        Roots roots;
        // The current presenter has just been independently verified by the
        // owner gate. The token is not retained as a callable pointer later.
        void* presenter = roots.Keep(reinterpret_cast<void*>(g_sceneOwner.presenterIdentity));
        if (!presenter || g_faceRuntime.objectClass(presenter) != presenterClass) return Fail("ensemble owner type changed");
        void* go{};
        if (!Call(StageLoader::componentGo, presenter, nullptr, go) || !go ||
            StageLoader::SceneOfFreshGo(go) != g_sceneOwner.sceneHandle) return Fail("ensemble owner scene changed");
        void *model{}, *threeD{}, *settings{}, *layout{};
        if (!FieldValue(presenter, modelField, model) || !(model = roots.Keep(model)) ||
            !FieldValue(presenter, threeDField, threeD) || !(threeD = roots.Keep(threeD)) ||
            g_faceRuntime.objectClass(model) != modelClass || g_faceRuntime.objectClass(threeD) != threeDClass ||
            !Call(getSettings, model, nullptr, settings) || !(settings = roots.Keep(settings)) ||
            g_faceRuntime.objectClass(settings) != settingClass || !Int(getCapacity, settings, frame.capacity) ||
            frame.capacity < 1 || frame.capacity > int(policy::SlotLimit) ||
            !Call(getLayout, threeD, nullptr, layout) || !(layout = roots.Keep(layout)))
            return Fail("ensemble layout/capacity unavailable or exceeds supported three-slot bound");
        frame.owner = g_sceneOwner.presenterIdentity; frame.scene = g_sceneOwner.sceneHandle;
        frame.layout = reinterpret_cast<uintptr_t>(layout);
        for (int index = 0; index < frame.capacity; ++index)
        {
            void* wrapper{}; void* args[]{&index};
            if (!Call(getPhotoActor, threeD, args, wrapper)) return Fail("ensemble slot getter failed");
            if (!wrapper) continue;
            wrapper = roots.Keep(wrapper);
            auto& slot = frame.slots[size_t(index)];
            if (!wrapper || g_faceRuntime.objectClass(wrapper) != photoActorClass || !Built(wrapper, slot.built))
                return Fail("ensemble slot wrapper unavailable");
            if (!slot.built) continue;
            void* actor{};
            if (!Call(getActor, wrapper, nullptr, actor) || !(actor = roots.Keep(actor)) ||
                !StageLoader::assignable || !StageLoader::assignable(actorClass, g_faceRuntime.objectClass(actor)) ||
                !Int(StageLoader::instanceId, actor, slot.instanceId) || !slot.instanceId ||
                !Call(StageLoader::componentGo, actor, nullptr, go) || !go ||
                StageLoader::SceneOfFreshGo(go) != frame.scene) return Fail("ensemble actor identity/owner unavailable");
            for (int earlier = 0; earlier < index; ++earlier)
                if (frame.slots[size_t(earlier)].actor == reinterpret_cast<uintptr_t>(actor))
                    return Fail("ensemble slots alias the same actor");
            void* root = HelperGetComponentTransform(actor); void* hips = HelperGetBoneTransform(actor, 0);
            slot.actor = reinterpret_cast<uintptr_t>(actor); slot.actorRoot = reinterpret_cast<uintptr_t>(root);
            slot.hips = reinterpret_cast<uintptr_t>(hips); slot.actorPose = ReadPose(root); slot.beforePose = ReadPose(hips);
            slot.actorParent = reinterpret_cast<uintptr_t>(root ? g_getParentNative(root) : nullptr);
            slot.hipsParent = reinterpret_cast<uintptr_t>(hips ? g_getParentNative(hips) : nullptr);
            slot.identityAvailable = true;
            if (const auto* entry = GakuMotion::Core::Instance().sceneActorRegistry.Find(actor))
                slot.rosterMatched = entry->instanceId == slot.instanceId;
            if (getIndex) Int(getIndex, actor, slot.controllerIndex);
        }
        frame.available = true;
        readyForInjection.store(readiness.Validate(frame));
        available.store(true, std::memory_order_release);
        detail.store("bounded native slot observation; visual verification pending", std::memory_order_release); return true;
    }
    inline void Publish()
    {
        if (!frame.number) return;
        const bool withinLogBudget = !session.Exhausted();
        auto evidence = withinLogBudget ? session.Observe(frame) : policy::Session::Compare(previousFrame, frame);
        if (!withinLogBudget)
        {
            evidence.frames = policy::FrameBudget;
            // Once the diagnostic budget is over, identity changes fail closed
            // until the user explicitly restarts the observation session.
            evidence.readyForInjection = false;
        }
        previousFrame = frame;
        const bool ready = readiness.Observe(frame, evidence, withinLogBudget);
        readyForInjection.store(ready);
        observedFrames.store(evidence.frames); populatedSlots.store(evidence.populatedSlots);
        crossFrameAvailable.store(evidence.crossFrameAvailable); stableActorRoots.store(evidence.stableActorRoots);
        appliedComparisonAvailable.store(evidence.appliedComparisonAvailable);
        if (withinLogBudget && g_hostContext.logInfo && (evidence.frames <= 2 || evidence.frames % 30 == 0 ||
            evidence.changedHipsAfterApply || !evidence.snapshotAvailable))
        {
            g_hostContext.logInfo("[GakuMotion][PhotoEnsembleProbe] frame=%llu owner=%p scene=%d capacity=%d snapshot_available=%d populated=%u cross_frame_available=%d matching=%u actor_roots_stable=%u applied_comparison_available=%d hips_changed_after_prior_apply=%u injection_ready=%d visual_verified=0",
                static_cast<unsigned long long>(frame.number), reinterpret_cast<void*>(frame.owner), frame.scene, frame.capacity,
                evidence.snapshotAvailable, evidence.populatedSlots, evidence.crossFrameAvailable, evidence.matchingSlots,
                evidence.stableActorRoots, evidence.appliedComparisonAvailable, evidence.changedHipsAfterApply, ready);
            for (int i = 0; i < frame.capacity && i < int(policy::SlotLimit); ++i)
            {
                const auto& slot = frame.slots[size_t(i)]; const auto& p = slot.actorPose.position;
                g_hostContext.logInfo("[GakuMotion][PhotoEnsembleSlot] frame=%llu slot=%d built=%d identity_available=%d actor=%p instance=%d controller_index=%d actor_root=%p hips=%p root_pose_available=%d root_xyz=(%.6f,%.6f,%.6f) callbacks=%u",
                    static_cast<unsigned long long>(frame.number), i, slot.built, slot.identityAvailable,
                    reinterpret_cast<void*>(slot.actor), slot.instanceId, slot.controllerIndex,
                    reinterpret_cast<void*>(slot.actorRoot), reinterpret_cast<void*>(slot.hips), slot.actorPose.available,
                    p.x, p.y, p.z, slot.callbacks);
            }
        }
    }
    inline void Tick(uint64_t number, void* callbackActor = nullptr)
    {
        if (!Eligible())
        {
            if (active) { session.Reset(); readiness.Reset(); extraActors.Reset(); extraRosterEpoch = 0; frame = {}; previousFrame = {}; active = false; }
            available.store(false); crossFrameAvailable.store(false); appliedComparisonAvailable.store(false);
            readyForInjection.store(false);
            return;
        }
        const auto generation = requestGeneration.load(std::memory_order_acquire);
        if (!active || activeGeneration != generation)
        {
            session.Reset(); readiness.Reset(); extraActors.Reset(); extraRosterEpoch = 0; frame = {}; previousFrame = {};
            readyForInjection.store(false); active = true; activeGeneration = generation;
        }
        if (!number) return;
        if (frame.number != number)
        {
            Publish();
            Capture(number);
        }
        if (!frame.available || !callbackActor) return;
        const auto epoch = GakuMotion::Core::Instance().sceneActorRegistry.Epoch();
        if (extraRosterEpoch != epoch) { extraActors.Reset(); extraRosterEpoch = epoch; }
        bool officialActor = false;
        for (const auto& slot : frame.slots)
            if (slot.actor == reinterpret_cast<uintptr_t>(callbackActor)) { officialActor = true; break; }
        if (!officialActor)
        {
            policy::ExtraActorSample extra;
            if (SampleExtraActor(callbackActor, number, extra)) extraActors.Observe(extra);
        }
        for (auto& slot : frame.slots)
            if (slot.actor == reinterpret_cast<uintptr_t>(callbackActor))
            {
                ++slot.callbacks;
                int instance{};
                void* root = HelperGetComponentTransform(callbackActor);
                void* hips = HelperGetBoneTransform(callbackActor, 0);
                const auto rootParent = reinterpret_cast<uintptr_t>(root ? g_getParentNative(root) : nullptr);
                const auto hipsParent = reinterpret_cast<uintptr_t>(hips ? g_getParentNative(hips) : nullptr);
                slot.actorPose = ReadPose(root); slot.beforePose = ReadPose(hips);
                if (!Int(StageLoader::instanceId, callbackActor, instance) || instance != slot.instanceId ||
                    reinterpret_cast<uintptr_t>(root) != slot.actorRoot || reinterpret_cast<uintptr_t>(hips) != slot.hips ||
                    rootParent != slot.actorParent || hipsParent != slot.hipsParent)
                { slot.identityAvailable = false; readyForInjection.store(false); }
                readyForInjection.store(readiness.Validate(frame));
            }
    }
    inline bool ReadyForInjection()
    {
        return Eligible() && readyForInjection.load(std::memory_order_acquire) && frame.available &&
            g_faceUnityFrame && frame.number == static_cast<uint64_t>(g_faceUnityFrame());
    }
    inline const policy::Frame& CurrentFrame() noexcept { return frame; } // Unity thread only.
    inline bool ActorEligible(void* actor)
    {
        if (!actor || !Eligible() || !frame.available || !g_faceUnityFrame ||
            frame.number != static_cast<uint64_t>(g_faceUnityFrame())) return false;
        for (const auto& slot : frame.slots)
            if (slot.built && slot.identityAvailable && slot.rosterMatched && slot.actor == reinterpret_cast<uintptr_t>(actor))
            {
                int instance{}; void* go{};
                return Int(StageLoader::instanceId, actor, instance) && instance == slot.instanceId &&
                    Call(StageLoader::componentGo, actor, nullptr, go) && go &&
                    StageLoader::SceneOfFreshGo(go) == frame.scene && frame.scene == g_sceneOwner.sceneHandle &&
                    reinterpret_cast<uintptr_t>(HelperGetComponentTransform(actor)) == slot.actorRoot &&
                    reinterpret_cast<uintptr_t>(HelperGetBoneTransform(actor, 0)) == slot.hips;
            }
        policy::ExtraActorSample extra;
        return SampleExtraActor(actor, frame.number, extra) && extraActors.Eligible(extra);
    }
    inline void NoteApplied(uint64_t number, void* callbackActor)
    {
        if (!Eligible() || !frame.available || frame.number != number || !callbackActor) return;
        for (auto& slot : frame.slots)
            if (slot.actor == reinterpret_cast<uintptr_t>(callbackActor))
            {
                int instance{};
                if (Int(StageLoader::instanceId, callbackActor, instance) && instance == slot.instanceId)
                    slot.appliedPose = ReadPose(HelperGetBoneTransform(callbackActor, 0));
            }
    }
}
