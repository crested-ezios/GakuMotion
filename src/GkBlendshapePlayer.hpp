#pragma once
#include "GkCharacterNames.hpp"
#include "GkMotionData.hpp"
#include "GakuFaceDiagnostics.h"
#include "GkFacialAdapter.hpp"
#include "GkMikuFacialFit.hpp"
#include "GkRestoreChecks.hpp"
#include <chrono>
#include <cstring>
#include <limits>

namespace GakuMotion
{
	// gkclip stores unit weights; recorded VLActorFaceModel weights use percentages.
	// All calls are made by IdolController on Unity's main thread.
	struct FaceApi
	{
		void* (*getModel)(void* actor){nullptr};
		int (*getCount)(void* model){nullptr};
		std::string (*getName)(void* model, int index){nullptr};
		float (*getWeight)(void* model, int index){nullptr};
		void (*setWeight)(void* model, int index, float weight){nullptr};
		uint64_t (*getTicks)(){nullptr}; // QPC on the Windows host, optional in tests.
		int (*getUnityFrame)(){nullptr};
		void* (*getFacial)(void* actor){nullptr};
		bool (*readBlink)(void* facial, bool& useBlink, bool& autoDisabled){nullptr};
		bool (*writeBlink)(void* facial, bool useBlink, bool autoDisabled){nullptr};
		bool (*refreshHighlight)(void* actor){nullptr};
		void (*beginBinding)(){nullptr}; // Reset one-time runtime notices on rebind.
		std::string (*getActorName)(void* actor){nullptr};
		void* (*getTalk)(void* actor, int slot){nullptr};
		bool (*readTalk)(void* talk, bool& enabled){nullptr};
		bool (*writeTalk)(void* talk, bool enabled){nullptr};
		std::string (*getModelName)(void* model){nullptr}; // Existing Unity Object identity, bind-time only.
		void (*reportFacialFit)(const char* component, const char* decision, const char* faceName){nullptr};

		bool Ready() const { return getModel && getCount && getName && getWeight && setWeight; }
	};

	// Separate from morph ownership: even a body-only clip owns native talk
	// while its pose is loaded. Resolve through the live actor before restoring.
	struct NativeTalkBinding
	{
		void* talk{nullptr};
		bool captured{false}, savedEnabled{false}, applied{false};
		void Restore(void* actor, int slot, const FaceApi& api)
		{
			bool enabled = true;
			if (applied && api.getTalk && api.readTalk && api.writeTalk &&
				api.getTalk(actor, slot) == talk && api.readTalk(talk, enabled) && !enabled)
				api.writeTalk(talk, savedEnabled);
			*this = {};
		}
		CaptureRestoreStatus RestoreForCapture(void* actor, int slot, const FaceApi& api)
		{
			CaptureRestoreStatus result{applied, true};
			if (!applied) return result;
			const auto previous = *this;
			Restore(actor, slot, api);
			bool enabled = !previous.savedEnabled;
			result.confirmed = previous.captured && api.getTalk && api.readTalk &&
				api.getTalk(actor, slot) == previous.talk && api.readTalk(previous.talk, enabled) &&
				enabled == previous.savedEnabled;
			if (!result.confirmed) *this = previous;
			return result;
		}
		void Suppress(void* actor, int slot, const FaceApi& api)
		{
			if (!api.getTalk || !api.readTalk || !api.writeTalk) return;
			void* current = api.getTalk(actor, slot);
			if (current != talk) { Restore(actor, slot, api); talk = current; }
			if (!talk) return;
			if (!captured) captured = api.readTalk(talk, savedEnabled);
			if (captured && api.writeTalk(talk, false)) applied = true;
		}
	};

