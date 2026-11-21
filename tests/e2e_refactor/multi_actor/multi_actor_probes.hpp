#pragma once

#include "GakuMotion.hpp"
#include "GkCamera.hpp"
#include "GkIdolController.hpp"
#include "GkGuiOverlay.hpp"
#include "GkSceneActors.hpp"
#include "GkRuntimeConfig.hpp"
#include "GkMotionPlayer.hpp"
#include "GkBurstBridgeMultiClient.hpp"
#include "e2e_report.hpp"

#if __has_include("GkStageFormation.hpp")
#define GK_E2E_HAS_STAGE_FORMATION 1
#include "GkStageFormation.hpp"
#else
#define GK_E2E_HAS_STAGE_FORMATION 0
#endif

#include <string>
#include <string_view>
#include <type_traits>

namespace gkmotion::e2e::multi_actor::probes {

template <typename T>
concept HasFollowSlotIndex = requires(const T& x) { x.followSlotIndex; };

template <typename T>
concept HasLegacyEnsembleProbe = requires(const T& x) { x.fnEnsembleProbe; };

template <typename T>
concept HasLegacyEnsembleSet = requires(const T& x) { x.fnSetEnsemble; };

template <typename T>
concept HasLegacyEnsemblePreset = requires(const T& x) { x.fnEnsemblePreset; };

template <typename T>
concept HasLegacyEnsembleSnapshot = requires(const T& x) { x.ensembleSnapshot; };

template <typename T>
concept HasLegacyEnsembleDraft = requires(const T& x) { x.ensembleDraft; };

template <typename T>
concept HasLegacyEnsembleActive = requires(const T& x) { x.EnsembleActive(); };

template <typename T>
concept HasFormationState = requires(const T& x) { x.formation; };

template <typename T>
concept HasStageFormationMember = requires(T& x) { x.stageFormation; };

template <typename T>
concept HasAnyFormationMember = HasFormationState<T> || HasStageFormationMember<T>;

template <typename T>
concept HasSelectSlot = requires(T& x) { x.SelectSlot(0); };

template <typename T>
concept HasSwapSlots = requires(T& x) { x.SwapSlots(0, 1); };

template <typename T>
concept HasSetSlotOffset = requires(T& x) { x.SetSlotOffset(0, GakuMotion::Vector3{}, 0.0f); };

template <typename T>
concept HasSaveSlotOffset = requires(T& x) { x.SaveSlotOffset(0); };

template <typename T>
concept HasSetSlotSource = requires(T& x) { x.SetSlotSource(0, 0); };

template <typename T>
concept HasSelectFormationSlot = requires(T& x) { x.SelectFormationSlot(0); };

template <typename T>
concept HasSwapFormationSlots = requires(T& x) { x.SwapFormationSlots(0, 1); };

template <typename T>
concept HasSetFormationSlotOffset = requires(T& x) { x.SetFormationSlotOffset(0, GakuMotion::Vector3{}, 0.0f); };

template <typename T>
concept HasSaveFormationSlotOffset = requires(T& x) { x.SaveFormationSlotOffset(0); };

template <typename T>
concept HasSetFormationSlotSource = requires(T& x) { x.SetFormationSlotSource(0, 0); };

template <typename T>
concept HasFormationApi =
    (HasSelectSlot<T> && HasSwapSlots<T>) ||
    (HasSelectFormationSlot<T> && HasSwapFormationSlots<T>) ||
    HasAnyFormationMember<T>;

template <typename T>
concept HasSetReferenceAnchor = requires(T& ctrl, const GakuMotion::Vector3& pos, const GakuMotion::Quaternion& rot) {
    ctrl.SetReferenceAnchor(pos, rot);
};

template <typename T>
concept HasSetAnchorOverride = requires(T& ctrl, const GakuMotion::Vector3& pos, const GakuMotion::Quaternion& rot) {
    ctrl.SetAnchorOverride(pos, rot);
};

template <typename T>
concept HasClearReferenceAnchor = requires(T& ctrl) {
    ctrl.ClearReferenceAnchor();
};

template <typename T>
concept HasClearAnchorOverride = requires(T& ctrl) {
    ctrl.ClearAnchorOverride();
};

template <typename T>
concept HasAnchorOverride =
    HasSetReferenceAnchor<T> ||
    HasSetAnchorOverride<T> ||
    requires(const T& ctrl) { ctrl.referenceAnchorPos; } ||
    requires(const T& ctrl) { ctrl.anchorPos; };

template <typename T>
concept HasActorFacialWeightsSave = requires(T& c) {
    c.SaveFacialWeights();
};

template <typename T>
concept HasSetSlotFacialWeights = requires(T& c) {
    c.SetSlotFacialWeights(0, GakuMotion::FacialWeights{});
};

template <typename T>
concept HasFormationVerified = requires(const T& f) { f.verified; };

template <typename T>
concept HasFormationValidated = requires(const T& f) { f.validated; };

template <typename T>
concept HasStageFormationMemberGui = requires(const T& x) { x.stageFormation; };

inline constexpr bool IsM3CoreReady() noexcept {
    return (GK_E2E_HAS_STAGE_FORMATION != 0) &&
           HasAnyFormationMember<GakuMotion::Core> &&
           HasFormationApi<GakuMotion::Core> &&
           HasAnchorOverride<GakuMotion::IdolController> &&
           HasFollowSlotIndex<GakuMotion::CameraSettings> &&
           !HasLegacyEnsembleProbe<GakuMotion::GuiContext> &&
           !HasLegacyEnsembleSnapshot<GakuMotion::GuiContext> &&
           !HasLegacyEnsembleActive<GakuMotion::Core>;
}

inline constexpr bool IsM3ImGuiReady() noexcept {
    return !HasLegacyEnsembleProbe<GakuMotion::GuiContext> &&
           !HasLegacyEnsembleSnapshot<GakuMotion::GuiContext> &&
           !HasLegacyEnsembleDraft<GakuMotion::GuiOverlay> &&
           !HasLegacyEnsembleActive<GakuMotion::Core>;
}

inline bool RequireM3CoreOrPending(gke2e::CaseContext& ctx, std::string_view featureNote) {
    if constexpr (IsM3CoreReady()) {
        (void)featureNote;
        return true;
    } else {
        std::string msg = "PENDING_IMPL (pre-M3): ";
        msg.append(featureNote);
        msg.append(" [hasStageFormation=");
        msg.append(GK_E2E_HAS_STAGE_FORMATION ? "1" : "0");
        msg.append(", hasFormationMember=");
        msg.append(HasAnyFormationMember<GakuMotion::Core> ? "1" : "0");
        msg.append(", hasFormationApi=");
        msg.append(HasFormationApi<GakuMotion::Core> ? "1" : "0");
        msg.append(", hasAnchorOverride=");
        msg.append(HasAnchorOverride<GakuMotion::IdolController> ? "1" : "0");
        msg.append(", hasFollowSlotIndex=");
        msg.append(HasFollowSlotIndex<GakuMotion::CameraSettings> ? "1" : "0");
        msg.append(", legacyProbeRemoved=");
        msg.append(!HasLegacyEnsembleProbe<GakuMotion::GuiContext> ? "1" : "0");
        msg.append(", legacyCoreRemoved=");
        msg.append(!HasLegacyEnsembleActive<GakuMotion::Core> ? "1" : "0");
        msg.append("]");
        ctx.Pending(msg);
        return false;
    }
}

inline bool RequireM3ImGuiOrPending(gke2e::CaseContext& ctx, std::string_view featureNote) {
    if constexpr (IsM3ImGuiReady()) {
        (void)featureNote;
        return true;
    } else {
        std::string msg = "PENDING_IMPL (pre-M3 ImGui): ";
        msg.append(featureNote);
        msg.append(" [legacyGuiContextRemoved=");
        msg.append(!HasLegacyEnsembleSnapshot<GakuMotion::GuiContext> ? "1" : "0");
        msg.append(", legacyGuiOverlayRemoved=");
        msg.append(!HasLegacyEnsembleDraft<GakuMotion::GuiOverlay> ? "1" : "0");
        msg.append("]");
        ctx.Pending(msg);
        return false;
    }
}

template <typename Fn>
inline bool WithCameraFollowSlotIndex(GakuMotion::CameraSettings& settings, gke2e::CaseContext& ctx,
                                      std::string_view reason, Fn&& fn) {
    if constexpr (HasFollowSlotIndex<GakuMotion::CameraSettings>) {
        fn(settings);
        return true;
    } else {
        (void)settings;
        (void)fn;
        ctx.Pending(reason);
        return false;
    }
}

template <typename Fn>
inline bool WithAnchorController(GakuMotion::IdolController& ctrl, gke2e::CaseContext& ctx,
                                 std::string_view reason, Fn&& fn) {
    if constexpr (HasAnchorOverride<GakuMotion::IdolController>) {
        fn(ctrl);
        return true;
    } else {
        (void)ctrl;
        (void)fn;
        ctx.Pending(reason);
        return false;
    }
}

template <typename Fn>
inline bool WithFormationCore(GakuMotion::Core& core, gke2e::CaseContext& ctx,
                              std::string_view reason, Fn&& fn) {
    if constexpr (IsM3CoreReady() && HasFormationApi<GakuMotion::Core>) {
        fn(core);
        return true;
    } else {
        (void)core;
        (void)fn;
        ctx.Pending(reason);
        return false;
    }
}

} // namespace gkmotion::e2e::multi_actor::probes
