namespace GakuMotion::webpanel
{
	inline bool Bridge::ParseSoleSettingsValue(const jsonlite::Value& value, SoleMeasurementSettings& settings, std::string& error)
	{
		if (!value.IsObject()) { error = "settings object is required"; return false; }
		const auto boolField = [&](const char* name, bool& target) {
			const jsonlite::Value* field = value.Find(name);
			if (!field) return true;
			if (!field->IsBool()) { error = std::string(name) + " must be a boolean"; return false; }
			target = field->boolValue; return true;
		};
		const auto floatField = [&](const char* name, float& target) {
			const jsonlite::Value* field = value.Find(name);
			if (!field) return true;
			if (!field->IsNumber() || !std::isfinite(field->numberValue))
			{ error = std::string(name) + " must be a finite number"; return false; }
			target = static_cast<float>(field->numberValue); return true;
		};
		if (!boolField("enabled", settings.enabled) || !boolField("planeCalibrated", settings.planeCalibrated) ||
			!boolField("shoeCalibrated", settings.shoeCalibrated) || !boolField("useBootProxy", settings.useBootProxy) ||
			!floatField("planeOffsetM", settings.planeOffsetM) || !floatField("displayBandM", settings.displayBandM))
			return false;
		if (const jsonlite::Value* normal = value.Find("planeNormal"))
		{
			if (!normal->IsObject()) { error = "planeNormal must be an {x,y,z} object"; return false; }
			double x = 0, y = 0, z = 0;
			if (!NumberParam(*normal, "x", x) || !NumberParam(*normal, "y", y) || !NumberParam(*normal, "z", z))
			{ error = "planeNormal must carry finite x/y/z"; return false; }
			settings.planeNormal = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
		}
		if (const jsonlite::Value* points = value.Find("points"))
		{
			if (!points->IsArray() || points->Size() != 2) { error = "points must be a 2x3 array"; return false; }
			for (size_t side = 0; side < 2; ++side)
			{
				const jsonlite::Value* foot = points->At(side);
				if (!foot || !foot->IsArray() || foot->Size() != 3) { error = "each foot needs three points"; return false; }
				for (size_t i = 0; i < 3; ++i)
				{
					const jsonlite::Value* point = foot->At(i);
					if (!point || !point->IsObject()) { error = "point must be an {x,y,z} object"; return false; }
					double x = 0, y = 0, z = 0;
					if (!NumberParam(*point, "x", x) || !NumberParam(*point, "y", y) || !NumberParam(*point, "z", z))
					{ error = "point must carry finite x/y/z"; return false; }
					settings.points[side][i] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
				}
			}
		}
		if (!settings.IsValid()) { error = "鞋底测量设置无效"; return false; }
		return true;
	}

	inline bool Bridge::ParseStudioSettings(const jsonlite::Value& value, studio::Settings& settings, std::string& error)
	{
		if (!value.IsObject()) { error = "settings object is required"; return false; }
		const auto boolField = [&](const jsonlite::Value& object, const char* name, bool& target) {
			const jsonlite::Value* field = object.Find(name);
			if (!field) return true;
			if (!field->IsBool()) { error = std::string(name) + " must be a boolean"; return false; }
			target = field->boolValue; return true;
		};
		const auto floatField = [&](const jsonlite::Value& object, const char* name, float& target) {
			const jsonlite::Value* field = object.Find(name);
			if (!field) return true;
			if (!field->IsNumber() || !std::isfinite(field->numberValue))
			{ error = std::string(name) + " must be a finite number"; return false; }
			target = static_cast<float>(field->numberValue); return true;
		};
		const auto colorField = [&](const jsonlite::Value& object, const char* name, std::array<float, 3>& target) {
			const jsonlite::Value* field = object.Find(name);
			if (!field) return true;
			if (!field->IsArray() || field->Size() != 3) { error = std::string(name) + " must be an RGB array"; return false; }
			for (size_t i = 0; i < 3; ++i)
			{
				const jsonlite::Value* component = field->At(i);
				if (!component || !component->IsNumber() || !std::isfinite(component->numberValue))
				{ error = std::string(name) + " must contain finite numbers"; return false; }
				target[i] = static_cast<float>(component->numberValue);
			}
			return true;
		};
		if (const jsonlite::Value* lighting = value.Find("lighting"))
		{
			if (!lighting->IsObject()) { error = "lighting must be an object"; return false; }
			auto& l = settings.lighting;
			if (!boolField(*lighting, "enabled", l.enabled) || !boolField(*lighting, "actorStyle", l.actorStyle) ||
				!boolField(*lighting, "decalEnabled", l.decalEnabled) ||
				!colorField(*lighting, "keyColor", l.keyColor) || !colorField(*lighting, "fillColor", l.fillColor) ||
				!colorField(*lighting, "rimColor", l.rimColor) || !colorField(*lighting, "decalTint", l.decalTint))
				return false;
			for (auto [name, target] : {std::pair{"keyIntensity", &l.keyIntensity}, {"fillIntensity", &l.fillIntensity},
				{"rimIntensity", &l.rimIntensity}, {"keyYaw", &l.keyYaw}, {"keyPitch", &l.keyPitch},
				{"fillYaw", &l.fillYaw}, {"fillPitch", &l.fillPitch}, {"rimYaw", &l.rimYaw}, {"rimPitch", &l.rimPitch},
				{"actorIntensity", &l.actorIntensity}, {"rimPower", &l.rimPower}, {"decalIntensityScale", &l.decalIntensityScale}})
				if (!floatField(*lighting, name, *target)) return false;
		}
		if (const jsonlite::Value* video = value.Find("video"))
		{
			if (!video->IsObject()) { error = "video must be an object"; return false; }
			auto& v = settings.video;
			if (!boolField(*video, "enabled", v.enabled) || !boolField(*video, "paused", v.paused) ||
				!boolField(*video, "loop", v.loop)) return false;
			if (const jsonlite::Value* path = video->Find("path"))
			{
				if (!path->IsString()) { error = "video.path must be a string"; return false; }
				v.path = path->stringValue;
			}
			if (!floatField(*video, "distance", v.distance) || !floatField(*video, "width", v.width) ||
				!floatField(*video, "offsetX", v.offsetX) || !floatField(*video, "offsetY", v.offsetY))
				return false;
		}
		return settings.Validate(error);
	}