	struct BlendshapeBinding
	{
		void* model{nullptr};
		std::vector<std::string> names;
		std::vector<int> indices;
		// A curve replaces its expression family. Missing sibling curves take
		// their character default prior or zero, not "keep the
		// photography pose's smile / frown / viseme".
		std::vector<int> ownedIndices, ownedClipSlots;
		std::vector<std::string> ownedNames;
		std::vector<float> ownedSavedWeights;
		bool written{false};
		bool ownsEyes{false};
		void* facial{nullptr};
		bool blinkCaptured{false}, blinkSuppressed{false};
		bool savedUseBlink{false}, savedAutoDisabled{false};
		bool enableBaseline{true};
		float baselineMultiplier{1.0f};
		FacialWeights facialWeights;
		std::string characterOverride;
		std::string boundCharacterOverride;
		std::string detectedCharacter;
		bool enableMikuMouthFit{false}, boundMikuMouthFit{false};
		bool enableMikuEyeFit{false}, boundMikuEyeFit{false};
		bool facialFitAllowUnverified{false}, boundFacialFitAllowUnverified{false};
		// The provider registers after this plugin activates on a real install,
		// so a binding made before that must notice the later appearance.
		unsigned boundFacialFitGeneration{0};
		const MotionClip* boundClip{nullptr};
		std::string boundFaceName;
		MikuFitDecision mikuFitDecision{MikuFitDecision::Disabled};
		MikuFitDecision mikuEyeDecision{MikuFitDecision::Disabled};
		uint32_t mikuReportedDecisions{0}; // Each distinct decision once per binding, never per-frame spam.
		uint32_t mikuEyeReportedDecisions{0};
		// 1..N = compiled per-character table, N+1 = published profile, 0 = none.
		unsigned fitRole{0};
		ExternalFacialFit externalFit;
		const CharacterBaselineProfile* activeProfile{nullptr};
		std::chrono::steady_clock::time_point observerUntil{};
		uint64_t diagnosticSequence{0}, diagnosticTicks{0};
		uint32_t diagnosticStatus{GAKU_FACE_PENDING};
		float diagnosticTime{0};
		int diagnosticUnityFrame{-1};
		void* diagnosticModel{nullptr};
		std::vector<GakuFaceChannelDiagnostic> diagnosticChannels;

		void Observe() { observerUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2); }

		static float NativeWeight(float unitWeight) { return unitWeight * 100.0f; }

		static unsigned ExpressionFamily(const std::string& name)
		{
			return FacialFamily(name); // Iris/gaze and corrections stay native.
		}

		void RestoreBlink(void* actor, const FaceApi& api)
		{
			// As with FaceModel, never restore a component replaced by a costume.
			if (blinkSuppressed && api.getFacial && api.writeBlink && api.getFacial(actor) == facial)
				api.writeBlink(facial, savedUseBlink, savedAutoDisabled);
			facial = nullptr;
			blinkCaptured = blinkSuppressed = false;
		}

		void CaptureBlink(void* actor, const FaceApi& api)
		{
			if (!ownsEyes || !api.getFacial || !api.readBlink || !api.writeBlink) return;
			void* current = api.getFacial(actor);
			if (current != facial)
			{
				RestoreBlink(actor, api);
				facial = current;
			}
			if (!facial) return;
			if (!blinkCaptured)
				blinkCaptured = api.readBlink(facial, savedUseBlink, savedAutoDisabled);
		}

		void SuppressBlink(void* actor, const FaceApi& api)
		{
			CaptureBlink(actor, api);
			if (blinkCaptured && api.writeBlink && api.writeBlink(facial, false, true)) blinkSuppressed = true;
		}

		void Restore(void* actor, const FaceApi& api, bool* highlightResult = nullptr)
		{
			if (highlightResult) *highlightResult = true;
			// A costume/scene may destroy the previous model. Never write through
			// a stale model pointer; restore only when the actor still owns it.
			const bool liveModel = written && api.Ready() && api.getModel(actor) == model;
			if (liveModel)
			{
				for (size_t i = 0; i < ownedIndices.size(); ++i)
					api.setWeight(model, ownedIndices[i], ownedSavedWeights[i]);
			}
			RestoreBlink(actor, api);
			if (liveModel && ownsEyes)
			{
				const bool refreshed = api.refreshHighlight && api.refreshHighlight(actor);
				if (highlightResult) *highlightResult = refreshed;
			}
			const bool keepEnableBaseline = enableBaseline;
			const float keepMultiplier = baselineMultiplier;
			const auto keepFacialWeights = facialWeights;
			const std::string keepOverride = characterOverride;
			const bool keepMikuFit = enableMikuMouthFit;
			const bool keepMikuEyeFit = enableMikuEyeFit;
			const bool keepAllowUnverified = facialFitAllowUnverified;
			*this = BlendshapeBinding{};
			enableBaseline = keepEnableBaseline;
			baselineMultiplier = keepMultiplier;
			facialWeights = keepFacialWeights;
			characterOverride = keepOverride;
			enableMikuMouthFit = keepMikuFit;
			enableMikuEyeFit = keepMikuEyeFit;
			facialFitAllowUnverified = keepAllowUnverified;
		}

