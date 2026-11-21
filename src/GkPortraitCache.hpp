#pragma once

#include "GkWicCover.hpp"
#include "GkWindowsPaths.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif
#endif

namespace GakuMotion::portrait
{
	struct OctoBundleRecord
	{
		uint64_t id{0};
		std::string name;
		uint64_t size{0};
		std::string md5;
	};

	struct CachedFileSignature
	{
		bool exists{false};
		long long writeTime{0};
		long long size{0};

		bool operator==(const CachedFileSignature&) const noexcept = default;
	};

	inline CachedFileSignature StatCacheFile(const std::filesystem::path& path)
	{
		CachedFileSignature sig;
		if (path.empty()) return sig;
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec) || ec) return sig;
		sig.exists = true;
		const auto t = std::filesystem::last_write_time(path, ec);
		if (!ec) sig.writeTime = static_cast<long long>(t.time_since_epoch().count());
		const auto sz = std::filesystem::file_size(path, ec);
		if (!ec) sig.size = static_cast<long long>(sz);
		return sig;
	}

	inline bool IsSafeCharacterCode(std::string_view code) noexcept
	{
		if (code.empty() || code.size() > 32) return false;
		for (unsigned char ch : code)
		{
			const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
				(ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
			if (!ok) return false;
		}
		return true;
	}

	inline std::string NormalizeCharacterCode(std::string_view code)
	{
		if (!IsSafeCharacterCode(code)) return {};
		std::string out(code);
		for (char& ch : out)
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return out;
	}

	inline std::filesystem::path PortraitCachePath(const std::string& configRootUtf8, std::string_view code)
	{
		if (configRootUtf8.empty()) return {};
		const std::string norm = NormalizeCharacterCode(code);
		if (norm.empty()) return {};
		return MotionPathFromUtf8(configRootUtf8) / "portraits" / (norm + ".png");
	}

	inline std::array<std::string, 4> PortraitAssetCandidates(std::string_view code)
	{
		const std::string norm = NormalizeCharacterCode(code);
		if (norm.empty()) return {};
		return {
			"img_chr_" + norm + "_00-bustup",
			"img_chr_" + norm + "_00-audition",
			"img_adv_speaker_" + norm + "_000-000",
			"img_chr_" + norm + "_00-thumb-circle",
		};
	}

	inline bool IsPortraitAssetName(std::string_view name) noexcept
	{
		return name.rfind("img_chr_", 0) == 0 || name.rfind("img_adv_speaker_", 0) == 0;
	}

	inline bool IsValidPngBytes(const std::vector<uint8_t>& bytes) noexcept
	{
		static constexpr uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
		if (bytes.size() < 45 || std::memcmp(bytes.data(), kPngSig, 8) != 0) return false;
		if (std::memcmp(bytes.data() + 12, "IHDR", 4) != 0) return false;

		auto readU32BE = [&](size_t off) -> uint32_t {
			return (static_cast<uint32_t>(bytes[off]) << 24) |
				(static_cast<uint32_t>(bytes[off + 1]) << 16) |
				(static_cast<uint32_t>(bytes[off + 2]) << 8) |
				static_cast<uint32_t>(bytes[off + 3]);
		};
		const uint32_t width = readU32BE(16);
		const uint32_t height = readU32BE(20);
		if (width == 0 || height == 0 || width > 8192 || height > 8192) return false;

		size_t pos = 8;
		bool sawIend = false;
		while (pos + 12 <= bytes.size())
		{
			const uint32_t chunkLen = readU32BE(pos);
			const size_t remaining = bytes.size() - pos - 12;
			if (chunkLen > remaining) return false;
			if (std::memcmp(bytes.data() + pos + 4, "IEND", 4) == 0)
			{
				sawIend = (chunkLen == 0);
				pos += 12;
				break;
			}
			pos += 12 + static_cast<size_t>(chunkLen);
		}
		return sawIend && pos == bytes.size();
	}

	inline std::string HexLower(const uint8_t* data, size_t len)
	{
		static constexpr char kHex[] = "0123456789abcdef";
		std::string out;
		out.reserve(len * 2);
		for (size_t i = 0; i < len; ++i)
		{
			out.push_back(kHex[(data[i] >> 4) & 0x0F]);
			out.push_back(kHex[data[i] & 0x0F]);
		}
		return out;
	}

	inline std::string UpperHexAscii(std::string_view ascii)
	{
		static constexpr char kHex[] = "0123456789ABCDEF";
		std::string out;
		out.reserve(ascii.size() * 2);
		for (unsigned char ch : ascii)
		{
			out.push_back(kHex[(ch >> 4) & 0x0F]);
			out.push_back(kHex[ch & 0x0F]);
		}
		return out;
	}

	namespace detail
	{
		inline bool ReadVarint(const uint8_t* buf, size_t size, size_t& pos, uint64_t& out) noexcept
		{
			out = 0;
			int shift = 0;
			while (pos < size && shift <= 63)
			{
				const uint8_t b = buf[pos++];
				if (shift == 63 && (b & 0x7E) != 0) return false;
				out |= static_cast<uint64_t>(b & 0x7Fu) << shift;
				if ((b & 0x80u) == 0) return true;
				shift += 7;
			}
			return false;
		}

		inline bool SkipProtoField(const uint8_t* buf, size_t size, size_t& pos, uint8_t wireType) noexcept
		{
			uint64_t tmp = 0;
			switch (wireType)
			{
			case 0:
				return ReadVarint(buf, size, pos, tmp);
			case 1:
				if (size - pos < 8) return false;
				pos += 8;
				return true;
			case 2:
				if (!ReadVarint(buf, size, pos, tmp) || tmp > size - pos) return false;
				pos += static_cast<size_t>(tmp);
				return true;
			case 5:
				if (size - pos < 4) return false;
				pos += 4;
				return true;
			default:
				return false;
			}
		}

		inline bool ParseBundleSubMessage(const uint8_t* buf, size_t size, OctoBundleRecord& out)
		{
			size_t pos = 0;
			while (pos < size)
			{
				uint64_t tag = 0;
				if (!ReadVarint(buf, size, pos, tag)) return false;
				const uint64_t fn = tag >> 3;
				const uint8_t wt = static_cast<uint8_t>(tag & 7u);
				if (wt == 0)
				{
					uint64_t val = 0;
					if (!ReadVarint(buf, size, pos, val)) return false;
					if (fn == 1) out.id = val;
					else if (fn == 4) out.size = val;
				}
				else if (wt == 2)
				{
					uint64_t len = 0;
					if (!ReadVarint(buf, size, pos, len) || len > size - pos) return false;
					if (fn == 3)
						out.name.assign(reinterpret_cast<const char*>(buf + pos), static_cast<size_t>(len));
					else if (fn == 10)
						out.md5.assign(reinterpret_cast<const char*>(buf + pos), static_cast<size_t>(len));
					pos += static_cast<size_t>(len);
				}
				else
				{
					if (!SkipProtoField(buf, size, pos, wt)) return false;
				}
			}
			return true;
		}
	} // namespace detail

	inline bool ComputeMd5(const uint8_t* data, size_t size, std::array<uint8_t, 16>& outDigest)
	{
		outDigest.fill(0);
#ifndef _WIN32
		(void)data;
		(void)size;
		return false;
#else
		struct AlgGuard
		{
			BCRYPT_ALG_HANDLE h{nullptr};
			~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
		} alg;
		if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_MD5_ALGORITHM, nullptr, 0))) return false;
		struct HashGuard
		{
			BCRYPT_HASH_HANDLE h{nullptr};
			~HashGuard() { if (h) BCryptDestroyHash(h); }
		} hash;
		if (!NT_SUCCESS(BCryptCreateHash(alg.h, &hash.h, nullptr, 0, nullptr, 0, 0))) return false;
		if (size > 0)
		{
			constexpr size_t kChunk = 64u * 1024u * 1024u;
			for (size_t off = 0; off < size;)
			{
				const ULONG step = static_cast<ULONG>((std::min)(kChunk, size - off));
				if (!NT_SUCCESS(BCryptHashData(hash.h, const_cast<PUCHAR>(data + off), step, 0))) return false;
				off += step;
			}
		}
		return NT_SUCCESS(BCryptFinishHash(hash.h, outDigest.data(), static_cast<ULONG>(outDigest.size()), 0));
