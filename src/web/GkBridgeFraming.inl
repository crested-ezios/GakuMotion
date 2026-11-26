namespace GakuMotion::webpanel
{
	inline GakuStatus Bridge::GetAssets(GakuWebPanelAssetsV1* output)
	{
		if (!output) return GAKU_E_INVALID_ARGUMENT;
		if (closed_.load(std::memory_order_acquire)) return GAKU_E_SHUTTING_DOWN;
		if (!hooks_.assetsDirs) return GAKU_E_UNAVAILABLE;
		for (const std::wstring& dir : hooks_.assetsDirs())
		{
			if (dir.empty()) continue;
			std::error_code ec;
			const std::filesystem::path entry = std::filesystem::path(dir) / L"index.html";
			if (!std::filesystem::is_regular_file(entry, ec) || ec) continue;
			assetsDirCache_ = dir;
			output->abiVersion = GAKU_WEB_PANEL_VERSION;
			output->structSize = sizeof(GakuWebPanelAssetsV1);
			output->assetsDir = assetsDirCache_.c_str();
			output->entryPath = L"index.html";
			output->virtualHost = L"gakumotion.invalid";
			return GAKU_OK;
		}
		return GAKU_E_UNAVAILABLE;
	}

	inline void Bridge::OnShow()
	{
		visible_ = true;
		baselinePending_ = true;
	}

	inline void Bridge::SetEventSink(GakuWebPanelEventSink sink, void* userData)
	{
		std::lock_guard<std::mutex> lock(sinkMutex_);
		sink_ = sink;
		sinkUserData_ = userData;
		if (sink) baselinePending_ = true;
	}

	inline GakuStatus Bridge::HandleMessage(const char* requestJson, uint32_t responseCapacity, char* responseJson, uint32_t* requiredBytes)
	{
		if (!requestJson) return GAKU_E_INVALID_ARGUMENT;
		std::string response;
		try { response = DispatchRequest(requestJson); }
		catch (...) { response = MakeError(0, "internal", "Unhandled bridge exception"); }
		const uint64_t needed = static_cast<uint64_t>(response.size()) + 1;
		if (needed > 0xFFFFFFFFull) return GAKU_E_LIMIT;
		if (requiredBytes) *requiredBytes = static_cast<uint32_t>(needed);
		if (!responseJson || responseCapacity < needed) return GAKU_E_SIZE;
		std::memcpy(responseJson, response.c_str(), static_cast<size_t>(needed));
		return GAKU_OK;
	}

	inline void Bridge::OnUiTick()
	{
		if (closed_.load(std::memory_order_acquire)) return;
		try { Tick(); }
		catch (...) { /* a diagnostic tick must never break the host UI */ }
	}

	inline void Bridge::OnShutdown()
	{
		if (closed_.load(std::memory_order_acquire)) return;
		Emit("bridge.closed", "{\"reason\":\"service-shutdown\"}");
		if (dialogPending_ && hooks_.releaseFileDialog) hooks_.releaseFileDialog();
		dialogPending_ = false;
		visible_ = false;
		dropEnabled_ = false;
		if (hooks_.dropSetTarget && lastDropTargetSent_)
		{
			std::string ignored;
			hooks_.dropSetTarget(false, ignored);
		}
		lastDropTargetSent_ = false;
		cachedDropPeek_ = {};
		pendingDrops_.clear();
		{
			std::lock_guard<std::mutex> lock(sinkMutex_);
			sink_ = nullptr;
			sinkUserData_ = nullptr;
			sessionId_ = NextSessionId();
			seq_ = 0;
		}
		ResetChangeTracking();
	}

	inline void Bridge::Invalidate(const char* /*reason*/)
	{
		closed_.store(true, std::memory_order_release);
		dialogPending_ = false;
		if (hooks_.releaseFileDialog) hooks_.releaseFileDialog();
		std::lock_guard<std::mutex> lock(sinkMutex_);
		sink_ = nullptr;
		sinkUserData_ = nullptr;
	}

	inline std::string Bridge::MakeOk(uint64_t id, const std::string& result)
	{
		return "{\"v\":1,\"type\":\"response\",\"id\":" + JNum(static_cast<double>(id)) +
			",\"ok\":true,\"result\":" + result + "}";
	}

	inline std::string Bridge::MakeError(uint64_t id, const std::string& code, const std::string& message, const std::string& details)
	{
		std::string error = "{\"code\":" + JString(code) + ",\"message\":" + JString(message);
		if (!details.empty()) error += ",\"details\":" + details;
		error += "}";
		return "{\"v\":1,\"type\":\"response\",\"id\":" + JNum(static_cast<double>(id)) +
			",\"ok\":false,\"error\":" + error + "}";
	}

	inline void Bridge::Emit(const std::string& name, const std::string& payloadJson)
	{
		std::lock_guard<std::mutex> lock(sinkMutex_);
		if (!sink_ || closed_.load(std::memory_order_acquire)) return;
		const std::string json = "{\"v\":1,\"type\":\"event\",\"name\":" + JString(name) +
			",\"payload\":" + payloadJson + ",\"seq\":" + JNum(static_cast<double>(++seq_)) +
			",\"sid\":" + JNum(static_cast<double>(sessionId_)) + "}";
		sink_(sinkUserData_, json.c_str());
	}

	inline std::string Bridge::DispatchRequest(const char* requestJson)
	{
		jsonlite::Value root;
		std::string parseError;
		if (!jsonlite::ParseText(requestJson, root, parseError) || !root.IsObject())
			return MakeError(0, "invalid_params", "Malformed request JSON");
		const jsonlite::Value* version = root.Find("v");
		const jsonlite::Value* type = root.Find("type");
		const jsonlite::Value* idValue = root.Find("id");
		const jsonlite::Value* methodValue = root.Find("method");
		uint64_t id = 0;
		if (idValue && idValue->IsNumber() && idValue->numberValue >= 0)
			id = static_cast<uint64_t>(idValue->numberValue);
		if (!version || !version->IsNumber() || version->numberValue != 1 ||
			!type || !type->IsString() || type->stringValue != "request" ||
			!idValue || !idValue->IsNumber() ||
			!methodValue || !methodValue->IsString() || methodValue->stringValue.empty())
			return MakeError(id, "invalid_params", "Request must carry v=1, type=request, numeric id and method");
		if (closed_.load(std::memory_order_acquire))
			return MakeError(id, "not_allowed", "Bridge is closed");
		const jsonlite::Value* params = root.Find("params");
		if (params && !params->IsObject() && !params->IsNull())
			return MakeError(id, "invalid_params", "params must be an object");
		static const jsonlite::Value emptyParams{[] { jsonlite::Value v; v.type = jsonlite::Type::Object; return v; }()};
		const std::string& method = methodValue->stringValue;
		CmdResult result;
		try { result = Dispatch(method, params ? *params : emptyParams); }
		catch (...) { result = CmdResult::Err("internal", "Unhandled method exception"); }
		if (result.ok)
		{
			if (IsSlowDomainMutatingMethod(method))
				forceDomainRefresh_.store(true, std::memory_order_release);
			return MakeOk(id, result.body);
		}
		return MakeError(id, result.code, result.message, result.details);
	}

	inline const jsonlite::Value* Bridge::Param(const jsonlite::Value& params, const char* name)
	{
		return params.Find(name);
	}

	inline bool Bridge::NumberParam(const jsonlite::Value& params, const char* name, double& out)
	{
		const jsonlite::Value* value = params.Find(name);
		if (!value || !value->IsNumber() || !std::isfinite(value->numberValue)) return false;
		out = value->numberValue;
		return true;
	}

	inline bool Bridge::IntParam(const jsonlite::Value& params, const char* name, int64_t& out)
	{
		double number = 0;
		if (!NumberParam(params, name, number) || number != std::floor(number)) return false;
		out = static_cast<int64_t>(number);
		return true;
	}

	inline bool Bridge::BoolParam(const jsonlite::Value& params, const char* name, bool& out)
	{
		const jsonlite::Value* value = params.Find(name);
		if (!value || !value->IsBool()) return false;
		out = value->boolValue;
		return true;
	}

	inline bool Bridge::StringParam(const jsonlite::Value& params, const char* name, std::string& out)
	{
		const jsonlite::Value* value = params.Find(name);
		if (!value || !value->IsString()) return false;
		out = value->stringValue;
		return true;
	}

	inline bool Bridge::VecParam(const jsonlite::Value& params, const char* name, Vector3& out)
	{
		const jsonlite::Value* value = params.Find(name);
		if (!value || !value->IsObject()) return false;
		double x = 0, y = 0, z = 0;
		if (!NumberParam(*value, "x", x) || !NumberParam(*value, "y", y) || !NumberParam(*value, "z", z))
			return false;
		out = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
		return true;
	}
} // namespace GakuMotion::webpanel
