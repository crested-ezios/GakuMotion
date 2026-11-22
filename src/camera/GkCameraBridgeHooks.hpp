#pragma once

#include "GkCameraBridgeApply.hpp"

namespace CameraBridge
{
	inline void ReapplyTransform(void* tr)
	{
		if (!OnUnityThread()) return;
		if (!writing && Controller().owned &&
			(nativeSelf ? tr == ownedNativeTransform : Controller().BlocksTransform(tr)))
			Controller().BeforeRender(Controller().camera);
	}

	inline void SharedTransformWritten(void*, uint32_t kind, void* transformSelf)
	{
		// Provider notifications can also originate from non-Unity writers. Do
		// not inspect controller state on those threads or after retirement.
		if (!OnUnityThread() || kind >= GAKULOCALIFY_CAMERA_WRITE_COUNT ||
			!sharedWritesEnabled.load(std::memory_order_acquire)) return;
		ReapplyTransform(transformSelf);
	}

	inline bool SubscribeSharedTransformWrites(void* const (&targets)[GAKULOCALIFY_CAMERA_WRITE_COUNT])
	{
		if (!GakuMotion::host::active) return true;
		GakuLocalifyCameraWriteService candidate{};
		const auto& api = GakuMotion::host::api;
		const auto queried = api.queryService
			? api.queryService(api.hostContext, GAKULOCALIFY_SERVICE_CAMERA_WRITE,
				GAKULOCALIFY_CAMERA_WRITE_VERSION, sizeof(candidate), &candidate)
			: GAKU_E_INVALID_ARGUMENT;
		// An absent provider keeps the existing exclusive-lease path. A present
		// but incompatible provider must never waive any interception lease.
		if (queried == GAKU_E_UNAVAILABLE) return true;
		if (queried != GAKU_OK || candidate.abiVersion != GAKULOCALIFY_CAMERA_WRITE_VERSION ||
			candidate.structSize < sizeof(candidate) || !candidate.getTarget ||
			!candidate.subscribe || !candidate.unsubscribe)
		{
			if (g_hostContext.logWarn) g_hostContext.logWarn(
				"[GakuMotion][Camera] Localify camera-write service incompatible: status=%d version=%u size=%u",
				queried, candidate.abiVersion, candidate.structSize);
			return false;
		}
		void* providerTargets[GAKULOCALIFY_CAMERA_WRITE_COUNT]{};
		bool anyProvided = false;
		for (uint32_t kind = 0; kind < GAKULOCALIFY_CAMERA_WRITE_COUNT; ++kind)
		{
			void* target = candidate.getTarget(candidate.context, kind);
			// A disabled or partially initialized provider owns only its non-null
			// targets. Unprovided entries still require our exclusive hook lease.
			if (target && (!targets[kind] || target != targets[kind]))
			{
				if (g_hostContext.logWarn) g_hostContext.logWarn(
					"[GakuMotion][Camera] Localify camera-write target mismatch: kind=%u provider=%p expected=%p",
					kind, target, targets[kind]);
				return false;
			}
			providerTargets[kind] = target;
			anyProvided |= target != nullptr;
		}
		if (!anyProvided) return true;
		const auto subscribed = candidate.subscribe(candidate.context, SharedTransformWritten, nullptr);
		if (subscribed != GAKU_OK)
		{
			if (g_hostContext.logWarn) g_hostContext.logWarn(
				"[GakuMotion][Camera] Localify camera-write subscription refused: status=%d", subscribed);
			return false;
		}
		sharedWrites = candidate;
		sharedWritesSubscribed = true;
		for (uint32_t kind = 0; kind < GAKULOCALIFY_CAMERA_WRITE_COUNT; ++kind)
			sharedWriteTargets[kind] = providerTargets[kind];
		if (g_hostContext.logInfo) g_hostContext.logInfo(
			"[GakuMotion][Camera] Localify camera-write observer connected: exact Transform targets validated");
		return true;
	}

