#pragma once

#include "multi_actor_probes.hpp"
#include "vmd_fixture_gen.hpp"
#include "GkJsonLite.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace gkmotion::e2e::multi_actor {

inline std::filesystem::path ResolveRepoRoot() {
    // This header resides in <repo>/tests/e2e_refactor/multi_actor/multi_actor_fixture.hpp
    std::filesystem::path p(__FILE__);
    return p.parent_path().parent_path().parent_path().parent_path();
}

inline std::filesystem::path ResolveMultiActorTmpRoot() {
    std::filesystem::path tmpRoot;
#ifdef _WIN32
    wchar_t* wbuf = nullptr;
    size_t wlen = 0;
    if (_wdupenv_s(&wbuf, &wlen, L"GKE2E_TMP_DIR") == 0 && wbuf != nullptr) {
        if (wbuf[0] != L'\0') {
            tmpRoot = std::filesystem::path(wbuf);
        }
        std::free(wbuf);
    }
#else
    if (const char* envTmp = std::getenv("GKE2E_TMP_DIR"); envTmp && envTmp[0] != '\0') {
        tmpRoot = gke2e::PathFromUtf8(envTmp);
    }
#endif
    if (tmpRoot.empty()) {
        tmpRoot = ResolveRepoRoot() / "build" / "e2e_refactor" / "multi_actor" / "tmp";
    }
    std::error_code ec;
    std::filesystem::create_directories(tmpRoot, ec);
    return tmpRoot;
}

class TempWorkspace {
public:
    explicit TempWorkspace(std::string_view tag) {
        static std::atomic<uint64_t> counter{0};
        const uint64_t seq = ++counter;
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = ResolveMultiActorTmpRoot() / (std::string(tag) + "_" + std::to_string(seq) + "_" + std::to_string(tick));
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
    }

    ~TempWorkspace() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;

    const std::filesystem::path& Root() const noexcept { return root_; }
    std::filesystem::path Sub(std::string_view rel) const { return root_ / rel; }

private:
    std::filesystem::path root_;
};

struct FakeTransform {
    struct FakeActor* owner{nullptr};
    FakeTransform* parent{nullptr};
    GakuMotion::Vector3 localPosition{};
    GakuMotion::Quaternion localRotation{GakuMotion::Quaternion::Identity()};
};

struct FakeAnimator {
    struct FakeActor* owner{nullptr};
    bool enabled{true};
    void* controller{nullptr};
};

struct FakeActor {
    FakeTransform root;
    FakeTransform hips;
    FakeTransform spine;
    FakeTransform chest;
    FakeTransform head;
    FakeTransform leftUpperArm;
    FakeAnimator animator;
    bool alive{true};
    int instanceId{0};
    std::string character;
    std::string displayName;
    std::string faceName;
    std::string presetName;
    int reads{0};
    int writes{0};
    int invalidTouches{0};

    void Init(int id, std::string_view charCode, std::string_view dispName,
              const GakuMotion::Vector3& worldRootPos,
              const GakuMotion::Quaternion& worldRootRot = GakuMotion::Quaternion::Identity()) {
        instanceId = id;
        character = std::string(charCode);
        displayName = std::string(dispName);
        alive = true;
        reads = writes = invalidTouches = 0;

        root.owner = this;
        root.parent = nullptr;
        root.localPosition = worldRootPos;
        root.localRotation = worldRootRot;

        hips.owner = this;
        hips.parent = &root;
        hips.localPosition = {0.0f, 1.0f, 0.0f};
        hips.localRotation = GakuMotion::Quaternion::Identity();

        spine.owner = this;
        spine.parent = &hips;
        spine.localPosition = {0.0f, 0.25f, 0.0f};
        spine.localRotation = GakuMotion::Quaternion::Identity();

        chest.owner = this;
        chest.parent = &spine;
        chest.localPosition = {0.0f, 0.25f, 0.0f};
        chest.localRotation = GakuMotion::Quaternion::Identity();

        head.owner = this;
        head.parent = &chest;
        head.localPosition = {0.0f, 0.22f, 0.0f};
        head.localRotation = GakuMotion::Quaternion::Identity();

        leftUpperArm.owner = this;
        leftUpperArm.parent = &chest;
        leftUpperArm.localPosition = {-0.18f, 0.12f, 0.0f};
        leftUpperArm.localRotation = GakuMotion::Quaternion::Identity();

        animator.owner = this;
        animator.enabled = true;
        animator.controller = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000u + static_cast<unsigned>(id)));
    }
};