		CaptureRestoreStatus RestoreForCapture(void* actor, const FaceApi& api)
		{
			CaptureRestoreStatus result{written || blinkSuppressed, true};
			if (!result.modified) return result;
			const auto previous = *this;
			if (previous.written)
			{
				if (!api.Ready() || api.getModel(actor) != previous.model ||
					previous.ownedIndices.size() != previous.ownedSavedWeights.size() ||
					previous.ownedIndices.size() != previous.ownedNames.size()) return {true, false};
				const int count = api.getCount(previous.model);
				for (size_t i = 0; i < previous.ownedIndices.size(); ++i)
					if (previous.ownedIndices[i] < 0 || previous.ownedIndices[i] >= count ||
						api.getName(previous.model, previous.ownedIndices[i]) != previous.ownedNames[i]) return {true, false};
			}
			bool highlightRestored = true;
			Restore(actor, api, &highlightRestored);
			if (previous.written)
			{
				result.confirmed = api.Ready() && api.getModel(actor) == previous.model &&
					previous.ownedIndices.size() == previous.ownedSavedWeights.size();
				if (result.confirmed)
				{
					const int count = api.getCount(previous.model);
					for (size_t i = 0; i < previous.ownedIndices.size(); ++i)
					{
						const int index = previous.ownedIndices[i];
						if (index < 0 || index >= count) { result.confirmed = false; continue; }
						const float actual = api.getWeight(previous.model, index);
						const float expected = previous.ownedSavedWeights[i];
						if (!std::isfinite(actual) || !std::isfinite(expected) || std::fabs(actual - expected) > 1e-3f)
							result.confirmed = false;
					}
				}
			}
			if (previous.blinkSuppressed)
			{
				bool useBlink = !previous.savedUseBlink, disabled = !previous.savedAutoDisabled;
				if (!api.getFacial || !api.readBlink || api.getFacial(actor) != previous.facial ||
					!api.readBlink(previous.facial, useBlink, disabled) || useBlink != previous.savedUseBlink ||
					disabled != previous.savedAutoDisabled) result.confirmed = false;
			}
			// Highlight refresh is a real mutation with no readable acknowledgement
			// beyond its existing success callback. Require that callback when owned.
			if (!highlightRestored) result.confirmed = false;
			if (!result.confirmed) *this = previous;
			return result;
		}