	inline bool Bridge::TriStateParam(const jsonlite::Value& params, const char* name, int& target)
	{
		const jsonlite::Value* value = params.Find(name);
		if (!value) return true;
		if (value->IsNull()) { target = -1; return true; }
		if (value->IsBool()) { target = value->boolValue ? 1 : 0; return true; }
		if (value->IsNumber() && value->numberValue == std::floor(value->numberValue) &&
			value->numberValue >= -1 && value->numberValue <= 1)
		{ target = static_cast<int>(value->numberValue); return true; }
		return false;
	}

	inline bool Bridge::ParseCameraSettings(const jsonlite::Value& value, CameraSettings& settings, std::string& error)
	{
		const auto boolField = [&](const char* name, bool& target) {
			const jsonlite::Value* field = value.Find(name);
			if (!field) return true;
			if (!field->IsBool()) { error = std::string(name) + " must be a boolean"; return false; }
			target = field->boolValue;
			return true;
		};
		const auto intField = [&](const char* name, int& target, int low, int high) {
			const jsonlite::Value* field = value.Find(name);
			if (!field) return true;
			if (!field->IsNumber() || field->numberValue != std::floor(field->numberValue) ||
				field->numberValue < low || field->numberValue > high)
			{ error = std::string(name) + " is out of range"; return false; }
			target = static_cast<int>(field->numberValue);
			return true;
		};
		const auto floatField = [&](const char* name, float& target) {
			const jsonlite::Value* field = value.Find(name);
			if (!field) return true;
			if (!field->IsNumber() || !std::isfinite(field->numberValue))
			{ error = std::string(name) + " must be a finite number"; return false; }
			target = static_cast<float>(field->numberValue);
			return true;
		};
		const auto vecField = [&](const char* name, Vector3& target) {
			const jsonlite::Value* field = value.Find(name);
			if (!field) return true;
			if (!field->IsObject()) { error = std::string(name) + " must be an {x,y,z} object"; return false; }
			double x = 0, y = 0, z = 0;
			if (!NumberParam(*field, "x", x) || !NumberParam(*field, "y", y) || !NumberParam(*field, "z", z))
			{ error = std::string(name) + " must carry finite x/y/z"; return false; }
			target = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
			return true;
		};
		const auto transformField = [&](const char* name, CameraModeTransform& target) {
			const jsonlite::Value* field = value.Find(name);
			if (!field) return true;
			if (!field->IsObject()) { error = std::string(name) + " must be an object"; return false; }
			const jsonlite::Value* position = field->Find("position");
			const jsonlite::Value* rotation = field->Find("rotation");
			if (!position || !rotation) { error = std::string(name) + " needs position and rotation"; return false; }
			const auto parseVec = [&](const jsonlite::Value* object, Vector3& out, const char* label) {
				double x = 0, y = 0, z = 0;
				if (!object->IsObject() || !NumberParam(*object, "x", x) ||
					!NumberParam(*object, "y", y) || !NumberParam(*object, "z", z))
				{ error = std::string(name) + label + " must carry finite x/y/z"; return false; }
				out = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
				return true;
			};
			if (!parseVec(position, target.position, ".position") ||
				!parseVec(rotation, target.rotation, ".rotation"))
				return false;
			return true;
		};
		int mode = static_cast<int>(settings.mode);
		int followBehavior = static_cast<int>(settings.followBehavior);
		int followTarget = static_cast<int>(settings.followTarget);
		if (!boolField("enabled", settings.enabled) ||
			!intField("mode", mode, 0, 2) ||
			!intField("followBehavior", followBehavior, 0, 1) ||
			!intField("followTarget", followTarget, 0, 2) ||
			!intField("followSlotIndex", settings.followSlotIndex, -1, 1024) ||
			!boolField("followActor", settings.followActor) ||
			!boolField("limitToScene", settings.limitToScene) ||
			!vecField("boundsMin", settings.boundsMin) ||
			!vecField("boundsMax", settings.boundsMax) ||
			!vecField("followOffset", settings.followOffset) ||
			!vecField("targetOffset", settings.targetOffset) ||
			!vecField("vmdOffset", settings.vmdOffset) ||
			!floatField("vmdRangeScale", settings.vmdRangeScale) ||
			!vecField("followFactor", settings.followFactor) ||
			!vecField("followRate", settings.followRate) ||
			!floatField("followFov", settings.followFov) ||
			!boolField("srpFinalOverwriteFallback", settings.srpFinalOverwriteFallback) ||
			!intField("actorShadowExperimentMode", settings.actorShadowExperimentMode, 0, 2) ||
			!transformField("transformTrack", settings.transformTrack) ||
			!transformField("transformFollowMove", settings.transformFollowMove) ||
			!transformField("transformLookAt", settings.transformLookAt) ||
			!boolField("usePerModeTransforms", settings.usePerModeTransforms))
			return false;
		settings.mode = static_cast<CameraMode>(mode);
		settings.followBehavior = static_cast<FollowBehavior>(followBehavior);
		settings.followTarget = static_cast<CameraFollowTarget>(followTarget);
		return true;
	}