inline void TouchActor(FakeActor* actor, bool isWrite = false) {
    if (!actor) return;
    if (isWrite) ++actor->writes;
    else ++actor->reads;
    if (!actor->alive) ++actor->invalidTouches;
}

inline GakuMotion::Quaternion WorldRotation(const FakeTransform* t) {
    if (!t) return GakuMotion::Quaternion::Identity();
    return t->parent ? (WorldRotation(t->parent) * t->localRotation).Normalized() : t->localRotation.Normalized();
}

inline GakuMotion::Vector3 WorldPosition(const FakeTransform* t) {
    if (!t) return {};
    return t->parent
        ? (WorldPosition(t->parent) + WorldRotation(t->parent).Rotate(t->localPosition))
        : t->localPosition;
}

inline void FakeGetWorldPos(void* p, float* out) {
    auto* t = static_cast<FakeTransform*>(p);
    TouchActor(t ? t->owner : nullptr, false);
    const auto v = WorldPosition(t);
    out[0] = v.x; out[1] = v.y; out[2] = v.z;
}

inline void FakeGetWorldRot(void* p, float* out) {
    auto* t = static_cast<FakeTransform*>(p);
    TouchActor(t ? t->owner : nullptr, false);
    const auto q = WorldRotation(t);
    out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
}

inline void FakeSetWorldPos(void* p, float x, float y, float z) {
    auto* t = static_cast<FakeTransform*>(p);
    TouchActor(t ? t->owner : nullptr, true);
    if (!t) return;
    const GakuMotion::Vector3 v{x, y, z};
    t->localPosition = t->parent
        ? WorldRotation(t->parent).Inverse().Rotate(v - WorldPosition(t->parent))
        : v;
}

inline void FakeSetWorldRot(void* p, float x, float y, float z, float w) {
    auto* t = static_cast<FakeTransform*>(p);
    TouchActor(t ? t->owner : nullptr, true);
    if (!t) return;
    const GakuMotion::Quaternion q{x, y, z, w};
    t->localRotation = t->parent ? (WorldRotation(t->parent).Inverse() * q).Normalized() : q.Normalized();
}

inline void FakeGetLocalPos(void* p, float* out) {
    auto* t = static_cast<FakeTransform*>(p);
    TouchActor(t ? t->owner : nullptr, false);
    if (!t) { out[0] = out[1] = out[2] = 0.0f; return; }
    out[0] = t->localPosition.x; out[1] = t->localPosition.y; out[2] = t->localPosition.z;
}

inline void FakeGetLocalRot(void* p, float* out) {
    auto* t = static_cast<FakeTransform*>(p);
    TouchActor(t ? t->owner : nullptr, false);
    if (!t) { out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; return; }
    out[0] = t->localRotation.x; out[1] = t->localRotation.y; out[2] = t->localRotation.z; out[3] = t->localRotation.w;
}

inline void FakeSetLocalPos(void* p, float x, float y, float z) {
    auto* t = static_cast<FakeTransform*>(p);
    TouchActor(t ? t->owner : nullptr, true);
    if (t) t->localPosition = {x, y, z};
}

inline void FakeSetLocalRot(void* p, float x, float y, float z, float w) {
    auto* t = static_cast<FakeTransform*>(p);
    TouchActor(t ? t->owner : nullptr, true);
    if (t) t->localRotation = {x, y, z, w};
}

inline void* FakeGetParent(void* p) {
    auto* t = static_cast<FakeTransform*>(p);
    return t ? t->parent : nullptr;
}

inline void* FakeGetBoneTransform(void* actorPtr, int hid) {
    auto* a = static_cast<FakeActor*>(actorPtr);
    TouchActor(a, false);
    if (!a) return nullptr;
    switch (hid) {
        case 0: return &a->hips;
        case 7: return &a->spine;
        case 8: return &a->chest;
        case 10: return &a->head;
        case 13: return &a->leftUpperArm;
        default: return nullptr;
    }
}

inline void* FakeGetComponentTransform(void* actorPtr) {
    auto* a = static_cast<FakeActor*>(actorPtr);
    TouchActor(a, false);
    return a ? &a->root : nullptr;
}

inline void* FakeGetAnimatorController(void* animPtr) {
    auto* anim = static_cast<FakeAnimator*>(animPtr);
    TouchActor(anim ? anim->owner : nullptr, false);
    return anim ? anim->controller : nullptr;
}

inline void FakeSetAnimatorController(void* animPtr, void* ctrl) {
    auto* anim = static_cast<FakeAnimator*>(animPtr);
    TouchActor(anim ? anim->owner : nullptr, true);
    if (anim) anim->controller = ctrl;
}