		// Capture the original face BEFORE Animator detach/disable/Rebind. New
		// bindings only read; identity changes first release any existing ownership.
		// Reused after Stop even when the bone binding itself remains valid.
		void Prepare(void* actor, const MotionClip& clip, const FaceApi& api)
		{
			if (!api.Ready()) return;
			facial_fit::Ensure();
			void* current = api.getModel(actor);
			if (current != model || boundClip != &clip || names != clip.blendshape_names || boundCharacterOverride != characterOverride ||
				boundMikuMouthFit != enableMikuMouthFit || boundMikuEyeFit != enableMikuEyeFit ||
				boundFacialFitAllowUnverified != facialFitAllowUnverified ||
				boundFacialFitGeneration != facial_fit::generation)
			{
				// Preserve this observer frame across the binding reset. Stop() still
				// clears it, so an old write cannot masquerade as active playback.
				auto lease = observerUntil;
				auto diagnostic = std::move(diagnosticChannels);
				auto sequence = diagnosticSequence;
				auto ticks = diagnosticTicks;
				auto status = diagnosticStatus;
				auto unityFrame = diagnosticUnityFrame;
				auto clipTime = diagnosticTime;
				Restore(actor, api);
				observerUntil = lease;
				diagnosticChannels = std::move(diagnostic);
				diagnosticSequence = sequence;
				diagnosticTicks = ticks;
				diagnosticStatus = status;
				diagnosticTime = clipTime;
				diagnosticUnityFrame = unityFrame;
				diagnosticModel = current;
				if (!current || clip.blendshape_names.empty()) return;
				const int count = api.getCount(current);
				if (count <= 0 || count > 1024) return;
				model = current;
				boundClip = &clip;
				names = clip.blendshape_names;
				boundCharacterOverride = characterOverride;
				boundMikuMouthFit = enableMikuMouthFit;
				boundMikuEyeFit = enableMikuEyeFit;
				boundFacialFitAllowUnverified = facialFitAllowUnverified;
				boundFacialFitGeneration = facial_fit::generation;
				boundFaceName = api.getModelName ? api.getModelName(model) : "";
				indices.assign(names.size(), -1);
				std::vector<std::string> modelNames;
				unsigned families = 0;

				std::string charId = characterOverride;
				if (charId.empty() || charId == "auto")
				{
					// A replacement can retain the scene actor's original name. Adapt
					// the loaded face first, as the scene-role menu does.
					const auto identity = facial_fit::ResolveIdentity(boundFaceName);
					charId = identity.character;
					if (charId.empty()) charId = DetectIdolCharacterCode(boundFaceName);
					if (charId.empty() && api.getActorName) charId = DetectIdolCharacterCode(api.getActorName(actor));
					detectedCharacter = charId;
				}
				else detectedCharacter = DetectIdolCharacterCode(charId);
				activeProfile = FindCharacterBaseline(charId);

				// Name matching is per actor, per face model. File order and game
				// blendshape indices are intentionally unrelated.
				for (int j = 0; j < count; ++j)
				{
					const std::string name = api.getName(model, j);
					modelNames.push_back(name);
					auto found = std::find(names.begin(), names.end(), name);
					if (found == names.end()) continue;
					size_t i = static_cast<size_t>(found - names.begin());
					indices[i] = j;
					ownsEyes = ownsEyes || name.rfind("b_eye.", 0) == 0;
					families |= ExpressionFamily(name);
				}
				for (const auto& name : names)
				{
					const auto* semantic = FacialSemantic(name);
					if (!semantic) continue;
					for (const char* candidate : semantic->candidates)
					{
						if (!candidate || std::find(modelNames.begin(), modelNames.end(), candidate) == modelNames.end()) continue;
						families |= semantic->family;
						ownsEyes = ownsEyes || semantic->family == 2;
						break;
					}
				}
				// Raw-source names are metadata, not native indices. A verified
				// profile still owns the entire mouth family so omitted siblings
				// cannot add the photography smile/default to fitted or zero output.
				if (enableMikuMouthFit && HasMikuRawSource(names))
				{
					const auto* fitted = FindMikuMouthFace(boundFaceName);
					bool complete = fitted != nullptr;
					if (fitted) for (const char* name : fitted->shapes)
						complete = complete && std::count(modelNames.begin(), modelNames.end(), name) == 1;
					if (complete) families |= 4;
				}
				// Claim both parts of an applicable eye/brow fit, including keys
				// that have no legacy mapped counterpart in the clip.
				externalFit = BindExternalFacialFit(boundFaceName, names, modelNames,
					clip.blendshape_weights, facialFitAllowUnverified);
				if (externalFit.face && (characterOverride.empty() || characterOverride == "auto" ||
					characterOverride == externalFit.character))
				{
					if (enableMikuMouthFit && externalFit.mouthClaim) families |= 4;
					if (enableMikuEyeFit && externalFit.eyeClaim) { families |= 3; ownsEyes = true; }
				}
				for (int j = 0; j < count; ++j)
				{
					const auto found = std::find(names.begin(), names.end(), modelNames[j]);
					const int slot = found == names.end() ? -1 : static_cast<int>(found - names.begin());
					if (slot < 0 && !(families & ExpressionFamily(modelNames[j]))) continue;
					ownedIndices.push_back(j);
					ownedClipSlots.push_back(slot);
					ownedNames.push_back(modelNames[j]);
					const float saved = api.getWeight(model, j);
					ownedSavedWeights.push_back(std::isfinite(saved) ? saved : 0.0f);
				}
				ResolveExternalFitSlots(externalFit, ownedNames);
				fitRole = 0;
				if (const auto* compiled = FindMikuMouthFace(boundFaceName))
					fitRole = static_cast<unsigned>(compiled - kMikuMouthFits.data()) + 1;
				else if (externalFit.face)
					fitRole = static_cast<unsigned>(kMikuMouthFits.size()) + 1;
				if (api.beginBinding) api.beginBinding();
			}
			if (model) CaptureBlink(actor, api);
		}

