#pragma once

#include "GkCameraBridgeFinder.hpp"

namespace CameraBridge
{
	inline void WriteVirtualCamerasFov(void* camera, float fov)
	{
		if (!std::isfinite(fov) || fov < 1.0f || fov > 179.0f) return;

		if (trackedVcams.empty())
		{
			ResolveAllVirtualCameras(camera);
		}

		for (auto& item : trackedVcams)
		{
			void* v = item.vcamPtr;
			if (!v || !IsAlive(v)) continue;

			// Write m_Lens.FieldOfView
			if (vcamLensOffset >= 0)
			{
				item.lensModified = true;
				float* pFov = reinterpret_cast<float*>(reinterpret_cast<char*>(v) + vcamLensOffset + unboxedLensFovOffset);
				if (!item.originalFovSaved && std::isfinite(*pFov) && *pFov > 1.0f && *pFov < 179.0f)
				{
					item.originalFov = *pFov;
					item.originalFovSaved = true;
				}
				*pFov = fov;

				if (vcamLensField && g_faceRuntime.readField && g_faceRuntime.writeField)
				{
					char lensBuffer[128]{};
					g_faceRuntime.readField(v, vcamLensField, lensBuffer);
					*reinterpret_cast<float*>(lensBuffer + unboxedLensFovOffset) = fov;
					g_faceRuntime.writeField(v, vcamLensField, lensBuffer);
				}
			}

			// Write m_State.Lens.FieldOfView
			if (vcamStateOffset >= 0)
			{
				item.stateModified = true;
				float* pStateFov = reinterpret_cast<float*>(
					reinterpret_cast<char*>(v) + vcamStateOffset + unboxedCameraStateLensOffset + unboxedLensFovOffset);
				*pStateFov = fov;
			}
		}

		// Also update CinemachineBrain CurrentCameraState if brain is present
		if (camera && g_getComponentNative && brainTypeObj && brainCurrentStateOffset >= 0)
		{
			void* brain = g_getComponentNative(camera, brainTypeObj);
			if (brain && IsAlive(brain))
			{
				auto found = std::find_if(trackedBrains.begin(),trackedBrains.end(),[&](const TrackedBrain& item) { return item.object==brain; });
				if (found==trackedBrains.end())
				{
					TrackedBrain item; item.object=brain;
					item.saved=ReadNativeFov(brain,brainCurrentStateOffset + unboxedCameraStateLensOffset + unboxedLensFovOffset,item.fov);
					trackedBrains.push_back(item); found=std::prev(trackedBrains.end());
				}
				found->modified=true;
				float* pBrainFov = reinterpret_cast<float*>(
					reinterpret_cast<char*>(brain) + brainCurrentStateOffset + unboxedCameraStateLensOffset + unboxedLensFovOffset);
				*pBrainFov = fov;
			}
		}
	}

	inline bool ReadFocalPoint(void* model, float& value)
	{
		if (!model || !modelKlass || !g_faceRuntime.findMethod || !g_faceRuntime.returnType ||
			!g_faceRuntime.typeCode || !g_faceRuntime.methodFlags || !g_faceRuntime.invoke || !g_safeBones.unbox) return false;
		if (!modelGetFocalPointMethod)
		{
			const void* getter=g_faceRuntime.findMethod(modelKlass,"get_FocalPoint",0);
			uint32_t impl=0;
			void* returns=getter ? g_faceRuntime.returnType(getter):nullptr;
			if (!getter || !returns || (g_faceRuntime.methodFlags(getter,&impl)&0x10) ||
				g_faceRuntime.typeCode(returns)!=0x0c) return false;
			modelGetFocalPointMethod=getter;
		}
		void* exception=nullptr;
		void* boxed=g_faceRuntime.invoke(modelGetFocalPointMethod,model,nullptr,&exception);
		void* raw=boxed && !exception ? g_safeBones.unbox(boxed):nullptr;
		if (!raw) return false;
		value=*static_cast<float*>(raw);
		return std::isfinite(value);
	}

