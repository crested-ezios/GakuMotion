#pragma once

#include "../include/GakuAudio.h"
#include "../include/GakuPlugin.h"
#include "../include/GakuPluginMetadata.h"
#include "../include/GkAudioMetadata.hpp"
#include "GkAudioPlayback.hpp"
#include "GkJsonLite.hpp"
#include "GkMotionData.hpp"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace GakuMotion::audio
{
	struct EffectPresetInfo
	{
		std::string id;
		std::string name;
		std::string category;
		std::string description;
	};

	struct AudioProbeResult
	{
		bool valid{false};
		uint32_t sampleRate{0};
		uint32_t channels{0};
		bool hasCover{false};
		uint32_t coverByteSize{0};
		double durationSeconds{0.0};
		std::string codec;
		std::string title;
		std::string artist;
		std::string album;
		std::string coverMime;
		std::vector<uint8_t> coverBytes;
	};

	// Thin wrapper over "gakuaudio.core/v1" (GakuAudio plugin) with offline metadata
	// probing fallback via GkAudioMetadata.hpp so track inspection works even when
	// GakuAudio is absent or in headless unit tests.
	class AudioPluginClient
	{
	public:
		struct PluginAvailability
		{
			bool registryChecked{false};
			bool registryAvailable{false};
			bool knownUnavailable{false};
			GakuPluginLoadState state{GAKU_PLUGIN_STATE_NOT_FOUND};
			std::string version;
		};

		static const char* PluginStateName(GakuPluginLoadState state)
		{
			switch (state)
			{
			case GAKU_PLUGIN_STATE_NOT_FOUND: return "not_found";
			case GAKU_PLUGIN_STATE_DISABLED: return "disabled";
			case GAKU_PLUGIN_STATE_DEPENDENCY_ERROR: return "dependency_error";
			case GAKU_PLUGIN_STATE_INCOMPATIBLE: return "incompatible";
			case GAKU_PLUGIN_STATE_QUERIED: return "queried";
			case GAKU_PLUGIN_STATE_ACTIVATED: return "activated";
			case GAKU_PLUGIN_STATE_RUNTIME_READY: return "runtime_ready";
			case GAKU_PLUGIN_STATE_FAILED: return "failed";
			default: return "unknown";
			}
		}

		void BindHost(const GakuHostApi* hostApi)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			hostApi_ = hostApi;
			directService_ = nullptr;
			cachedService_ = nullptr;
			queriedService_ = GakuAudioServiceV1{};
			availability_ = PluginAvailability{};
			unavailableNotified_ = false;
		}

		void BindDirectService(const GakuAudioServiceV1* service)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			directService_ = IsValidService(service) ? service : nullptr;
			cachedService_ = directService_;
			queriedService_ = GakuAudioServiceV1{};
			availability_ = PluginAvailability{};
			unavailableNotified_ = false;
		}

		PluginAvailability InspectPluginAvailability(bool refreshIfPending = false)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			InspectHostPluginsLocked(refreshIfPending);
			return availability_;
		}

		bool TryClaimUnavailableNotice(PluginAvailability& out, bool refreshIfPending = false)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			InspectHostPluginsLocked(refreshIfPending);
			out = availability_;
			if (!availability_.knownUnavailable || unavailableNotified_) return false;
			unavailableNotified_ = true;
			return true;
		}

		void SetConfigRoot(const std::filesystem::path& configRoot)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			configRoot_ = configRoot;
			LoadPersistedPresetLocked();
		}

		bool Available()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return ResolveLocked() != nullptr;
		}

		bool OpenAndPlay(const std::string& pathUtf8, double startSeconds)
		{
			const GakuAudioServiceV1* svc = Resolve();
			if (!svc || !svc->openAndPlay) return false;
			ApplyPendingPreset(svc);
			return svc->openAndPlay(svc->context, pathUtf8.c_str(), startSeconds) == GAKU_OK;
		}

		bool Pause()
		{
			const GakuAudioServiceV1* svc = Resolve();
			if (!svc || !svc->pause) return false;
			return svc->pause(svc->context) == GAKU_OK;
		}

		bool ResumeAt(double seconds)
		{
			const GakuAudioServiceV1* svc = Resolve();
			if (!svc || !svc->resumeAt) return false;
			ApplyPendingPreset(svc);
			return svc->resumeAt(svc->context, seconds) == GAKU_OK;
		}

		bool Seek(double seconds, bool whilePlaying)
		{
			const GakuAudioServiceV1* svc = Resolve();
			if (!svc || !svc->seek) return false;
			return svc->seek(svc->context, seconds, whilePlaying ? 1u : 0u) == GAKU_OK;
		}

		bool Stop()
		{
			const GakuAudioServiceV1* svc = Resolve();
			if (!svc || !svc->stop) return false;
			return svc->stop(svc->context) == GAKU_OK;
		}

		bool Observe(Observation& out, double* outPositionSeconds = nullptr, double* outDurationSeconds = nullptr)
		{
			const GakuAudioServiceV1* svc = Resolve();
			if (!svc || !svc->observe) return false;
			GakuAudioObservationV1 raw{};
			raw.abiVersion = GAKU_AUDIO_VERSION;
			raw.structSize = sizeof(raw);
			if (svc->observe(svc->context, &raw) != GAKU_OK) return false;
			out.state = MapState(raw.state);
			out.sourceOpen = raw.sourceOpen != 0;
			out.confirmed = raw.confirmed != 0;
			out.sourceEpoch = raw.sourceEpoch;
			out.errorCode = raw.errorCode < 0 ? static_cast<uint32_t>(-raw.errorCode) : static_cast<uint32_t>(raw.errorCode);
			if (outPositionSeconds) *outPositionSeconds = raw.positionSeconds;
			if (outDurationSeconds) *outDurationSeconds = raw.durationSeconds;
			return true;
		}

		bool SetVolume(float volume)
		{
			const GakuAudioServiceV1* svc = Resolve();
			if (!svc || !svc->setVolume) return false;
			return svc->setVolume(svc->context, volume) == GAKU_OK;
		}

		bool SetPlaybackRate(float rate)
		{
			const GakuAudioServiceV1* svc = Resolve();
			if (!svc || !svc->setPlaybackRate) return false;
			return svc->setPlaybackRate(svc->context, rate) == GAKU_OK;
		}

		AudioProbeResult ProbeFile(const std::string& pathUtf8, bool includeCoverBytes = false)
		{
			AudioProbeResult result;
			if (pathUtf8.empty() || GakuAudioMeta::IsNetworkPath(pathUtf8)) return result;

			if (const GakuAudioServiceV1* svc = Resolve(); svc && svc->probeFile)
			{
				GakuAudioMetadataV1 raw{};
				raw.abiVersion = GAKU_AUDIO_VERSION;
				raw.structSize = sizeof(raw);
				if (svc->probeFile(svc->context, pathUtf8.c_str(), &raw) == GAKU_OK && raw.valid != 0)
				{
					result.valid = true;
					result.sampleRate = raw.sampleRate;
					result.channels = raw.channels;
					result.hasCover = raw.hasCover != 0;
					result.coverByteSize = raw.coverByteSize;
					result.durationSeconds = raw.durationSeconds;
					result.codec = raw.codec;
					result.title = raw.title;
					result.artist = raw.artist;
					result.album = raw.album;
					result.coverMime = raw.coverMime;
					if (includeCoverBytes && result.hasCover && result.coverByteSize > 0 && svc->extractCover)
					{
						std::vector<uint8_t> buf(result.coverByteSize);
						uint32_t outBytes = 0;
						char mime[32]{};
						if (svc->extractCover(svc->context, pathUtf8.c_str(), buf.data(),
								static_cast<uint32_t>(buf.size()), &outBytes, mime) == GAKU_OK && outBytes > 0)
						{
							buf.resize(outBytes);
							result.coverBytes = std::move(buf);
							if (mime[0] != '\0') result.coverMime = mime;
						}
					}
					return result;
				}
			}

			const auto meta = GakuAudioMeta::ProbeFileUtf8(pathUtf8);
			if (meta.valid)
			{
				result.valid = true;
				result.sampleRate = meta.sampleRate;
				result.channels = meta.channels;
				result.hasCover = !meta.coverBytes.empty();
				result.coverByteSize = static_cast<uint32_t>(meta.coverBytes.size());
				result.durationSeconds = meta.durationSeconds;
				result.codec = meta.codec;
				result.title = meta.title;
				result.artist = meta.artist;
				result.album = meta.album;
				result.coverMime = meta.coverMime;
				if (includeCoverBytes)
					result.coverBytes = meta.coverBytes;
			}
			return result;
		}

		std::vector<EffectPresetInfo> Presets()
		{
			std::vector<EffectPresetInfo> list;
			const GakuAudioServiceV1* svc = Resolve();
			if (!svc || !svc->getPresetCount || !svc->getPreset)
			{
				list.push_back({ "flat", "原声", "flat", "直通输出 (GakuAudio 未加载)" });
				return list;
			}
			const uint32_t count = svc->getPresetCount(svc->context);
			list.reserve(count);
			for (uint32_t i = 0; i < count; ++i)
			{
				GakuAudioEffectPresetV1 p{};
				p.abiVersion = GAKU_AUDIO_VERSION;
				p.structSize = sizeof(p);
				if (svc->getPreset(svc->context, i, &p) == GAKU_OK)
				{
					list.push_back({ p.id, p.name, p.category, p.description });
				}
			}
			if (list.empty())
				list.push_back({ "flat", "原声", "flat", "直通输出" });
			return list;
		}

		std::string ActivePresetId()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const GakuAudioServiceV1* svc = ResolveLocked();
			if (svc && svc->getActivePreset)
			{
				if (hasPendingOverride_ && !desiredPresetId_.empty() && svc->setActivePreset)
				{
					svc->setActivePreset(svc->context, desiredPresetId_.c_str());
					hasPendingOverride_ = false;
				}
				char idBuf[64]{};
				if (svc->getActivePreset(svc->context, idBuf, sizeof(idBuf)) == GAKU_OK && idBuf[0] != '\0')
				{
					desiredPresetId_ = idBuf;
					return desiredPresetId_;
				}
			}
			return desiredPresetId_.empty() ? std::string("flat") : desiredPresetId_;
		}

		bool SetActivePreset(const std::string& presetId, std::string* error = nullptr)
		{
			if (presetId.empty())
			{
				if (error) *error = "presetId cannot be empty";
				return false;
			}
			std::lock_guard<std::mutex> lock(mutex_);
			const GakuAudioServiceV1* svc = ResolveLocked();
			if (svc && svc->setActivePreset)
			{
				const GakuStatus st = svc->setActivePreset(svc->context, presetId.c_str());
				if (st != GAKU_OK)
				{
					if (error) *error = "unknown audio effect preset: " + presetId;
					return false;
				}
				desiredPresetId_ = presetId;
				hasPendingOverride_ = false;
				SavePersistedPresetLocked();
				return true;
			}
			if (error) *error = "GakuAudio service not loaded";
			return false;
		}

	private:
		static bool IsValidService(const GakuAudioServiceV1* svc)
		{
			return svc != nullptr &&
				svc->abiVersion == GAKU_AUDIO_VERSION &&
				svc->structSize >= offsetof(GakuAudioServiceV1, renderOfflineWav) + sizeof(svc->renderOfflineWav);
		}

		static PlaybackState MapState(uint32_t raw)
		{
			switch (raw)
			{
			case GAKU_AUDIO_STARTING: return PlaybackState::Starting;
			case GAKU_AUDIO_PLAYING: return PlaybackState::Playing;
			case GAKU_AUDIO_PAUSED: return PlaybackState::Paused;
			case GAKU_AUDIO_FAILED: return PlaybackState::Failed;
			default: return PlaybackState::Stopped;
			}
		}

		static bool IsUnavailablePluginState(GakuPluginLoadState state)
		{
			return state == GAKU_PLUGIN_STATE_NOT_FOUND ||
				state == GAKU_PLUGIN_STATE_DISABLED ||
				state == GAKU_PLUGIN_STATE_DEPENDENCY_ERROR ||
				state == GAKU_PLUGIN_STATE_INCOMPATIBLE;
		}

		void InspectHostPluginsLocked(bool refreshIfPending)
		{
			if (directService_ || cachedService_) return;
			if (availability_.knownUnavailable) return;
			if (availability_.registryChecked && (!refreshIfPending || !availability_.registryAvailable)) return;
			if (!hostApi_ || !hostApi_->queryService || !hostApi_->hostContext) return;

			availability_.registryChecked = true;
			GakuPluginsServiceV1 plugins{};
			const GakuStatus st = hostApi_->queryService(
				hostApi_->hostContext,
				GAKU_SERVICE_PLUGINS,
				GAKU_PLUGINS_SERVICE_VERSION,
				sizeof(plugins),
				&plugins);
			if (st != GAKU_OK ||
				plugins.abiVersion != GAKU_PLUGINS_SERVICE_VERSION ||
				plugins.structSize < sizeof(GakuPluginsServiceV1) ||
				!plugins.findPlugin)
			{
				return;
			}

			availability_.registryAvailable = true;
			GakuPluginSummaryV1 summary{};
			if (plugins.findPlugin("gakuaudio", sizeof(summary), &summary) != GAKU_OK ||
				summary.abiVersion != GAKU_PLUGINS_SERVICE_VERSION ||
				summary.structSize < sizeof(GakuPluginSummaryV1))
			{
				return;
			}

			availability_.state = summary.state;
			summary.version[sizeof(summary.version) - 1] = '\0';
			availability_.version = summary.version;
			availability_.knownUnavailable = IsUnavailablePluginState(summary.state);
		}

		const GakuAudioServiceV1* Resolve()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return ResolveLocked();
		}

		const GakuAudioServiceV1* ResolveLocked()
		{
			if (directService_) return directService_;
			if (cachedService_) return cachedService_;
			if (!hostApi_ || !hostApi_->queryService || !hostApi_->hostContext) return nullptr;
			InspectHostPluginsLocked(false);
			if (availability_.knownUnavailable) return nullptr;

			GakuAudioServiceV1 candidate{};
			const GakuStatus st = hostApi_->queryService(
				hostApi_->hostContext,
				GAKU_SERVICE_AUDIO_CORE,
				GAKU_AUDIO_VERSION,
				sizeof(candidate),
				&candidate);
			if (st == GAKU_OK && IsValidService(&candidate))
			{
				queriedService_ = candidate;
				cachedService_ = &queriedService_;
				if (hasPendingOverride_ && !desiredPresetId_.empty() && cachedService_->setActivePreset)
				{
					cachedService_->setActivePreset(cachedService_->context, desiredPresetId_.c_str());
					hasPendingOverride_ = false;
				}
			}
			return cachedService_;
		}

		void ApplyPendingPreset(const GakuAudioServiceV1* svc)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (hasPendingOverride_ && !desiredPresetId_.empty() && svc && svc->setActivePreset)
			{
				svc->setActivePreset(svc->context, desiredPresetId_.c_str());
				hasPendingOverride_ = false;
			}
		}

		std::filesystem::path PresetFilePathLocked() const
		{
			if (configRoot_.empty()) return {};
			return configRoot_ / "audio_effects.json";
		}

		void LoadPersistedPresetLocked()
		{
			const auto path = PresetFilePathLocked();
			if (path.empty()) return;
			std::error_code ec;
			if (!std::filesystem::exists(path, ec)) return;
			std::ifstream in(path, std::ios::binary);
			if (!in) return;
			std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			jsonlite::Value root;
			std::string err;
			if (!jsonlite::ParseText(text, root, err) || root.type != jsonlite::Type::Object) return;
			const std::string saved = root.StringPath("activeEffectId", "");
			if (!saved.empty())
			{
				desiredPresetId_ = saved;
				hasPendingOverride_ = true;
			}
		}

		void SavePersistedPresetLocked() const
		{
			const auto path = PresetFilePathLocked();
			if (path.empty()) return;
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out) return;
			out << "{\n  \"activeEffectId\": \"" << jsonlite::EscapeString(desiredPresetId_) << "\"\n}\n";
		}

		std::mutex mutex_;
		const GakuHostApi* hostApi_{nullptr};
		const GakuAudioServiceV1* directService_{nullptr};
		const GakuAudioServiceV1* cachedService_{nullptr};
		GakuAudioServiceV1 queriedService_{};
		std::filesystem::path configRoot_;
		std::string desiredPresetId_{"flat"};
		bool hasPendingOverride_{false};
		PluginAvailability availability_{};
		bool unavailableNotified_{false};
	};
} // namespace GakuMotion::audio
