#pragma once

#include "../GkRuntimeConfig.hpp"
#include "../GkMotionPlayer.hpp"
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace GakuMotion::webpanel
{
	// -------------------------------------------------------------------------
	// Host hooks supplied by the plugin TU (Windows/services) or by tests.
	// -------------------------------------------------------------------------
	struct DropPeek
	{
		bool ok{false};
		std::string visual{"none"}; // none | armed | hover | invalid
		uint32_t pendingCount{0};
		uint32_t fileCount{0};
		uint32_t accepted{0};
		std::vector<std::string> dragPaths;
		bool hostElevated{false};
		bool hostBlocked{false};
	};

	struct DropBatch
	{
		bool ok{false};
		std::vector<std::string> paths;
		std::string notice;
	};

	struct AppearanceInfo
	{
		bool ok{false};
		int theme{0};
		float uiScale{1.0f};
	};

	struct AudioEffectPresetView
	{
		std::string id;
		std::string name;
		std::string category;
		std::string description;
	};

	struct AudioEffectState
	{
		bool available{false};
		std::string activeId{"flat"};
		std::vector<AudioEffectPresetView> presets;
	};

	struct AudioProbeView
	{
		bool valid{false};
		uint32_t sampleRate{0};
		uint32_t channels{0};
		bool hasCover{false};
		double durationSeconds{0.0};
		std::string codec;
		std::string title;
		std::string artist;
		std::string album;
		std::string coverDataUri;
	};

	struct ScreenshotCapabilitiesView
	{
		bool available{false};
		bool runtimeReady{false};
		bool cameraAvailable{false};
		uint32_t windowWidth{0};
		uint32_t windowHeight{0};
		std::string notice;
	};

	struct ScreenshotCaptureView
	{
		bool ok{false};
		uint32_t width{0};
		uint32_t height{0};
		std::string dataUri;
		std::string error;
	};

	struct AsyncTrackCoverResult
	{
		std::string key;
		bool hasCover{false};
		std::string coverDataUri;
	};

	struct HostHooks
	{
		// Ordered asset-directory candidates supplied by the plugin (the plugin
		// module directory's GakuMotion.UI). GetAssets picks the first one that
		// actually contains index.html.
		std::function<std::vector<std::wstring>()> assetsDirs;
		std::function<bool(std::string& notice)> openLibraryFolder;
		std::function<bool(int webKind, bool multiple, std::string& notice)> beginWebFileDialog;
		std::function<void()> releaseFileDialog;
		std::function<bool(bool enabled, std::string& notice)> dropSetTarget;
		std::function<DropPeek()> dropPeek;
		std::function<DropBatch()> dropConsume;
		std::function<AppearanceInfo()> appearance;
		std::function<AudioProbeView(const std::string& pathUtf8, bool includeCover)> probeAudio;
		std::function<std::string(const std::string& trackDirUtf8)> getTrackCoverDataUri;
		std::function<bool(const std::string& key, const std::string& trackDirUtf8, std::string& outDataUri, bool& outPending)> queryTrackCoverAsync;
		std::function<std::vector<AsyncTrackCoverResult>(size_t maxCount)> drainReadyCovers;
		std::function<void(const std::string& key)> invalidateTrackCover;
		std::function<std::string(const std::string& pathUtf8)> readImagePreviewDataUri;
		std::function<bool(const std::string& normCharCode, std::string& outDataUri, bool& outCached)> getActorPortraitDataUri;
		std::function<ScreenshotCapabilitiesView()> getScreenshotCapabilities;
		std::function<ScreenshotCaptureView()> captureScreenshot;
		std::function<AudioEffectState()> getAudioEffectsState;
		std::function<bool(const std::string& presetId, std::string& error)> setAudioEffectPreset;
		std::string pluginVersion{"0.2.0-beta.1"};
	};

	// -------------------------------------------------------------------------
	// Command result helpers.
	// -------------------------------------------------------------------------
	struct CmdResult
	{
		bool ok{true};
		std::string body{"{}"};
		std::string code;
		std::string message;
		std::string details;

		static CmdResult Ok(std::string result = "{}") { CmdResult r; r.body = std::move(result); return r; }
		static CmdResult Err(std::string code_, std::string message_, std::string details_ = "")
		{
			CmdResult r;
			r.ok = false;
			r.code = std::move(code_);
			r.message = std::move(message_);
			r.details = std::move(details_);
			return r;
		}
	};

	inline constexpr const char* kSupportedMethods[] = {
		"bridge.hello", "bridge.ping", "motion.hello", "state.get",
		"player.play", "player.pause", "player.resume", "player.stop", "player.cycle",
		"player.seek", "player.seekFrame", "player.stepFrame",
		"player.scrubBegin", "player.scrubUpdate", "player.scrubEnd",
		"player.setSpeed", "player.setVolume", "player.saveVolume", "player.reload",
		"playlist.setMode",
		"library.refresh", "library.select", "library.delete", "library.openFolder",
		"library.import", "library.probeFiles", "library.startEdit",
		"library.probeAudio", "library.getCover", "library.setCover", "library.readImagePreview",
		"screenshot.capabilities", "screenshot.capture",
		"clock.setRunMode", "clock.configure",
		"audioEffect.set",
		"actors.selectTarget", "actors.getPortrait",
		"formation.selectSlot", "formation.swapSlots", "formation.setSlotOffset",
		"formation.resetSlotOffset", "formation.saveSlotOffset", "formation.setSlotSource",
		"facial.setWeights", "facial.save",
		"position.set", "position.save",
		"camera.set", "camera.save",
		"stage.set", "stage.scan", "stage.toggleNode", "stage.setLoadReplacement",
		"settings.setPhysics",
		"shell.setTextInputActive",
		"appearance.get",
		"dialog.pickFile", "dialog.poll",
		"drop.setTarget", "drop.poll", "drop.peek",
		"bone.get", "bone.set", "bone.reset", "bone.resetAll", "bone.setMaster",
		"bone.reload", "bone.saveToMotion", "bone.saveToGlobal",
		"gaze.get", "gaze.set", "gaze.cycle",
		"sole.get", "sole.set", "sole.save", "sole.load",
		"solePitch.get", "solePitch.set",
		"studio.get", "studio.set", "studio.reset",
		"experiments.get", "experiments.set",
		"telemetry.get",
	};

	inline constexpr const char* kSupportedEvents[] = {
		"player.stateChanged", "permission.changed", "library.changed", "library.coverReady", "loaded.changed",
		"playlist.changed", "actors.changed", "formation.changed", "camera.changed", "stage.changed",
		"stage.nodesChanged",
		"facial.changed", "position.changed", "settings.changed",
		"import.progress", "appearance.changed", "bridge.closed",
		"gaze.changed", "bone.changed", "sole.changed", "solePitch.changed",
		"studio.changed", "experiments.changed",
		"clock.changed", "audioEffect.changed",
	};
} // namespace GakuMotion::webpanel
