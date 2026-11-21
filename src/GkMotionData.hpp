#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>
#include "KineTexture/Sampling.hpp"
#include "KineTexture/ScalarTrack.hpp"

namespace GakuMotion
{
	// Product-facing file paths use UTF-8; filesystem streams must consume the
	// native path type so non-ASCII track directories do not depend on Windows ACP.
	inline std::filesystem::path MotionPathFromUtf8(const std::string& path)
	{
		return std::filesystem::path(std::u8string(path.begin(), path.end()));
	}

	inline std::string MotionPathToUtf8(const std::filesystem::path& path)
	{
		const auto value = path.u8string();
		return std::string(value.begin(), value.end());
	}

	using Vector3 = KineTexture::Vector3;
	using Quaternion = KineTexture::Quaternion;
	struct LegacyCameraFrame { Vector3 position, target; float fov{45}; };
	static_assert(sizeof(LegacyCameraFrame) == 28);
	static_assert(sizeof(Vector3) == 12 && sizeof(Quaternion) == 16);

	struct BoneMeta
	{
		uint16_t humanoid_id{0};
		std::string name;
		bool has_position{false};
		bool head_space_eye_delta{false}; // v5 bit1: Head-space eye delta, not an absolute local pose.
	};

	// Locate the clip inside a track directory, in priority order.
	//   motion.gkclip        role-agnostic canonical deltas  (preferred)
	//   motion.gk_world_dump absolute world, recorded truth
	//   motion.gkmotion      legacy world-space file, kept for migration
	// Exactly one file is used per track; see ACTION_FORMATS.md section 0 for why
	// the two roles cannot share one file.
	inline std::string FindMotionFile(const std::string& dir)
	{
		static const char* kCandidates[] = {
			"/motion.gkclip", "/motion.gk_world_dump", "/motion.gkmotion"
		};
		for (const char* c : kCandidates)
		{
			const auto p = MotionPathFromUtf8(dir + c);
			std::ifstream probe(p, std::ios::binary);
			if (probe.is_open()) return MotionPathToUtf8(p);
		}
		return std::string();
	}

	inline bool ParseSlotDirectoryIndex(std::string_view name, int& outSlot)
	{
		constexpr std::string_view kPrefix = "slot_";
		if (name.size() <= kPrefix.size() || name.rfind(kPrefix, 0) != 0) return false;
		const std::string_view digits = name.substr(kPrefix.size());
		if (digits.empty() || digits.front() == '0') return false;
		int value = 0;
		for (char ch : digits)
		{
			if (ch < '0' || ch > '9') return false;
			value = value * 10 + (ch - '0');
			if (value > 256) return false;
		}
		if (value < 2) return false;
		outSlot = value;
		return true;
	}

	inline std::string ResolveMotionSlotDirectory(const std::string& trackDirectory, int slot1Based)
	{
		if (slot1Based <= 1 || trackDirectory.empty()) return trackDirectory;
		std::string base = trackDirectory;
		while (!base.empty() && (base.back() == '/' || base.back() == '\\')) base.pop_back();
		return base + "/positions/slot_" + std::to_string(slot1Based);
	}

	inline std::string FindMotionSlotFile(const std::string& trackDirectory, int slot1Based)
	{
		return FindMotionFile(ResolveMotionSlotDirectory(trackDirectory, slot1Based));
	}

