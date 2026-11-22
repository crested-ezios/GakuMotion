	static std::string Il2CppStringToUtf8(void* strObj)
	{
		if (!strObj) return "<null>";
		auto s = reinterpret_cast<Il2CppString*>(strObj);
		if (!s || s->length <= 0) return "";
		int size = WideCharToMultiByte(CP_UTF8, 0, s->chars, s->length, nullptr, 0, nullptr, nullptr);
		if (size <= 0) return "<conv_err>";
		std::string res(size, '\0');
		WideCharToMultiByte(CP_UTF8, 0, s->chars, s->length, res.data(), size, nullptr, nullptr);
		return res;
	}

	void* HelperGetFaceModel(void* actor)
	{
		if (!actor || !g_faceModelTypeObj || !g_getComponentInChildrenNative) return nullptr;
		return g_getComponentInChildrenNative(actor, g_faceModelTypeObj);
	}

	uint64_t HelperFaceQpcTicks()
	{
		LARGE_INTEGER ticks{};
		QueryPerformanceCounter(&ticks);
		return static_cast<uint64_t>(ticks.QuadPart);
	}

	int (*g_faceUnityFrame)(){nullptr};
	float (*g_unityUnscaledDeltaTime)(){nullptr};
	int (*g_unityRenderedFrameCount)(){nullptr};

	std::string HelperGetFaceShapeName(void* model, int index)
	{
		return g_faceShapeName ? Il2CppStringToUtf8(g_faceShapeName(model, index)) : "";
	}

	void* HelperGetFacial(void* actor) { return g_faceRuntime.Facial(actor); }

	bool HelperReadBlink(void* facial, bool& useBlink, bool& autoDisabled)
	{
		const bool ok = g_faceRuntime.ReadBlink(facial, useBlink, autoDisabled);
		if (!ok && g_hostContext.logWarn)
		{
			void* klass = g_faceRuntime.objectClass ? g_faceRuntime.objectClass(facial) : nullptr;
			if (g_blinkUnavailableClasses.insert(klass).second)
				g_hostContext.logWarn("[GakuMotion][Face] Blink fields unavailable; facial weights remain playable");
		}
		return ok;
	}

	bool HelperWriteBlink(void* facial, bool useBlink, bool autoDisabled)
	{
		const bool ok = g_faceRuntime.WriteBlink(facial, useBlink, autoDisabled);
		if (!ok && g_hostContext.logWarn)
		{
			void* klass = g_faceRuntime.objectClass ? g_faceRuntime.objectClass(facial) : nullptr;
			if (g_blinkUnavailableClasses.insert(klass).second)
				g_hostContext.logWarn(
					"[GakuMotion][Face] Blink fields cannot be written; native blink configuration retained");
		}
		return ok;
	}

	void HelperBeginFaceBinding()
	{
		g_blinkUnavailableClasses.clear();
		g_highlightUnavailableClasses.clear();
	}

	bool HelperRefreshHighlight(void* actor)
	{
		const bool ok = g_faceRuntime.RefreshHighlight(actor);
		if (!ok && g_hostContext.logWarn)
		{
			void* klass = g_faceRuntime.objectClass ? g_faceRuntime.objectClass(actor) : nullptr;
			if (g_highlightUnavailableClasses.insert(klass).second)
				g_hostContext.logWarn(
					"[GakuMotion][Face] Highlight refresh unavailable or raised a managed exception; native component remains enabled");
		}
		return ok;
	}

	std::string HelperGetActorName(void* actor)
	{
		if (!actor) return "";
		if (g_getGameObjectNative && g_getNameNative)
		{
			void* go = g_getGameObjectNative(actor, nullptr);
			if (go)
			{
				std::string name = Il2CppStringToUtf8(g_getNameNative(go));
				if (!name.empty()) return name;
			}
		}
		if (g_getNameNative)
		{
			std::string name = Il2CppStringToUtf8(g_getNameNative(actor));
			if (!name.empty()) return name;
		}
		return "";
	}

	std::string HelperGetFaceModelName(void* model)
	{
		// VLActorFaceModel is an existing Component. Use the same confirmed
		// Unity Component/Object methods as actor naming, without new hooks.
		if (!model || !g_getGameObjectNative || !g_getNameNative || !g_componentKlass ||
			!g_faceRuntime.objectClass || !g_faceRuntime.parentClass) return "";
		bool component = false;
		void* klass = g_faceRuntime.objectClass(model);
		for (int depth = 0; klass && depth < 64; ++depth, klass = g_faceRuntime.parentClass(klass))
			if (klass == g_componentKlass) { component = true; break; }
		if (!component) return "";
		void* go = g_getGameObjectNative(model, nullptr);
		return go ? Il2CppStringToUtf8(g_getNameNative(go)) : "";
	}

	void ProbeTransformHierarchy(void* rootTransform, const char* label, int depth = 0, int maxDepth = 3)
	{
		if (!rootTransform || depth > maxDepth) return;
		std::string indent(depth * 2, ' ');
		std::string name = "<unknown>";
		if (g_getNameNative)
		{
			name = Il2CppStringToUtf8(g_getNameNative(rootTransform));
		}
		if (g_hostContext.logInfo)
		{
			g_hostContext.logInfo("[GakuMotion][BoneTree][%s] %s- %s (ptr=%p)", label, indent.c_str(), name.c_str(),
			                      rootTransform);
		}
		if (g_getChildCountNative && g_getChildNative)
		{
			int count = g_getChildCountNative(rootTransform);
			for (int i = 0; i < count; ++i)
			{
				void* child = g_getChildNative(rootTransform, i);
				ProbeTransformHierarchy(child, label, depth + 1, maxDepth);
			}
		}
	}

	int HelperGetChildCount(void* transform)
	{
		if (!transform || !g_getChildCountNative) return 0;
		return g_getChildCountNative(transform);
	}

	void* HelperGetChild(void* transform, int index)
	{
		if (!transform || !g_getChildNative) return nullptr;
		return g_getChildNative(transform, index);
	}

	std::string HelperGetTransformName(void* transform)
	{
		if (!transform || !g_getNameNative) return "";
		return Il2CppStringToUtf8(g_getNameNative(transform));
	}

	void HelperGetLocalPos(void* transform, float* outPos)
	{
		if (g_getPosNative && transform && outPos)
		{
			UnityVector3 v = g_getPosNative(transform);
			outPos[0] = v.x;
			outPos[1] = v.y;
			outPos[2] = v.z;
		}
	}

	void HelperSetLocalPos(void* transform, float x, float y, float z)
	{
		if (g_setPosNative && transform)
		{
			UnityVector3 v{x, y, z};
			g_setPosNative(transform, v);
		}
	}

	void HelperGetLocalRot(void* transform, float* outRot)
	{
		if (g_getRotNative && transform && outRot)
		{
			UnityQuaternion q = g_getRotNative(transform);
			outRot[0] = q.x;
			outRot[1] = q.y;
			outRot[2] = q.z;
			outRot[3] = q.w;
		}
	}

	void HelperSetLocalRot(void* transform, float x, float y, float z, float w)
	{
		if (g_setRotNative && transform)
		{
			UnityQuaternion q{x, y, z, w};
			g_setRotNative(transform, q);
		}
	}

	void HelperGetWorldPos(void* transform, float* outPos)
	{
		if (g_getWorldPosNative && transform && outPos)
		{
			UnityVector3 v = g_getWorldPosNative(transform);
			outPos[0] = v.x;
			outPos[1] = v.y;
			outPos[2] = v.z;
		}
	}

	void HelperSetWorldPos(void* transform, float x, float y, float z)
	{
		if (g_setWorldPosNative && transform)
		{
			UnityVector3 v{x, y, z};
			g_setWorldPosNative(transform, v);
		}
	}

	void HelperGetWorldRot(void* transform, float* outRot)
	{
		if (g_getWorldRotNative && transform && outRot)
		{
			UnityQuaternion q = g_getWorldRotNative(transform);
			outRot[0] = q.x;
			outRot[1] = q.y;
			outRot[2] = q.z;
			outRot[3] = q.w;
		}
	}

	void HelperSetWorldRot(void* transform, float x, float y, float z, float w)
	{
		if (g_setWorldRotNative && transform)
		{
			UnityQuaternion q{x, y, z, w};
			g_setWorldRotNative(transform, q);
		}
	}

	void* HelperGetComponentTransform(void* component)
	{
		if (g_getComponentTransformNative && component)
		{
			return g_getComponentTransformNative(component);
		}
		return nullptr;
	}

	void* HelperGetGameObjectTransform(void* go)
	{
		if (g_getGameObjectTransformNative && go)
		{
			return g_getGameObjectTransformNative(go);
		}
		return nullptr;
	}

	void HelperSetAnimatorEnabled(void* animator, bool enabled)
	{
		if (g_setAnimatorEnabledNative && animator)
		{
			g_setAnimatorEnabledNative(animator, enabled);
		}
	}

	bool HelperGetAnimatorEnabled(void* animator)
	{
		return animator && g_getAnimatorEnabledNative && g_getAnimatorEnabledNative(animator);
	}

	void HelperAnimatorRebind(void* animator)
	{
		if (g_animatorRebindNative && animator)
		{
			g_animatorRebindNative(animator);
		}
	}

	void* HelperGetAnimatorController(void* animator)
	{
		if (g_getAnimatorControllerNative && animator)
		{
			return g_getAnimatorControllerNative(animator);
		}
		return nullptr;
	}

	void HelperSetAnimatorController(void* animator, void* controller)
	{
		if (g_setAnimatorControllerNative && animator)
		{
			g_setAnimatorControllerNative(animator, controller);
		}
	}

	void HelperPauseDirector(void* director)
	{
		if (g_pauseDirectorNative && director)
		{
			g_pauseDirectorNative(director);
		}
	}