inline bool FakeGetAnimatorEnabled(void* animPtr) {
    auto* anim = static_cast<FakeAnimator*>(animPtr);
    TouchActor(anim ? anim->owner : nullptr, false);
    return anim ? anim->enabled : false;
}

inline void FakeSetAnimatorEnabled(void* animPtr, bool val) {
    auto* anim = static_cast<FakeAnimator*>(animPtr);
    TouchActor(anim ? anim->owner : nullptr, true);
    if (anim) anim->enabled = val;
}

inline bool FakeCanDriveActor(void* actorPtr) {
    auto* a = static_cast<FakeActor*>(actorPtr);
    return a && a->alive;
}

inline void WireIdolController(GakuMotion::IdolController& ctrl) {
    ctrl.fnGetBoneTransform = FakeGetBoneTransform;
    ctrl.fnGetComponentTransform = FakeGetComponentTransform;
    ctrl.fnGetTransformParent = FakeGetParent;
    ctrl.fnGetWorldPosition = FakeGetWorldPos;
    ctrl.fnSetWorldPosition = FakeSetWorldPos;
    ctrl.fnGetWorldRotation = FakeGetWorldRot;
    ctrl.fnSetWorldRotation = FakeSetWorldRot;
    ctrl.fnGetLocalPosition = FakeGetLocalPos;
    ctrl.fnSetLocalPosition = FakeSetLocalPos;
    ctrl.fnGetLocalRotation = FakeGetLocalRot;
    ctrl.fnSetLocalRotation = FakeSetLocalRot;
    ctrl.fnGetAnimatorController = FakeGetAnimatorController;
    ctrl.fnSetAnimatorController = FakeSetAnimatorController;
    ctrl.fnGetAnimatorEnabled = FakeGetAnimatorEnabled;
    ctrl.fnSetAnimatorEnabled = FakeSetAnimatorEnabled;
    ctrl.fnCanDriveActor = FakeCanDriveActor;
    ctrl.useAnimatorRebind = false;
    ctrl.SetExperimentalWaistSupportEnabled(false);
    ctrl.auxDriveMode = GakuMotion::kAuxSilence;
}

struct FakeCameraRig {
    bool cameraAlive{true};
    bool transformAlive{true};
    GakuMotion::CameraFrame currentFrame{};
    int readCalls{0};
    int writeCalls{0};
    int tickProbeCalls{0};
    bool vcamModified{false};
    bool vcamConfirmed{true};
    bool allowChest{true};
    bool allowHead{true};

    void ResetDefault() {
        cameraAlive = true;
        transformAlive = true;
        currentFrame = {};
        currentFrame.position = {0.0f, 1.5f, -3.5f};
        currentFrame.rotation = GakuMotion::Quaternion::Identity();
        currentFrame.fov = 45.0f;
        currentFrame.orthographicSize = 2.0f;
        readCalls = writeCalls = tickProbeCalls = 0;
        vcamModified = false;
        vcamConfirmed = true;
        allowChest = allowHead = true;
    }
};

inline FakeCameraRig*& ActiveFakeCameraRig() {
    static FakeCameraRig* rig = nullptr;
    return rig;
}