	// Gakumas humanoid parent chain, keyed by humanoid id.
	//
	// Source of truth: GAKUMAS_PARENT in tools/vmd2gkmotion/convert.py, cross-checked
	// against the bone table of a real .gkclip. The two MUST stay in sync -- if the
	// writer and this table disagree, a gkclip gets decomposed along one chain while
	// playback accumulates along another, and every bone below the first divergence
	// absorbs its ancestors' error. That is DEFECT 1 in handoff #019.
	//
	// Verified equal to Unity's HumanBodyBones numbering for all 52 emitted bones
	// (including UpperChest = 54), so no translation layer is needed.
	inline int GakumasParentHid(int hid)
	{
		switch (hid)
		{
		case 0: return -1; // Hips, chain root
		case 7: return 0; // Spine        <- Hips
		case 8: return 7; // Chest        <- Spine
		case 54: return 8; // UpperChest   <- Chest
		case 9: return 54; // Neck         <- UpperChest
		case 10: return 9; // Head         <- Neck
		case 21: return 10; // LeftEye      <- Head (neutral Head_Face intermediate)
		case 22: return 10; // RightEye     <- Head
		case 11: return 54; // LeftShoulder <- UpperChest
		case 12: return 54; // RightShoulder<- UpperChest
		case 13: return 11; // LeftUpperArm <- LeftShoulder
		case 14: return 12; // RightUpperArm<- RightShoulder
		case 15: return 13; // LeftLowerArm <- LeftUpperArm
		case 16: return 14; // RightLowerArm<- RightUpperArm
		case 17: return 15; // LeftHand     <- LeftLowerArm
		case 18: return 16; // RightHand    <- RightLowerArm
		case 1: return 0; // LeftUpperLeg <- Hips
		case 2: return 0; // RightUpperLeg<- Hips
		case 3: return 1; // LeftLowerLeg <- LeftUpperLeg
		case 4: return 2; // RightLowerLeg<- RightUpperLeg
		case 5: return 3; // LeftFoot     <- LeftLowerLeg
		case 6: return 4; // RightFoot    <- RightLowerLeg
		case 19: return 5; // LeftToes     <- LeftFoot
		case 20: return 6; // RightToes    <- RightFoot
		// Left hand fingers
		case 24: return 17;
		case 25: return 24;
		case 26: return 25;
		case 27: return 17;
		case 28: return 27;
		case 29: return 28;
		case 30: return 17;
		case 31: return 30;
		case 32: return 31;
		case 33: return 17;
		case 34: return 33;
		case 35: return 34;
		case 36: return 17;
		case 37: return 36;
		case 38: return 37;
		// Right hand fingers
		case 39: return 18;
		case 40: return 39;
		case 41: return 40;
		case 42: return 18;
		case 43: return 42;
		case 44: return 43;
		case 45: return 18;
		case 46: return 45;
		case 47: return 46;
		case 48: return 18;
		case 49: return 48;
		case 50: return 49;
		case 51: return 18;
		case 52: return 51;
		case 53: return 52;
		default: return -1;
		}
	}

	// Build MotionClip::bone_parents from the bone table.
	//
	// Returns, for each bone, the INDEX of its parent within `bones`, or -1 when
	// the parent is absent from the clip -- which makes that bone a chain root,
	// exactly what world_to_canonical_deltas() does for names missing from
	// gk_parents.
	inline std::vector<int> BuildBoneParentIndex(const std::vector<BoneMeta>& bones)
	{
		// At most 256 bones. Avoid attacker-controlled max-HID scratch allocation.
		std::vector<int> parents(bones.size(), -1);
		for (size_t i = 0; i < bones.size(); ++i)
		{
			const int ph = GakumasParentHid(bones[i].humanoid_id);
			if (ph < 0) continue;
			for (size_t j = 0; j < bones.size(); ++j)
				if (bones[j].humanoid_id == ph) { parents[i] = static_cast<int>(j); break; }
		}
		return parents;
	}

	// Build MotionClip::bone_parent_hids -- the SAME chain as BuildBoneParentIndex()
	// but expressed in humanoid ids instead of `bones` indices.
	//
	// WHY BOTH: the two are NOT interchangeable. A gkclip's bone table is emitted in
	// topological order with a GAKUMAS_BONE_ORDER walk, so for 51 of the 52 bones
	// `index != humanoid_id` (only Hips happens to be 0==0). Playback looks parents
	// up in writtenWorldRots[], which is indexed BY HID, so handing it a `bones`
	// index silently reads the wrong bone: Chest (hid 8, index 2) would take
	// LeftUpperLeg (hid 1) as its parent, LeftUpperArm (hid 13, index 8) would take
	// RightFoot (hid 6), and so on for 48 of 51 bones. That is precisely the
	// "arms up, legs twisted, whole body limp" collapse reported on 2026-10-05.
	//
	// Unit tests that use a synthetic hid-contiguous bone table CANNOT catch this,
	// because there index == hid by construction. Always test with a real gkclip.
	inline std::vector<int> BuildBoneParentHids(const std::vector<BoneMeta>& bones)
	{
		std::vector<int> parents(bones.size(), -1);
		for (size_t i = 0; i < bones.size(); ++i)
		{
			parents[i] = GakumasParentHid(bones[i].humanoid_id);
		}
		return parents;
	}

