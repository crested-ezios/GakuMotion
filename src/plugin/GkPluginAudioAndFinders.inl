	// -----------------------------------------------------------------------------
	// Windows MCI Audio Helpers：硬件播放、毫秒时钟与「进度跳转 / 断点续播」
	// -----------------------------------------------------------------------------
	std::string g_lastAudioPath{};
	GakuMotion::audio::Observation g_audioObservation{};
	MCIERROR g_audioCommandError{0}; // A successful status query cannot erase a failed transport.
	float g_musicVolume{1.0f};
	int g_musicVolumeSupport{0}; // 0=no open source, 1=available, -1=unavailable
	bool g_audioUsesDigitalVideo{false};
	decltype(&mciSendStringA) g_sendMciAudio = &GakuMotion::windows_path::SendMciUtf8;
	GakuMotion::bgm::Lease g_bgmLease;
	GakuMotion::bgm::Lease g_voiceLease;
	GakuMotion::bgm::Lease g_seLease;
	GakuMotion::audio::AudioPluginClient g_audioPlugin;

	bool HelperAudioSupportsRate()
	{
		return g_audioPlugin.Available();
	}

	void HelperAudioSetRate(float rate)
	{
		if (g_audioPlugin.Available())
			g_audioPlugin.SetPlaybackRate(rate);
	}

	std::string HelperActiveAudioEffectId()
	{
		return g_audioPlugin.ActivePresetId();
	}

	bool HelperAudioMissingAlias(MCIERROR error)
	{
		return error == MCIERR_INVALID_DEVICE_ID || error == MCIERR_INVALID_DEVICE_NAME;
	}

	bool HelperAudioSetVolume(float volume)
	{
		if (!std::isfinite(volume)) return false;
		g_musicVolume = std::clamp(volume, 0.0f, 1.0f);
		if (g_audioPlugin.Available())
		{
			g_musicVolumeSupport = 1;
			return g_audioPlugin.SetVolume(g_musicVolume);
		}
		if (!g_audioObservation.sourceOpen) { g_musicVolumeSupport = 0; return true; }
		if (!g_audioUsesDigitalVideo) { g_musicVolumeSupport = -1; return false; }
		char cmd[80]{};
		std::snprintf(cmd, sizeof(cmd), "setaudio mmd_song volume to %d",
		              static_cast<int>(std::lround(g_musicVolume * 1000.0f)));
		const MCIERROR result = g_sendMciAudio(cmd, NULL, 0, NULL);
		g_musicVolumeSupport = result ? -1 : 1;
		// A volume failure must not poison transport observation or synchronization.
		return result == 0;
	}
	int HelperAudioVolumeSupport()
	{
		if (g_audioPlugin.Available()) return 1;
		return g_musicVolumeSupport;
	}

	GakuMotion::audio::Observation HelperAudioObserve()
	{
		if (g_audioPlugin.Available())
		{
			GakuMotion::audio::Observation obs{};
			if (g_audioPlugin.Observe(obs))
			{
				g_audioObservation = obs;
				return obs;
			}
		}
		auto observed = g_audioObservation;
		if (!observed.sourceOpen) return observed;
		char mode[32]{};
		const MCIERROR rc = g_sendMciAudio("status mmd_song mode", mode, sizeof(mode), NULL);
		if (HelperAudioMissingAlias(rc)) observed.sourceOpen = false;
		observed.errorCode = g_audioCommandError ? g_audioCommandError : rc;
		observed.confirmed = observed.errorCode == 0;
		observed.state = observed.errorCode ? GakuMotion::audio::PlaybackState::Failed :
			std::strcmp(mode, "playing") == 0 ? GakuMotion::audio::PlaybackState::Playing :
			std::strcmp(mode, "paused") == 0 ? GakuMotion::audio::PlaybackState::Paused :
			std::strcmp(mode, "stopped") == 0 ? GakuMotion::audio::PlaybackState::Stopped : GakuMotion::audio::PlaybackState::Starting;
		if (observed.state == GakuMotion::audio::PlaybackState::Starting) observed.confirmed = false;
		g_audioObservation = observed;
		return observed;
	}

	void HelperAudioRecordCommand(MCIERROR error)
	{
		g_audioCommandError = error;
		g_audioObservation.errorCode = error;
		g_audioObservation.confirmed = false;
		if (error) g_audioObservation.state = GakuMotion::audio::PlaybackState::Failed;
		g_audioObservation = HelperAudioObserve();
	}

	bool HelperAudioIsOpen()
	{
		if (g_audioPlugin.Available())
			return HelperAudioObserve().sourceOpen;
		char buf[64]{};
		const MCIERROR error = g_sendMciAudio("status mmd_song mode", buf, sizeof(buf), NULL);
		if (error)
		{
			if (HelperAudioMissingAlias(error)) g_audioObservation.sourceOpen = false;
			HelperAudioRecordCommand(error);
			return false;
		}
		g_audioObservation.sourceOpen = buf[0] != '\0';
		return g_audioObservation.sourceOpen;
	}

	int HelperAudioMsFromSeconds(float seconds)
	{
		if (!std::isfinite(seconds) || seconds <= 0.0f) return 0;
		return static_cast<int>((std::min)(double(seconds) * 1000.0 + 0.5,
			static_cast<double>((std::numeric_limits<int>::max)())));
	}

	// 打开并以 startMs 为起点播放
	void HelperAudioOpenAndPlay(const std::string& path, int startMs)
	{
		if (g_audioPlugin.Available())
		{
			if (g_audioObservation.sourceOpen)
				g_sendMciAudio("close mmd_song", NULL, 0, NULL);
			g_audioPlugin.SetVolume(g_musicVolume);
			g_audioPlugin.OpenAndPlay(path, static_cast<double>((std::max)(0, startMs)) / 1000.0);
			HelperAudioObserve();
			return;
		}
		const MCIERROR closeError = g_sendMciAudio("close mmd_song", NULL, 0, NULL);
		if (closeError && !HelperAudioMissingAlias(closeError))
		{
			// The old alias may still be playing; do not replace its identity or
			// pretend it was closed because a new open was requested.
			g_audioObservation.sourceOpen = true;
			HelperAudioRecordCommand(closeError);
			return;
		}
		const uint64_t previousEpoch = g_audioObservation.sourceEpoch;
		g_audioObservation = {};
		g_musicVolumeSupport = 0;
		g_audioUsesDigitalVideo = false;
		g_audioObservation.sourceEpoch = previousEpoch;
		g_audioObservation.state = GakuMotion::audio::PlaybackState::Starting;
		g_audioCommandError = 0;
		std::string cmdOpen = "open \"" + path + "\" type mpegvideo alias mmd_song";
		MCIERROR rc = g_sendMciAudio(cmdOpen.c_str(), NULL, 0, NULL);
		if (!rc) g_audioUsesDigitalVideo = true;
		else
		{
			// Preserve the original playback backend if DirectShow cannot open this WAV.
			cmdOpen = "open \"" + path + "\" type waveaudio alias mmd_song";
			rc = g_sendMciAudio(cmdOpen.c_str(), NULL, 0, NULL);
		}
		if (!rc)
		{
			g_audioObservation.sourceOpen = true;
			g_audioObservation.sourceEpoch = previousEpoch + 1;
			if (!g_audioObservation.sourceEpoch) g_audioObservation.sourceEpoch = 1;
			rc = g_sendMciAudio("set mmd_song time format milliseconds", NULL, 0, NULL);
			if (!rc) HelperAudioSetVolume(g_musicVolume);
		}
		char cmd[128]{};
		std::snprintf(cmd, sizeof(cmd), "play mmd_song from %d", (startMs > 0) ? startMs : 0);
		if (!rc) rc = g_sendMciAudio(cmd, NULL, 0, NULL);
		if (rc)
		{
			if (g_audioObservation.sourceOpen)
			{
				const MCIERROR cleanup = g_sendMciAudio("close mmd_song", NULL, 0, NULL);
				if (!cleanup || HelperAudioMissingAlias(cleanup))
				{ g_audioObservation.sourceOpen = false; g_musicVolumeSupport = 0; g_audioUsesDigitalVideo = false; }
			}
			HelperAudioRecordCommand(rc);
			return;
		}
		HelperAudioRecordCommand(0);
	}

	// 从头播放（保持改动前的既有语义）
	void HelperAudioPlay(const std::string& path)
	{
		g_lastAudioPath = path;
		HelperAudioOpenAndPlay(path, 0);
	}

	void HelperAudioPause()
	{
		if (g_audioPlugin.Available())
		{
			g_audioPlugin.Pause();
			HelperAudioObserve();
			return;
		}
		HelperAudioRecordCommand(g_sendMciAudio("pause mmd_song", NULL, 0, NULL));
	}

	// Always use the requested absolute timeline position, including zero.
	void HelperAudioResumeAt(float seconds)
	{
		if (g_audioPlugin.Available())
		{
			g_audioPlugin.ResumeAt(seconds);
			HelperAudioObserve();
			return;
		}
		const int ms = HelperAudioMsFromSeconds(seconds);
		if (HelperAudioIsOpen())
		{
			char cmd[128]{};
			std::snprintf(cmd, sizeof(cmd), "play mmd_song from %d", ms);
			HelperAudioRecordCommand(g_sendMciAudio(cmd, NULL, 0, NULL));
		}
		else if (!g_lastAudioPath.empty())
		{
			HelperAudioOpenAndPlay(g_lastAudioPath, ms);
		}
		else HelperAudioRecordCommand(MCIERR_INVALID_DEVICE_ID);
	}

	// 进度跳转：仅在音频已打开时生效；未打开时由播放器下一次 Play() 负责对齐
	void HelperAudioSeekTo(float seconds, bool playing)
	{
		if (g_audioPlugin.Available())
		{
			g_audioPlugin.Seek(seconds, playing);
			HelperAudioObserve();
			return;
		}
		if (!HelperAudioIsOpen()) return;
		const int ms = HelperAudioMsFromSeconds(seconds);
		char cmd[128]{};
		if (playing)
			std::snprintf(cmd, sizeof(cmd), "play mmd_song from %d", ms);
		else
			std::snprintf(cmd, sizeof(cmd), "seek mmd_song to %d", ms);
		HelperAudioRecordCommand(g_sendMciAudio(cmd, NULL, 0, NULL));
	}
	void HelperAudioSeek(float seconds) { HelperAudioSeekTo(seconds, true); }
	void HelperAudioSeekPaused(float seconds) { HelperAudioSeekTo(seconds, false); }

	void HelperAudioStop()
	{
		if (g_audioPlugin.Available())
		{
			g_audioPlugin.Stop();
			HelperAudioObserve();
			return;
		}
		MCIERROR stopError = g_sendMciAudio("stop mmd_song", NULL, 0, NULL);
		const MCIERROR closeError = g_sendMciAudio("close mmd_song", NULL, 0, NULL);
		if (HelperAudioMissingAlias(stopError)) stopError = 0;
		const bool closed = !closeError || HelperAudioMissingAlias(closeError);
		const uint64_t epoch = g_audioObservation.sourceEpoch;
		g_audioObservation = {};
		g_audioObservation.sourceEpoch = epoch;
		g_audioObservation.sourceOpen = !closed;
		if (closed) { g_musicVolumeSupport = 0; g_audioUsesDigitalVideo = false; }
		HelperAudioRecordCommand(stopError ? stopError : closed ? 0 : closeError);
		if (closed && !g_audioCommandError) g_audioObservation.confirmed = true;
	}

	float HelperAudioGetTime()
	{
		if (g_audioPlugin.Available())
		{
			GakuMotion::audio::Observation obs{};
			double posSec = -1.0;
			if (g_audioPlugin.Observe(obs, &posSec) && obs.EffectivePlaying() &&
				std::isfinite(posSec) && posSec >= 0.0)
			{
				return static_cast<float>(posSec);
			}
			return -1.0f;
		}
		// A failed seek/resume cannot pull the motion back to an old alias clock.
		if (!HelperAudioObserve().EffectivePlaying()) return -1.0f;
		char buf[64]{};
		MCIERROR err = g_sendMciAudio("status mmd_song position", buf, sizeof(buf), NULL);
		if (err == 0 && buf[0] != '\0')
		{
			int ms = atoi(buf);
			return static_cast<float>(ms) / 1000.0f;
		}
		return -1.0f;
	}

	bool HelperAudioObserveWithTime(GakuMotion::audio::Observation* outObs, float* outTimeSeconds)
	{
		if (!g_audioPlugin.Available()) return false;
		GakuMotion::audio::Observation obs{};
		double posSec = -1.0;
		if (g_audioPlugin.Observe(obs, &posSec))
		{
			g_audioObservation = obs;
			if (outObs) *outObs = obs;
			if (outTimeSeconds)
			{
				*outTimeSeconds = (obs.EffectivePlaying() && std::isfinite(posSec) && posSec >= 0.0)
					? static_cast<float>(posSec)
					: -1.0f;
			}
			return true;
		}
		return false;
	}

	void* FindClass(const char* assemblyName, const char* namespaze, const char* className)
	{
		if (!il2cpp_domain_get || !il2cpp_domain_assembly_open || !il2cpp_assembly_get_image || !il2cpp_class_from_name)
		{
			return nullptr;
		}
		void* domain = il2cpp_domain_get();
		if (!domain) return nullptr;
		void* assembly = il2cpp_domain_assembly_open(domain, assemblyName);
		if (!assembly) return nullptr;
		void* image = il2cpp_assembly_get_image(assembly);
		if (!image) return nullptr;
		return il2cpp_class_from_name(image, namespaze, className);
	}

	void* FindMethodPointer(void* klass, const char* methodName, int argsCount)
	{
		if (!klass || !il2cpp_class_get_method_from_name) return nullptr;
		Il2CppMethodInfo* m = il2cpp_class_get_method_from_name(klass, methodName, argsCount);
		return m ? m->methodPointer : nullptr;
	}