	inline bool InstallHook(const char* name, void* target, void* detour, void** original, int sharedKind = -1)
	{
		if (!RequireDependency(name, target)) return false;
		if (sharedWritesSubscribed && sharedKind >= 0 && sharedKind < GAKULOCALIFY_CAMERA_WRITE_COUNT &&
			sharedWriteTargets[sharedKind] == target) return true;
		if (GakuMotion::host::active)
		{
			const auto acquired = GakuMotion::host::hooks.acquire(
				GakuMotion::host::hooks.context, target, detour, original);
			if (acquired == GAKU_OK) installed.push_back(target);
			else if (g_hostContext.logWarn) g_hostContext.logWarn(
				"[GakuMotion][Camera] GakuLoader hook lease refused: %s status=%d", name, acquired);
			return acquired == GAKU_OK;
		}
		const auto created = MH_CreateHook(target, detour, original);
		if (created != MH_OK)
		{
			if (g_hostContext.logWarn) g_hostContext.logWarn(
				"[GakuMotion][Camera] MH_CreateHook failed: %s target=%p status=%s", name, target,
				MH_StatusToString(created));
			return false;
		}
		installed.push_back(target);
		const auto enabled = MH_EnableHook(target);
		if (enabled != MH_OK && g_hostContext.logWarn)
			g_hostContext.logWarn("[GakuMotion][Camera] MH_EnableHook failed: %s target=%p status=%s", name, target,
				MH_StatusToString(enabled));
		return enabled == MH_OK;
	}

	inline void ReapplyLens(void* c)
	{
		if (!OnUnityThread()) return;
		if (!writing && Controller().owned &&
			(nativeSelf ? c == ownedNativeCamera : Controller().BlocksCamera(c)))
			Controller().BeforeRender(Controller().camera);
	}

	inline void WorldPosition(void* tr, UnityVector3* p)
	{
		worldPosition(tr, p);
		ReapplyTransform(tr);
	}

	inline void LocalPosition(void* tr, UnityVector3* p)
	{
		localPosition(tr, p);
		ReapplyTransform(tr);
	}

	inline void WorldRotation(void* tr, UnityQuaternion* q)
	{
		worldRotation(tr, q);
		ReapplyTransform(tr);
	}

	inline void LocalRotation(void* tr, UnityQuaternion* q)
	{
		localRotation(tr, q);
		ReapplyTransform(tr);
	}

	inline void WorldPose(void* tr, UnityVector3* p, UnityQuaternion* q)
	{
		worldPose(tr, p, q);
		ReapplyTransform(tr);
	}

	inline void LocalPose(void* tr, UnityVector3* p, UnityQuaternion* q)
	{
		localPose(tr, p, q);
		ReapplyTransform(tr);
	}

	inline void Fov(void* c, float v)
	{
		setFov(c, v);
		ReapplyLens(c);
	}

	inline void Size(void* c, float v)
	{
		setSize(c, v);
		ReapplyLens(c);
	}

	inline void Orthographic(void* c, bool v)
	{
		setOrthographic(c, v);
		ReapplyLens(c);
	}

	inline void SharedCameraBegin(void*, void* camera)
	{
		if (sharedBeginEnabled.load(std::memory_order_acquire) && OnUnityThread())
			Controller().BeforeRender(camera);
	}
	// selected=false keeps independent leases. A present but invalid provider
	// fails visibly rather than requesting the same exclusive leases.
	inline bool ConnectSharedCameraBegin(void* begin, void* manager, bool& selected)
	{
		selected = false;
		if (!GakuMotion::host::active) return true;
		GakuVrifyCameraBeginService service{};
		const auto status = GakuMotion::host::api.queryService(GakuMotion::host::api.hostContext,
			GAKU_VRIFY_CAMERA_BEGIN_SERVICE, 1, sizeof(service), &service);
		if (status == GAKU_E_UNAVAILABLE) return true;
		selected = true;
		if (status != GAKU_OK || service.abiVersion != 1 || service.structSize < sizeof(service) ||
			!service.context || !service.getTarget || !service.subscribe || !service.unsubscribe ||
			(!begin && !manager))
		{
			if (g_hostContext.logWarn) g_hostContext.logWarn("[GakuMotion][Camera] VRify camera-begin service invalid: status=%d", status);
			return false;
		}
		// No installed VR begin hooks (VR disabled/native-only) leaves Motion sole owner.
		if (!service.getTarget(service.context, 0) && !service.getTarget(service.context, 1))
		{
			selected = false;
			return true;
		}
		if (service.getTarget(service.context, 0) != begin ||
			service.getTarget(service.context, 1) != manager)
		{
			if (g_hostContext.logWarn) g_hostContext.logWarn("[GakuMotion][Camera] VRify camera-begin provider incompatible or target mismatch: status=%d", status);
			return false;
		}
		const auto subscribed = service.subscribe(service.context, SharedCameraBegin, nullptr);
		if (subscribed != GAKU_OK) return false;
		sharedBegin = service;
		sharedBeginSubscribed = true;
		sharedBeginEnabled.store(true, std::memory_order_release);
		if (g_hostContext.logInfo) g_hostContext.logInfo("[GakuMotion][Camera] VRify camera-begin observer connected: exact render targets validated");
		return true;
	}

