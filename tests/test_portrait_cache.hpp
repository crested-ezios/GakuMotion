#pragma once

#include "GkPortraitCache.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace gk_portrait_test
{
	inline std::vector<uint8_t> MakeSyntheticRgbaPng(uint32_t w, uint32_t h, bool withAlpha)
	{
		std::vector<uint8_t> out;
#ifdef _WIN32
		if (w == 0 || h == 0) return out;
		cover::detail::CoInitGuard comGuard;
		cover::detail::SimpleComPtr<IWICImagingFactory> factory;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		                            IID_PPV_ARGS(factory.Put()))))
		{
			return out;
		}
		const uint32_t stride = w * 4u;
		std::vector<uint8_t> bgra(static_cast<size_t>(stride) * h);
		for (uint32_t y = 0; y < h; ++y)
		{
			for (uint32_t x = 0; x < w; ++x)
			{
				const size_t idx = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
				bgra[idx + 0] = static_cast<uint8_t>((x * 255u) / (std::max)(1u, w - 1u));
				bgra[idx + 1] = static_cast<uint8_t>((y * 255u) / (std::max)(1u, h - 1u));
				bgra[idx + 2] = 180u;
				bgra[idx + 3] = (withAlpha && x < w / 3u) ? 96u : 255u;
			}
		}
		cover::detail::SimpleComPtr<IWICBitmap> bitmap;
		if (FAILED(factory->CreateBitmapFromMemory(
				w, h, GUID_WICPixelFormat32bppBGRA, stride, static_cast<UINT>(bgra.size()),
				bgra.data(), bitmap.Put())))
		{
			return out;
		}
		cover::detail::SimpleComPtr<IStream> outStream;
		if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, outStream.Put()))) return out;
		cover::detail::SimpleComPtr<IWICBitmapEncoder> encoder;
		if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put()))) return out;
		if (FAILED(encoder->Initialize(outStream.Get(), WICBitmapEncoderNoCache))) return out;
		cover::detail::SimpleComPtr<IWICBitmapFrameEncode> frame;
		if (FAILED(encoder->CreateNewFrame(frame.Put(), nullptr))) return out;
		if (FAILED(frame->Initialize(nullptr))) return out;
		if (FAILED(frame->SetSize(w, h))) return out;
		WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
		if (FAILED(frame->SetPixelFormat(&fmt))) return out;
		if (FAILED(frame->WriteSource(bitmap.Get(), nullptr))) return out;
		if (FAILED(frame->Commit())) return out;
		if (FAILED(encoder->Commit())) return out;

		STATSTG stat{};
		if (FAILED(outStream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0) return out;
		out.resize(static_cast<size_t>(stat.cbSize.QuadPart));
		LARGE_INTEGER zero{};
		outStream->Seek(zero, STREAM_SEEK_SET, nullptr);
		ULONG readBytes = 0;
		if (FAILED(outStream->Read(out.data(), static_cast<ULONG>(out.size()), &readBytes)) ||
			readBytes != out.size())
		{
			out.clear();
		}
#else
		(void)w;
		(void)h;
		(void)withAlpha;
#endif
		return out;
	}

	inline uint32_t ReadU32BE(const uint8_t* p)
	{
		return (static_cast<uint32_t>(p[0]) << 24) |
			(static_cast<uint32_t>(p[1]) << 16) |
			(static_cast<uint32_t>(p[2]) << 8) |
			static_cast<uint32_t>(p[3]);
	}

	inline void AppendVarint(std::vector<uint8_t>& buf, uint64_t val)
	{
		while (val >= 0x80)
		{
			buf.push_back(static_cast<uint8_t>((val & 0x7Fu) | 0x80u));
			val >>= 7;
		}
		buf.push_back(static_cast<uint8_t>(val));
	}

	inline void AppendProtoBytes(std::vector<uint8_t>& buf, uint32_t fieldNum, const std::vector<uint8_t>& data)
	{
		AppendVarint(buf, (static_cast<uint64_t>(fieldNum) << 3) | 2u);
		AppendVarint(buf, data.size());
		buf.insert(buf.end(), data.begin(), data.end());
	}

	inline void AppendProtoString(std::vector<uint8_t>& buf, uint32_t fieldNum, std::string_view s)
	{
		AppendVarint(buf, (static_cast<uint64_t>(fieldNum) << 3) | 2u);
		AppendVarint(buf, s.size());
		buf.insert(buf.end(), s.begin(), s.end());
	}

	inline void AppendProtoVarint(std::vector<uint8_t>& buf, uint32_t fieldNum, uint64_t v)
	{
		AppendVarint(buf, (static_cast<uint64_t>(fieldNum) << 3) | 0u);
		AppendVarint(buf, v);
	}

	inline std::vector<uint8_t> BuildSyntheticBundle(std::string_view bundleName)
	{
		std::vector<uint8_t> buf;
		const char magic[] = "UnityFS";
		buf.insert(buf.end(), magic, magic + 8); // includes trailing NUL
		buf.push_back(0); buf.push_back(0); buf.push_back(0); buf.push_back(8); // format
		const char ver1[] = "5.x.x";
		buf.insert(buf.end(), ver1, ver1 + sizeof(ver1));
		const char ver2[] = "6000.0.77f1";
		buf.insert(buf.end(), ver2, ver2 + sizeof(ver2));
		const size_t totalSizeOffset = buf.size();
		buf.resize(buf.size() + 8, 0);
		for (int i = 0; i < 300; ++i) buf.push_back(static_cast<uint8_t>((i * 17) & 0xFF));
		const uint64_t totalSize = buf.size();
		for (int b = 0; b < 8; ++b)
			buf[totalSizeOffset + b] = static_cast<uint8_t>((totalSize >> ((7 - b) * 8)) & 0xFF);

		// Obfuscate with the same symmetric interleaved XOR cipher using non-zero salt c0.
		const size_t L = bundleName.size();
		const uint8_t c0 = 0x5Au;
		const size_t limit = (std::min)(size_t{256}, buf.size());
		for (size_t j = 0; j < limit; ++j)
		{
			const size_t i = j / 2;
			const uint8_t k = (j % 2 == 0)
				? static_cast<uint8_t>(static_cast<uint8_t>(bundleName[i % L]) ^ c0)
				: static_cast<uint8_t>(static_cast<uint8_t>(bundleName[L - 1 - (i % L)]) ^ static_cast<uint8_t>(c0 ^ 0xFFu));
			buf[j] ^= k;
		}
		return buf;
	}

	inline bool EncryptSyntheticManifest(const std::vector<uint8_t>& pbBytes, std::vector<uint8_t>& outEncrypted)
	{
		outEncrypted.clear();
#ifndef _WIN32
		(void)pbBytes;
		return false;
#else
		std::array<uint8_t, 16> digest{};
		if (!portrait::ComputeMd5(pbBytes.data(), pbBytes.size(), digest)) return false;
		std::vector<uint8_t> plaintext;
		plaintext.reserve(16 + pbBytes.size());
		plaintext.insert(plaintext.end(), digest.begin(), digest.end());
		plaintext.insert(plaintext.end(), pbBytes.begin(), pbBytes.end());

		static constexpr char kKeySeed[] = "1nuv9td1bw1udefk";
		static constexpr char kIvSeed[] = "LvAUtf+tnz";
		std::array<uint8_t, 16> key{};
		std::array<uint8_t, 16> iv{};
		if (!portrait::ComputeMd5(reinterpret_cast<const uint8_t*>(kKeySeed), sizeof(kKeySeed) - 1, key) ||
			!portrait::ComputeMd5(reinterpret_cast<const uint8_t*>(kIvSeed), sizeof(kIvSeed) - 1, iv))
			return false;

		BCRYPT_ALG_HANDLE hAlg = nullptr;
		if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return false;
		if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
				reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
				sizeof(BCRYPT_CHAIN_MODE_CBC), 0) < 0)
		{
			BCryptCloseAlgorithmProvider(hAlg, 0);
			return false;
		}
		BCRYPT_KEY_HANDLE hKey = nullptr;
		if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, key.data(), static_cast<ULONG>(key.size()), 0) < 0)
		{
			BCryptCloseAlgorithmProvider(hAlg, 0);
			return false;
		}
		std::array<uint8_t, 16> ivCopy = iv;
		ULONG cbCipher = 0;
		BCryptEncrypt(hKey, plaintext.data(), static_cast<ULONG>(plaintext.size()), nullptr,
			ivCopy.data(), static_cast<ULONG>(ivCopy.size()), nullptr, 0, &cbCipher, BCRYPT_BLOCK_PADDING);
		std::vector<uint8_t> cipher(cbCipher);
		ivCopy = iv;
		const NTSTATUS st = BCryptEncrypt(hKey, plaintext.data(), static_cast<ULONG>(plaintext.size()), nullptr,
			ivCopy.data(), static_cast<ULONG>(ivCopy.size()), cipher.data(), cbCipher, &cbCipher, BCRYPT_BLOCK_PADDING);
		BCryptDestroyKey(hKey);
		BCryptCloseAlgorithmProvider(hAlg, 0);
		if (st < 0) return false;
		cipher.resize(cbCipher);
		outEncrypted.reserve(1 + cipher.size());
		outEncrypted.push_back(0x01);
		outEncrypted.insert(outEncrypted.end(), cipher.begin(), cipher.end());
		return true;