	inline bool Bridge::ParseStageSettings(const jsonlite::Value& value, StageSettings& settings, std::string& error)
	{
		const auto boolField = [&](const char* name, bool& target) {
			const jsonlite::Value* field = value.Find(name);
			if (!field) return true;
			if (!field->IsBool()) { error = std::string(name) + " must be a boolean"; return false; }
			target = field->boolValue;
			return true;
		};
		const auto floatField = [&](const char* name, float& target) {
			const jsonlite::Value* field = value.Find(name);
			if (!field) return true;
			if (!field->IsNumber() || !std::isfinite(field->numberValue))
			{ error = std::string(name) + " must be a finite number"; return false; }
			target = static_cast<float>(field->numberValue);
			return true;
		};
		const jsonlite::Value* color = value.Find("greenScreenColor");
		if (color)
		{
			if (!color->IsArray() || color->Size() != 4)
			{ error = "greenScreenColor must be an RGBA array of 4 numbers"; return false; }
			for (size_t i = 0; i < 4; ++i)
			{
				const jsonlite::Value* component = color->At(i);
				if (!component || !component->IsNumber() || !std::isfinite(component->numberValue) ||
					component->numberValue < 0 || component->numberValue > 1)
				{ error = "greenScreenColor components must be within 0..1"; return false; }
				settings.greenScreenColor[i] = static_cast<float>(component->numberValue);
			}
		}
		const jsonlite::Value* preset = value.Find("selectedPresetIndex");
		if (preset)
		{
			if (!preset->IsNumber() || preset->numberValue != std::floor(preset->numberValue) ||
				preset->numberValue < 0 || preset->numberValue > 64)
			{ error = "selectedPresetIndex is out of range"; return false; }
			settings.selectedPresetIndex = static_cast<int>(preset->numberValue);
		}
		const auto stringField = [&](const char* name, std::string& target) {
			const jsonlite::Value* field = value.Find(name);
			if (!field) return true;
			if (!field->IsString() || field->stringValue.size() > 512 ||
				field->stringValue.find('\0') != std::string::npos)
			{ error = std::string(name) + " must be a short string"; return false; }
			target = field->stringValue;
			return true;
		};
		return boolField("hideBackground", settings.hideBackground) &&
			boolField("greenScreenActive", settings.greenScreenActive) &&
			floatField("screenDistance", settings.screenDistance) &&
			floatField("screenScale", settings.screenScale) &&
			floatField("screenHeightOffset", settings.screenHeightOffset) &&
			boolField("overrideCameraClear", settings.overrideCameraClear) &&
			stringField("customStageKey", settings.customStageKey) &&
			floatField("stageFloorYOffset", settings.stageFloorYOffset) &&
			floatField("stageYawOffset", settings.stageYawOffset) &&
			floatField("stagePosXOffset", settings.stagePosXOffset) &&
			floatField("stagePosZOffset", settings.stagePosZOffset) &&
			boolField("lightingEffectsEnabled", settings.lightingEffectsEnabled) &&
			boolField("customNodeControl", settings.customNodeControl);
	}
} // namespace GakuMotion::webpanel