	class MotionClip
	{
	public:
		// Two formats share this container because they differ only in the SPACE
		// the stored quaternions live in, not in their layout:
		//
		//   kGkWorldDump ("GKWDMP")  absolute actor-relative WORLD rotations.
		//                            Recorded ground truth. Portable = no.
		//   kGkClip      ("GKCLIP")   CANONICAL LOCAL DELTAS against a T-pose
		//                            whose local rest is Identity on every bone.
		//                            Contains no character constant, so it is the
		//                            distributable format.
		//
		// See DevAgents/architecture/ACTION_FORMATS.md sections 2 and 3.
		enum class Format : uint8_t
		{
			kGkWorldDump = 0,
			kGkClip = 1,
			kUnknown = 255,
			kLegacyGkmo = 2, // old ".gkmotion", world space, read-only
		};

		Format format{Format::kLegacyGkmo};

		uint32_t version{1};
		float fps{30.0f};
		uint32_t total_frames{0};
		float duration_seconds{0.0f};
		uint32_t bone_count{0};
		float root_scale{0.083f};

		std::vector<BoneMeta> bones;
		// Parent chain for gkclip playback: bone_parents[i] is the INDEX into
		// `bones` of bone i's parent, or -1 when the parent is not in the clip
		// (making that bone a chain root). Parallel to `bones`.
		//
		// Required because a gkclip stores CANONICAL LOCAL deltas: converting one
		// into the world rotation the aux solver and writtenWorldRots[] need means
		// accumulating along the parent chain. Bones are written in topological
		// (parents-first) order, so a single forward pass at playback time
		// suffices -- no per-frame graph walk.
		//
		// Populated by BuildBoneParentIndex() at load time. Never used by the
		// world-space formats, which already store absolute rotations.
		std::vector<int> bone_parents;
		// Same chain as bone_parents, but in humanoid ids. USE THIS for any lookup
		// into a hid-indexed table such as writtenWorldRots[]. bone_parents[] is
		// for indexing `bones` / per-bone arrays (restLocals, boneTransforms) and
		// must never be fed to a hid-indexed table -- see BuildBoneParentHids().
		std::vector<int> bone_parent_hids;
		std::vector<Vector3> root_positions; // size = total_frames
		std::vector<LegacyCameraFrame> camera_frames; // optional existing 7-float tail
		std::vector<Quaternion> bone_rotations; // size = total_frames * bone_count
		// world for world dump / kGkClip local deltas
		// v4 channels use VLActorFaceModel names and normalized [0,1] weights.
		std::vector<std::string> blendshape_names;
		std::vector<float> blendshape_weights; // frame-major

		// True when bone_rotations holds canonical LOCAL deltas and therefore has
		// to be written as aligned local rotations through setLocalRotation.
		// A gkclip must never be written through setWorldRotation: doing so
		// back-subtracts the parent chain, which is a known failure mode.
		bool IsLocalDeltaStream() const { return format == Format::kGkClip; }

		static constexpr size_t kDefaultLoadBudget = 256u * 1024u * 1024u;

		static Format DetectFormat(const char* magic8)
		{
			if (std::memcmp(magic8, "GKCLIPCL", 8) == 0 || std::memcmp(magic8, "GKCLIP\0\0", 8) == 0) return Format::kGkClip;
			if (std::memcmp(magic8, "GKWDMPWD", 8) == 0 || std::memcmp(magic8, "GKWDMP\0\0", 8) == 0) return Format::kGkWorldDump;
			if (std::memcmp(magic8, "GKMO", 4) == 0) return Format::kLegacyGkmo;
			return Format::kUnknown;
		}

		// Parse into a separate candidate. Any rejected input leaves this clip intact.
		bool LoadFromFile(const std::string& filepath, std::string* error = nullptr,
		                  size_t allocationBudget = kDefaultLoadBudget)
		{
			if (error) error->clear();
			try
			{
				MotionClip candidate;
				if (!candidate.ReadValidatedFile(filepath, allocationBudget, error)) return false;
				*this = std::move(candidate);
				return true;
			}
			catch (const std::bad_alloc&)
			{
				if (error) *error = "allocation failed";
				return false;
			}
			catch (const std::length_error&)
			{
				if (error) *error = "allocation length exceeded";
				return false;
			}
		}

