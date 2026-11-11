#pragma once
#include "GkLog.hpp"

// Included in the plugin's private namespace, after ActorGroundShadow.
// Custom.acf's CategoryName table contains Bgm / Se / Voice. Only Bgm and
// Voice are leased; MCI audio and native Se are independent of these mutes.
namespace NativeAudio
{
	using namespace GakuMotion::bgm;
	// CRI's native x64 ABI (also exposed by this game's CriWare metadata).
	struct CategoryInfo
	{
		uint32_t group, id;
		const char* name;
		uint32_t cueLimits;
		float volume;
	};
	struct AcfInfo
	{
		const char* name;
		uint32_t size, version;
		int encoding, dspSettings, categories, categoriesPerPlayback, reacts,
			aisacControls, globalAisacs, gameVariables, maxBuses, buses, voiceGroups, outputPorts;
	};
	static_assert(sizeof(CategoryInfo) == 24 && sizeof(AcfInfo) == 64);
	inline int (*getAcf)(AcfInfo*) = nullptr;
	inline int (*getCategory)(const char*, CategoryInfo*) = nullptr;
	inline int (*isMuted)(uint32_t) = nullptr;
	inline void (*mute)(uint32_t, int) = nullptr;
	inline HMODULE module = nullptr;
	inline decltype(&GetModuleHandleW) getModule = &GetModuleHandleW;

	struct Category
	{
		const char* name;
		Scope scope;
		uint64_t generation{0};
		const char *acfIdentity{nullptr}, *categoryIdentity{nullptr};
		uint32_t acfSize{0}, acfVersion{0}, id{0}, group{0};
		bool present{false};
	};
	inline Category bgm{"Bgm", Scope::BgmOnly}, voice{"Voice", Scope::CharacterVoiceOnly};

	inline bool MainThread(void*)
	{
		return GetCurrentThreadId() == ActorGroundShadow::unityThreadId.load(std::memory_order_acquire);
	}
	inline bool ResolveExports()
	{
		HMODULE current = getModule(L"cri_ware_unity.dll");
		if (!current) return false; // Never load/initialise another CRI instance.
		if (current != module || !getAcf || !getCategory || !isMuted || !mute)
		{
			module = current;
			getAcf = reinterpret_cast<decltype(getAcf)>(GetProcAddress(module, "criAtomExAcf_GetAcfInfo"));
			getCategory = reinterpret_cast<decltype(getCategory)>(GetProcAddress(module, "criAtomExAcf_GetCategoryInfoByName"));
			isMuted = reinterpret_cast<decltype(isMuted)>(GetProcAddress(module, "criAtomExCategory_IsMutedById"));
			mute = reinterpret_cast<decltype(mute)>(GetProcAddress(module, "criAtomExCategory_MuteById"));
		}
		return getAcf && getCategory && isMuted && mute;
	}
	inline ReadStatus Resolve(void* context, Target& target)
	{
		if (!MainThread(nullptr)) return ReadStatus::Unavailable;
		auto& category = *static_cast<Category*>(context);
		AcfInfo acf{};
		CategoryInfo info{};
		if (!ResolveExports() || !getAcf(&acf) || !acf.name || !acf.size ||
			!getCategory(category.name, &info) || !info.name || std::strcmp(info.name, category.name))
		{
			category.present = false;
			return ReadStatus::Unavailable;
		}
		if (!category.present || category.acfIdentity != acf.name || category.acfSize != acf.size ||
			category.acfVersion != acf.version || category.categoryIdentity != info.name ||
			category.id != info.id || category.group != info.group)
		{
			++category.generation;
			if (!category.generation) ++category.generation;
			category.acfIdentity = acf.name; category.acfSize = acf.size; category.acfVersion = acf.version;
			category.categoryIdentity = info.name; category.id = info.id; category.group = info.group;
			category.present = true;
		}
		target = {reinterpret_cast<uintptr_t>(module), category.generation, info.id, category.scope};
		return ReadStatus::Ok;
	}
	inline ReadStatus Read(void* context, const Target& target, MuteState& state)
	{
		Target current;
		if (Resolve(context, current) != ReadStatus::Ok || !(current == target)) return ReadStatus::Expired;
		state = {isMuted(static_cast<uint32_t>(target.id)) != 0, 0};
		return ReadStatus::Ok;
	}
	inline WriteResult Write(void* context, const Target& target, const MuteState& expected, bool value)
	{
		MuteState current;
		if (Read(context, target, current) != ReadStatus::Ok) return {WriteStatus::Expired};
		if (!(current == expected)) return {WriteStatus::Conflict};
		mute(static_cast<uint32_t>(target.id), value ? 1 : 0);
		MuteState after;
		if (Read(context, target, after) != ReadStatus::Ok) return {WriteStatus::Uncertain};
		return {after.muted == value ? WriteStatus::Applied : WriteStatus::FailedNoMutation, after, true};
	}
	inline Backend BackendFor(Category& category) { return {&category, MainThread, Resolve, Read, Write}; }
	inline void Release(ReleaseReason reason)
	{
		g_bgmLease.Release(reason);
		g_voiceLease.Release(reason);
	}
	inline bool ReleaseForCapture()
	{
		// Evaluate both even if one fails, so one uncertain category never
		// prevents an independent owned category from being restored.
		const bool bgmClean = GakuMotion::bgm::ReleaseForCapture(g_bgmLease);
		const bool voiceClean = GakuMotion::bgm::ReleaseForCapture(g_voiceLease);
		return bgmClean && voiceClean;
	}
	inline void Sync()
	{
		const auto& player = GakuMotion::Core::Instance().player;
		const bool active = g_motionSceneAllowed && player.clip.total_frames != 0 &&
			player.state != GakuMotion::PlaybackState::Stopped;
		// Ownership follows the performance, including preview, pause, scrub,
		// muted songs and clips without song.wav. Stop/scene loss releases it.
		const Status bgmStatus = g_bgmLease.Sync(BackendFor(bgm), active, 1);
		const Status voiceStatus = g_voiceLease.Sync(BackendFor(voice), active, 1);
		static Status previousBgm = Status::Idle, previousVoice = Status::Idle;
		if (previousBgm != bgmStatus || previousVoice != voiceStatus)
			GM_DEBUG("[GakuMotion][NativeAudio] Bgm=%s Voice=%s performance=%d",
				StatusName(bgmStatus), StatusName(voiceStatus), active);
		previousBgm = bgmStatus; previousVoice = voiceStatus;
	}
}
