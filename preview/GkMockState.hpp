#pragma once

// =============================================================================
// GkMockState.hpp — GakuMotion 控制面板最薄 Mock 状态契约
// =============================================================================
// 纯标准 C++ 对象与函数指针回调，不依赖 Unity、IL2CPP、GakuLoader 或游戏环境。
// 为 GUI Preview 提供与游戏实机 1:1 契约结构的展示数据与响应闭环。
// =============================================================================

#include "../src/GkGuiOverlay.hpp"
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

namespace GakuMotion
{
	class MockGuiState
	{
	public:
		MotionPlayer player;
		std::vector<MotionEntry> motions;
		int selectedMotionIndex{0};
		uint64_t loadedMotionGeneration{1}, libraryRevision{1};
		std::string libraryNotice{"曲库已是最新"};
		int playlistMode{1};
		std::string playlistNotice;
		bool importBusy{false};
		ImportProgressState importProgress;
		MotionImportRequest pendingImportReq;
		float mockImportElapsed{0.0f};

		LookAtManager lookAt;
		RuntimeConfig config;

		CameraController::Snapshot cameraSnapshot;
		std::string cameraNotice;
		std::string cameraSource{"spurt_choreo/camera.gkcamera"};
		uint32_t cameraFrames{3600};
		float cameraFps{60.0f};

		StageSettings stageSettings;
		std::vector<StageCandidate> stageCandidates;
		std::string stageSceneName{"LiveStage_01_ProLive"};
		std::string stageMountedKey{"env_3d_live_all001-00-noon"};
		std::string stageStatusNotice{"Stage mock loaded successfully"};

		BoneOverrideSet overrides;
		bool masterOverridesEnabled{true};
		unsigned knownActorCount{3};
		uint64_t selectedActorToken{1};
		std::string initialPositionNotice;

		SoleMeasurementSettings soleSettings;
		SoleMeasurementSnapshot soleSnapshot;
		std::string soleNotice{"Mock sole telemetry active (oriented proxy distances)"};
		uint64_t soleCalibrationGeneration{1};

		sole_pitch::Settings experimentalSolePitchSettings;
		sole_pitch::Snapshot experimentalSolePitchSnapshot;

		int physicsBackend{1}; // SpringBone
		bool beforeQuartzExperiment{true};
		bool experimentalWaistSupport{true};
		std::string statusLine{"GakuMotion GUI Preview | Mock State Active | F5 Standalone Debug Target"};

		MockGuiState()
		{
			Reset();
		}