	inline Render endFrameRender = nullptr, managerEndFrameRender = nullptr;
	inline void (*onFrameRenderCompleted)(){nullptr};

	inline void AfterFrameRendering(void* context, void* cameras, void* method)
	{
		if (endFrameRender) endFrameRender(context, cameras, method);
		if (OnUnityThread() && onFrameRenderCompleted) onFrameRenderCompleted();
	}

	inline void AfterFrameRenderingManager(void* context, void* cameras, void* method)
	{
		if (managerEndFrameRender) managerEndFrameRender(context, cameras, method);
		if (OnUnityThread() && onFrameRenderCompleted) onFrameRenderCompleted();
	}

	inline void BeforeCameraRendering(void* context, void* camera, void* method)
	{
		if (render) render(context, camera, method);
		// Event handlers and the host run first, then the MMD pose wins at render.
		if (OnUnityThread()) Controller().BeforeRender(camera);
	}

	inline void BeforeCameraRenderingManager(void* context, void* camera, void* method)
	{
		if (managerRender) managerRender(context, camera, method);
		if (OnUnityThread()) Controller().BeforeRender(camera);
	}

	using AddRenderPassesFn = void (*)(void*, void*, void*, void*);
	inline AddRenderPassesFn orig_AddRenderPasses = nullptr;
	using ShadowObserver = void (*)(void*, void*, const char*);
	inline ShadowObserver shadowObserver = nullptr;
	inline void ObserveShadow(void* feature, void* data, const char* phase) noexcept
	{
		// A diagnostic failure must never suppress or repeat the game's draw.
		try { if (shadowObserver) shadowObserver(feature, data, phase); }
		catch (...) {}
	}
	inline void InvokeShadowOriginal(void* self, void* renderer, void* data, void* method)
	{
		ObserveShadow(self, data, "native");
		try { if (orig_AddRenderPasses) orig_AddRenderPasses(self, renderer, data, method); }
		catch (...) { ObserveShadow(self, data, "aborted"); throw; }
	}
	inline int renderingDataCameraDataOffset = -1;
	inline int cameraDataViewMatrixOffset = -1;
	inline int cameraDataWorldPosOffset = -1;
	inline bool shadowOffsetsResolved = false;

	inline void ResolveShadowFieldOffsets()
	{
		if (shadowOffsetsResolved) return;
		shadowOffsetsResolved = true;

		void* renderingDataClass = FindClass("Unity.RenderPipelines.Universal.Runtime.dll",
											 "UnityEngine.Rendering.Universal", "RenderingData");
		void* cameraDataClass = FindClass("Unity.RenderPipelines.Universal.Runtime.dll",
										  "UnityEngine.Rendering.Universal", "CameraData");
		if (!renderingDataClass || !cameraDataClass || !g_faceRuntime.findField || !il2cpp_field_get_offset)
			return;

		constexpr int kBoxedHeader = 16;
		void* fCamData = g_faceRuntime.findField(renderingDataClass, "cameraData");
		if (fCamData)
		{
			int off = il2cpp_field_get_offset(fCamData);
			if (off >= kBoxedHeader) renderingDataCameraDataOffset = off - kBoxedHeader;
		}

		void* fView = g_faceRuntime.findField(cameraDataClass, "m_ViewMatrix");
		if (!fView) fView = g_faceRuntime.findField(cameraDataClass, "viewMatrix");
		if (fView)
		{
			int off = il2cpp_field_get_offset(fView);
			if (off >= kBoxedHeader) cameraDataViewMatrixOffset = off - kBoxedHeader;
		}

		void* fPos = g_faceRuntime.findField(cameraDataClass, "worldSpaceCameraPos");
		if (fPos)
		{
			int off = il2cpp_field_get_offset(fPos);
			if (off >= kBoxedHeader) cameraDataWorldPosOffset = off - kBoxedHeader;
		}

		if (g_hostContext.logInfo)
		{
			g_hostContext.logInfo("[GakuMotion][ShadowExp] rdCamOff=%d viewOff=%d posOff=%d",
								  renderingDataCameraDataOffset, cameraDataViewMatrixOffset, cameraDataWorldPosOffset);
		}
	}