#endif
	}
} // namespace gk_portrait_test

static void TestSceneActorMetadata()
{
	BeginTest("scene actor character, displayName and shortName metadata");
	bool allShortNamesPresent = true;
	for (const auto& entry : kIdolNames)
	{
		if (!entry.shortChinese || !entry.shortChinese[0] || !LookupIdolShortName(entry.code))
		{
			allShortNamesPresent = false;
			break;
		}
	}
	Check(allShortNamesPresent, "all kIdolNames entries define non-empty short names");
	Check(std::string_view(LookupIdolShortName("hski")) == "咲季" &&
		std::string_view(LookupIdolShortName("HSKI")) == "咲季" &&
		std::string_view(LookupIdolShortName("ttmr")) == "手毬" &&
		std::string_view(LookupIdolShortName("nasr")) == "亚纱里" &&
		LookupIdolShortName("unknown") == nullptr,
		"LookupIdolShortName resolves playable idols + nasr case-insensitively");

	GuiSceneActor legacyPositionalInit{101, "compat · 1", true};
	Check(legacyPositionalInit.token == 101 && legacyPositionalInit.character.empty() &&
		legacyPositionalInit.displayName.empty() && legacyPositionalInit.shortName.empty(),
		"GuiSceneActor preserves 3-arg positional aggregate initialization compatibility");

	void* a = reinterpret_cast<void*>(0x8100);
	void* b = reinterpret_cast<void*>(0x8200);
	void* c = reinterpret_cast<void*>(0x8300);
	void* d = reinterpret_cast<void*>(0x8400);

	Core core;
	core.SetControlActive(true);
	core.PublishSceneActors({
		{a, 101, "hski"},
		{b, 102, "custom_npc", "自定义NPC"},
		{c, 103, "", "", "", "StageStaff"},
		{d, 104, ""}
	});
	const auto ctx = core.BuildGuiContext();
	Check(ctx.sceneActors.size() == 4, "all four actors published with metadata");
	if (ctx.sceneActors.size() == 4)
	{
		Check(ctx.sceneActors[0].character == "hski" &&
			ctx.sceneActors[0].displayName == "花海咲季" &&
			ctx.sceneActors[0].shortName == "咲季" &&
			ctx.sceneActors[0].label == "花海咲季 (hski) · 1",
			"known idol populates character, displayName, shortName while keeping legacy label");
		Check(ctx.sceneActors[1].character == "custom_npc" &&
			ctx.sceneActors[1].displayName == "自定义NPC" &&
			ctx.sceneActors[1].shortName == "自定义NPC",
			"custom adapter actor falls back to displayName for shortName");
		Check(ctx.sceneActors[2].character.empty() &&
			ctx.sceneActors[2].displayName == "StageStaff" &&
			ctx.sceneActors[2].shortName == "StageStaff",
			"preset-only actor propagates presetName to displayName and shortName");
		Check(ctx.sceneActors[3].character.empty() &&
			ctx.sceneActors[3].displayName.empty() &&
			ctx.sceneActors[3].shortName == "未识别角色" &&
			ctx.sceneActors[3].label.rfind("未识别角色", 0) == 0,
			"empty unknown actor falls back safely without crashing");
	}
}