		void Apply(void* actor, const MotionClip& clip, float time, const FaceApi& api)
		{
			mikuFitDecision = !enableMikuMouthFit ? MikuFitDecision::Disabled
				: HasMikuRawSource(clip.blendshape_names) ? MikuFitDecision::UnknownFace : MikuFitDecision::MissingRawSource;
			mikuEyeDecision = !enableMikuEyeFit ? MikuFitDecision::Disabled
				: HasMikuRawEyeSource(clip.blendshape_names) ? MikuFitDecision::UnknownFace : MikuFitDecision::MissingRawEyeSource;
			const bool observe = std::chrono::steady_clock::now() < observerUntil;
			if (observe)
			{
				++diagnosticSequence;
				diagnosticTicks = api.getTicks ? api.getTicks() : 0;
				diagnosticTime = time;
				diagnosticUnityFrame = api.getUnityFrame ? api.getUnityFrame() : -1;
				diagnosticModel = nullptr;
				diagnosticStatus = GAKU_FACE_API_MISSING;
				diagnosticChannels.clear();
				std::vector<float> target;
				clip.SampleBlendshapes(time, target);
				for (size_t i = 0; i < target.size() && i < GAKU_FACE_DIAGNOSTICS_CAPACITY; ++i)
				{
					GakuFaceChannelDiagnostic d{};
					std::memcpy(d.name, clip.blendshape_names[i].data(),
					            std::min(clip.blendshape_names[i].size(), size_t(GAKU_FACE_NAME_BYTES - 1)));
					d.index = -1;
					d.target = NativeWeight(target[i]);
					d.before = d.after = std::numeric_limits<float>::quiet_NaN();
					diagnosticChannels.push_back(d);
				}
			}
			if (!api.Ready()) return;
			void* current = api.getModel(actor);
			if (observe)
			{
				diagnosticModel = current;
				diagnosticStatus = current ? GAKU_FACE_COUNT_INVALID : GAKU_FACE_MODEL_MISSING;
			}
			Prepare(actor, clip, api);
			if (!model) return;
			SuppressBlink(actor, api);
			if (observe) diagnosticStatus = GAKU_FACE_BOUND;
			std::vector<float> weights;
			clip.SampleBlendshapes(time, weights);
			auto adapted = EvaluateFacialAdapter(clip.blendshape_names, weights, ownedNames,
			                                         activeProfile, enableBaseline, baselineMultiplier);
			mikuFitDecision = enableMikuMouthFit
				? (externalFit.mouthPresent
					? TryApplyExternalMouthFit(externalFit, clip.blendshape_names, weights, characterOverride,
						facialFitAllowUnverified, ownedNames, adapted)
					: TryApplyMikuMouthFit(clip.blendshape_names, weights, ownedNames, boundFaceName, characterOverride, adapted))
				: MikuFitDecision::Disabled;
			mikuEyeDecision = enableMikuEyeFit
				? (externalFit.eyesPresent
					? TryApplyExternalEyeFit(externalFit, clip.blendshape_names, weights, characterOverride,
						ownedNames, adapted)
					: MikuFitDecision::UnknownFace)
				: MikuFitDecision::Disabled;
			const uint32_t decisionBit = 1u << static_cast<unsigned>(mikuFitDecision);
			if (enableMikuMouthFit && api.reportFacialFit && !(mikuReportedDecisions & decisionBit))
			{
				api.reportFacialFit("mouth", MikuFitDecisionName(mikuFitDecision), boundFaceName.c_str());
				mikuReportedDecisions |= decisionBit;
			}
			const uint32_t eyeDecisionBit = 1u << static_cast<unsigned>(mikuEyeDecision);
			if (enableMikuEyeFit && api.reportFacialFit && !(mikuEyeReportedDecisions & eyeDecisionBit))
			{
				api.reportFacialFit("eyes", MikuFitDecisionName(mikuEyeDecision), boundFaceName.c_str());
				mikuEyeReportedDecisions |= eyeDecisionBit;
			}

			for (size_t i = 0; i < ownedIndices.size(); ++i)
			{
				const int slot = ownedClipSlots[i];
				const float target = NativeWeight(facialWeights.Apply(ownedNames[i], adapted[i]));
				size_t diagnosticSlot = slot >= 0 ? static_cast<size_t>(slot) : diagnosticChannels.size();
				if (observe && slot < 0 && diagnosticChannels.size() < GAKU_FACE_DIAGNOSTICS_CAPACITY)
				{
					GakuFaceChannelDiagnostic d{};
					std::memcpy(d.name, ownedNames[i].data(),
					            std::min(ownedNames[i].size(), size_t(GAKU_FACE_NAME_BYTES - 1)));
					d.before = d.after = std::numeric_limits<float>::quiet_NaN();
					diagnosticChannels.push_back(d);
				}
				if (observe && diagnosticSlot < diagnosticChannels.size())
				{
					auto& d = diagnosticChannels[diagnosticSlot];
					d.target = target;
					d.index = ownedIndices[i];
					d.before = api.getWeight(model, ownedIndices[i]);
				}
				api.setWeight(model, ownedIndices[i], target);
				if (observe && diagnosticSlot < diagnosticChannels.size())
					diagnosticChannels[diagnosticSlot].after = api.getWeight(model, ownedIndices[i]);
				written = true;
			}
			// The actor may already have calculated its material before the host
			// hook. Recalculate only the eye highlight from the final custom eyes.
			if (written && ownsEyes && api.refreshHighlight) api.refreshHighlight(actor);
		}
	};
} // namespace GakuMotion