inline void WireCameraController(GakuMotion::CameraController& cam, FakeCameraRig& rig) {
    ActiveFakeCameraRig() = &rig;
    cam.fnGetCamera = []() -> void* {
        auto* r = ActiveFakeCameraRig();
        return (r && r->cameraAlive) ? r : nullptr;
    };
    cam.fnGetTransform = [](void* c) -> void* {
        auto* r = static_cast<FakeCameraRig*>(c);
        return (r && r->transformAlive) ? &r->currentFrame : nullptr;
    };
    cam.fnAlive = [](void* p) -> bool {
        auto* r = ActiveFakeCameraRig();
        if (!r || !p) return false;
        if (p == r) return r->cameraAlive;
        if (p == &r->currentFrame) return r->transformAlive;
        // Could also be an actor pointer
        return static_cast<FakeActor*>(p)->alive;
    };
    cam.fnRead = [](void*, void*, GakuMotion::CameraFrame& out) -> bool {
        auto* r = ActiveFakeCameraRig();
        if (!r || !r->cameraAlive || !r->transformAlive) return false;
        ++r->readCalls;
        out = r->currentFrame;
        return true;
    };
    cam.fnWrite = [](void*, void*, const GakuMotion::CameraFrame& in) {
        auto* r = ActiveFakeCameraRig();
        if (!r || !r->cameraAlive || !r->transformAlive) return;
        ++r->writeCalls;
        r->currentFrame = in;
    };
    cam.fnActorPose = [](void* actorPtr, GakuMotion::Vector3& origin, GakuMotion::Quaternion& rot, GakuMotion::Vector3& hips) -> bool {
        auto* a = static_cast<FakeActor*>(actorPtr);
        if (!a || !a->alive) return false;
        origin = WorldPosition(&a->root);
        rot = WorldRotation(&a->root);
        hips = WorldPosition(&a->hips);
        return true;
    };
    cam.fnFollowTarget = [](void* actorPtr, GakuMotion::CameraFollowTarget target, GakuMotion::Vector3& out) -> bool {
        auto* a = static_cast<FakeActor*>(actorPtr);
        auto* r = ActiveFakeCameraRig();
        if (!a || !a->alive) return false;
        switch (target) {
            case GakuMotion::CameraFollowTarget::Hips:
                out = WorldPosition(&a->hips);
                return true;
            case GakuMotion::CameraFollowTarget::Chest:
                if (r && !r->allowChest) return false;
                out = WorldPosition(&a->chest);
                return true;
            case GakuMotion::CameraFollowTarget::Head:
                if (r && !r->allowHead) return false;
                out = WorldPosition(&a->head);
                return true;
        }
        return false;
    };
    cam.fnReleaseVcam = [](bool) {};
    cam.fnReleaseVcamForCapture = []() -> GakuMotion::CaptureRestoreStatus {
        auto* r = ActiveFakeCameraRig();
        if (!r) return {false, true};
        return {r->vcamModified, r->vcamConfirmed};
    };
}

struct AudioCounterState {
    int plays{0};
    int pauses{0};
    int stops{0};
    int resumes{0};
    int seeks{0};
    std::string lastPath;

    void Reset() {
        plays = pauses = stops = resumes = seeks = 0;
        lastPath.clear();
    }
};

inline AudioCounterState& GlobalAudioCounters() {
    static AudioCounterState state;
    return state;
}

inline void WireCoreAudioCounters(GakuMotion::Core& core) {
    GlobalAudioCounters().Reset();
    core.player.fnPlayAudio = [](const std::string& path) {
        ++GlobalAudioCounters().plays;
        GlobalAudioCounters().lastPath = path;
    };
    core.player.fnPauseAudio = []() { ++GlobalAudioCounters().pauses; };
    core.player.fnStopAudio = []() { ++GlobalAudioCounters().stops; };
    core.player.fnResumeAudio = [](float) { ++GlobalAudioCounters().resumes; };
    core.player.fnSeekAudio = [](float) { ++GlobalAudioCounters().seeks; };
    core.player.fnSeekPausedAudio = [](float) { ++GlobalAudioCounters().seeks; };
}

inline bool NearFloat(float a, float b, float eps = 1e-4f) {
    return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= eps;
}

inline bool NearVec3(const GakuMotion::Vector3& a, const GakuMotion::Vector3& b, float eps = 1e-4f) {
    return NearFloat(a.x, b.x, eps) && NearFloat(a.y, b.y, eps) && NearFloat(a.z, b.z, eps);
}

inline bool NearQuat(const GakuMotion::Quaternion& a, const GakuMotion::Quaternion& b, float eps = 1e-4f) {
    const auto na = a.Normalized();
    const auto nb = b.Normalized();
    const float dot = std::fabs(na.x * nb.x + na.y * nb.y + na.z * nb.z + na.w * nb.w);
    return std::isfinite(dot) && dot >= (1.0f - eps);
}

inline uint64_t FileFnv1a64(const std::filesystem::path& path, bool& ok) {
    ok = false;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return 0;
    uint64_t h = 14695981039346656037ull;
    char buf[4096];
    while (ifs.good()) {
        ifs.read(buf, sizeof(buf));
        const auto n = ifs.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= static_cast<uint8_t>(buf[i]);
            h *= 1099511628211ull;
        }
    }
    ok = true;
    return h;
}

inline std::vector<GakuMotion::SceneActorObservation> MakeRoster(std::vector<FakeActor>& actors) {
    std::vector<GakuMotion::SceneActorObservation> out;
    out.reserve(actors.size());
    for (auto& a : actors) {
        GakuMotion::SceneActorObservation obs;
        obs.actor = &a;
        obs.instanceId = a.instanceId;
        obs.character = a.character;
        obs.displayName = a.displayName;
        obs.faceName = a.faceName;
        obs.presetName = a.presetName;
        out.push_back(std::move(obs));
    }
    return out;
}

} // namespace gkmotion::e2e::multi_actor