static void TestPortraitCacheUnit()
{
	BeginTest("portrait cache sanitization, 4-tier fallback, WIC 512px PNG & synthetic Octo roundtrip");
	namespace fs = std::filesystem;

	// 1. Code sanitization & path traversal guard
	Check(portrait::IsSafeCharacterCode("hski") && portrait::IsSafeCharacterCode("myu0") &&
		portrait::IsSafeCharacterCode("custom_npc-1") && portrait::NormalizeCharacterCode("HSKI") == "hski",
		"safe codes normalize to lowercase");
	Check(!portrait::IsSafeCharacterCode("") &&
		!portrait::IsSafeCharacterCode("../hski") &&
		!portrait::IsSafeCharacterCode("a/b") &&
		!portrait::IsSafeCharacterCode("a\\b") &&
		!portrait::IsSafeCharacterCode("c:test") &&
		!portrait::IsSafeCharacterCode("hski bad") &&
		portrait::PortraitCachePath("C:/cfg", "../evil").empty(),
		"path traversal and malformed codes are rejected");

	// 2. 4-tier asset fallback order
	const auto cands = portrait::PortraitAssetCandidates("hski");
	Check(cands[0] == "img_chr_hski_00-bustup" &&
		cands[1] == "img_chr_hski_00-audition" &&
		cands[2] == "img_adv_speaker_hski_000-000" &&
		cands[3] == "img_chr_hski_00-thumb-circle",
		"4-tier portrait candidate order matches bustup -> audition -> speaker -> thumb-circle");

	const fs::path root = fs::temp_directory_path() / "gk_portrait_cache_unit";
	std::error_code ec;
	fs::remove_all(root, ec);
	const fs::path cfgRoot = root / "config";
	const fs::path gameRoot = root / "game";
	fs::create_directories(cfgRoot, ec);

	// 3. Synthetic Octo manifest (with higher-revision selection & negative checks) + XOR bundle
	const std::string bustupName = "img_chr_hski_00-bustup";
	const uint64_t bustupId = 4242;
	auto cleanOrObfBundle = gk_portrait_test::BuildSyntheticBundle(bustupName);
	std::array<uint8_t, 16> bundleMd5{};
	Check(portrait::ComputeMd5(cleanOrObfBundle.data(), cleanOrObfBundle.size(), bundleMd5),
		"MD5 computed for synthetic bundle");
	const std::string bundleMd5Hex = portrait::HexLower(bundleMd5.data(), bundleMd5.size());
	const fs::path bundleDiskPath = gameRoot / "octo" / "v1" / "400" /
		std::to_string(bustupId % 10) / portrait::UpperHexAscii("A" + std::to_string(bustupId)) / bundleMd5Hex;
	fs::create_directories(bundleDiskPath.parent_path(), ec);
	{
		std::ofstream out(bundleDiskPath, std::ios::binary);
		out.write(reinterpret_cast<const char*>(cleanOrObfBundle.data()), static_cast<std::streamsize>(cleanOrObfBundle.size()));
	}

	std::vector<uint8_t> sub1;
	gk_portrait_test::AppendProtoVarint(sub1, 1, 100);
	gk_portrait_test::AppendProtoString(sub1, 3, "other_unused_bundle");
	gk_portrait_test::AppendProtoVarint(sub1, 4, 123);
	gk_portrait_test::AppendProtoString(sub1, 10, "00000000000000000000000000000000");

	std::vector<uint8_t> sub2;
	gk_portrait_test::AppendProtoVarint(sub2, 1, bustupId);
	gk_portrait_test::AppendProtoString(sub2, 3, bustupName);
	gk_portrait_test::AppendProtoVarint(sub2, 4, cleanOrObfBundle.size());
	gk_portrait_test::AppendProtoString(sub2, 10, bundleMd5Hex);

	std::vector<uint8_t> pb;
	gk_portrait_test::AppendProtoBytes(pb, 2, sub1);
	gk_portrait_test::AppendProtoBytes(pb, 2, sub2);

	std::vector<uint8_t> encManifest;
	Check(gk_portrait_test::EncryptSyntheticManifest(pb, encManifest) && !encManifest.empty(),
		"synthetic manifest encrypted with Win32 CNG AES-128-CBC");

	// Write corrupt old rev 705100 and valid new rev 705200 -> DiscoverOctoManifestPath must pick 705200
	WriteTextFile(gameRoot / "octo" / "pdb" / "400" / "705100" / "octocacheevai", "bad-old-rev");
	const fs::path latestManifest = gameRoot / "octo" / "pdb" / "400" / "705200" / "octocacheevai";
	fs::create_directories(latestManifest.parent_path(), ec);
	{
		std::ofstream out(latestManifest, std::ios::binary);
		out.write(reinterpret_cast<const char*>(encManifest.data()), static_cast<std::streamsize>(encManifest.size()));
	}
	Check(portrait::DiscoverOctoManifestPath(gameRoot) == latestManifest,
		"DiscoverOctoManifestPath selects highest numeric Octo revision");

	// Negative manifest decryption tests
	std::vector<uint8_t> pbOut;
	std::string decErr;
	auto badMagic = encManifest;
	badMagic[0] = 0x02;
	Check(!portrait::DecryptOctoManifest(badMagic, pbOut, decErr), "manifest decrypt rejects wrong header byte");
	auto badAlign = encManifest;
	badAlign.pop_back();
	Check(!portrait::DecryptOctoManifest(badAlign, pbOut, decErr), "manifest decrypt rejects unaligned ciphertext");
	auto badCipher = encManifest;
	badCipher[badCipher.size() / 2] ^= 0x55;
	Check(!portrait::DecryptOctoManifest(badCipher, pbOut, decErr), "manifest decrypt rejects corrupted ciphertext/MD5");

	// 4. PortraitCache WIC 512px resize + alpha preservation + disk persistence + resident-first priority + live deletion
	const auto bigPng = gk_portrait_test::MakeSyntheticRgbaPng(1024, 1024, true);
	Check(!bigPng.empty() && portrait::IsValidPngBytes(bigPng), "WIC built 1024x1024 transparent test PNG");

	portrait::PortraitCache cache;
	cache.Configure(MotionPathToUtf8(cfgRoot), MotionPathToUtf8(gameRoot));

	std::string uri;
	bool isCached = false;
	Check(!cache.QueryOrEnqueue("HSKI", uri, isCached) && !isCached && uri.empty(),
		"uncached query returns miss and enqueues character");

	int bundleCalls = 0;
	const bool extracted = cache.ProcessOnePending(
		nullptr,
		[&](const std::string& assetName, const std::vector<uint8_t>& bundleBytes, std::vector<uint8_t>& outRawPng) -> bool
		{
			++bundleCalls;
			if (assetName == bustupName && bundleBytes.size() >= 24 &&
				std::memcmp(bundleBytes.data(), "UnityFS\0", 8) == 0)
			{
				outRawPng = bigPng;
				return true;
			}
			return false;
		});
	Check(extracted && bundleCalls == 1, "ProcessOnePending decrypts Octo index, de-obfuscates bundle, and stores PNG");
	const fs::path hskiPngPath = portrait::PortraitCachePath(MotionPathToUtf8(cfgRoot), "hski");
	Check(fs::is_regular_file(hskiPngPath), "512px PNG saved at <configRoot>/portraits/hski.png");

	// Hit after extraction must not re-enqueue or re-extract
	Check(cache.QueryOrEnqueue("hski", uri, isCached) && isCached && uri.rfind("data:image/png;base64,", 0) == 0,
		"cached portrait returns data:image/png;base64 Data URI");
	std::string decMime;
	std::vector<uint8_t> decBytes;
	Check(cover::DecodeDataUri(uri, decMime, decBytes) && decMime == "image/png" &&
		portrait::IsValidPngBytes(decBytes) &&
		gk_portrait_test::ReadU32BE(decBytes.data() + 16) == 512 &&
		gk_portrait_test::ReadU32BE(decBytes.data() + 20) == 512,
		"1024x1024 portrait downscaled cleanly to <=512px PNG with IHDR dimensions 512x512");
	Check(!cache.ProcessOnePending(nullptr, nullptr) && bundleCalls == 1,
		"cache hit short-circuits without calling extraction again");

	// Live deletion from disk must yield miss
	fs::remove(hskiPngPath, ec);
	Check(!cache.QueryOrEnqueue("hski", uri, isCached) && !isCached && uri.empty(),
		"deleting PNG from disk causes immediate live cache miss");

	// Resident Texture2D extractor priority beats Octo bundle extractor
	int resCalls = 0;
	int bunCallsAfterRes = 0;
	const bool resWon = cache.ProcessOnePending(
		[&](const std::string& assetName, std::vector<uint8_t>& outRawPng) -> bool
		{
			++resCalls;
			if (assetName == bustupName) { outRawPng = bigPng; return true; }
			return false;
		},
		[&](const std::string&, const std::vector<uint8_t>&, std::vector<uint8_t>&) -> bool
		{
			++bunCallsAfterRes;
			return false;
		});
	Check(resWon && resCalls == 1 && bunCallsAfterRes == 0,
		"resident Texture2D extraction short-circuits before Octo bundle lookup");

	// Corrupt cached PNG on disk is rejected safely
	const fs::path badPngPath = portrait::PortraitCachePath(MotionPathToUtf8(cfgRoot), "ttmr");
	{
		std::ofstream out(badPngPath, std::ios::binary);
		const uint8_t partialPng[50]{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
		out.write(reinterpret_cast<const char*>(partialPng), sizeof(partialPng));
	}
	Check(!cache.QueryOrEnqueue("ttmr", uri, isCached) && !isCached && uri.empty(),
		"corrupt truncated PNG on disk is rejected safely");
	// Drain queued "ttmr" before testing isolated negative-miss suppression on "jkno".
	Check(!cache.ProcessOnePending(nullptr, nullptr), "corrupt-PNG miss drains cleanly");

	// Negative miss suppression (non-existent character across all 4 tiers)
	int missChecks = 0;
	Check(!cache.QueryOrEnqueue("jkno", uri, isCached), "unknown jkno enqueued once");
	Check(!cache.ProcessOnePending(
		[&](const std::string&, std::vector<uint8_t>&) { ++missChecks; return false; },
		nullptr) && missChecks == 4,
		"all 4 asset fallback candidates attempted before marking missed");
	Check(!cache.QueryOrEnqueue("jkno", uri, isCached) &&
		!cache.ProcessOnePending(
			[&](const std::string&, std::vector<uint8_t>&) { ++missChecks; return false; },
			nullptr) && missChecks == 4,
		"missed character does not trigger repeated extraction on subsequent queries");

	fs::remove_all(root, ec);
}

static void TestOfflineOctoRealCache(const std::filesystem::path& gameRoot, bool required)
{
	BeginTest("offline Octo real manifest decryption & UnityFS bundle de-obfuscation");
	const auto manifestPath = portrait::DiscoverOctoManifestPath(gameRoot);
	if (manifestPath.empty())
	{
		if (required)
		{
			Check(false, "Octo manifest octocacheevai missing under " + MotionPathToUtf8(gameRoot));
		}
		else
		{
			std::printf("  [SKIP] real Octo cache unavailable at %s\n", MotionPathToUtf8(gameRoot).c_str());
		}
		return;
	}

	std::ifstream ifs(manifestPath, std::ios::binary | std::ios::ate);
	Check(ifs.is_open(), "opened real octocacheevai read-only");
	const auto sz = ifs.tellg();
	if (!ifs || sz <= 17) return;
	ifs.seekg(0);
	std::vector<uint8_t> raw(static_cast<size_t>(sz));
	ifs.read(reinterpret_cast<char*>(raw.data()), sz);

	std::vector<uint8_t> pb;
	std::string err;
	Check(portrait::DecryptOctoManifest(raw, pb, err), "real octocacheevai decrypted + MD5 verified: " + err);
	std::unordered_map<std::string, portrait::OctoBundleRecord> index;
	Check(portrait::ParseOctoAssetBundleIndex(pb, index, true) && !index.empty(),
		"parsed Octo AssetBundle portrait index entries (" + std::to_string(index.size()) + ")");

	portrait::PortraitCache cache;
	cache.Configure("", MotionPathToUtf8(gameRoot));

	// All 13 playable idols + nasr bustup + hkne audition + krnh speaker fallback
	const std::array<const char*, 16> codes = {
		"amao", "atbm", "fktn", "hmsz", "hrnm", "hski", "hume", "jsna",
		"kcna", "kllj", "shro", "ssmk", "ttmr", "nasr", "hkne", "krnh"
	};
	for (const char* code : codes)
	{
		portrait::OctoBundleRecord rec;
		std::string assetName;
		std::filesystem::path resolvedRoot;
		const bool found = cache.LookupOctoCandidate(code, rec, assetName, resolvedRoot);
		Check(found, std::string("Octo fallback resolved cached bundle for ") + code + " -> " + assetName);
		if (found)
		{
			std::vector<uint8_t> bundleBytes;
			std::string loadErr;
			Check(portrait::LoadAndDeobfuscateOctoBundle(gameRoot, rec, bundleBytes, loadErr) &&
				bundleBytes.size() >= 24 && std::memcmp(bundleBytes.data(), "UnityFS\0", 8) == 0,
				std::string("de-obfuscated valid UnityFS bundle for ") + assetName + " (" + loadErr + ")");
		}
	}
}