	private:
		bool ReadValidatedFile(const std::string& filepath, size_t budget, std::string* error)
		{
			std::ifstream file(MotionPathFromUtf8(filepath), std::ios::binary | std::ios::ate);
			auto fail = [&](const char* reason)
			{
				if (error) *error = std::string(reason) + " at byte " + std::to_string(file.tellg());
				return false;
			};
			if (!file) return fail("cannot open");
			const auto end = file.tellg();
			if (end < 8 || static_cast<uint64_t>(end) > budget) return fail("file size/budget");
			file.seekg(0);
			size_t charged = 0;
			auto charge = [&](size_t count, size_t width)
			{
				if (width && count > (budget - charged) / width) return false;
				charged += count * width;
				return true;
			};
			auto available = [&](size_t count, size_t width)
			{
				const auto position = file.tellg();
				return position >= 0 && position <= end && (!width || count <= static_cast<uint64_t>(end - position) / width);
			};
			auto read = [&](auto& value) { return bool(file.read(reinterpret_cast<char*>(&value), sizeof(value))); };
			char magic[8]{};
			file.read(magic, sizeof(magic));
			format = DetectFormat(magic);
			if (format == Format::kUnknown) return fail("unknown magic/format marker");
			file.seekg(format == Format::kLegacyGkmo ? 4 : 8);
			if (!read(version) || !read(fps) || !read(total_frames) || !read(duration_seconds) ||
				!read(bone_count) || !read(root_scale)) return fail("truncated header");
			if (!std::isfinite(fps) || fps <= 0 || !std::isfinite(duration_seconds) || duration_seconds < 0 ||
				!std::isfinite(root_scale) || root_scale <= 0 || !total_frames || total_frames > 1000000 ||
				!bone_count || bone_count > 256 || version < 1 || version > (format == Format::kGkClip ? 5u : 4u))
				return fail("unsupported header/version");
			if (!charge(bone_count, sizeof(BoneMeta) + 2 * sizeof(int))) return fail("bone allocation budget");
			bones.reserve(bone_count);
			for (uint32_t i = 0; i < bone_count; ++i)
			{
				uint16_t hid{};
				uint8_t nlen{}, hasPos{};
				if (!read(hid) || !read(nlen) || !read(hasPos) || hasPos > (version >= 5 ? 3 : 1) || !nlen ||
					!available(nlen, 1) || !charge(nlen + 1, 1)) return fail("invalid/truncated bone entry");
				const bool eyeDelta = (hasPos & 2) != 0;
				if (eyeDelta && ((hid != 21 && hid != 22) || !(hasPos & 1))) return fail("invalid eye delta flag");
				std::string name(nlen, '\0');
				file.read(name.data(), nlen);
				if (!file || name.find('\0') != std::string::npos) return fail("invalid bone name");
				if (std::any_of(bones.begin(), bones.end(), [&](const BoneMeta& b) { return b.humanoid_id == hid || b.name == name; }))
					return fail("duplicate bone id/name");
				bones.push_back({hid, std::move(name), (hasPos & 1) != 0, eyeDelta});
			}
			bone_parents = BuildBoneParentIndex(bones);
			bone_parent_hids = BuildBoneParentHids(bones);
			for (size_t i = 0; i < bones.size(); ++i)
				if (bones[i].head_space_eye_delta && bone_parents[i] < 0) return fail("eye delta requires Head");
			if (format == Format::kGkClip)
				for (size_t i = 0; i < bone_parents.size(); ++i)
					if (bone_parents[i] >= static_cast<int>(i)) return fail("non-topological legacy bone order");
			const size_t rotations = static_cast<size_t>(total_frames) * bone_count;
			if (!charge(total_frames, sizeof(Vector3)) || !charge(rotations, sizeof(Quaternion)) ||
				!available(total_frames, sizeof(Vector3) + sizeof(Quaternion) * bone_count)) return fail("motion size/budget");
			root_positions.resize(total_frames);
			bone_rotations.resize(rotations);
			for (uint32_t f = 0; f < total_frames; ++f)
			{
				if (!read(root_positions[f]) || !file.read(reinterpret_cast<char*>(&bone_rotations[static_cast<size_t>(f) * bone_count]),
					static_cast<std::streamsize>(sizeof(Quaternion) * bone_count))) return fail("truncated pose");
			}
			const KineTexture::DenseClipView view{fps, total_frames, bone_count, root_positions, bone_rotations};
			if (!KineTexture::ValidateDenseClip(view)) return fail("nonfinite pose/nonunit rotation");
			if ((format == Format::kGkClip && version >= 4) || file.peek() != std::char_traits<char>::eof())
			{
				uint8_t hasCamera{};
				if (!read(hasCamera) || hasCamera > 1) return fail("camera flag");
				if (hasCamera)
				{
					if (!charge(total_frames, sizeof(LegacyCameraFrame)) || !available(total_frames, sizeof(LegacyCameraFrame))) return fail("camera size/budget");
					camera_frames.resize(total_frames);
					file.read(reinterpret_cast<char*>(camera_frames.data()), static_cast<std::streamsize>(sizeof(LegacyCameraFrame) * total_frames));
					if (!file) return fail("truncated camera");
					for (const auto& c : camera_frames)
						if (!KineTexture::IsFinite(c.position) || !KineTexture::IsFinite(c.target) || !std::isfinite(c.fov) || c.fov < 1 || c.fov > 179)
							return fail("invalid camera value");
				}
			}
			if (format == Format::kGkClip && version >= 4)
			{
				uint32_t count{};
				if (!read(count) || count > 1024 || !charge(count, sizeof(std::string))) return fail("morph count/budget");
				blendshape_names.reserve(count);
				for (uint32_t i = 0; i < count; ++i)
				{
					uint16_t length{};
					if (!read(length) || !length || !available(length, 1) || !charge(length + 1, 1)) return fail("morph name size/budget");
					std::string name(length, '\0');
					file.read(name.data(), length);
					if (!file || name.find('\0') != std::string::npos ||
						std::find(blendshape_names.begin(), blendshape_names.end(), name) != blendshape_names.end()) return fail("duplicate/invalid morph name");
					blendshape_names.push_back(std::move(name));
				}
				const size_t weights = static_cast<size_t>(total_frames) * count;
				if (!charge(weights, sizeof(float)) || !available(weights, sizeof(float))) return fail("morph size/budget");
				blendshape_weights.resize(weights);
				if (weights) file.read(reinterpret_cast<char*>(blendshape_weights.data()), static_cast<std::streamsize>(weights * sizeof(float)));
				if (!file || std::any_of(blendshape_weights.begin(), blendshape_weights.end(), [](float w) { return !std::isfinite(w) || w < 0 || w > 1; }))
					return fail("invalid/truncated morph weights");
			}
			if (file.peek() != std::char_traits<char>::eof()) return fail("unexpected trailing payload");
			return true;
		}

	public:

		void SampleBlendshapes(float timeSeconds, std::vector<float>& out) const
		{
			const size_t count = blendshape_names.size();
			out.clear();
			const KineTexture::DenseScalarTracksView view{fps, total_frames, count, blendshape_weights, {}};
			if (!KineTexture::ValidateDenseScalarTracksShape(view)) return;
			out.resize(count);
			// The legacy adapter deliberately maps invalid time to frame zero.
			if (!KineTexture::SampleDenseScalarTracks(view, std::isfinite(timeSeconds) ? timeSeconds : 0.0f,
				out, {}, KineTexture::ScalarKeyEvaluation::LinearExpression))
				out.clear();
		}

		void Sample(float timeSeconds, Vector3& outRootPos, std::vector<Quaternion>& outRotations) const
		{
			const KineTexture::DenseClipView view{fps, total_frames, bone_count, root_positions, bone_rotations};
			if (!std::isfinite(timeSeconds) || !KineTexture::ValidateDenseClipShape(view)) return;
			outRotations.resize(bone_count);
			KineTexture::SampleDensePose(view, timeSeconds, outRootPos, std::span<Quaternion>(outRotations));
		}
	};
} // namespace GakuMotion