	inline void Detour_AddRenderPasses(void* self, void* renderer, void* renderingData, void* methodInfo)
	{
		ObserveShadow(self, renderingData, "entry");
		const int expMode = Controller().settings.actorShadowExperimentMode;

		// Mode 0: Native game default
		if (expMode == 0 || !renderingData || !OnUnityThread())
		{
			InvokeShadowOriginal(self, renderer, renderingData, methodInfo);
			ObserveShadow(self, renderingData, "exit");
			return;
		}

		// Mode 2: Bypass actor shadow pass completely
		if (expMode == 2)
		{
			ObserveShadow(self, renderingData, "bypass");
			return;
		}

		// Mode 1: Anchor shadow camera near actor if distance is large
		ResolveShadowFieldOffsets();
		if (renderingDataCameraDataOffset < 0 || cameraDataViewMatrixOffset < 0 || cameraDataWorldPosOffset < 0)
		{
			InvokeShadowOriginal(self, renderer, renderingData, methodInfo);
			ObserveShadow(self, renderingData, "exit");
			return;
		}

		uint8_t* pCamData = reinterpret_cast<uint8_t*>(renderingData) + renderingDataCameraDataOffset;
		float* pWorldPos = reinterpret_cast<float*>(pCamData + cameraDataWorldPosOffset);
		float* pView = reinterpret_cast<float*>(pCamData + cameraDataViewMatrixOffset);

		float origPos[3] = { pWorldPos[0], pWorldPos[1], pWorldPos[2] };
		float origViewTrans[3] = { pView[12], pView[13], pView[14] };

		bool anchored = false;
		struct RestoreShadowCamera
		{
			float* position;
			float* view;
			const float* savedPosition;
			const float* savedTranslation;
			bool& active;
			void Restore() noexcept
			{
				if (!active) return;
				std::memcpy(position, savedPosition, 3 * sizeof(float));
				std::memcpy(view + 12, savedTranslation, 3 * sizeof(float));
				active = false;
			}
			~RestoreShadowCamera() { Restore(); }
		} restore{pWorldPos, pView, origPos, origViewTrans, anchored};
		auto& ctrl = Controller();
		if (ctrl.actor && ctrl.fnActorPose)
		{
			GakuMotion::Vector3 actorPos, hips;
			GakuMotion::Quaternion actorRot;
			if (ctrl.fnActorPose(ctrl.actor, actorPos, actorRot, hips))
			{
				float dx = origPos[0] - actorPos.x;
				float dy = origPos[1] - (actorPos.y + 1.0f);
				float dz = origPos[2] - actorPos.z;
				float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

				if (dist > 2.0f && std::isfinite(dist))
				{
					float scale = 1.8f / dist;
					float anchorPos[3] = {
						actorPos.x + dx * scale,
						(actorPos.y + 1.0f) + dy * scale,
						actorPos.z + dz * scale
					};

					float newTx = -(pView[0] * anchorPos[0] + pView[4] * anchorPos[1] + pView[8] * anchorPos[2]);
					float newTy = -(pView[1] * anchorPos[0] + pView[5] * anchorPos[1] + pView[9] * anchorPos[2]);
					float newTz = -(pView[2] * anchorPos[0] + pView[6] * anchorPos[1] + pView[10] * anchorPos[2]);

					pWorldPos[0] = anchorPos[0];
					pWorldPos[1] = anchorPos[1];
					pWorldPos[2] = anchorPos[2];

					pView[12] = newTx;
					pView[13] = newTy;
					pView[14] = newTz;

					anchored = true;
				}
			}
		}

		InvokeShadowOriginal(self, renderer, renderingData, methodInfo);
		restore.Restore();
		ObserveShadow(self, renderingData, "exit");
	}
} // namespace CameraBridge