	inline GakuMotion::CaptureRestoreStatus ReleaseVirtualCamerasForCapture()
	{
		GakuMotion::CaptureRestoreStatus result;
		for (const auto& item:trackedVcams)
		{
			if (!item.lensModified && !item.stateModified) continue;
			result.modified=true;
			if (!IsAlive(item.vcamPtr)) { result.confirmed=false; continue; }
			if (item.lensModified && (!item.originalFovSaved || !RestoreNativeFov(item.vcamPtr,
				vcamLensOffset + unboxedLensFovOffset,item.originalFov))) result.confirmed=false;
			if (item.stateModified && (!item.originalStateFovSaved || !RestoreNativeFov(item.vcamPtr,
				vcamStateOffset + unboxedCameraStateLensOffset + unboxedLensFovOffset,item.originalStateFov))) result.confirmed=false;
		}
		for (const auto& item:trackedBrains)
		{
			if (!item.modified) continue;
			result.modified=true;
			if (!item.saved || !IsAlive(item.object) || !RestoreNativeFov(item.object,
				brainCurrentStateOffset + unboxedCameraStateLensOffset + unboxedLensFovOffset,item.fov)) result.confirmed=false;
		}
		if (focalPointModified)
		{
			result.modified=true;
			void* currentModel=nullptr;
			if (IsAlive(focalOwner) && actCtrlModelField && g_faceRuntime.readField)
				g_faceRuntime.readField(focalOwner,actCtrlModelField,&currentModel);
			if (!originalFocalPointSaved || currentModel!=focalModel || !modelSetFocalPointMethod || !g_faceRuntime.invoke)
				result.confirmed=false;
			else
			{
				void* args[]{&originalFocalPoint}; void* exception=nullptr;
				g_faceRuntime.invoke(modelSetFocalPointMethod,focalModel,args,&exception);
				float actual=0;
				if (exception || !ReadFocalPoint(focalModel,actual) || std::fabs(actual-originalFocalPoint)>1e-4f) result.confirmed=false;
			}
		}
		if (result.confirmed)
		{
			trackedVcams.clear(); trackedBrains.clear(); focalOwner=focalModel=nullptr;
			originalFocalPointSaved=focalPointModified=false;
			trackedActCtrl=nullptr; trackedModel=nullptr;
		}
		return result;
	}

	inline void ReleaseVirtualCameras(bool restore)
	{
		if (restore) { ReleaseVirtualCamerasForCapture(); return; }
		trackedVcams.clear(); trackedBrains.clear(); focalOwner=focalModel=nullptr;
		originalFocalPointSaved=focalPointModified=false;
		trackedActCtrl=nullptr; trackedModel=nullptr;
	}

	inline void WritePhotographyDoFFocalPoint(float focalDistance)
	{
		if (!std::isfinite(focalDistance) || focalDistance <= 0.05f) return;
		ResolvePhotographyFocusController();

		if (trackedModel && modelSetFocalPointMethod && g_faceRuntime.invoke)
		{
			// Do not extend ownership to a replacement model while the old focus
			// mutation still needs restoration.
			if (focalPointModified && trackedModel!=focalModel) return;
			if (!focalPointModified)
			{
				focalOwner=trackedActCtrl; focalModel=trackedModel;
				originalFocalPointSaved=ReadFocalPoint(focalModel,originalFocalPoint);
			}
			focalPointModified=true;
			void* args[1] = { &focalDistance };
			void* ex = nullptr;
			g_faceRuntime.invoke(modelSetFocalPointMethod, trackedModel, args, &ex);
		}
	}