#endif
	}

	inline bool DecryptOctoManifest(
		const std::vector<uint8_t>& raw,
		std::vector<uint8_t>& outProtobuf,
		std::string& outError)
	{
		outProtobuf.clear();
		outError.clear();
#ifndef _WIN32
		(void)raw;
		outError = "Win32 CNG unavailable";
		return false;
#else
		if (raw.size() < 17 || raw[0] != 0x01 || ((raw.size() - 1) % 16) != 0)
		{
			outError = "Invalid octocacheevai header or cipher alignment";
			return false;
		}

		static constexpr char kKeySeed[] = "1nuv9td1bw1udefk";
		static constexpr char kIvSeed[] = "LvAUtf+tnz";
		std::array<uint8_t, 16> key{};
		std::array<uint8_t, 16> iv{};
		if (!ComputeMd5(reinterpret_cast<const uint8_t*>(kKeySeed), sizeof(kKeySeed) - 1, key) ||
			!ComputeMd5(reinterpret_cast<const uint8_t*>(kIvSeed), sizeof(kIvSeed) - 1, iv))
		{
			outError = "Failed to derive Octo manifest AES key/IV";
			return false;
		}

		struct AlgGuard
		{
			BCRYPT_ALG_HANDLE h{nullptr};
			~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
		} alg;
		if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_AES_ALGORITHM, nullptr, 0)))
		{
			outError = "BCryptOpenAlgorithmProvider(AES) failed";
			return false;
		}
		if (!NT_SUCCESS(BCryptSetProperty(
				alg.h, BCRYPT_CHAINING_MODE,
				reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
				sizeof(BCRYPT_CHAIN_MODE_CBC), 0)))
		{
			outError = "BCryptSetProperty(CBC) failed";
			return false;
		}

		struct KeyGuard
		{
			BCRYPT_KEY_HANDLE h{nullptr};
			~KeyGuard() { if (h) BCryptDestroyKey(h); }
		} keyHandle;
		if (!NT_SUCCESS(BCryptGenerateSymmetricKey(
				alg.h, &keyHandle.h, nullptr, 0, key.data(), static_cast<ULONG>(key.size()), 0)))
		{
			outError = "BCryptGenerateSymmetricKey failed";
			return false;
		}

		const ULONG cipherLen = static_cast<ULONG>(raw.size() - 1);
		std::vector<uint8_t> plaintext(cipherLen);
		std::array<uint8_t, 16> ivCopy = iv;
		ULONG cbResult = 0;
		const NTSTATUS status = BCryptDecrypt(
			keyHandle.h,
			const_cast<PUCHAR>(raw.data() + 1),
			cipherLen,
			nullptr,
			ivCopy.data(),
			static_cast<ULONG>(ivCopy.size()),
			plaintext.data(),
			cipherLen,
			&cbResult,
			BCRYPT_BLOCK_PADDING);
		if (!NT_SUCCESS(status))
		{
			outError = "BCryptDecrypt failed on octocacheevai";
			return false;
		}
		if (cbResult < 16)
		{
			outError = "Decrypted Octo manifest missing MD5 trailer";
			return false;
		}
		plaintext.resize(cbResult);

		std::array<uint8_t, 16> actualDigest{};
		if (!ComputeMd5(plaintext.data() + 16, plaintext.size() - 16, actualDigest))
		{
			outError = "Failed to hash decrypted Octo manifest payload";
			return false;
		}
		if (std::memcmp(actualDigest.data(), plaintext.data(), 16) != 0)
		{
			outError = "Decrypted Octo manifest MD5 integrity check failed";
			return false;
		}

		outProtobuf.assign(plaintext.begin() + 16, plaintext.end());
		return true;
