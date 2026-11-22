#pragma once

#include "../GkMotionData.hpp"
#include "../GkRuntimeConfig.hpp"
#include "../GkStageStudioSettings.hpp"
#include "../GkControlRequests.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <string>

namespace GakuMotion
{
	namespace control_v0 = control::v0;
	// -----------------------------------------------------------------------------
	// 日志格式化小工具与 Core 值类型
	// -----------------------------------------------------------------------------
	namespace detail
	{
		inline std::string FormatMsg(const char* fmt, ...)
		{
			char buf[1024]{};
			va_list args;
			va_start(args, fmt);
			std::vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);
			return std::string(buf);
		}

		struct CorePendingImportCover
		{
			bool set{false};
			bool clear{false};
			std::string dataUri;
		};

		struct CoreAppliedDeclaration
		{
			std::string selectedMotion;
			int gazeMode{-1};
			int desktopLookAt{-1};
			float eyeWeight{-1.0f};
			float headWeight{-1.0f};
			float bodyWeight{-1.0f};
			int useRestRotations{-1};
			int legRestBasis{-1};
			int eyePoseRelativeGain{-1};
			int preserveAnimatorForSecondaryPhysics{-1};
			int overridesEnabled{-1};
			int auxMode{-1};
			int experimentalWaistSupport{-1};
			int muteNativeSoundEffects{-1};
			std::string globalFile;
			std::string motionFile;
			float playbackSpeed{-1.0f};
			int facialEnableBaseline{-1};
			int facialMikuMouthFit{-1};
			int facialMikuEyeFit{-1};
			int facialMikuFitAllowUnverified{-1};
			float facialBaselineMultiplier{-1.0f};
			std::string facialCharacterOverride;
		};

		enum class CoreGuiAction
		{
			Select, Cycle, Play, Pause, Resume, Stop, Seek,
			ScrubBegin, ScrubUpdate, ScrubEnd, Refresh, Move,
			SaveList, LoadList, Mode, Mute, Speed, Import, ClearTarget, NextTarget, PickTarget,
			InitialPosition, SaveInitialPosition, SaveFacialWeights, MusicVolume, SaveMusicVolume, PhysicsBackend,
			CameraSettings, SaveCameraSettings, CameraImport, ReloadCamera,
			SoleSettings, SaveSoleSettings, LoadSoleSettings, ExperimentalSolePitch, ExperimentalWaistSupport,
			BeforeQuartzExperiment,
			MuteNativeSoundEffects,
			StageSettings, StagePreset, ToggleStageCandidate, ScanStageCandidates,
			MountCustomStage, UnloadCustomStage, RestoreCameraSkybox,
			SeekFrame, StepFrame, ReloadMotion, DeleteFiles, StageLoadReplacement, SelectTarget,
			Control, StudioSettings, StudioReset,
			FormationSelectSlot, FormationSwapSlots, FormationSetSlotOffset,
			FormationResetSlotOffset, FormationSaveSlotOffset, FormationSetSlotSource
		};

		struct CoreGuiCommand
		{
			CoreGuiAction action;
			float value{0};
			bool flag{false};
			std::string key;
			MotionImportRequest import;
			bool importCoverSet{false};
			bool importCoverClear{false};
			std::string importCoverDataUri;
			Vector3 position;
			GakuMotion::CameraSettings cameraSettings;
			CameraImportRequest cameraImport;
			SoleMeasurementSettings soleSettings;
			sole_pitch::Settings experimentalSolePitchSettings;
			GakuMotion::StageSettings stageSettings;
			studio::Settings studioSettings;
			int stagePresetIndex{0};
			int candidateIndex{-1};
			int64_t frame{0};
			uint64_t loadedGeneration{0}, sceneSession{0}, libraryRevision{0};
			uint64_t actorToken{0}, actorEpoch{0};
			control_v0::Command controlCommand{};
			uint64_t controlTicket{0};
		};
	} // namespace detail
} // namespace GakuMotion