		void Reset()
		{
			// 1. 曲目列表
			motions.clear();
			++libraryRevision; ++loadedMotionGeneration;
			{
				MotionEntry m;
				m.name = "spurt_choreo";
				m.title = "初恋水引 (Spurt) - Live Motion";
				m.durationSeconds = 60.0f;
				m.totalFrames = 3600;
				m.boneCount = 55;
				m.fps = 60.0f;
				m.hasAudio = true;
				m.hasFacial = true;
				m.hasCamera = true;
				m.hasOverrides = true;
				m.metadataLoaded = true;
				motions.push_back(m);
			}
			{
				MotionEntry m;
				m.name = "campus_idol";
				m.title = "Campus Walk - Casual Pose";
				m.durationSeconds = 24.5f;
				m.totalFrames = 1470;
				m.boneCount = 55;
				m.fps = 60.0f;
				m.hasAudio = false;
				m.hasFacial = false;
				m.hasCamera = false;
				m.hasOverrides = false;
				m.metadataLoaded = true;
				motions.push_back(m);
			}
			{
				MotionEntry m;
				m.name = "pro_stage_01";
				m.title = "Stage Choreo 01 - Full Unit";
				m.durationSeconds = 92.0f;
				m.totalFrames = 5520;
				m.boneCount = 55;
				m.fps = 60.0f;
				m.hasAudio = true;
				m.hasFacial = true;
				m.hasCamera = true;
				m.hasOverrides = true;
				m.metadataLoaded = true;
				motions.push_back(m);
			}
			for (auto& motion : motions)
			{
				motion.dir = "preview/motions/" + motion.name;
				motion.motionFile = motion.dir + "/motion.gkmotion";
			}
			selectedMotionIndex = 0;
			playlistMode = 1;
			playlistNotice = "Mock 播放列表加载正常 (共 3 曲目)";
			importBusy = false;
			importProgress = ImportProgressState{};
			pendingImportReq = MotionImportRequest{};
			mockImportElapsed = 0.0f;

			config.facialCharacterOverride = "auto";
			config.facialMikuMouthFit = 1;

			// 2. 播放器状态
			player.clip.total_frames = 3600;
			player.clip.duration_seconds = 60.0f;
			player.clip.bone_count = 158;
			player.clip.fps = 60.0f;
			player.currentTime = 14.25f;
			player.loadedMotionFile = "spurt_choreo/motion.gkmotion";
			player.state = PlaybackState::Playing;
			player.playbackSpeedMultiplier = 1.0f;
			player.muted = false;

			player.controller.currentBinding.actor = reinterpret_cast<void*>(0x00007FF712340000ULL);
			player.controller.currentBinding.boundBoneCount = 55;
			player.controller.currentBinding.boneTransforms.resize(55);
			player.controller.useRestRotations = true;
			player.controller.rootHeightCalibration.enabled = true;
			player.controller.rootHeightCalibrationEnabled = true;
			player.controller.currentBinding.calibratedRootHeight = 0.0125f;
			player.controller.initialPositionOffset = Vector3(0.0f, 0.0f, 0.0f);

			// 3. 视线
			lookAt.gazeMode.store(GazeMode::Auto);
			lookAt.desktopLookAtEnabled.store(true);
			lookAt.eyeWeight.store(0.75f);
			lookAt.headWeight.store(0.50f);
			lookAt.bodyWeight.store(0.20f);

			// 4. 镜头
			cameraSnapshot.owned = true;
			cameraSnapshot.hasTrack = true;
			cameraSnapshot.available = true;
			cameraSnapshot.position = {0.0f, 0.6f, 3.0f};
			cameraSnapshot.fov = 48.0f;
			cameraSnapshot.settings.enabled = true;
			cameraSnapshot.settings.mode = CameraMode::Follow;
			cameraSnapshot.settings.followBehavior = FollowBehavior::FollowMove;
			cameraSnapshot.settings.followActor = true;
			cameraSnapshot.settings.limitToScene = true;
			cameraSnapshot.settings.actorShadowExperimentMode = 1;
			cameraSnapshot.settings.boundsMin = {-10.0f, 0.0f, -10.0f};
			cameraSnapshot.settings.boundsMax = {10.0f, 5.0f, 10.0f};
			cameraSnapshot.settings.followOffset = {0.0f, 0.6f, 3.0f};
			cameraSnapshot.settings.targetOffset = {0.0f, 0.4f, 0.0f};
			cameraSnapshot.settings.vmdOffset = {0.0f, 0.0f, 0.0f};
			cameraSnapshot.settings.transformTrack = CameraSettings::DefaultTrackTransform();
			cameraSnapshot.settings.transformFollowMove = CameraSettings::DefaultFollowMoveTransform();
			cameraSnapshot.settings.transformLookAt = CameraSettings::DefaultLookAtTransform();
			cameraSnapshot.settings.usePerModeTransforms = true;
			cameraSnapshot.settings.followFactor = {1.0f, 1.0f, 1.0f};
			cameraSnapshot.settings.followFov = 48.0f;
			cameraNotice.clear();
			cameraSource = "spurt_choreo/camera.gkcamera";
			cameraFrames = 3600;
			cameraFps = 60.0f;

			// 5. 舞台与场景
			stageSettings = StageSettings{};
			stageSettings.lightingEffectsEnabled = true;
			stageSettings.customNodeControl = false;
			stageSettings.hideBackground = false;
			stageSettings.selectedPresetIndex = 0;
			stageSceneName = "LiveStage_01_ProLive";
			stageMountedKey = "env_3d_live_all001-00-noon";
			stageStatusNotice = "Stage mock loaded successfully";
			stageCandidates.clear();

			auto addNode = [&](const char* name, int parentIdx, int depth, bool hasChildren,
			                   bool isLightingRoot, bool isLightingNode, bool isIdentifiedBg, bool isColliderLocked)
			{
				StageCandidate c;
				c.name = name;
				c.displayName = name;
				c.parentIndex = parentIdx;
				c.depth = depth;
				c.hasChildren = hasChildren;
				c.isLightingRoot = isLightingRoot;
				c.isLightingNode = isLightingNode;
				c.isIdentifiedBackground = isIdentifiedBg;
				c.isColliderLocked = isColliderLocked;
				c.originalActive = true;
				c.currentlyActive = true;
				c.userMuted = false;
				stageCandidates.push_back(c);
			};

			// [0] Root: "Lighting&PostEffects" (isLightingRoot = true, hasChildren = true, depth = 0, parentIndex = -1)
			addNode("Lighting&PostEffects", -1, 0, true, true, false, false, false);
			// [1] Child of 0: "Directional Light" (isLightingNode = true, depth = 1, parentIndex = 0)
			addNode("Directional Light", 0, 1, false, false, true, false, false);
			// [2] Child of 0: "Reflection Probe" (isLightingNode = true, depth = 1, parentIndex = 0)
			addNode("Reflection Probe", 0, 1, false, false, true, false, false);
			// [3] Child of 0: "PostProcess Volume" (isLightingNode = true, hasChildren = true, depth = 1, parentIndex = 0)
			addNode("PostProcess Volume", 0, 1, true, false, true, false, false);
			// [4] Child of 3: "Bloom & ToneMapping" (isLightingNode = true, depth = 2, parentIndex = 3)
			addNode("Bloom & ToneMapping", 3, 2, false, false, true, false, false);

			// [5] Root: "Stage_Environment_Root" (isColliderLocked = true, hasChildren = true, depth = 0, parentIndex = -1)
			addNode("Stage_Environment_Root", -1, 0, true, false, false, false, true);
			// [6] Child of 5: "Floor_Collider_Main" (isColliderLocked = true, depth = 1, parentIndex = 5)
			addNode("Floor_Collider_Main", 5, 1, false, false, false, false, true);
			// [7] Child of 5: "Stage_Backdrop_LED" (isIdentifiedBackground = true, depth = 1, parentIndex = 5)
			addNode("Stage_Backdrop_LED", 5, 1, false, false, false, true, false);
			// [8] Child of 5: "Stage_Props_Group" (isIdentifiedBackground = true, hasChildren = true, depth = 1, parentIndex = 5)
			addNode("Stage_Props_Group", 5, 1, true, false, false, true, false);
			// [9] Child of 8: "Stage_Truss_Frame" (isIdentifiedBackground = true, depth = 2, parentIndex = 8)
			addNode("Stage_Truss_Frame", 8, 2, false, false, false, true, false);
			// [10] Child of 8: "Stage_Monitor_Array" (isIdentifiedBackground = true, depth = 2, parentIndex = 8)
			addNode("Stage_Monitor_Array", 8, 2, false, false, false, true, false);

			// 6. 骨骼覆盖
			overrides.offsets.clear();
			{
				BoneRotationOffset o;
				o.humanoidId = 13;
				o.boneName = "LeftUpperArm";
				o.eulerDeg = {5.0f, -10.0f, 0.0f};
				o.spaceWorld = false;
				o.enabled = true;
				overrides.offsets.push_back(o);
			}
			{
				BoneRotationOffset o;
				o.humanoidId = 14;
				o.boneName = "RightUpperArm";
				o.eulerDeg = {5.0f, 10.0f, 0.0f};
				o.spaceWorld = false;
				o.enabled = true;
				overrides.offsets.push_back(o);
			}
			{
				BoneRotationOffset o;
				o.humanoidId = 1;
				o.boneName = "LeftUpperLeg";
				o.eulerDeg = {0.0f, 0.0f, -2.0f};
				o.spaceWorld = false;
				o.enabled = true;
				overrides.offsets.push_back(o);
			}
			masterOverridesEnabled = true;
			knownActorCount = 3;
			selectedActorToken = 1;
			initialPositionNotice.clear();

			// 7. 鞋底测量 v2
			soleSettings.enabled = true;
			soleSettings.useBootProxy = true;
			soleSettings.planeCalibrated = true;
			soleSettings.shoeCalibrated = true;
			soleSettings.planeNormal = {0.0f, 1.0f, 0.0f};
			soleSettings.planeOffsetM = 0.0f;
			soleSettings.displayBandM = 0.005f;

			soleSnapshot.available = true;
			soleSnapshot.calibrated = true;
			soleSnapshot.clipTime = 14.25f;
			soleSnapshot.actor = player.controller.currentBinding.actor;

			// 设置左右双脚坐标与面片
			for (int side = 0; side < 2; ++side)
			{
				auto& foot = soleSnapshot.feet[side];
				foot.valid = true;
				foot.usingBootProxy = true;
				float sign = (side == 0) ? -1.0f : 1.0f;
				foot.pose.hip = {sign * 0.10f, 0.85f, 0.0f};
				foot.pose.knee = {sign * 0.10f, 0.45f, 0.02f};
				foot.pose.ankle = {sign * 0.10f, 0.08f, -0.01f};
				foot.pose.toe = {sign * 0.10f, 0.02f, 0.12f};
				foot.minimumDistanceM = 0.0015f;
				foot.maximumPenetrationM = 0.0f;

				// 后跟面片
				foot.patches[0].minimumDistanceM = 0.0020f;
				foot.patches[0].maximumDistanceM = 0.0150f;
				foot.patches[0].maximumPenetrationM = 0.0f;
				foot.patches[0].corners[0].world = {sign * 0.13f, 0.015f, -0.05f};
				foot.patches[0].corners[1].world = {sign * 0.07f, 0.015f, -0.05f};
				foot.patches[0].corners[2].world = {sign * 0.07f, 0.015f, 0.02f};
				foot.patches[0].corners[3].world = {sign * 0.13f, 0.015f, 0.02f};

				// 前掌面片
				foot.patches[1].minimumDistanceM = 0.0015f;
				foot.patches[1].maximumDistanceM = 0.0120f;
				foot.patches[1].maximumPenetrationM = 0.0f;
				foot.patches[1].corners[0].world = {sign * 0.13f, 0.010f, 0.03f};
				foot.patches[1].corners[1].world = {sign * 0.07f, 0.010f, 0.03f};
				foot.patches[1].corners[2].world = {sign * 0.07f, 0.010f, 0.14f};
				foot.patches[1].corners[3].world = {sign * 0.13f, 0.010f, 0.14f};

				foot.points[0].world = {sign * 0.10f, 0.015f, -0.02f}; // 鞋跟
				foot.points[0].signedDistanceM = 0.0020f;
				foot.points[1].world = {sign * 0.10f, 0.010f, 0.08f};  // 前掌
				foot.points[1].signedDistanceM = 0.0015f;
				foot.points[2].world = {sign * 0.10f, 0.010f, 0.13f};  // 脚尖
				foot.points[2].signedDistanceM = 0.0018f;
			}
			soleNotice = "Mock sole telemetry active (oriented proxy distances)";
			soleCalibrationGeneration = 1;

			// 8. Pitch Demo [Experimental]
			experimentalSolePitchSettings.experimentalSolePitchCorrection = true;
			experimentalSolePitchSettings.useMeshEnvelope = true;
			experimentalSolePitchSettings.maxPitchDegrees = 15.0f;

			experimentalSolePitchSnapshot.clipTime = 14.25f;
			experimentalSolePitchSnapshot.feet[0].status = "Solved (Envelope)";
			experimentalSolePitchSnapshot.feet[0].solution.valid = true;
			experimentalSolePitchSnapshot.feet[0].solution.pitchDegrees = -3.5f;
			experimentalSolePitchSnapshot.feet[0].solution.beforePenetrationM = 0.006f;
			experimentalSolePitchSnapshot.feet[0].solution.usedMeshEnvelope = true;
			experimentalSolePitchSnapshot.feet[0].measuredAfterValid = true;
			experimentalSolePitchSnapshot.feet[0].measuredAfterMinimumDistanceM = 0.001f;
			experimentalSolePitchSnapshot.feet[0].applied = true;
			experimentalSolePitchSnapshot.feet[0].setterReadbackMatches = true;

			experimentalSolePitchSnapshot.feet[1].status = "Grounded (No Penetration)";
			experimentalSolePitchSnapshot.feet[1].solution.valid = false;

			// 9. 配置与运行态
			config.path = "gakumas-local/control.json";
			config.fileFound = true;
			config.pollIntervalSeconds = 0.1f;
			config.appliedCommandSeq = 108;
			config.lastChangeLog = "Preview session active";
			physicsBackend = 1;
			beforeQuartzExperiment = true;
			experimentalWaistSupport = true;
			statusLine = "GakuMotion GUI Preview | Mock State Active | F5 Standalone Debug Target";
		}