	inline void Write(void* camera, void* transform, const GakuMotion::CameraFrame& frame)
	{
		if (!OnUnityThread() || !ready || !IsAlive(camera) || !IsAlive(transform)) return;
		void* cameraSelf = Self(camera);
		void* transformSelf = Self(transform);
		if (!cameraSelf || !transformSelf) return;
		ownedNativeCamera = cameraSelf;
		ownedNativeTransform = transformSelf;
		writing = true;
		UnityVector3 position{frame.position.x, frame.position.y, frame.position.z};
		auto q = frame.rotation;
		// The host already hooks world setters. Its free-camera detour can replace
		// our arguments, so apply through the native LOCAL pair setter instead.
		// Parent conversion makes this work for camera rigs as well as scene roots.
		if (void* parent = g_getParentNative(transform); IsAlive(parent))
		{
			UnityVector3 local{};
			void* parentSelf = Self(parent);
			if (!parentSelf)
			{
				writing = false;
				return;
			}
			inversePoint(parentSelf, &position, &local);
			position = local;
			UnityQuaternion p = g_getWorldRotNative(parent);
			q = GakuMotion::Quaternion(p.x, p.y, p.z, p.w).Inverse() * q;
		}
		UnityQuaternion rotation{q.x, q.y, q.z, q.w};
		localPose(transformSelf, &position, &rotation);
		// Drive active Cinemachine Virtual Camera Lens with VMD FOV
		WriteVirtualCamerasFov(camera, frame.fov);

		// Drive Photography DoF Focal Point from VMD camera distance
		float focalDistance = 0.0f;
		if ((frame.flags & 1u) == 0 && std::isfinite(frame.fov) && frame.fov > 1.0f && frame.fov < 179.0f)
		{
			constexpr float kPi = 3.14159265358979323846f;
			float tanHalfFov = std::tan(frame.fov * (kPi / 360.0f));
			if (tanHalfFov > 1e-4f && frame.orthographicSize > 0.0015f)
			{
				focalDistance = frame.orthographicSize / tanHalfFov;
			}
		}

		// Fallback to Euclidean distance to actor if distance is ~0.0 or unavailable (e.g. SOS camera)
		if (focalDistance < 0.2f)
		{
			auto& ctrl = Controller();
			if (ctrl.actor && ctrl.fnActorPose)
			{
				GakuMotion::Vector3 actorPos, hips;
				GakuMotion::Quaternion actorRot;
				if (ctrl.fnActorPose(ctrl.actor, actorPos, actorRot, hips))
				{
					GakuMotion::Vector3 delta = frame.position - actorPos;
					focalDistance = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
				}
			}
		}

		if (focalDistance >= 0.2f && focalDistance <= 50.0f)
		{
			WritePhotographyDoFFocalPoint(focalDistance);
		}

		// Use icalls rather than the host's managed FOV detour.
		setOrthographic(cameraSelf, (frame.flags & 1u) != 0);
		setSize(cameraSelf, frame.orthographicSize);
		setFov(cameraSelf, frame.fov);
		writing = false;
	}

	inline bool Read(void* camera, void* transform, GakuMotion::CameraFrame& frame)
	{
		if (!OnUnityThread() || !IsAlive(camera) || !IsAlive(transform)) return false;
		void* cameraSelf = Self(camera);
		if (!cameraSelf) return false;
		UnityVector3 p = g_getWorldPosNative(transform);
		UnityQuaternion q = g_getWorldRotNative(transform);
		frame.position = {p.x, p.y, p.z};
		frame.rotation = {q.x, q.y, q.z, q.w};
		frame.fov = getFov(cameraSelf);
		frame.orthographicSize = getSize(cameraSelf);
		frame.flags = getOrthographic(cameraSelf) ? 1u : 0u;
		float norm = GakuMotion::Quaternion::Dot(frame.rotation, frame.rotation);
		return GakuMotion::CameraFinite(frame.position) && std::isfinite(frame.fov) &&
			frame.fov >= 1 && frame.fov <= 179 && std::isfinite(frame.orthographicSize) &&
			frame.orthographicSize > 0 && std::isfinite(norm) && norm > 1e-8f;
	}
} // namespace CameraBridge
