#include <map>
#include "GkLog.hpp"
#include "GkWebPanelBridge.hpp"
#include "GkSceneActors.hpp"
#include "GkCamera.hpp"
#include "GkPlaylist.hpp"
#include "GkRuntimeConfig.hpp"

#if defined(__has_include)
#  if __has_include("GkPortraitCache.hpp")
#    define GK_E2E_HAS_PORTRAIT_CACHE 1
#    include "GkPortraitCache.hpp"
#  else
#    define GK_E2E_HAS_PORTRAIT_CACHE 0
#  endif
#  if __has_include("GkStageFormation.hpp")
#    define GK_E2E_HAS_STAGE_FORMATION 1
#    include "GkStageFormation.hpp"
#  else
#    define GK_E2E_HAS_STAGE_FORMATION 0
#  endif
#else
#  define GK_E2E_HAS_PORTRAIT_CACHE 0
#  define GK_E2E_HAS_STAGE_FORMATION 0
#endif

#include "e2e_report.hpp"

#include <iterator>
#include <string_view>

GK_E2E_DEFINE_LOG_SINK()

namespace {

template <std::size_t N>
bool ContainsName(const char* const (&arr)[N], std::string_view needle) {
    for (std::size_t i = 0; i < N; ++i) {
        if (arr[i] && std::string_view(arr[i]) == needle) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    gke2e::Runner runner;

    runner.Add("SP-M1-01", 1, "F2", "Probe MotionEntry group-song fields",
        [](gke2e::CaseContext& ctx) {
            GakuMotion::MotionEntry entry;
            GKE2E_WITH_MEMBER_PROBE(ctx, gke2e::probes::HasIsGroup, entry,
                "M1 not yet merged: MotionEntry::isGroup absent", {
                GKE2E_CHECK_EQ(ctx, probed.isGroup, false);
            });
            GKE2E_WITH_MEMBER_PROBE(ctx, gke2e::probes::HasPositionCount, entry,
                "M1 not yet merged: MotionEntry::positionCount absent", {
                GKE2E_CHECK(ctx, probed.positionCount >= 0);
            });
            GKE2E_WITH_MEMBER_PROBE(ctx, gke2e::probes::HasPositions, entry,
                "M1 not yet merged: MotionEntry::positions absent", {
                GKE2E_CHECK(ctx, probed.positions.empty());
            });
            GKE2E_PASS(ctx, "M1 MotionEntry group fields present");
        });

    runner.Add("SP-M2-01", 1, "F5", "Probe GkPortraitCache.hpp header presence",
        [](gke2e::CaseContext& ctx) {
            GKE2E_REQUIRE_HEADER(ctx, GK_E2E_HAS_PORTRAIT_CACHE, "GkPortraitCache.hpp");
            GKE2E_PASS(ctx, "GkPortraitCache.hpp present");
        });

    runner.Add("SP-M2-02", 1, "F6", "Probe SceneActorObservation and GuiSceneActor shortName",
        [](gke2e::CaseContext& ctx) {
            GakuMotion::SceneActorObservation obs;
            GKE2E_WITH_MEMBER_PROBE(ctx, gke2e::probes::HasShortName, obs,
                "M2 not yet merged: SceneActorObservation::shortName absent", {
                GKE2E_CHECK(ctx, probed.shortName.empty());
            });
            GakuMotion::GuiSceneActor guiActor;
            GKE2E_WITH_MEMBER_PROBE(ctx, gke2e::probes::HasCharacter, guiActor,
                "M2 not yet merged: GuiSceneActor::character absent", {
                GKE2E_CHECK(ctx, probed.character.empty());
            });
            GKE2E_PASS(ctx, "M2 SceneActor metadata fields present");
        });

    runner.Add("SP-M3-01", 1, "F9", "Probe GkStageFormation.hpp header presence",
        [](gke2e::CaseContext& ctx) {
            GKE2E_REQUIRE_HEADER(ctx, GK_E2E_HAS_STAGE_FORMATION, "GkStageFormation.hpp");
            GKE2E_PASS(ctx, "GkStageFormation.hpp present");
        });

    runner.Add("SP-M3-02", 1, "F11", "Probe CameraSettings::followSlotIndex",
        [](gke2e::CaseContext& ctx) {
            GakuMotion::CameraSettings cam;
            GKE2E_WITH_MEMBER_PROBE(ctx, gke2e::probes::HasFollowSlotIdx, cam,
                "M3 not yet merged: CameraSettings::followSlotIndex absent", {
                GKE2E_CHECK(ctx, probed.followSlotIndex >= -1);
            });
            GKE2E_PASS(ctx, "M3 CameraSettings::followSlotIndex present");
        });

    runner.Add("SP-M4-01", 1, "F13", "Probe WebView2 bridge portrait and formation RPCs",
        [](gke2e::CaseContext& ctx) {
            if (!ContainsName(GakuMotion::webpanel::kSupportedMethods, "actors.getPortrait")) {
                GKE2E_PENDING(ctx, "M4 not yet merged: actors.getPortrait not registered");
            }
            if (!ContainsName(GakuMotion::webpanel::kSupportedMethods, "formation.selectSlot")) {
                GKE2E_PENDING(ctx, "M4 not yet merged: formation.selectSlot not registered");
            }
            if (!ContainsName(GakuMotion::webpanel::kSupportedEvents, "formation.changed")) {
                GKE2E_PENDING(ctx, "M4 not yet merged: formation.changed not registered");
            }
            GKE2E_PASS(ctx, "M4 bridge portrait and formation RPCs present");
        });

    return runner.Run(argc, argv);
}