#endif
	}

	inline bool ParseOctoAssetBundleIndex(
		const std::vector<uint8_t>& protobufBytes,
		std::unordered_map<std::string, OctoBundleRecord>& outIndex,
		bool portraitOnly = true)
	{
		outIndex.clear();
		size_t pos = 0;
		const size_t size = protobufBytes.size();
		const uint8_t* buf = protobufBytes.data();
		while (pos < size)
		{
			uint64_t tag = 0;
			if (!detail::ReadVarint(buf, size, pos, tag)) return false;
			const uint64_t fn = tag >> 3;
			const uint8_t wt = static_cast<uint8_t>(tag & 7u);
			if (fn == 2 && wt == 2)
			{
				uint64_t len = 0;
				if (!detail::ReadVarint(buf, size, pos, len) || len > size - pos) return false;
				OctoBundleRecord rec;
				if (!detail::ParseBundleSubMessage(buf + pos, static_cast<size_t>(len), rec)) return false;
				pos += static_cast<size_t>(len);
				if (!rec.name.empty() && (!portraitOnly || IsPortraitAssetName(rec.name)))
				{
					outIndex[rec.name] = std::move(rec);
				}
			}
			else
			{
				if (!detail::SkipProtoField(buf, size, pos, wt)) return false;
			}
		}
		return true;
	}

	inline std::filesystem::path DiscoverOctoManifestPath(const std::filesystem::path& gameRoot)
	{
		if (gameRoot.empty()) return {};
		const auto pdbRoot = gameRoot / "octo" / "pdb" / "400";
		std::error_code ec;
		if (!std::filesystem::is_directory(pdbRoot, ec) || ec) return {};

		bool hasBest = false;
		bool bestIsNum = false;
		uint64_t bestNum = 0;
		std::string bestName;
		std::filesystem::path bestPath;

		for (const auto& entry : std::filesystem::directory_iterator(pdbRoot, ec))
		{
			if (ec || !entry.is_directory(ec)) { ec.clear(); continue; }
			const auto candidate = entry.path() / "octocacheevai";
			if (!std::filesystem::is_regular_file(candidate, ec) || ec) { ec.clear(); continue; }
			const std::string dirName = entry.path().filename().string();
			bool isNum = !dirName.empty();
			uint64_t num = 0;
			for (unsigned char c : dirName)
			{
				if (c < '0' || c > '9') { isNum = false; break; }
				num = num * 10 + (c - '0');
			}
			bool better = !hasBest;
			if (hasBest)
			{
				if (isNum != bestIsNum) better = isNum;
				else if (isNum) better = num > bestNum;
				else better = dirName > bestName;
			}
			if (better)
			{
				hasBest = true;
				bestIsNum = isNum;
				bestNum = num;
				bestName = dirName;
				bestPath = candidate;
			}
		}
		return bestPath;
	}

	inline std::filesystem::path ResolveOctoBundleFilePath(
		const std::filesystem::path& gameRoot,
		const OctoBundleRecord& rec)
	{
		if (gameRoot.empty() || rec.md5.empty()) return {};
		const std::string shard = std::to_string(rec.id % 10);
		const std::string folder = UpperHexAscii("A" + std::to_string(rec.id));
		const auto primary = gameRoot / "octo" / "v1" / "400" / shard / folder / rec.md5;
		std::error_code ec;
		if (std::filesystem::is_regular_file(primary, ec)) return primary;
		const auto altFlat = gameRoot / "octo" / "v1" / "400" / ("A" + rec.md5);
		if (std::filesystem::is_regular_file(altFlat, ec)) return altFlat;
		const auto altData = gameRoot / "gakumas_Data" / "Octo" / shard / folder / rec.md5;
		if (std::filesystem::is_regular_file(altData, ec)) return altData;
		return {};
	}

	inline bool DeobfuscateOctoBundle(
		std::vector<uint8_t>& bytes,
		std::string_view bundleName,
		std::string& outError)
	{
		outError.clear();
		if (bytes.size() < 24 || bundleName.empty())
		{
			outError = "Octo bundle buffer too short or name empty";
			return false;
		}
		const size_t L = bundleName.size();
		const uint8_t nb0 = static_cast<uint8_t>(bundleName[0]);
		const uint8_t c0 = static_cast<uint8_t>(bytes[0] ^ static_cast<uint8_t>('U') ^ nb0);
		const size_t limit = (std::min)(size_t{256}, bytes.size());
		for (size_t j = 0; j < limit; ++j)
		{
			const size_t i = j / 2;
			const uint8_t k = (j % 2 == 0)
				? static_cast<uint8_t>(static_cast<uint8_t>(bundleName[i % L]) ^ c0)
				: static_cast<uint8_t>(static_cast<uint8_t>(bundleName[L - 1 - (i % L)]) ^ static_cast<uint8_t>(c0 ^ 0xFFu));
			bytes[j] ^= k;
		}

		if (std::memcmp(bytes.data(), "UnityFS\0", 8) != 0)
		{
			outError = "Deobfuscated bundle does not begin with UnityFS magic";
			return false;
		}

		size_t p = 8 + 4; // skip "UnityFS\0" + uint32 BE format
		for (int s = 0; s < 2; ++s)
		{
			while (p < bytes.size() && bytes[p] != 0) ++p;
			if (p >= bytes.size())
			{
				outError = "Malformed UnityFS header version string";
				return false;
			}
			++p;
		}
		if (p + 8 > bytes.size())
		{
			outError = "Truncated UnityFS totalSize field";
			return false;
		}
		uint64_t totalSize = 0;
		for (int b = 0; b < 8; ++b)
			totalSize = (totalSize << 8) | bytes[p + b];
		if (totalSize != bytes.size())
		{
			outError = "UnityFS totalSize mismatch";
			return false;
		}
		return true;
	}

	inline bool LoadAndDeobfuscateOctoBundle(
		const std::filesystem::path& gameRoot,
		const OctoBundleRecord& rec,
		std::vector<uint8_t>& outBundle,
		std::string& outError)
	{
		outBundle.clear();
		outError.clear();
		const auto path = ResolveOctoBundleFilePath(gameRoot, rec);
		if (path.empty())
		{
			outError = "Octo bundle file not cached locally";
			return false;
		}
		std::ifstream ifs(path, std::ios::binary | std::ios::ate);
		if (!ifs)
		{
			outError = "Cannot open cached Octo bundle";
			return false;
		}
		const auto len = ifs.tellg();
		if (len <= 24 || (rec.size > 0 && static_cast<uint64_t>(len) != rec.size))
		{
			outError = "Octo bundle size mismatch";
			return false;
		}
		ifs.seekg(0);
		outBundle.resize(static_cast<size_t>(len));
		if (!ifs.read(reinterpret_cast<char*>(outBundle.data()), len))
		{
			outBundle.clear();
			outError = "Failed to read cached Octo bundle";
			return false;
		}
		if (!rec.md5.empty())
		{
			std::array<uint8_t, 16> digest{};
			if (!ComputeMd5(outBundle.data(), outBundle.size(), digest) ||
				HexLower(digest.data(), digest.size()) != rec.md5)
			{
				outBundle.clear();
				outError = "Octo bundle MD5 check failed";
				return false;
			}
		}
		if (!DeobfuscateOctoBundle(outBundle, rec.name, outError))
		{
			outBundle.clear();
			return false;
		}
		return true;
	}

	class PortraitCache
	{
		struct MemoryEntry
		{
			CachedFileSignature sig;
			std::string dataUri;
		};

		mutable std::mutex mutex_;
		std::string configRoot_;
		std::string gameRoot_;
		bool octoLoaded_{false};
		std::unordered_map<std::string, OctoBundleRecord> octoIndex_;
		std::unordered_map<std::string, MemoryEntry> memoryByCode_;
		std::deque<std::string> pendingQueue_;
		std::unordered_set<std::string> pendingSet_;
		std::unordered_set<std::string> missedCodes_;

		bool EnsureOctoIndexLocked()
		{
			if (octoLoaded_) return !octoIndex_.empty();
			octoLoaded_ = true;
			octoIndex_.clear();
			if (gameRoot_.empty()) return false;
			const auto manifestPath = DiscoverOctoManifestPath(MotionPathFromUtf8(gameRoot_));
			if (manifestPath.empty()) return false;
			std::ifstream ifs(manifestPath, std::ios::binary | std::ios::ate);
			if (!ifs) return false;
			const auto len = ifs.tellg();
			if (len <= 17) return false;
			ifs.seekg(0);
			std::vector<uint8_t> raw(static_cast<size_t>(len));
			if (!ifs.read(reinterpret_cast<char*>(raw.data()), len)) return false;
			std::vector<uint8_t> pb;
			std::string err;
			if (!DecryptOctoManifest(raw, pb, err)) return false;
			if (!ParseOctoAssetBundleIndex(pb, octoIndex_, true))
			{
				octoIndex_.clear();
				return false;
			}
			return !octoIndex_.empty();
		}

		bool TryLoadDiskLocked(const std::string& normCode, std::string& outDataUri)
		{
			outDataUri.clear();
			const auto path = PortraitCachePath(configRoot_, normCode);
			if (path.empty()) return false;

			const CachedFileSignature sig = StatCacheFile(path);
			if (!sig.exists || sig.size < 45 || sig.size > 16LL * 1024LL * 1024LL)
			{
				memoryByCode_.erase(normCode);
				return false;
			}

			if (const auto it = memoryByCode_.find(normCode);
				it != memoryByCode_.end() && it->second.sig == sig && !it->second.dataUri.empty())
			{
				outDataUri = it->second.dataUri;
				return true;
			}

			std::ifstream ifs(path, std::ios::binary | std::ios::ate);
			if (!ifs) return false;
			const auto len = ifs.tellg();
			if (len < 45 || len > 16LL * 1024LL * 1024LL) return false;
			ifs.seekg(0);
			std::vector<uint8_t> bytes(static_cast<size_t>(len));
			if (!ifs.read(reinterpret_cast<char*>(bytes.data()), len)) return false;
			if (!IsValidPngBytes(bytes))
			{
				memoryByCode_.erase(normCode);
				return false;
			}

			outDataUri = cover::ToDataUri(bytes, "image/png");
			if (outDataUri.empty()) return false;
			memoryByCode_[normCode] = {sig, outDataUri};
			missedCodes_.erase(normCode);
			return true;
		}

	public:
		void Configure(const std::string& configRootUtf8, const std::string& gameRootUtf8)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (configRoot_ != configRootUtf8)
			{
				configRoot_ = configRootUtf8;
				memoryByCode_.clear();
				missedCodes_.clear();
			}
			if (gameRoot_ != gameRootUtf8)
			{
				gameRoot_ = gameRootUtf8;
				octoLoaded_ = false;
				octoIndex_.clear();
				missedCodes_.clear();
			}
		}

		bool QueryOrEnqueue(std::string_view code, std::string& outDataUri, bool& outCached)
		{
			outDataUri.clear();
			outCached = false;
			const std::string norm = NormalizeCharacterCode(code);
			if (norm.empty()) return false;

			std::lock_guard<std::mutex> lock(mutex_);
			if (TryLoadDiskLocked(norm, outDataUri))
			{
				outCached = true;
				return true;
			}
			if (!missedCodes_.contains(norm) && !pendingSet_.contains(norm))
			{
				pendingSet_.insert(norm);
				pendingQueue_.push_back(norm);
			}
			return false;
		}

		void EnqueueIfUncached(std::string_view code)
		{
			const std::string norm = NormalizeCharacterCode(code);
			if (norm.empty()) return;
			std::lock_guard<std::mutex> lock(mutex_);
			if (missedCodes_.contains(norm) || pendingSet_.contains(norm)) return;
			const auto path = PortraitCachePath(configRoot_, norm);
			if (!path.empty() && StatCacheFile(path).exists) return;
			pendingSet_.insert(norm);
			pendingQueue_.push_back(norm);
		}

		bool PopNextPending(std::string& outNormCode)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			while (!pendingQueue_.empty())
			{
				std::string code = std::move(pendingQueue_.front());
				pendingQueue_.pop_front();
				pendingSet_.erase(code);
				std::string ignoredUri;
				if (TryLoadDiskLocked(code, ignoredUri)) continue;
				if (missedCodes_.contains(code)) continue;
				outNormCode = std::move(code);
				return true;
			}
			return false;
		}

		void MarkMissed(std::string_view code)
		{
			const std::string norm = NormalizeCharacterCode(code);
			if (norm.empty()) return;
			std::lock_guard<std::mutex> lock(mutex_);
			missedCodes_.insert(norm);
		}

		bool LookupOctoCandidate(
			std::string_view code,
			OctoBundleRecord& outRecord,
			std::string& outAssetName,
			std::filesystem::path& outGameRoot)
		{
			const std::string norm = NormalizeCharacterCode(code);
			if (norm.empty()) return false;
			std::lock_guard<std::mutex> lock(mutex_);
			if (!EnsureOctoIndexLocked()) return false;
			outGameRoot = MotionPathFromUtf8(gameRoot_);
			const auto candidates = PortraitAssetCandidates(norm);
			for (const auto& candidate : candidates)
			{
				const auto it = octoIndex_.find(candidate);
				if (it == octoIndex_.end()) continue;
				if (!ResolveOctoBundleFilePath(outGameRoot, it->second).empty())
				{
					outRecord = it->second;
					outAssetName = candidate;
					return true;
				}
			}
			return false;
		}

		bool StorePortraitPng(
			std::string_view code,
			const uint8_t* rawImageBytes,
			size_t rawSize,
			std::string& outDataUri,
			std::string& outError)
		{
			outDataUri.clear();
			outError.clear();
			const std::string norm = NormalizeCharacterCode(code);
			if (norm.empty() || !rawImageBytes || rawSize == 0)
			{
				outError = "Invalid portrait code or image payload";
				return false;
			}

			std::vector<uint8_t> encoded;
			std::string mime;
			if (!cover::ResizeAndEncodeCover(rawImageBytes, rawSize, 512, encoded, mime, /*forcePng=*/true) ||
				!IsValidPngBytes(encoded))
			{
				outError = "Failed to encode portrait PNG via WIC";
				return false;
			}

			std::lock_guard<std::mutex> lock(mutex_);
			const auto finalPath = PortraitCachePath(configRoot_, norm);
			if (finalPath.empty())
			{
				outError = "Portrait cache root not configured";
				return false;
			}
			std::error_code ec;
			std::filesystem::create_directories(finalPath.parent_path(), ec);
			if (ec)
			{
				outError = "Cannot create portraits directory: " + ec.message();
				return false;
			}

			auto tmpPath = finalPath;
			tmpPath += ".tmp";
			{
				std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
				if (!out)
				{
					outError = "Cannot open temp portrait file for writing";
					return false;
				}
				out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
				if (!out.good())
				{
					out.close();
					std::filesystem::remove(tmpPath, ec);
					outError = "Failed to write temp portrait file";
					return false;
				}
			}

#ifdef _WIN32
			if (!MoveFileExW(tmpPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				outError = "Failed to commit portrait PNG";
				std::filesystem::remove(tmpPath, ec);
				return false;
			}
#else
			std::filesystem::rename(tmpPath, finalPath, ec);
			if (ec)
			{
				outError = "Failed to commit portrait PNG: " + ec.message();
				std::filesystem::remove(tmpPath, ec);
				return false;
			}
#endif
			outDataUri = cover::ToDataUri(encoded, "image/png");
			memoryByCode_[norm] = {StatCacheFile(finalPath), outDataUri};
			missedCodes_.erase(norm);
			pendingSet_.erase(norm);
			return !outDataUri.empty();
		}

		// Unit-testable orchestration path mirroring the runtime main-thread tick:
		// resident Texture2D check -> Octo manifest & bundle de-obfuscation -> WIC 512px PNG cache.
		using ResidentExtractFn = std::function<bool(const std::string& assetName, std::vector<uint8_t>& outRawPng)>;
		using BundleExtractFn = std::function<bool(const std::string& assetName, const std::vector<uint8_t>& bundleBytes, std::vector<uint8_t>& outRawPng)>;

		bool ProcessOnePending(const ResidentExtractFn& extractResident, const BundleExtractFn& extractBundle)
		{
			std::string code;
			if (!PopNextPending(code)) return false;

			const auto candidates = PortraitAssetCandidates(code);
			if (extractResident)
			{
				for (const auto& candidate : candidates)
				{
					std::vector<uint8_t> rawPng;
					if (extractResident(candidate, rawPng) && !rawPng.empty())
					{
						std::string uri, err;
						if (StorePortraitPng(code, rawPng.data(), rawPng.size(), uri, err)) return true;
					}
				}
			}

			OctoBundleRecord rec;
			std::string assetName;
			std::filesystem::path gameRoot;
			if (extractBundle && LookupOctoCandidate(code, rec, assetName, gameRoot))
			{
				std::vector<uint8_t> bundleBytes;
				std::string err;
				if (LoadAndDeobfuscateOctoBundle(gameRoot, rec, bundleBytes, err))
				{
					std::vector<uint8_t> rawPng;
					if (extractBundle(assetName, bundleBytes, rawPng) && !rawPng.empty())
					{
						std::string uri;
						if (StorePortraitPng(code, rawPng.data(), rawPng.size(), uri, err)) return true;
					}
				}
			}

			MarkMissed(code);
			return false;
		}
	};
} // namespace GakuMotion::portrait