		void Update(float dt)
		{
			if (player.state == PlaybackState::Playing)
			{
				player.currentTime += dt * player.playbackSpeedMultiplier;
				if (player.currentTime > player.clip.duration_seconds)
				{
					player.currentTime = 0.0f;
				}
				soleSnapshot.clipTime = player.currentTime;
				experimentalSolePitchSnapshot.clipTime = player.currentTime;
			}

			if (importBusy)
			{
				mockImportElapsed += dt;
				if (mockImportElapsed < 0.3f)
				{
					importProgress = {true, false, false, 0.15f, "PARSE", "正在解析 VMD 动作数据...", ""};
				}
				else if (mockImportElapsed < 0.7f)
				{
					importProgress = {true, false, false, 0.40f, "IK_SOLVE", "正在计算 IK 反向动力学与足底对齐...", ""};
				}
				else if (mockImportElapsed < 1.1f)
				{
					importProgress = {true, false, false, 0.65f, "RETARGET", "正在重定向骨骼旋转至学园偶像 Humanoid...", ""};
				}
				else if (mockImportElapsed < 1.4f)
				{
					importProgress = {true, false, false, 0.82f, "MORPH", "正在映射表情并烘焙 Blendshape 权重通道...", ""};
				}
				else if (mockImportElapsed < 1.7f)
				{
					importProgress = {true, false, false, 0.94f, "CAMERA", "正在生成相机轨道与平滑滤波...", ""};
				}
				else
				{
					importBusy = false;
					importProgress.active = false;
					importProgress.finished = true;
					importProgress.succeeded = true;
					importProgress.progress = 1.0f;
					importProgress.stage = "DONE";
					importProgress.statusText = "导入完成";
					importProgress.errorDetail.clear();

					if (!pendingImportReq.editKey.empty())
					{
						for (auto& m : motions)
						{
							if (m.name == pendingImportReq.editKey)
							{
								if (!pendingImportReq.title.empty())
								{
									m.title = pendingImportReq.title;
								}
								if (pendingImportReq.clearAudio)
								{
									m.hasAudio = false;
								}
								else if (!pendingImportReq.audio.empty())
								{
									m.hasAudio = true;
								}
								if (pendingImportReq.clearCamera)
								{
									m.hasCamera = false;
								}
								else if (!pendingImportReq.cameraVmd.empty())
								{
									m.hasCamera = true;
								}
								if (pendingImportReq.disableMorph)
								{
									m.hasFacial = false;
								}
								else if (!pendingImportReq.morphVmd.empty())
								{
									m.hasFacial = true;
								}
								break;
							}
						}
					}
					else
					{
						MotionEntry m;
						std::string stem;
						if (!pendingImportReq.vmd.empty())
						{
							stem = std::filesystem::path(Utf8ToWide(pendingImportReq.vmd)).stem().string();
						}
						m.title = !pendingImportReq.title.empty() ? pendingImportReq.title : (!stem.empty() ? stem : "新导入曲目");
						m.name = !stem.empty() ? stem : ("motion_" + std::to_string(motions.size() + 1));
						m.dir = "motions/" + m.name;
						m.durationSeconds = 60.0f;
						m.totalFrames = 3600;
						m.boneCount = 55;
						m.fps = 60.0f;
						m.hasAudio = !pendingImportReq.clearAudio && !pendingImportReq.audio.empty();
						m.hasCamera = !pendingImportReq.clearCamera && !pendingImportReq.cameraVmd.empty();
						m.hasFacial = !pendingImportReq.disableMorph && (!pendingImportReq.morphVmd.empty() || ProbeVmdFile(pendingImportReq.vmd).CanUseAsFacial());
						m.hasOverrides = false;
						m.metadataLoaded = true;
						motions.push_back(m);
						selectedMotionIndex = static_cast<int>(motions.size() - 1);
					}
				}
			}
		}

		GuiContext BuildContext()
		{
			GuiContext ctx;
			ctx.injectionAllowed = true; // Offline preview has no game/online capture path.
			ctx.self = this;

			// ---- 只读状态映射 ----
			ctx.player = &player;
			ctx.motions = &motions;
			ctx.selectedMotionIndex = &selectedMotionIndex;
			ctx.libraryRevision = libraryRevision;
			ctx.loadedMotionGeneration = loadedMotionGeneration;
			ctx.libraryNotice = libraryNotice;
			if (selectedMotionIndex >= 0 && selectedMotionIndex < static_cast<int>(motions.size()))
				ctx.loadedMotionKey = motions[selectedMotionIndex].name;
			static const char* actorCodes[] = {"hski", "ttmr", "hmsz"};
			ctx.actorRosterEpoch = 1;
			for (unsigned i = 0; i < knownActorCount && i < 3; ++i)
			{
				const uint64_t token = i + 1;
				const std::string label = FormatIdolDisplayName(actorCodes[i]);
				ctx.sceneActors.push_back({token, label, token == selectedActorToken});
				if (token == selectedActorToken) ctx.boundActorLabel = ctx.targetActorLabel = label;
			}
			ctx.fnSelectTargetActor = [](void* self, uint64_t token, uint64_t epoch)
			{
				auto& state = *static_cast<MockGuiState*>(self);
				if (epoch == 1 && token > 0 && token <= state.knownActorCount && token <= 3)
					state.selectedActorToken = token;
			};
			ctx.playlistMode = playlistMode;
			ctx.playlistNotice = playlistNotice;
			ctx.importBusy = importBusy;
			ctx.importProgress = &importProgress;

			ctx.lookAt = &lookAt;
			ctx.config = &config;

			ctx.cameraSnapshot = cameraSnapshot;
			ctx.cameraNotice = cameraNotice;
			ctx.cameraSource = cameraSource;
			ctx.cameraFrames = cameraFrames;
			ctx.cameraFps = cameraFps;

			ctx.stageSettings = stageSettings;
			ctx.stageCandidates = stageCandidates;
			ctx.stageSceneName = stageSceneName;
			ctx.stageMountedKey = stageMountedKey;
			ctx.stageStatusNotice = stageStatusNotice;

			ctx.overrides = &overrides;
			ctx.masterOverridesEnabled = &masterOverridesEnabled;
			ctx.knownActorCount = knownActorCount;
			ctx.initialPositionNotice = initialPositionNotice;

			ctx.soleSettings = soleSettings;
			ctx.soleSnapshot = soleSnapshot;
			ctx.soleNotice = soleNotice;
			ctx.soleCalibrationGeneration = soleCalibrationGeneration;

			ctx.experimentalSolePitchSettings = experimentalSolePitchSettings;
			ctx.experimentalSolePitchSnapshot = experimentalSolePitchSnapshot;

			ctx.physicsBackend = physicsBackend;
			ctx.beforeQuartzExperiment = beforeQuartzExperiment;
			ctx.experimentalWaistSupport = experimentalWaistSupport;
			ctx.statusLine = statusLine;

			// ---- 播放控制回调 ----
			ctx.fnPlay = [](void* self)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->player.state = PlaybackState::Playing;
			};
			ctx.fnPause = [](void* self)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->player.state = PlaybackState::Paused;
			};
			ctx.fnResume = [](void* self)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->player.state = PlaybackState::Playing;
			};
			ctx.fnStop = [](void* self)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->player.state = PlaybackState::Stopped;
				s->player.currentTime = 0.0f;
			};
			ctx.fnSeek = [](void* self, float seconds)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->player.Seek(seconds);
			};
			ctx.fnSeekFrame = [](void* self, int64_t frame) { static_cast<MockGuiState*>(self)->player.SeekFrame(frame); };
			ctx.fnStepFrame = [](void* self, int delta) { static_cast<MockGuiState*>(self)->player.StepFrame(delta); };
			ctx.fnReloadMotion = [](void* self) {
				auto* s = static_cast<MockGuiState*>(self);
				++s->loadedMotionGeneration;
				s->player.state = PlaybackState::Paused;
				s->libraryNotice = "预览示例：已重新加载，保持暂停";
			};
			ctx.fnScrubBegin = [](void* self, float seconds)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->player.currentTime = seconds;
			};
			ctx.fnScrubUpdate = [](void* self, float seconds)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->player.currentTime = seconds;
			};
			ctx.fnScrubEnd = [](void* self, float seconds)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->player.currentTime = seconds;
			};
			ctx.fnSetSpeed = [](void* self, float speed)
			{
				static_cast<MockGuiState*>(self)->player.playbackSpeedMultiplier = speed;
			};
			ctx.fnSetMuted = [](void* self, bool muted)
			{
				static_cast<MockGuiState*>(self)->player.muted = muted;
			};
			ctx.fnSelectMotion = [](void* self, int index, bool autoplay) -> bool
			{
				auto* s = static_cast<MockGuiState*>(self);
				if (index >= 0 && index < static_cast<int>(s->motions.size()))
				{
					s->selectedMotionIndex = index;
					++s->loadedMotionGeneration;
					const auto& entry = s->motions[index];
					s->player.loadedMotionFile = entry.motionFile.empty() ? entry.name + "/motion.gkmotion" : entry.motionFile;
					s->player.currentMotionDir = entry.dir;
					s->player.clip.total_frames = entry.totalFrames;
					s->player.clip.duration_seconds = entry.durationSeconds;
					s->player.clip.fps = entry.fps;
					s->player.clip.bone_count = entry.boneCount;
					s->player.currentTime = 0.0f;
					s->player.state = autoplay ? PlaybackState::Playing : PlaybackState::Preview;
					return true;
				}
				return false;
			};
			ctx.fnCycleMotion = [](void* self, int delta) -> bool
			{
				auto* s = static_cast<MockGuiState*>(self);
				if (!s->motions.empty())
				{
					s->selectedMotionIndex = (s->selectedMotionIndex + delta + static_cast<int>(s->motions.size())) % static_cast<int>(s->motions.size());
					++s->loadedMotionGeneration;
					const auto& entry = s->motions[s->selectedMotionIndex];
					s->player.loadedMotionFile = entry.motionFile.empty() ? entry.name + "/motion.gkmotion" : entry.motionFile;
					s->player.currentMotionDir = entry.dir;
					s->player.clip.total_frames = entry.totalFrames;
					s->player.clip.duration_seconds = entry.durationSeconds;
					s->player.clip.fps = entry.fps;
					s->player.clip.bone_count = entry.boneCount;
					s->player.currentTime = 0.0f;
					if (s->player.state == PlaybackState::Stopped) s->player.state = PlaybackState::Preview;
					return true;
				}
				return false;
			};
			ctx.fnRefreshMotions = [](void* self) -> bool {
				auto* s = static_cast<MockGuiState*>(self); ++s->libraryRevision;
				s->libraryNotice = "曲库已是最新（离线示例）"; return true;
			};
			ctx.fnPlaylistAction = [](void* self, int action, const char* key, int value)
			{
				auto* s = static_cast<MockGuiState*>(self);
				std::string keyStr = key ? key : "";
				const auto operation = static_cast<GuiPlaylistAction>(action);
				if (operation == GuiPlaylistAction::DeleteFiles && s->importBusy)
				{ s->playlistNotice = "离线预览：模拟导入中，暂不能删除；未操作本地文件"; return; }
				if (operation == GuiPlaylistAction::DeleteFiles)
				{
					if (value != 1)
					{ s->playlistNotice = "离线预览：尚未确认删除，状态未变；未操作本地文件"; return; }
					const int index = FindPlaylistIndex(s->motions, keyStr);
					if (index < 0) return;
					const auto loaded = s->selectedMotionIndex >= 0 && s->selectedMotionIndex < static_cast<int>(s->motions.size())
						? s->motions[s->selectedMotionIndex].name : "";
					s->motions.erase(s->motions.begin() + index);
					s->selectedMotionIndex = loaded == keyStr ? -1 : FindPlaylistIndex(s->motions, loaded);
					if (loaded == keyStr)
					{
						s->player.state = PlaybackState::Stopped;
						s->player.currentTime = 0.0f;
						s->player.audioStarted = false;
						s->player.clip = {};
						s->player.loadedMotionFile.clear();
						s->player.currentMotionDir.clear();
						++s->loadedMotionGeneration;
					}
					++s->libraryRevision;
					s->playlistNotice = "离线预览：已模拟永久删除；未操作本地文件: " + keyStr;
				}
				else if (action == 1) // Move
				{
					int delta = value;
					int idx = -1;
					for (size_t i = 0; i < s->motions.size(); ++i)
					{
						if (s->motions[i].name == keyStr) { idx = static_cast<int>(i); break; }
					}
					int target = idx + delta;
					if (idx >= 0 && target >= 0 && target < static_cast<int>(s->motions.size()))
					{
						std::swap(s->motions[idx], s->motions[target]);
						if (s->selectedMotionIndex == idx) s->selectedMotionIndex = target;
						else if (s->selectedMotionIndex == target) s->selectedMotionIndex = idx;
					}
				}
				else if (action == 5) // Mode
				{
					s->playlistMode = value;
				}
			};

			ctx.fnImportMotion = [](void* self, const MotionImportRequest& req)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->importBusy = true;
				s->mockImportElapsed = 0.0f;
				s->pendingImportReq = req;
				s->importProgress = {true, false, false, 0.05f, "INIT", "正在校验输入文件...", ""};
			};

			// ---- 视线回调 ----
			ctx.fnCycleGaze = [](void* self) -> int
			{
				auto* s = static_cast<MockGuiState*>(self);
				int next = (static_cast<int>(s->lookAt.gazeMode.load()) + 1) % 3;
				s->lookAt.gazeMode.store(static_cast<GazeMode>(next));
				return next;
			};

			// ---- 镜头回调 ----
			ctx.fnSetCameraSettings = [](void* self, const CameraSettings& settings)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->cameraSnapshot.settings = settings;
				s->cameraSnapshot.fov = settings.followFov;
				if (settings.mode == CameraMode::Follow)
				{
					if (settings.followBehavior == FollowBehavior::LookAt)
						s->cameraSnapshot.position = settings.transformLookAt.position;
					else
						s->cameraSnapshot.position = settings.transformFollowMove.position;
				}
				else
				{
					s->cameraSnapshot.position = settings.transformTrack.position;
				}
			};
			ctx.fnSaveCameraSettings = [](void* self)
			{
				static_cast<MockGuiState*>(self)->cameraNotice = "镜头设置已保存（Mock）";
			};
			ctx.fnReloadCamera = [](void* self)
			{
				static_cast<MockGuiState*>(self)->cameraNotice = "已重新载入镜头轨道（Mock）";
			};

			// ---- 舞台回调 ----
			ctx.fnSetStageSettings = [](void* self, const StageSettings& settings)
			{
				auto* s = static_cast<MockGuiState*>(self);
				const bool customChanged = (s->stageSettings.customNodeControl != settings.customNodeControl);
				const bool hideChanged = (s->stageSettings.hideBackground != settings.hideBackground);
				const bool lightingChanged = (s->stageSettings.lightingEffectsEnabled != settings.lightingEffectsEnabled);

				s->stageSettings = settings;

				if (customChanged)
				{
					if (!settings.customNodeControl)
					{
						// customNodeControl true -> false: reset userMuted and sync
						for (auto& c : s->stageCandidates)
						{
							c.userMuted = false;
							if (c.isColliderLocked) continue;

							if (c.isIdentifiedBackground)
							{
								c.currentlyActive = !settings.hideBackground;
							}
							else if (c.isLightingNode)
							{
								c.currentlyActive = settings.lightingEffectsEnabled;
							}
							else if (!c.isLightingRoot)
							{
								c.currentlyActive = c.originalActive;
							}
						}
					}
				}
				else if (!settings.customNodeControl)
				{
					if (hideChanged)
					{
						for (auto& c : s->stageCandidates)
						{
							if (c.isColliderLocked || c.isLightingRoot || c.isLightingNode) continue;
							if (c.isIdentifiedBackground)
							{
								c.currentlyActive = !settings.hideBackground;
							}
						}
					}
					if (lightingChanged)
					{
						for (auto& c : s->stageCandidates)
						{
							if (c.isColliderLocked) continue;
							if (c.isLightingNode)
							{
								c.currentlyActive = settings.lightingEffectsEnabled;
							}
							else if (c.isLightingRoot && c.hasChildren && settings.lightingEffectsEnabled)
							{
								c.currentlyActive = true;
							}
						}
					}
				}
			};
			ctx.fnSetStagePreset = [](void* self, int presetIndex)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->stageSettings.selectedPresetIndex = presetIndex;
				if (presetIndex >= 0 && presetIndex < kStagePresetCount)
				{
					const auto& p = kDefaultStagePresets[presetIndex];
					s->stageStatusNotice = std::string("已切换至预设: ") + p.displayName;
					if (p.isChromaScreen)
					{
						s->stageSettings.customStageKey.clear();
						s->stageSettings.hideBackground = true;
						s->stageSettings.greenScreenActive = true;
						s->stageSettings.overrideCameraClear = true;
						s->stageSettings.greenScreenColor[0] = p.defaultColor[0];
						s->stageSettings.greenScreenColor[1] = p.defaultColor[1];
						s->stageSettings.greenScreenColor[2] = p.defaultColor[2];
						s->stageSettings.greenScreenColor[3] = p.defaultColor[3];
						if (!s->stageSettings.customNodeControl)
						{
							for (auto& c : s->stageCandidates)
							{
								if (c.isIdentifiedBackground && !c.isColliderLocked) c.currentlyActive = false;
							}
						}
					}
					else if (std::strcmp(p.key, "native") == 0)
					{
						s->stageSettings.hideBackground = false;
						s->stageSettings.greenScreenActive = false;
						s->stageSettings.overrideCameraClear = false;
						s->stageSettings.customStageKey.clear();
						if (!s->stageSettings.customNodeControl)
						{
							for (auto& c : s->stageCandidates)
							{
								if (c.isIdentifiedBackground && !c.isColliderLocked) c.currentlyActive = true;
							}
						}
					}
				}
			};
			ctx.fnToggleStageCandidate = [](void* self, int index, bool active)
			{
				auto* s = static_cast<MockGuiState*>(self);
				if (index >= 0 && index < static_cast<int>(s->stageCandidates.size()))
				{
					auto& c = s->stageCandidates[index];
					if (!c.isColliderLocked)
					{
						c.currentlyActive = active;
						c.userMuted = !active;
					}
				}
			};
			ctx.fnScanStageCandidates = [](void* self)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->stageStatusNotice = "场景节点扫描完成（Mock 刷新）";
			};
			ctx.fnMountCustomStage = [](void* self, const char* key)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->stageMountedKey = key ? key : "";
				s->stageStatusNotice = std::string("已挂载自定义舞台: ") + s->stageMountedKey;
			};
			ctx.fnUnloadCustomStage = [](void* self)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->stageMountedKey.clear();
				s->stageStatusNotice = "已卸载自定义舞台，恢复原生环境";
			};
			ctx.fnRestoreCameraSkybox = [](void* self)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->stageSettings.greenScreenActive = false;
				s->stageSettings.overrideCameraClear = false;
				s->stageStatusNotice = "已重置相机天空盒模式";
			};

			// ---- 角色与骨骼回调 ----
			ctx.fnSetInitialPosition = [](void* self, const Vector3& offset)
			{
				static_cast<MockGuiState*>(self)->player.controller.initialPositionOffset = offset;
			};
			ctx.fnSaveInitialPosition = [](void* self)
			{
				static_cast<MockGuiState*>(self)->initialPositionNotice = "初始位置已保存（Mock）";
			};
			ctx.fnToggleOverrides = [](void* self) -> bool
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->masterOverridesEnabled = !s->masterOverridesEnabled;
				return s->masterOverridesEnabled;
			};
			ctx.fnReloadOverrides = [](void* self)
			{
				auto* s = static_cast<MockGuiState*>(self);
				s->overrides.offsets.clear();
				{
					BoneRotationOffset o;
					o.humanoidId = 13;
					o.boneName = "LeftUpperArm";
					o.eulerDeg = {5.0f, -10.0f, 0.0f};
					o.spaceWorld = false;
					o.enabled = true;
					s->overrides.offsets.push_back(o);
				}
				{
					BoneRotationOffset o;
					o.humanoidId = 14;
					o.boneName = "RightUpperArm";
					o.eulerDeg = {5.0f, 10.0f, 0.0f};
					o.spaceWorld = false;
					o.enabled = true;
					s->overrides.offsets.push_back(o);
				}
			};
			ctx.fnResetAllOverrides = [](void* self)
			{
				static_cast<MockGuiState*>(self)->overrides.offsets.clear();
			};
			ctx.fnSaveOverridesToMotion = [](void*) -> bool { return true; };
			ctx.fnSaveOverridesToGlobal = [](void*) -> bool { return true; };
			ctx.fnSetBoneOverride = [](void* self, int hid, const char* name, float pitch, float yaw, float roll,
			                           bool world, bool enabled, bool symmetrical)
			{
				auto* s = static_cast<MockGuiState*>(self);
				auto applyOne = [&](int targetHid, const char* targetName, float p, float y, float r)
				{
					for (auto& entry : s->overrides.offsets)
					{
						if (entry.humanoidId == targetHid)
						{
							entry.eulerDeg = {p, y, r};
							entry.spaceWorld = world;
							entry.enabled = enabled;
							return;
						}
					}
					BoneRotationOffset newEntry;
					newEntry.humanoidId = targetHid;
					newEntry.boneName = targetName ? targetName : "";
					newEntry.eulerDeg = {p, y, r};
					newEntry.spaceWorld = world;
					newEntry.enabled = enabled;
					s->overrides.offsets.push_back(newEntry);
				};
				applyOne(hid, name, pitch, yaw, roll);
				if (symmetrical)
				{
					int symmHid = BoneOverrideSet::GetSymmetricalHumanoidId(hid);
					if (symmHid >= 0)
					{
						std::string symmName = BoneOverrideSet::GetBoneNameByHumanoidId(symmHid);
						applyOne(symmHid, symmName.c_str(), pitch, -yaw, -roll);
					}
				}
			};
			ctx.fnResetBoneOverride = [](void* self, int hid, const char*, bool symmetrical)
			{
				auto* s = static_cast<MockGuiState*>(self);
				auto removeOne = [&](int targetHid)
				{
					s->overrides.offsets.erase(
						std::remove_if(s->overrides.offsets.begin(), s->overrides.offsets.end(),
						               [targetHid](const BoneRotationOffset& e) { return e.humanoidId == targetHid; }),
						s->overrides.offsets.end());
				};
				removeOne(hid);
				if (symmetrical)
				{
					int symmHid = BoneOverrideSet::GetSymmetricalHumanoidId(hid);
					if (symmHid >= 0) removeOne(symmHid);
				}
			};
			ctx.fnGetBoneOverride = [](void* self, int hid, const char*, float* outPitch, float* outYaw,
			                           float* outRoll, bool* outWorld, bool* outEnabled) -> bool
			{
				auto* s = static_cast<MockGuiState*>(self);
				for (const auto& entry : s->overrides.offsets)
				{
					if (entry.humanoidId == hid)
					{
						if (outPitch) *outPitch = entry.eulerDeg.x;
						if (outYaw) *outYaw = entry.eulerDeg.y;
						if (outRoll) *outRoll = entry.eulerDeg.z;
						if (outWorld) *outWorld = entry.spaceWorld;
						if (outEnabled) *outEnabled = entry.enabled;
						return true;
					}
				}
				return false;
			};

			// ---- 鞋底测量回调 ----
			ctx.fnSetSoleSettings = [](void* self, const SoleMeasurementSettings& settings)
			{
				static_cast<MockGuiState*>(self)->soleSettings = settings;
			};
			ctx.fnSaveSoleSettings = [](void* self)
			{
				static_cast<MockGuiState*>(self)->soleNotice = "鞋底测量设置已保存（Mock）";
			};
			ctx.fnLoadSoleSettings = [](void* self)
			{
				static_cast<MockGuiState*>(self)->soleNotice = "已重新读取鞋底测量设置（Mock）";
			};

			// ---- Pitch Demo 回调 ----
			ctx.fnSetExperimentalSolePitch = [](void* self, const sole_pitch::Settings& settings)
			{
				static_cast<MockGuiState*>(self)->experimentalSolePitchSettings = settings;
			};

			// ---- 配置页回调 ----
			ctx.fnSetPhysicsBackend = [](void* self, int backend)
			{
				static_cast<MockGuiState*>(self)->physicsBackend = backend;
			};

			return ctx;
		}
	};
} // namespace GakuMotion
