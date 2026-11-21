// E2E-3 Test Harness: Portrait Cache (F5), Scene Actor Metadata (F6),
// and WebView2 Bridge C++ Contracts (F13) + Offline Octo Verification.
//
// Strictly opaque-box; compiles cleanly with C++20 /W3 before and after M2/M3/M4 merge
// via __has_include, C++20 requires-probes, and runtime motion.hello capability checks.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <map>
#include "GkLog.hpp"
#include "GakuMotion.hpp"
#include "GkWicCover.hpp"
#include "GkWebPanelBridge.hpp"
#include "GkSceneActors.hpp"

#if defined(__has_include) && __has_include("GkPortraitCache.hpp")
#  define GK_E2E_HAS_PORTRAIT_CACHE 1
#  include "GkPortraitCache.hpp"
#else
#  define GK_E2E_HAS_PORTRAIT_CACHE 0
#endif

#if defined(__has_include) && __has_include("GkStageFormation.hpp")
#  define GK_E2E_HAS_STAGE_FORMATION 1
#  include "GkStageFormation.hpp"
#else
#  define GK_E2E_HAS_STAGE_FORMATION 0
#endif

#include "e2e_report.hpp"
#include "vmd_fixture_gen.hpp"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

GK_E2E_DEFINE_LOG_SINK()

namespace {

using namespace GakuMotion;
using namespace GakuMotion::webpanel;
namespace fs = std::filesystem;

// -----------------------------------------------------------------------------
// Temp Directory RAII & Environment Helpers
// -----------------------------------------------------------------------------
fs::path GetBaseBuildDir() {
    if (const char* env = std::getenv("GKE2E_BUILD_DIR"); env && env[0]) {
        return gke2e::PathFromUtf8(env);
    }
    return fs::current_path();
}

fs::path GetGameRoot() {
    if (const char* env = std::getenv("GAKUMOTION_GAME_ROOT"); env && env[0]) {
        return gke2e::PathFromUtf8(env);
    }
    return fs::path(L"f:\\Games\\gakumas");
}

fs::path GetOctoExtractedDir() {
    if (const char* env = std::getenv("GKE2E_OCTO_EXTRACTED_DIR"); env && env[0]) {
        return gke2e::PathFromUtf8(env);
    }
    return GetBaseBuildDir() / "octo_extracted";
}

std::string Utf8FromPath(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

struct TempDirGuard {
    fs::path path;
    explicit TempDirGuard(std::string_view tag) {
        path = GetBaseBuildDir() / (std::string("tmp_") + std::string(tag));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDirGuard() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::vector<uint8_t> ReadFileBytes(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs.is_open()) return {};
    ifs.seekg(0, std::ios::end);
    const auto sz = ifs.tellg();
    if (sz <= 0) return {};
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), sz)) return {};
    return buf;
}

bool WriteFileBytes(const fs::path& p, const std::vector<uint8_t>& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;
    if (!data.empty()) {
        ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return ofs.good();
}

// -----------------------------------------------------------------------------
// Pure C++ Synthetic RGBA PNG Generator (via Win32 WIC) & PNG IHDR Inspector
// -----------------------------------------------------------------------------
struct PngInfo {
    bool valid{false};
    uint32_t width{0};
    uint32_t height{0};
    uint8_t bitDepth{0};
    uint8_t colorType{0};
};

PngInfo InspectPngHeader(const std::vector<uint8_t>& bytes) {
    PngInfo info;
    if (bytes.size() < 26) return info;
    static constexpr uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(bytes.data(), kSig, 8) != 0) return info;
    if (std::memcmp(bytes.data() + 12, "IHDR", 4) != 0) return info;
    auto readU32BE = [&](size_t off) -> uint32_t {
        return (static_cast<uint32_t>(bytes[off]) << 24) |
               (static_cast<uint32_t>(bytes[off + 1]) << 16) |
               (static_cast<uint32_t>(bytes[off + 2]) << 8) |
               static_cast<uint32_t>(bytes[off + 3]);
    };
    info.width = readU32BE(16);
    info.height = readU32BE(20);
    info.bitDepth = bytes[24];
    info.colorType = bytes[25];
    info.valid = info.width > 0 && info.height > 0;
    return info;
}

std::vector<uint8_t> MakeSyntheticRgbaPng(uint32_t w, uint32_t h, bool withTransparency) {
    std::vector<uint8_t> out;
    if (w == 0 || h == 0) return out;

    cover::detail::CoInitGuard comGuard;
    cover::detail::SimpleComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.Put())))) {
        return out;
    }

    const uint32_t stride = w * 4u;
    std::vector<uint8_t> bgra(static_cast<size_t>(stride) * h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t idx = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
            bgra[idx + 0] = static_cast<uint8_t>((x * 255u) / (std::max)(1u, w - 1u));
            bgra[idx + 1] = static_cast<uint8_t>((y * 255u) / (std::max)(1u, h - 1u));
            bgra[idx + 2] = static_cast<uint8_t>(((x + y) * 127u) / (std::max)(1u, w + h - 2u));
            if (withTransparency && (x < w / 4u || y < h / 4u)) {
                bgra[idx + 3] = 64u;
            } else {
                bgra[idx + 3] = 255u;
            }
        }
    }

    cover::detail::SimpleComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(
            w, h, GUID_WICPixelFormat32bppBGRA, stride, static_cast<UINT>(bgra.size()),
            bgra.data(), bitmap.Put()))) {
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

    HGLOBAL hGlobal = nullptr;
    if (FAILED(GetHGlobalFromStream(outStream.Get(), &hGlobal)) || !hGlobal) return out;
    STATSTG stat{};
    if (FAILED(outStream->Stat(&stat, STATFLAG_NONAME))) return out;
    const size_t sz = static_cast<size_t>(stat.cbSize.QuadPart);
    const void* locked = GlobalLock(hGlobal);
    if (!locked || sz == 0) return out;
    out.assign(static_cast<const uint8_t*>(locked), static_cast<const uint8_t*>(locked) + sz);
    GlobalUnlock(hGlobal);
    return out;
}

bool PngHasNonOpaqueAlpha(const std::vector<uint8_t>& pngBytes) {
    if (pngBytes.empty()) return false;
    cover::detail::CoInitGuard comGuard;
    cover::detail::SimpleComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.Put())))) {
        return false;
    }
    cover::detail::SimpleComPtr<IStream> inStream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, inStream.Put()))) return false;
    ULONG written = 0;
    if (FAILED(inStream->Write(pngBytes.data(), static_cast<ULONG>(pngBytes.size()), &written)) ||
        written != pngBytes.size()) {
        return false;
    }
    LARGE_INTEGER zero{};
    if (FAILED(inStream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    cover::detail::SimpleComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(inStream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                              decoder.Put()))) {
        return false;
    }
    cover::detail::SimpleComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.Put()))) return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;
    return cover::detail::HasNonOpaqueAlpha(factory.Get(), frame.Get(), w, h);
}

// -----------------------------------------------------------------------------
// Win32 CNG (bcrypt.lib) MD5 + AES-128-CBC + Protobuf Octo Manifest & XOR
// -----------------------------------------------------------------------------
bool ComputeMd5(const uint8_t* data, size_t size, std::array<uint8_t, 16>& out) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_MD5_ALGORITHM, nullptr, 0))) {
        return false;
    }
    bool ok = false;
    if (NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0))) {
        if (size == 0 || NT_SUCCESS(BCryptHashData(hHash, const_cast<PUCHAR>(data),
                                                   static_cast<ULONG>(size), 0))) {
            ok = NT_SUCCESS(BCryptFinishHash(hHash, out.data(), 16, 0));
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

std::string HexLower(const std::array<uint8_t, 16>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (uint8_t b : digest) {
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

std::string UpperHexAscii(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 2);
    for (unsigned char c : text) {
        out.push_back(kHex[(c >> 4) & 0xF]);
        out.push_back(kHex[c & 0xF]);
    }
    return out;
}

bool DecryptOctoManifestBytes(const std::vector<uint8_t>& raw, std::vector<uint8_t>& outPb, std::string& errOut) {
    outPb.clear();
    errOut.clear();
    if (raw.size() < 17 || raw[0] != 0x01) {
        errOut = "Invalid octocacheevai header or length";
        return false;
    }
    const size_t cipherLen = raw.size() - 1;
    if ((cipherLen % 16u) != 0) {
        errOut = "Ciphertext length not aligned to AES block size";
        return false;
    }

    static constexpr char kKeySeed[] = "1nuv9td1bw1udefk";
    static constexpr char kIvSeed[] = "LvAUtf+tnz";
    std::array<uint8_t, 16> key{};
    std::array<uint8_t, 16> iv{};
    if (!ComputeMd5(reinterpret_cast<const uint8_t*>(kKeySeed), sizeof(kKeySeed) - 1, key) ||
        !ComputeMd5(reinterpret_cast<const uint8_t*>(kIvSeed), sizeof(kIvSeed) - 1, iv)) {
        errOut = "CNG MD5 key/IV derivation failed";
        return false;
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0))) {
        errOut = "BCryptOpenAlgorithmProvider(AES) failed";
        return false;
    }
    bool ok = false;
    if (NT_SUCCESS(BCryptSetProperty(
            hAlg, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
            sizeof(BCRYPT_CHAIN_MODE_CBC), 0))) {
        if (NT_SUCCESS(BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, key.data(), 16, 0))) {
            std::vector<uint8_t> ivWork(iv.begin(), iv.end());
            std::vector<uint8_t> plain(cipherLen);
            ULONG plainLen = 0;
            const NTSTATUS st = BCryptDecrypt(
                hKey,
                const_cast<PUCHAR>(raw.data() + 1),
                static_cast<ULONG>(cipherLen),
                nullptr,
                ivWork.data(),
                16,
                plain.data(),
                static_cast<ULONG>(plain.size()),
                &plainLen,
                BCRYPT_BLOCK_PADDING);
            if (NT_SUCCESS(st) && plainLen >= 16) {
                plain.resize(plainLen);
                std::array<uint8_t, 16> actualMd5{};
                if (ComputeMd5(plain.data() + 16, plain.size() - 16, actualMd5) &&
                    std::memcmp(actualMd5.data(), plain.data(), 16) == 0) {
                    outPb.assign(plain.begin() + 16, plain.end());
                    ok = true;
                } else {
                    errOut = "Decrypted Octo manifest MD5 checksum mismatch";
                }
            } else {
                errOut = "BCryptDecrypt failed (corrupt ciphertext or PKCS7 padding)";
            }
            BCryptDestroyKey(hKey);
        } else {
            errOut = "BCryptGenerateSymmetricKey failed";
        }
    } else {
        errOut = "BCryptSetProperty(CBC) failed";
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

struct OctoBundleEntry {
    uint64_t id{0};
    std::string name;
    uint64_t size{0};
    std::string md5;
    int64_t state{0};
};

bool ReadPbVarint(const uint8_t* buf, size_t len, size_t& pos, uint64_t& out) {
    out = 0;
    unsigned shift = 0;
    while (pos < len && shift < 64) {
        const uint8_t b = buf[pos++];
        out |= static_cast<uint64_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) return true;
        shift += 7;
    }
    return false;
}

bool SkipPbField(const uint8_t* buf, size_t len, size_t& pos, uint32_t wt) {
    uint64_t v = 0;
    switch (wt) {
        case 0: return ReadPbVarint(buf, len, pos, v);
        case 1: if (pos + 8 > len) return false; pos += 8; return true;
        case 2: {
            if (!ReadPbVarint(buf, len, pos, v) || v > len - pos) return false;
            pos += static_cast<size_t>(v);
            return true;
        }
        case 5: if (pos + 4 > len) return false; pos += 4; return true;
        default: return false;
    }
}

bool ParseOctoBundleMsg(const uint8_t* buf, size_t len, OctoBundleEntry& out) {
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = 0;
        if (!ReadPbVarint(buf, len, pos, tag)) return false;
        const uint32_t fn = static_cast<uint32_t>(tag >> 3);
        const uint32_t wt = static_cast<uint32_t>(tag & 7u);
        if (fn == 1 && wt == 0) {
            if (!ReadPbVarint(buf, len, pos, out.id)) return false;
        } else if (fn == 3 && wt == 2) {
            uint64_t slen = 0;
            if (!ReadPbVarint(buf, len, pos, slen) || slen > len - pos) return false;
            out.name.assign(reinterpret_cast<const char*>(buf + pos), static_cast<size_t>(slen));
            pos += static_cast<size_t>(slen);
        } else if (fn == 4 && wt == 0) {
            if (!ReadPbVarint(buf, len, pos, out.size)) return false;
        } else if (fn == 9 && wt == 0) {
            uint64_t st = 0;
            if (!ReadPbVarint(buf, len, pos, st)) return false;
            out.state = static_cast<int64_t>(st);
        } else if (fn == 10 && wt == 2) {
            uint64_t slen = 0;
            if (!ReadPbVarint(buf, len, pos, slen) || slen > len - pos) return false;
            out.md5.assign(reinterpret_cast<const char*>(buf + pos), static_cast<size_t>(slen));
            pos += static_cast<size_t>(slen);
        } else {
            if (!SkipPbField(buf, len, pos, wt)) return false;
        }
    }
    return !out.name.empty();
}

bool ParseOctoDatabaseBundles(const std::vector<uint8_t>& pb,
                              std::unordered_map<std::string, OctoBundleEntry>& outByName,
                              size_t& totalBundleCount) {
    outByName.clear();
    totalBundleCount = 0;
    const uint8_t* buf = pb.data();
    const size_t len = pb.size();
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = 0;
        if (!ReadPbVarint(buf, len, pos, tag)) return false;
        const uint32_t fn = static_cast<uint32_t>(tag >> 3);
        const uint32_t wt = static_cast<uint32_t>(tag & 7u);
        if (fn == 2 && wt == 2) {
            uint64_t mlen = 0;
            if (!ReadPbVarint(buf, len, pos, mlen) || mlen > len - pos) return false;
            OctoBundleEntry entry;
            if (!ParseOctoBundleMsg(buf + pos, static_cast<size_t>(mlen), entry)) return false;
            ++totalBundleCount;
            outByName[entry.name] = std::move(entry);
            pos += static_cast<size_t>(mlen);
        } else {
            if (!SkipPbField(buf, len, pos, wt)) return false;
        }
    }
    return totalBundleCount > 0;
}

bool DeobfuscateOctoBundleHeader(std::vector<uint8_t>& bytes, std::string_view bundleName, uint64_t& outTotalSize) {
    outTotalSize = 0;
    if (bytes.size() < 24 || bundleName.empty()) return false;
    const size_t L = bundleName.size();
    const uint8_t c0 = static_cast<uint8_t>(bytes[0] ^ static_cast<uint8_t>('U') ^ static_cast<uint8_t>(bundleName[0]));
    const size_t limit = (std::min)(size_t{256}, bytes.size());
    for (size_t j = 0; j < limit; ++j) {
        const size_t i = j / 2u;
        const uint8_t k = (j % 2u == 0u)
            ? static_cast<uint8_t>(static_cast<uint8_t>(bundleName[i % L]) ^ c0)
            : static_cast<uint8_t>(static_cast<uint8_t>(bundleName[L - 1u - (i % L)]) ^ static_cast<uint8_t>(c0 ^ 0xFFu));
        bytes[j] ^= k;
    }
    if (std::memcmp(bytes.data(), "UnityFS\0", 8) != 0) return false;
    size_t p = 12; // "UnityFS\0" + uint32 format
    for (int s = 0; s < 2; ++s) {
        while (p < bytes.size() && bytes[p] != 0) ++p;
        if (p >= bytes.size()) return false;
        ++p;
    }
    if (p + 8 > bytes.size()) return false;
    uint64_t sz = 0;
    for (int b = 0; b < 8; ++b) {
        sz = (sz << 8) | static_cast<uint64_t>(bytes[p + static_cast<size_t>(b)]);
    }
    outTotalSize = sz;
    return outTotalSize == bytes.size();
}

// -----------------------------------------------------------------------------
// WebPanel Bridge Test Fixture & Capabilities Helpers
// -----------------------------------------------------------------------------
jsonlite::Value ParseJson(const std::string& text) {
    jsonlite::Value val;
    std::string err;
    jsonlite::ParseText(text, val, err);
    return val;
}

bool JsonArrayContainsString(const jsonlite::Value* arr, std::string_view needle) {
    if (!arr || !arr->IsArray()) return false;
    for (size_t i = 0; i < arr->Size(); ++i) {
        const auto* item = arr->At(i);
        if (item && item->IsString() && item->stringValue == needle) return true;
    }
    return false;
}

struct BridgeFixture {
    Core core;
    std::vector<std::string> events;
    std::vector<std::wstring> assetDirs;
    std::unique_ptr<Bridge> bridge;

    BridgeFixture() {
        core.enforceInjectionPolicy = true;
        core.injectionPolicy.SetSceneAllowed(true);
        core.injectionPolicy.SetCaptureHooksReady(true);
        core.motionsRoot = "synthetic/motions";
        HostHooks hooks;
        hooks.releaseFileDialog = [] {};
        hooks.pluginVersion = "0.2.0-e2e";
        hooks.assetsDirs = [this]() { return assetDirs; };
        bridge = std::make_unique<Bridge>(core, hooks);
        bridge->SetEventSink(
            [](void* userData, const char* json) {
                static_cast<std::vector<std::string>*>(userData)->push_back(json ? json : "");
            },
            &events);
        bridge->OnShow();
    }

    std::string Req(const std::string& method, const std::string& params = "{}") {
        return bridge->HandleMessageForTest(
            "{\"v\":1,\"id\":7,\"type\":\"request\",\"method\":\"" + method + "\",\"params\":" + params + "}");
    }

    jsonlite::Value ReqJson(const std::string& method, const std::string& params = "{}") {
        return ParseJson(Req(method, params));
    }

    bool IsOk(const jsonlite::Value& v) const {
        const auto* ok = v.Find("ok");
        return ok && ok->IsBool() && ok->boolValue;
    }

    std::string ErrorCode(const jsonlite::Value& v) const {
        const auto* err = v.Find("error");
        const auto* code = err ? err->Find("code") : nullptr;
        return (code && code->IsString()) ? code->stringValue : "";
    }
};

// Local C++20 capability probes extending gke2e::probes
std::string CheckActorsPortraitCapability(BridgeFixture& f) {
    const auto hello = f.ReqJson("motion.hello");
    const auto* res = hello.Find("result");
    const auto* methods = res ? res->Find("supportedMethods") : nullptr;
    if (!JsonArrayContainsString(methods, "actors.getPortrait")) {
        return "M4 WebPanel RPC 'actors.getPortrait' pending in GkWebPanelBridge.hpp";
    }
    return {};
}

std::string CheckM4HelloCapabilities(BridgeFixture& f) {
    const auto hello = f.ReqJson("motion.hello");
    const auto* res = hello.Find("result");
    const auto* methods = res ? res->Find("supportedMethods") : nullptr;
    const auto* events = res ? res->Find("supportedEvents") : nullptr;
    const std::array<const char*, 6> reqMethods = {
        "actors.getPortrait",
        "formation.selectSlot",
        "formation.swapSlots",
        "formation.setSlotOffset",
        "formation.saveSlotOffset",
        "formation.setSlotSource",
    };
    std::vector<std::string> missing;
    for (const char* m : reqMethods) {
        if (!JsonArrayContainsString(methods, m)) missing.push_back(m);
    }
    if (!JsonArrayContainsString(events, "formation.changed")) {
        missing.push_back("event:formation.changed");
    }
    if (missing.empty()) return {};
    std::string msg = "M4 WebPanel RPCs pending in GkWebPanelBridge.hpp (missing: ";
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i) msg += ", ";
        msg += missing[i];
    }
    msg += ")";
    return msg;
}

fs::path OctoBundlePathForEntry(const fs::path& gameRoot, const OctoBundleEntry& e) {
    return gameRoot / "octo" / "v1" / "400" /
        std::to_string(e.id % 10) / UpperHexAscii("A" + std::to_string(e.id)) / e.md5;
}

int ResolveCachedOctoPortraitTier(const fs::path& gameRoot,
                                  const std::unordered_map<std::string, OctoBundleEntry>& byName,
                                  std::string_view code,
                                  std::string& outBundleName,
                                  const OctoBundleEntry*& outEntry,
                                  fs::path& outBundlePath) {
    const std::array<std::string, 4> candidates = {
        std::string("img_chr_") + std::string(code) + "_00-bustup",
        std::string("img_chr_") + std::string(code) + "_00-audition",
        std::string("img_adv_speaker_") + std::string(code) + "_000-000",
        std::string("img_chr_") + std::string(code) + "_00-thumb-circle",
    };
    for (size_t idx = 0; idx < candidates.size(); ++idx) {
        const auto it = byName.find(candidates[idx]);
        if (it == byName.end()) continue;
        const fs::path p = OctoBundlePathForEntry(gameRoot, it->second);
        if (fs::is_regular_file(p)) {
            outBundleName = candidates[idx];
            outEntry = &it->second;
            outBundlePath = p;
            return static_cast<int>(idx + 1);
        }
    }
    return 0;
}

void CheckNoPortraitDataReturned(gke2e::CaseContext& ctx, const BridgeFixture& f, const jsonlite::Value& resp) {
    if (!f.IsOk(resp)) return;
    const auto* res = resp.Find("result");
    const auto* cached = res ? res->Find("cached") : nullptr;
    const auto* uri = res ? res->Find("dataUri") : nullptr;
    GKE2E_CHECK(ctx, cached && cached->IsBool() && !cached->boolValue);
    GKE2E_CHECK(ctx, !uri || uri->IsNull() || (uri->IsString() && uri->stringValue.empty()));
}

bool FindLastEventPayload(const std::vector<std::string>& events,
                          std::string_view eventName,
                          jsonlite::Value& outPayload) {
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        auto ev = ParseJson(*it);
        const auto* name = ev.Find("name");
        if (name && name->IsString() && name->stringValue == eventName) {
            const auto* payload = ev.Find("payload");
            if (payload && payload->IsObject()) {
                outPayload = *payload;
                return true;
            }
        }
    }
    return false;
}

template <typename GuiActorT, typename CoreT>
bool ProbeSingleIdolGuiShortName(gke2e::CaseContext& ctx,
                                 CoreT& core,
                                 std::string_view expectedChar,
                                 std::string_view expectedDisplay,
                                 std::string_view expectedShort) {
    if constexpr (gke2e::probes::HasCharacter<GuiActorT> &&
                  gke2e::probes::HasDisplayName<GuiActorT> &&
                  gke2e::probes::HasShortName<GuiActorT>) {
        const auto guiCtx = core.BuildGuiContext();
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors.size(), size_t{1});
        if (guiCtx.sceneActors.size() != size_t{1}) return true;
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[0].character, expectedChar);
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[0].displayName, expectedDisplay);
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[0].shortName, expectedShort);
        return true;
    } else {
        (void)ctx;
        (void)core;
        (void)expectedChar;
        (void)expectedDisplay;
        (void)expectedShort;
        return false;
    }
}

template <typename GuiActorT, typename CoreT>
bool ProbeMultiActorGuiMetadata(gke2e::CaseContext& ctx, CoreT& core) {
    if constexpr (gke2e::probes::HasCharacter<GuiActorT> &&
                  gke2e::probes::HasDisplayName<GuiActorT> &&
                  gke2e::probes::HasShortName<GuiActorT>) {
        const auto guiCtx = core.BuildGuiContext();
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors.size(), size_t{3});
        if (guiCtx.sceneActors.size() != size_t{3}) return true;
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[0].character, "hski");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[0].displayName, "花海咲季");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[0].shortName, "咲季");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[1].character, "ttmr");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[1].displayName, "月村手毬");
        GKE2E_CHECK(ctx, !guiCtx.sceneActors[1].shortName.empty());
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[2].character, "custom_npc");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[2].displayName, "自定义NPC");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[2].shortName, "自定义NPC");
        return true;
    } else {
        (void)ctx;
        (void)core;
        return false;
    }
}

template <typename GuiActorT, typename CoreT>
bool ProbeNpcGuiShortNameFallback(gke2e::CaseContext& ctx, CoreT& core) {
    if constexpr (gke2e::probes::HasCharacter<GuiActorT> &&
                  gke2e::probes::HasDisplayName<GuiActorT> &&
                  gke2e::probes::HasShortName<GuiActorT>) {
        const auto guiCtx = core.BuildGuiContext();
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors.size(), size_t{3});
        if (guiCtx.sceneActors.size() != size_t{3}) return true;
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[0].character, "custom_npc");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[0].displayName, "自定义NPC");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[0].shortName, "自定义NPC");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[1].character, "");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[1].displayName, "StageStaff");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[1].shortName, "StageStaff");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[2].character, "");
        GKE2E_CHECK_EQ(ctx, guiCtx.sceneActors[2].displayName, "");
        GKE2E_CHECK(ctx, guiCtx.sceneActors[2].label.rfind("未识别角色", 0) == 0);
        return true;
    } else {
        (void)ctx;
        (void)core;
        return false;
    }
}

template <typename EntryT>
bool PopulateGroupMotionEntry(EntryT& group) {
    if constexpr (gke2e::probes::HasIsGroup<EntryT> &&
                  gke2e::probes::HasPositionCount<EntryT> &&
                  gke2e::probes::HasPositions<EntryT>) {
        group.isGroup = true;
        group.positionCount = 3;
        using PosEntryT = typename std::remove_cvref_t<decltype(group.positions)>::value_type;
        for (int s = 1; s <= 3; ++s) {
            PosEntryT pos{};
            pos.slot = s;
            pos.x = static_cast<float>(s - 1) * 0.5f;
            pos.y = (s == 1) ? 0.0f : 0.1f;
            pos.z = (s == 1) ? 0.0f : -0.25f;
            pos.yawDegrees = (s == 1) ? 0.0f : static_cast<float>(s - 1) * 15.0f;
            pos.hasMotion = true;
            pos.hasFacial = (s == 1);
            group.positions.push_back(std::move(pos));
        }
        return true;
    } else {
        (void)group;
        return false;
    }
}

void CheckJsonActorMetadata(gke2e::CaseContext& ctx,
                            const jsonlite::Value* actorObj,
                            std::string_view expectedChar,
                            std::string_view expectedDisplay,
                            std::string_view expectedShortExact,
                            bool requireShortNonEmpty = true) {
    GKE2E_REQUIRE(ctx, actorObj && actorObj->IsObject());
    const auto* ch = actorObj->Find("character");
    const auto* dn = actorObj->Find("displayName");
    const auto* sn = actorObj->Find("shortName");
    GKE2E_REQUIRE(ctx, ch && ch->IsString());
    GKE2E_REQUIRE(ctx, dn && dn->IsString());
    GKE2E_REQUIRE(ctx, sn && sn->IsString());
    GKE2E_CHECK_EQ(ctx, ch->stringValue, expectedChar);
    GKE2E_CHECK_EQ(ctx, dn->stringValue, expectedDisplay);
    if (!expectedShortExact.empty()) {
        GKE2E_CHECK_EQ(ctx, sn->stringValue, expectedShortExact);
    } else if (requireShortNonEmpty) {
        GKE2E_CHECK(ctx, !sn->stringValue.empty());
    }
}

// -----------------------------------------------------------------------------
// F5 Tests (Tier 1 & Tier 2)
// -----------------------------------------------------------------------------
void Test_T1_F5_01(gke2e::CaseContext& ctx) {
    const auto srcPng = MakeSyntheticRgbaPng(1024, 1024, true);
    GKE2E_REQUIRE(ctx, !srcPng.empty());
    std::vector<uint8_t> outPng;
    std::string outMime;
    GKE2E_REQUIRE(ctx, cover::ResizeAndEncodeCover(srcPng.data(), srcPng.size(), 512, outPng, outMime, true));
    GKE2E_CHECK_EQ(ctx, outMime, "image/png");
    const PngInfo info = InspectPngHeader(outPng);
    GKE2E_CHECK(ctx, info.valid);
    GKE2E_CHECK_EQ(ctx, info.width, 512u);
    GKE2E_CHECK_EQ(ctx, info.height, 512u);
    GKE2E_CHECK(ctx, PngHasNonOpaqueAlpha(outPng));
}

void Test_T1_F5_02(gke2e::CaseContext& ctx) {
    // 1024x512 -> 512x256
    {
        const auto src = MakeSyntheticRgbaPng(1024, 512, false);
        GKE2E_REQUIRE(ctx, !src.empty());
        std::vector<uint8_t> out;
        std::string mime;
        GKE2E_REQUIRE(ctx, cover::ResizeAndEncodeCover(src.data(), src.size(), 512, out, mime, true));
        const PngInfo info = InspectPngHeader(out);
        GKE2E_CHECK(ctx, info.valid);
        GKE2E_CHECK_EQ(ctx, info.width, 512u);
        GKE2E_CHECK_EQ(ctx, info.height, 256u);
        GKE2E_CHECK_EQ(ctx, mime, "image/png");
    }
    // 512x1024 -> 256x512
    {
        const auto src = MakeSyntheticRgbaPng(512, 1024, false);
        std::vector<uint8_t> out;
        std::string mime;
        GKE2E_REQUIRE(ctx, cover::ResizeAndEncodeCover(src.data(), src.size(), 512, out, mime, true));
        const PngInfo info = InspectPngHeader(out);
        GKE2E_CHECK(ctx, info.valid);
        GKE2E_CHECK_EQ(ctx, info.width, 256u);
        GKE2E_CHECK_EQ(ctx, info.height, 512u);
    }
    // 256x256 -> 256x256 (no upscale), maxDim=0 defaults to 512
    {
        const auto src = MakeSyntheticRgbaPng(256, 256, false);
        std::vector<uint8_t> out;
        std::string mime;
        GKE2E_REQUIRE(ctx, cover::ResizeAndEncodeCover(src.data(), src.size(), 0, out, mime, true));
        const PngInfo info = InspectPngHeader(out);
        GKE2E_CHECK(ctx, info.valid);
        GKE2E_CHECK_EQ(ctx, info.width, 256u);
        GKE2E_CHECK_EQ(ctx, info.height, 256u);
    }
}

void Test_T1_F5_03(gke2e::CaseContext& ctx) {
    const auto src = MakeSyntheticRgbaPng(640, 640, true);
    GKE2E_REQUIRE(ctx, !src.empty());
    std::vector<uint8_t> encoded;
    std::string mime;
    GKE2E_REQUIRE(ctx, cover::ResizeAndEncodeCover(src.data(), src.size(), 512, encoded, mime, true));
    const std::string dataUri = cover::ToDataUri(encoded, mime);
    GKE2E_CHECK(ctx, dataUri.rfind("data:image/png;base64,", 0) == 0);

    std::string decodedMime;
    std::vector<uint8_t> decodedBytes;
    GKE2E_REQUIRE(ctx, cover::DecodeDataUri(dataUri, decodedMime, decodedBytes));
    GKE2E_CHECK_EQ(ctx, decodedMime, "image/png");
    GKE2E_CHECK(ctx, decodedBytes == encoded);
}

void Test_T1_F5_04(gke2e::CaseContext& ctx) {
    const fs::path gameRoot = GetGameRoot();
    const fs::path manifestPath = gameRoot / "octo" / "pdb" / "400" / "705100" / "octocacheevai";
    GKE2E_REQUIRE_MSG(ctx, fs::is_regular_file(manifestPath), "Missing Octo manifest at " + manifestPath.string());

    const auto rawManifest = ReadFileBytes(manifestPath);
    GKE2E_REQUIRE(ctx, !rawManifest.empty());

    std::vector<uint8_t> pb;
    std::string err;
    GKE2E_REQUIRE_MSG(ctx, DecryptOctoManifestBytes(rawManifest, pb, err), err);

    std::unordered_map<std::string, OctoBundleEntry> byName;
    size_t totalBundles = 0;
    GKE2E_REQUIRE(ctx, ParseOctoDatabaseBundles(pb, byName, totalBundles));
    GKE2E_CHECK(ctx, totalBundles > 20000);

    static constexpr const char* kCore14[] = {
        "amao", "atbm", "fktn", "hmsz", "hrnm", "hski", "hume",
        "jsna", "kcna", "kllj", "nasr", "shro", "ssmk", "ttmr"
    };
    for (const char* code : kCore14) {
        const std::string bundleName = std::string("img_chr_") + code + "_00-bustup";
        const auto it = byName.find(bundleName);
        GKE2E_REQUIRE_MSG(ctx, it != byName.end(), "Missing bustup bundle in manifest: " + bundleName);
        const auto& e = it->second;
        const std::string hexFolder = UpperHexAscii("A" + std::to_string(e.id));
        const fs::path bundlePath = gameRoot / "octo" / "v1" / "400" / std::to_string(e.id % 10) / hexFolder / e.md5;
        GKE2E_REQUIRE_MSG(ctx, fs::is_regular_file(bundlePath), "Missing cached bundle: " + bundlePath.string());
        auto bundleBytes = ReadFileBytes(bundlePath);
        GKE2E_CHECK_EQ(ctx, bundleBytes.size(), e.size);
        std::array<uint8_t, 16> md5{};
        GKE2E_CHECK(ctx, ComputeMd5(bundleBytes.data(), bundleBytes.size(), md5));
        GKE2E_CHECK_EQ(ctx, HexLower(md5), e.md5);
        uint64_t totalSz = 0;
        GKE2E_CHECK_MSG(ctx, DeobfuscateOctoBundleHeader(bundleBytes, bundleName, totalSz),
                        "XOR/UnityFS check failed for " + bundleName);
    }
}

void Test_T1_F5_05(gke2e::CaseContext& ctx) {
    GKE2E_REQUIRE_HEADER(ctx, GK_E2E_HAS_PORTRAIT_CACHE, "GkPortraitCache.hpp");
    BridgeFixture f;
    const std::string pendingReason = CheckActorsPortraitCapability(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    TempDirGuard tmp("t1_f5_05");
    f.core.configRoot = Utf8FromPath(tmp.path);

    const auto pngHski = MakeSyntheticRgbaPng(512, 512, true);
    const auto pngTtmr = MakeSyntheticRgbaPng(256, 256, false);
    GKE2E_REQUIRE(ctx, !pngHski.empty() && !pngTtmr.empty() && pngHski != pngTtmr);
    const fs::path hskiPath = tmp.path / "portraits" / "hski.png";
    const fs::path ttmrPath = tmp.path / "portraits" / "ttmr.png";
    GKE2E_REQUIRE(ctx, WriteFileBytes(hskiPath, pngHski));
    GKE2E_REQUIRE(ctx, WriteFileBytes(ttmrPath, pngTtmr));

    auto verifyHit = [&](const char* code, const std::vector<uint8_t>& expectedBytes) {
        const auto resp = f.ReqJson("actors.getPortrait", std::string("{\"character\":\"") + code + "\"}");
        GKE2E_REQUIRE(ctx, f.IsOk(resp));
        const auto* res = resp.Find("result");
        GKE2E_REQUIRE(ctx, res && res->IsObject());
        const auto* ch = res->Find("character");
        const auto* cached = res->Find("cached");
        const auto* uri = res->Find("dataUri");
        GKE2E_REQUIRE(ctx, ch && ch->IsString());
        GKE2E_CHECK_EQ(ctx, ch->stringValue, code);
        GKE2E_CHECK(ctx, cached && cached->IsBool() && cached->boolValue);
        GKE2E_REQUIRE(ctx, uri && uri->IsString());
        std::string decMime;
        std::vector<uint8_t> decBytes;
        GKE2E_REQUIRE(ctx, cover::DecodeDataUri(uri->stringValue, decMime, decBytes));
        GKE2E_CHECK_EQ(ctx, decMime, "image/png");
        GKE2E_CHECK(ctx, decBytes == expectedBytes);
    };

    verifyHit("hski", pngHski);
    verifyHit("ttmr", pngTtmr);

    // Deleting hski.png from <configRoot>/portraits/ must result in a miss (proving live disk lookup)
    std::error_code ec;
    GKE2E_REQUIRE(ctx, fs::remove(hskiPath, ec));
    const auto missResp = f.ReqJson("actors.getPortrait", "{\"character\":\"hski\"}");
    CheckNoPortraitDataReturned(ctx, f, missResp);
}

void Test_T1_F5_06(gke2e::CaseContext& ctx) {
    GKE2E_REQUIRE_HEADER(ctx, GK_E2E_HAS_PORTRAIT_CACHE, "GkPortraitCache.hpp");
    BridgeFixture f;
    const std::string pendingReason = CheckActorsPortraitCapability(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    // Part A: Real f:\Games\gakumas Octo 4-tier fallback verification (read-only offline)
    const fs::path gameRoot = GetGameRoot();
    const fs::path manifestPath = gameRoot / "octo" / "pdb" / "400" / "705100" / "octocacheevai";
    GKE2E_REQUIRE_MSG(ctx, fs::is_regular_file(manifestPath), "Missing Octo manifest at " + manifestPath.string());
    const auto rawManifest = ReadFileBytes(manifestPath);
    GKE2E_REQUIRE(ctx, !rawManifest.empty());

    std::vector<uint8_t> pb;
    std::string err;
    GKE2E_REQUIRE_MSG(ctx, DecryptOctoManifestBytes(rawManifest, pb, err), err);
    std::unordered_map<std::string, OctoBundleEntry> byName;
    size_t totalBundles = 0;
    GKE2E_REQUIRE(ctx, ParseOctoDatabaseBundles(pb, byName, totalBundles));

    auto verifyCachedBundle = [&](const OctoBundleEntry& e, std::string_view bundleName, const fs::path& bundlePath) {
        auto bytes = ReadFileBytes(bundlePath);
        GKE2E_CHECK_EQ(ctx, bytes.size(), e.size);
        std::array<uint8_t, 16> md5{};
        GKE2E_CHECK(ctx, ComputeMd5(bytes.data(), bytes.size(), md5));
        GKE2E_CHECK_EQ(ctx, HexLower(md5), e.md5);
        uint64_t totalSz = 0;
        GKE2E_CHECK_MSG(ctx, DeobfuscateOctoBundleHeader(bytes, bundleName, totalSz),
                        std::string("XOR/UnityFS check failed for ") + std::string(bundleName));
    };

    // Tier 1 (bustup): hski
    {
        std::string bname;
        const OctoBundleEntry* entry = nullptr;
        fs::path bpath;
        const int tier = ResolveCachedOctoPortraitTier(gameRoot, byName, "hski", bname, entry, bpath);
        GKE2E_CHECK_EQ(ctx, tier, 1);
        GKE2E_CHECK_EQ(ctx, bname, "img_chr_hski_00-bustup");
        if (entry) verifyCachedBundle(*entry, bname, bpath);
    }

    // Tier 2 (audition fallback): hkne (bustup in manifest ONLY, not on disk -> falls back to audition)
    {
        const auto itBust = byName.find("img_chr_hkne_00-bustup");
        GKE2E_REQUIRE(ctx, itBust != byName.end());
        GKE2E_CHECK(ctx, !fs::is_regular_file(OctoBundlePathForEntry(gameRoot, itBust->second)));
        std::string bname;
        const OctoBundleEntry* entry = nullptr;
        fs::path bpath;
        const int tier = ResolveCachedOctoPortraitTier(gameRoot, byName, "hkne", bname, entry, bpath);
        GKE2E_CHECK_EQ(ctx, tier, 2);
        GKE2E_CHECK_EQ(ctx, bname, "img_chr_hkne_00-audition");
        if (entry) verifyCachedBundle(*entry, bname, bpath);
    }

    // Tier 3 (speaker fallback): krnh (tiers 1 & 2 absent from manifest -> falls back to speaker 000-000)
    {
        GKE2E_CHECK(ctx, byName.find("img_chr_krnh_00-bustup") == byName.end());
        GKE2E_CHECK(ctx, byName.find("img_chr_krnh_00-audition") == byName.end());
        std::string bname;
        const OctoBundleEntry* entry = nullptr;
        fs::path bpath;
        const int tier = ResolveCachedOctoPortraitTier(gameRoot, byName, "krnh", bname, entry, bpath);
        GKE2E_CHECK_EQ(ctx, tier, 3);
        GKE2E_CHECK_EQ(ctx, bname, "img_adv_speaker_krnh_000-000");
        if (entry) verifyCachedBundle(*entry, bname, bpath);
    }

    // Tier 4 (thumb-circle only in manifest): trda, ktko; plus cached Tier-4 bundles amao & krnh
    for (const char* t4Only : {"trda", "ktko"}) {
        const std::string prefix = std::string("img_chr_") + t4Only + "_00";
        GKE2E_CHECK(ctx, byName.find(prefix + "-bustup") == byName.end());
        GKE2E_CHECK(ctx, byName.find(prefix + "-audition") == byName.end());
        GKE2E_CHECK(ctx, byName.find(std::string("img_adv_speaker_") + t4Only + "_000-000") == byName.end());
        GKE2E_CHECK(ctx, byName.find(prefix + "-thumb-circle") != byName.end());
    }
    for (const char* t4Cached : {"amao", "krnh"}) {
        const std::string t4Name = std::string("img_chr_") + t4Cached + "_00-thumb-circle";
        const auto it = byName.find(t4Name);
        GKE2E_REQUIRE_MSG(ctx, it != byName.end(), "Missing Tier-4 bundle in manifest: " + t4Name);
        const fs::path bpath = OctoBundlePathForEntry(gameRoot, it->second);
        GKE2E_REQUIRE_MSG(ctx, fs::is_regular_file(bpath), "Missing cached Tier-4 bundle: " + bpath.string());
        verifyCachedBundle(it->second, t4Name, bpath);
    }

    // Part B: Bridge actors.getPortrait across all 4 tier outputs (standalone even under -Tier 1)
    TempDirGuard tmp("t1_f5_06");
    f.core.configRoot = Utf8FromPath(tmp.path);
    struct TierPng {
        const char* code;
        std::vector<uint8_t> bytes;
    };
    const std::array<TierPng, 4> tierPngs = {{
        {"hski", MakeSyntheticRgbaPng(512, 512, true)},
        {"hkne", MakeSyntheticRgbaPng(512, 512, false)},
        {"krnh", MakeSyntheticRgbaPng(384, 384, true)},
        {"amao", MakeSyntheticRgbaPng(256, 256, true)},
    }};
    for (const auto& tp : tierPngs) {
        GKE2E_REQUIRE(ctx, !tp.bytes.empty());
        GKE2E_REQUIRE(ctx, WriteFileBytes(tmp.path / "portraits" / (std::string(tp.code) + ".png"), tp.bytes));
        const auto resp = f.ReqJson("actors.getPortrait", std::string("{\"character\":\"") + tp.code + "\"}");
        GKE2E_REQUIRE(ctx, f.IsOk(resp));
        const auto* res = resp.Find("result");
        GKE2E_REQUIRE(ctx, res && res->IsObject());
        GKE2E_CHECK(ctx, res->Find("cached") && res->Find("cached")->boolValue);
        const auto* uri = res->Find("dataUri");
        GKE2E_REQUIRE(ctx, uri && uri->IsString());
        std::string decMime;
        std::vector<uint8_t> decBytes;
        GKE2E_REQUIRE(ctx, cover::DecodeDataUri(uri->stringValue, decMime, decBytes));
        GKE2E_CHECK_EQ(ctx, decMime, "image/png");
        GKE2E_CHECK(ctx, decBytes == tp.bytes);
    }

    const auto missResp = f.ReqJson("actors.getPortrait", "{\"character\":\"unknown_tier_miss\"}");
    CheckNoPortraitDataReturned(ctx, f, missResp);
}

void Test_T2_F5_01(gke2e::CaseContext& ctx) {
    std::vector<uint8_t> out;
    std::string mime;
    GKE2E_CHECK(ctx, !cover::ResizeAndEncodeCover(nullptr, 16, 512, out, mime, true));
    GKE2E_CHECK(ctx, out.empty() && mime.empty());

    const std::vector<uint8_t> garbage = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    GKE2E_CHECK(ctx, !cover::ResizeAndEncodeCover(garbage.data(), 0, 512, out, mime, true));
    GKE2E_CHECK(ctx, !cover::ResizeAndEncodeCover(garbage.data(), garbage.size(), 512, out, mime, true));
    GKE2E_CHECK(ctx, out.empty() && mime.empty());

    // >64 MiB rejected immediately without allocation
    GKE2E_CHECK(ctx, !cover::ResizeAndEncodeCover(garbage.data(), 65ULL * 1024ULL * 1024ULL, 512, out, mime, true));

    // Truncated PNG header
    auto validPng = MakeSyntheticRgbaPng(64, 64, false);
    GKE2E_REQUIRE(ctx, validPng.size() > 32);
    validPng.resize(24);
    GKE2E_CHECK(ctx, !cover::ResizeAndEncodeCover(validPng.data(), validPng.size(), 512, out, mime, true));
}

void Test_T2_F5_02(gke2e::CaseContext& ctx) {
    std::string mime;
    std::vector<uint8_t> bytes;
    GKE2E_CHECK(ctx, !cover::DecodeDataUri("", mime, bytes));
    GKE2E_CHECK(ctx, !cover::DecodeDataUri("data:text/plain;base64,SGVsbG8=", mime, bytes));
    GKE2E_CHECK(ctx, !cover::DecodeDataUri("data:image/png,notbase64", mime, bytes));
    GKE2E_CHECK(ctx, !cover::DecodeDataUri("data:image/png;base64,!!!invalid!!!", mime, bytes));
    std::string huge = "data:image/png;base64,";
    huge.append(16ULL * 1024ULL * 1024ULL + 16ULL, 'A');
    GKE2E_CHECK(ctx, !cover::DecodeDataUri(huge, mime, bytes));
}

void Test_T2_F5_03(gke2e::CaseContext& ctx) {
    std::vector<uint8_t> pb;
    std::string err;

    // Empty & too short
    GKE2E_CHECK(ctx, !DecryptOctoManifestBytes({}, pb, err));
    GKE2E_CHECK(ctx, !DecryptOctoManifestBytes(std::vector<uint8_t>(16, 0x01), pb, err));

    // Wrong magic byte (0x02 instead of 0x01)
    std::vector<uint8_t> badMagic(33, 0x00);
    badMagic[0] = 0x02;
    GKE2E_CHECK(ctx, !DecryptOctoManifestBytes(badMagic, pb, err));

    // Non-block-aligned ciphertext
    std::vector<uint8_t> unaligned(20, 0x01);
    GKE2E_CHECK(ctx, !DecryptOctoManifestBytes(unaligned, pb, err));

    // Random garbage ciphertext -> invalid PKCS7 padding or MD5 mismatch
    std::vector<uint8_t> badCipher(1 + 64, 0x5A);
    badCipher[0] = 0x01;
    GKE2E_CHECK(ctx, !DecryptOctoManifestBytes(badCipher, pb, err));
}

void Test_T2_F5_04(gke2e::CaseContext& ctx) {
    uint64_t totalSz = 0;
    std::vector<uint8_t> empty;
    GKE2E_CHECK(ctx, !DeobfuscateOctoBundleHeader(empty, "img_chr_hski_00-bustup", totalSz));

    std::vector<uint8_t> shortBuf(120, 0x11);
    GKE2E_CHECK(ctx, !DeobfuscateOctoBundleHeader(shortBuf, "img_chr_hski_00-bustup", totalSz));

    std::vector<uint8_t> exact256(256, 0x33);
    GKE2E_CHECK(ctx, !DeobfuscateOctoBundleHeader(exact256, "img_chr_hski_00-bustup", totalSz));

    std::vector<uint8_t> badSize(300, 0x00);
    GKE2E_CHECK(ctx, !DeobfuscateOctoBundleHeader(badSize, "", totalSz));
}

void Test_T2_F5_05(gke2e::CaseContext& ctx) {
    GKE2E_REQUIRE_HEADER(ctx, GK_E2E_HAS_PORTRAIT_CACHE, "GkPortraitCache.hpp");
    BridgeFixture f;
    const std::string pendingReason = CheckActorsPortraitCapability(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    TempDirGuard tmp("t2_f5_05");
    f.core.configRoot = Utf8FromPath(tmp.path);
    const auto canary = MakeSyntheticRgbaPng(64, 64, false);
    GKE2E_REQUIRE(ctx, !canary.empty());
    GKE2E_REQUIRE(ctx, WriteFileBytes(tmp.path / "secret.png", canary));
    GKE2E_REQUIRE(ctx, WriteFileBytes(tmp.path / "portraits" / "sub" / "evil.png", canary));
    GKE2E_REQUIRE(ctx, WriteFileBytes(tmp.path / "hski.png", canary));

    const std::array<const char*, 12> badParamPayloads = {
        "{}",
        "{\"character\":\"\"}",
        "{\"character\":\".\"}",
        "{\"character\":\"..\"}",
        "{\"character\":\"../secret\"}",
        "{\"character\":\"..\\\\secret\"}",
        "{\"character\":\"sub/evil\"}",
        "{\"character\":\"sub\\\\evil\"}",
        "{\"character\":\"a\\\\b\"}",
        "{\"character\":\"../hski\"}",
        "{\"token\":0}",
        "{\"token\":-1}",
    };
    for (const char* payload : badParamPayloads) {
        const auto resp = f.ReqJson("actors.getPortrait", payload);
        GKE2E_CHECK_EQ(ctx, f.ErrorCode(resp), "invalid_params");
        const auto* res = resp.Find("result");
        GKE2E_CHECK(ctx, !res || !res->Find("dataUri"));
    }

    const std::string longCode(256, 'a');
    const auto longResp = f.ReqJson("actors.getPortrait", "{\"character\":\"" + longCode + "\"}");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(longResp), "invalid_params");
    GKE2E_CHECK(ctx, !longResp.Find("result"));

    const auto missingTokResp = f.ReqJson("actors.getPortrait", "{\"token\":999999}");
    CheckNoPortraitDataReturned(ctx, f, missingTokResp);
}

void Test_T2_F5_06(gke2e::CaseContext& ctx) {
    GKE2E_REQUIRE_HEADER(ctx, GK_E2E_HAS_PORTRAIT_CACHE, "GkPortraitCache.hpp");
    BridgeFixture f;
    const std::string pendingReason = CheckActorsPortraitCapability(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    TempDirGuard tmp("t2_f5_06");
    f.core.configRoot = Utf8FromPath(tmp.path);
    const fs::path portraitsDir = tmp.path / "portraits";

    // 1. Corrupt cached PNG files (0-byte, garbage bytes, truncated PNG header)
    GKE2E_REQUIRE(ctx, WriteFileBytes(portraitsDir / "hski.png", {}));
    GKE2E_REQUIRE(ctx, WriteFileBytes(portraitsDir / "ttmr.png", {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04}));
    auto truncPng = MakeSyntheticRgbaPng(64, 64, false);
    GKE2E_REQUIRE(ctx, truncPng.size() > 24);
    truncPng.resize(24);
    GKE2E_REQUIRE(ctx, WriteFileBytes(portraitsDir / "fktn.png", truncPng));

    for (const char* code : {"hski", "ttmr", "fktn"}) {
        const auto resp = f.ReqJson("actors.getPortrait", std::string("{\"character\":\"") + code + "\"}");
        CheckNoPortraitDataReturned(ctx, f, resp);
    }

    // 2. Total 4-tier miss on real f:\Games\gakumas Octo manifest + actors.getPortrait
    const fs::path manifestPath = GetGameRoot() / "octo" / "pdb" / "400" / "705100" / "octocacheevai";
    GKE2E_REQUIRE_MSG(ctx, fs::is_regular_file(manifestPath), "Missing Octo manifest at " + manifestPath.string());
    const auto rawManifest = ReadFileBytes(manifestPath);
    GKE2E_REQUIRE(ctx, !rawManifest.empty());
    std::vector<uint8_t> pb;
    std::string err;
    GKE2E_REQUIRE_MSG(ctx, DecryptOctoManifestBytes(rawManifest, pb, err), err);
    std::unordered_map<std::string, OctoBundleEntry> byName;
    size_t totalBundles = 0;
    GKE2E_REQUIRE(ctx, ParseOctoDatabaseBundles(pb, byName, totalBundles));

    GKE2E_CHECK(ctx, byName.find("img_chr_zzzz_unknown_999_00-bustup") == byName.end());
    GKE2E_CHECK(ctx, byName.find("img_chr_zzzz_unknown_999_00-audition") == byName.end());
    GKE2E_CHECK(ctx, byName.find("img_adv_speaker_zzzz_unknown_999_000-000") == byName.end());
    GKE2E_CHECK(ctx, byName.find("img_chr_zzzz_unknown_999_00-thumb-circle") == byName.end());

    const auto missResp = f.ReqJson("actors.getPortrait", "{\"character\":\"zzzz_unknown_999\"}");
    CheckNoPortraitDataReturned(ctx, f, missResp);
}

// -----------------------------------------------------------------------------
// F6 Tests (Tier 1 & Tier 2)
// -----------------------------------------------------------------------------
void Test_T1_F6_01(gke2e::CaseContext& ctx) {
    GKE2E_CHECK_EQ(ctx, std::string(LookupIdolChineseName("hski") ? LookupIdolChineseName("hski") : ""), "花海咲季");
    GKE2E_CHECK_EQ(ctx, std::string(LookupIdolChineseName("TTMR") ? LookupIdolChineseName("TTMR") : ""), "月村手毬");
    GKE2E_CHECK_EQ(ctx, std::string(LookupIdolChineseName("shro") ? LookupIdolChineseName("shro") : ""), "筱泽广");
    GKE2E_CHECK_EQ(ctx, std::string(LookupIdolChineseName("nasr") ? LookupIdolChineseName("nasr") : ""), "根绪亚纱里");
    GKE2E_CHECK_EQ(ctx, FormatIdolDisplayName("hski"), "花海咲季 (hski)");
    GKE2E_CHECK_EQ(ctx, FormatIdolDisplayName("nasr"), "根绪亚纱里 (nasr)");

    BridgeFixture f;
    int objHski = 1;
    f.core.PublishSceneActors({{&objHski, 101, "hski", "花海咲季", "chr_hski_face_00", ""}});
    if (!ProbeSingleIdolGuiShortName<GuiSceneActor>(ctx, f.core, "hski", "花海咲季", "咲季")) {
        GKE2E_PENDING(ctx, "GuiSceneActor::shortName pending M2.");
    }
}

void Test_T1_F6_02(gke2e::CaseContext& ctx) {
    GKE2E_CHECK_EQ(ctx, DetectIdolCharacterCode("chr_hski_face_00"), "hski");
    GKE2E_CHECK_EQ(ctx, DetectIdolCharacterCode("P_TTMR-0001 (Clone)"), "ttmr");
    GKE2E_CHECK_EQ(ctx, DetectIdolCharacterCode("nasr_00"), "nasr");
    GKE2E_CHECK_EQ(ctx, DetectIdolCharacterCode("fktn"), "fktn");
    GKE2E_CHECK_EQ(ctx, GakuActorsPresetName("hski | GakuActors[42] | preset:主唱"), "主唱");
    GKE2E_CHECK_EQ(ctx, GakuActorsPresetName("ttmr | GakuActors[-1] | preset:CenterSlot"), "CenterSlot");
}

void Test_T1_F6_03(gke2e::CaseContext& ctx) {
    SceneActorObservation a;
    a.character = "hski";
    a.displayName = "花海咲季";
    a.presetName = "主唱咲季";
    GKE2E_CHECK_EQ(ctx, FormatSceneActorLabel(a, 1), "主唱咲季 · 1");

    SceneActorObservation b;
    b.character = "ttmr";
    b.displayName = "月村手毬";
    GKE2E_CHECK_EQ(ctx, FormatSceneActorLabel(b, 2), "月村手毬 (ttmr) · 2");

    SceneActorObservation c;
    c.character = "shro";
    GKE2E_CHECK_EQ(ctx, FormatSceneActorLabel(c, 3), "筱泽广 (shro) · 3");

    SceneActorObservation d;
    GKE2E_CHECK_EQ(ctx, FormatSceneActorLabel(d, 4), "未识别角色 · 4");
}

void Test_T1_F6_04(gke2e::CaseContext& ctx) {
    SceneActorRegistry reg;
    int objA = 1, objB = 2, objC = 3;
    reg.Publish({
        {&objA, 101, "hski", "花海咲季", "chr_hski_face", ""},
        {&objB, 102, "ttmr", "月村手毬", "chr_ttmr_face", ""},
        {&objC, 103, "fktn", "藤田琴音", "chr_fktn_face", ""},
    });
    GKE2E_REQUIRE_EQ(ctx, reg.Entries().size(), size_t{3});
    const uint64_t tokA = reg.Entries()[0].token;
    const uint64_t tokB = reg.Entries()[1].token;
    const uint64_t tokC = reg.Entries()[2].token;
    GKE2E_CHECK(ctx, tokA < tokB && tokB < tokC);

    // Reorder Unity observations -> tokens remain sorted ascending and unchanged
    reg.Publish({
        {&objC, 103, "fktn", "藤田琴音", "chr_fktn_face", ""},
        {&objA, 101, "hski", "花海咲季", "chr_hski_face", ""},
        {&objB, 102, "ttmr", "月村手毬", "chr_ttmr_face", ""},
    });
    GKE2E_REQUIRE_EQ(ctx, reg.Entries().size(), size_t{3});
    GKE2E_CHECK_EQ(ctx, reg.Entries()[0].token, tokA);
    GKE2E_CHECK_EQ(ctx, reg.Entries()[1].token, tokB);
    GKE2E_CHECK_EQ(ctx, reg.Entries()[2].token, tokC);
}

void Test_T1_F6_05(gke2e::CaseContext& ctx) {
    SceneActorRegistry reg;
    int objA = 1, objB = 2;
    const uint64_t ep0 = reg.Epoch();
    reg.Publish({{&objA, 10, "hski", "花海咲季", "", ""}});
    const uint64_t ep1 = reg.Epoch();
    GKE2E_CHECK(ctx, ep1 > ep0);
    const uint64_t tokA = reg.Entries()[0].token;
    GKE2E_CHECK(ctx, reg.Resolve(tokA, ep1) == &objA);

    // Identity-preserving update keeps epoch stable
    reg.Publish({{&objA, 10, "hski", "花海咲季", "face", "preset1"}});
    GKE2E_CHECK_EQ(ctx, reg.Epoch(), ep1);

    // Adding actor bumps epoch
    reg.Publish({
        {&objA, 10, "hski", "花海咲季", "", ""},
        {&objB, 20, "ttmr", "月村手毬", "", ""},
    });
    const uint64_t ep2 = reg.Epoch();
    GKE2E_CHECK(ctx, ep2 > ep1);
    GKE2E_CHECK(ctx, reg.Resolve(tokA, ep1) == nullptr);
    GKE2E_CHECK(ctx, reg.Resolve(tokA, ep2) == &objA);

    reg.Clear();
    GKE2E_CHECK(ctx, reg.Entries().empty());
    GKE2E_CHECK(ctx, reg.Epoch() > ep2);
}

void Test_T1_F6_06(gke2e::CaseContext& ctx) {
    BridgeFixture f;
    int objA = 1, objB = 2, objNpc = 3;
    f.core.PublishSceneActors({
        {&objA,   101, "hski",       "花海咲季", "chr_hski_face_00", ""},
        {&objB,   102, "ttmr",       "月村手毬", "chr_ttmr_face_00", ""},
        {&objNpc, 103, "custom_npc", "自定义NPC", "npc_face",        ""},
    });
    if (!ProbeMultiActorGuiMetadata<GuiSceneActor>(ctx, f.core)) {
        GKE2E_PENDING(ctx, "GuiSceneActor::character/displayName/shortName pending M2.");
    }
    if (CheckM4HelloCapabilities(f).empty()) {
        const auto snap = f.ReqJson("state.get");
        GKE2E_REQUIRE(ctx, f.IsOk(snap));
        const auto* res = snap.Find("result");
        const auto* actorsObj = res ? res->Find("actors") : nullptr;
        const auto* arr = actorsObj ? actorsObj->Find("actors") : nullptr;
        GKE2E_REQUIRE(ctx, arr && arr->IsArray() && arr->Size() == 3);
        CheckJsonActorMetadata(ctx, arr->At(0), "hski", "花海咲季", "咲季");
        CheckJsonActorMetadata(ctx, arr->At(1), "ttmr", "月村手毬", "");
        CheckJsonActorMetadata(ctx, arr->At(2), "custom_npc", "自定义NPC", "自定义NPC");
    }
}

void Test_T2_F6_01(gke2e::CaseContext& ctx) {
    GKE2E_CHECK_EQ(ctx, DetectIdolCharacterCode("hski_ttmr"), "");
    GKE2E_CHECK_EQ(ctx, DetectIdolCharacterCode("ahskib"), "");
    GKE2E_CHECK_EQ(ctx, DetectIdolCharacterCode("1hski2"), "");
    GKE2E_CHECK_EQ(ctx, DetectIdolCharacterCode(""), "");
    GKE2E_CHECK_EQ(ctx, DetectIdolCharacterCode("hski_hski"), "hski");
    GKE2E_CHECK(ctx, LookupIdolChineseName("unknown_code") == nullptr);
    GKE2E_CHECK_EQ(ctx, FormatIdolDisplayName(""), "当前角色");
    GKE2E_CHECK_EQ(ctx, FormatIdolDisplayName("auto"), "当前角色");
}

void Test_T2_F6_02(gke2e::CaseContext& ctx) {
    const std::string code32(32, 'a');
    const std::string code33(33, 'a');
    const std::string label160(160, 'L');
    const std::string label161(161, 'L');

    GKE2E_CHECK_EQ(ctx, GakuActorsPresetName(code32 + " | GakuActors[-7] | preset:" + label160), label160);
    GKE2E_CHECK_EQ(ctx, GakuActorsPresetName(code33 + " | GakuActors[1] | preset:ok"), "");
    GKE2E_CHECK_EQ(ctx, GakuActorsPresetName("HSKI | GakuActors[1] | preset:ok"), "");
    GKE2E_CHECK_EQ(ctx, GakuActorsPresetName("hski | GakuActors[1] | preset:" + label161), "");
    GKE2E_CHECK_EQ(ctx, GakuActorsPresetName("hski | GakuActors[1] | preset:"), "");
    GKE2E_CHECK_EQ(ctx, GakuActorsPresetName(std::string("hski | GakuActors[1] | preset:bad\x1f")), "");
    GKE2E_CHECK_EQ(ctx, GakuActorsPresetName("hski | GakuActors[] | preset:ok"), "");
}

void Test_T2_F6_03(gke2e::CaseContext& ctx) {
    SceneActorRegistry reg;
    int objA = 1;
    reg.Publish({
        {nullptr, 1, "hski", "", "", ""},
        {&objA, 0, "hski", "", "", ""},
        {&objA, 101, "hski", "花海咲季", "", ""},
        {&objA, 999, "ttmr", "dupPtr", "", ""},
    });
    GKE2E_REQUIRE_EQ(ctx, reg.Entries().size(), size_t{1});
    const uint64_t tok1 = reg.Entries()[0].token;
    const uint64_t ep1 = reg.Epoch();

    // Respawn at same pointer with new instanceId -> strictly new token & bumped epoch
    reg.Publish({{&objA, 102, "hski", "花海咲季", "", ""}});
    GKE2E_REQUIRE_EQ(ctx, reg.Entries().size(), size_t{1});
    GKE2E_CHECK(ctx, reg.Entries()[0].token > tok1);
    GKE2E_CHECK(ctx, reg.Epoch() > ep1);
}

void Test_T2_F6_04(gke2e::CaseContext& ctx) {
    SceneActorRegistry reg;
    int objA = 1;
    reg.Publish({{&objA, 11, "hski", "花海咲季", "", ""}});
    const uint64_t tok = reg.Entries()[0].token;
    const uint64_t ep = reg.Epoch();

    GKE2E_CHECK(ctx, reg.Resolve(tok, ep - 1) == nullptr);
    GKE2E_CHECK(ctx, reg.Resolve(tok + 999, ep) == nullptr);
    GKE2E_CHECK(ctx, reg.Find(nullptr) == nullptr);

    reg.Clear();
    GKE2E_CHECK(ctx, reg.Resolve(tok, ep) == nullptr);
    GKE2E_CHECK(ctx, reg.Resolve(tok, reg.Epoch()) == nullptr);
}

void Test_T2_F6_05(gke2e::CaseContext& ctx) {
    SceneActorObservation npc;
    npc.character = "";
    npc.displayName = "";
    GKE2E_CHECK_EQ(ctx, FormatSceneActorLabel(npc, 1), "未识别角色 · 1");

    SceneActorObservation customNpc;
    customNpc.character = "custom_npc";
    customNpc.displayName = "自定义NPC";
    GKE2E_CHECK_EQ(ctx, FormatSceneActorLabel(customNpc, 2), "自定义NPC (custom_npc) · 2");

    BridgeFixture f;
    int a1 = 1, a2 = 2, a3 = 3;
    f.core.PublishSceneActors({
        {&a1, 201, "custom_npc", "自定义NPC",   "", ""},
        {&a2, 202, "",           "StageStaff", "", ""},
        {&a3, 203, "",           "",           "", ""},
    });
    if (!ProbeNpcGuiShortNameFallback<GuiSceneActor>(ctx, f.core)) {
        GKE2E_PENDING(ctx, "GuiSceneActor::shortName NPC fallback pending M2.");
    }
    if (CheckM4HelloCapabilities(f).empty()) {
        const auto snap = f.ReqJson("state.get");
        GKE2E_REQUIRE(ctx, f.IsOk(snap));
        const auto* res = snap.Find("result");
        const auto* actorsObj = res ? res->Find("actors") : nullptr;
        const auto* arr = actorsObj ? actorsObj->Find("actors") : nullptr;
        GKE2E_REQUIRE(ctx, arr && arr->IsArray() && arr->Size() == 3);
        CheckJsonActorMetadata(ctx, arr->At(0), "custom_npc", "自定义NPC", "自定义NPC");
        CheckJsonActorMetadata(ctx, arr->At(1), "", "StageStaff", "StageStaff");
        CheckJsonActorMetadata(ctx, arr->At(2), "", "", "", false);
    }
}

void Test_T2_F6_06(gke2e::CaseContext& ctx) {
    // Verify JSON escaping in ActorsJsonLocked survives CJK, quotes, backslashes, and control chars
    BridgeFixture f;
    int objNpc = 7;
    f.core.sceneActorRegistry.Publish({
        {&objNpc, 77, "", "NPC\"Quote\\Line\nTest\t\x01", "", ""},
    });
    const auto state = f.ReqJson("state.get");
    GKE2E_REQUIRE(ctx, f.IsOk(state));
    const auto* res = state.Find("result");
    const auto* actorsObj = res ? res->Find("actors") : nullptr;
    const auto* arr = actorsObj ? actorsObj->Find("actors") : nullptr;
    GKE2E_REQUIRE(ctx, arr && arr->IsArray() && arr->Size() == 1);
    const auto* label = arr->At(0)->Find("label");
    GKE2E_REQUIRE(ctx, label && label->IsString());
    GKE2E_CHECK(ctx, label->stringValue.find("NPC\"Quote\\Line\nTest\t\x01") != std::string::npos);

    // Mixed-case delimiter extraction & non-ASCII prefix boundary
    GKE2E_CHECK_EQ(ctx, DetectIdolCharacterCode("  HsKi  "), "hski");
    GKE2E_CHECK_EQ(ctx, DetectIdolCharacterCode("ANDK_01"), "andk");
}

// -----------------------------------------------------------------------------
// F13 C++ Tests (Tier 1 & Tier 2)
// -----------------------------------------------------------------------------
void Test_T1_F13_01(gke2e::CaseContext& ctx) {
    TempDirGuard tmp("t1_f13_01");
    const fs::path uiDir = tmp.path / "GakuMotion.UI";
    GKE2E_REQUIRE(ctx, WriteFileBytes(uiDir / "index.html", {'<', 'h', 't', 'm', 'l', '>'}));

    BridgeFixture f;
    f.assetDirs.push_back(uiDir.wstring());

    GakuWebPanelAssetsV1 assets{};
    GKE2E_REQUIRE_EQ(ctx, f.bridge->GetAssets(&assets), GAKU_OK);
    GKE2E_CHECK_EQ(ctx, assets.abiVersion, GAKU_WEB_PANEL_VERSION);
    GKE2E_CHECK(ctx, std::wstring_view(assets.virtualHost) == L"gakumotion.invalid");
    GKE2E_CHECK(ctx, std::wstring_view(assets.entryPath) == L"index.html");

    // GAKU_E_SIZE -> GAKU_OK NUL-terminated buffer negotiation
    const char* pingReq = "{\"v\":1,\"id\":1,\"type\":\"request\",\"method\":\"bridge.ping\",\"params\":{}}";
    char tiny[8]{};
    uint32_t needed = 0;
    GKE2E_CHECK_EQ(ctx, f.bridge->HandleMessage(pingReq, sizeof(tiny), tiny, &needed), GAKU_E_SIZE);
    GKE2E_REQUIRE(ctx, needed > sizeof(tiny));
    std::vector<char> fullBuf(needed, '\0');
    GKE2E_CHECK_EQ(ctx, f.bridge->HandleMessage(pingReq, needed, fullBuf.data(), &needed), GAKU_OK);
    GKE2E_CHECK(ctx, fullBuf[needed - 1] == '\0');

    const std::string pendingReason = CheckM4HelloCapabilities(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    const auto hello = f.ReqJson("motion.hello");
    const auto* res = hello.Find("result");
    const auto* methods = res ? res->Find("supportedMethods") : nullptr;
    const auto* events = res ? res->Find("supportedEvents") : nullptr;
    GKE2E_CHECK(ctx, !JsonArrayContainsString(methods, "ensemble.set"));
    GKE2E_CHECK(ctx, !JsonArrayContainsString(events, "ensemble.changed"));
}

void Test_T1_F13_02(gke2e::CaseContext& ctx) {
    BridgeFixture f;
    const std::string pendingReason = CheckM4HelloCapabilities(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    MotionEntry group;
    group.name = "grp-song";
    group.dir = "synthetic/motions/grp-song";
    group.title = "Group Song";
    group.durationSeconds = 10.0f;
    group.totalFrames = 300;
    if (!PopulateGroupMotionEntry(group)) {
        GKE2E_PENDING(ctx, "M1 MotionEntry group fields (isGroup/positionCount/positions) pending.");
    }
    f.core.motions = {group};

    int objA = 1;
    f.core.PublishSceneActors({{&objA, 10, "hski", "花海咲季", "chr_hski_face_00", ""}});

    const auto snap = f.ReqJson("state.get");
    GKE2E_REQUIRE(ctx, f.IsOk(snap));
    const auto* res = snap.Find("result");
    GKE2E_REQUIRE(ctx, res != nullptr);

    const auto* lib = res->Find("library");
    const auto* entries = lib ? lib->Find("entries") : nullptr;
    GKE2E_REQUIRE(ctx, entries && entries->IsArray() && entries->Size() == 1);
    const auto* e0 = entries->At(0);
    GKE2E_REQUIRE(ctx, e0 && e0->IsObject());
    GKE2E_CHECK(ctx, e0->Find("isGroup") && e0->Find("isGroup")->IsBool() && e0->Find("isGroup")->boolValue);
    GKE2E_CHECK(ctx, e0->Find("positionCount") && e0->Find("positionCount")->IsNumber() &&
                     e0->Find("positionCount")->numberValue == 3.0);
    const auto* posArr = e0->Find("positions");
    GKE2E_REQUIRE(ctx, posArr && posArr->IsArray() && posArr->Size() == 3);
    for (size_t s = 0; s < 3; ++s) {
        const auto* p = posArr->At(s);
        GKE2E_REQUIRE(ctx, p && p->IsObject());
        GKE2E_CHECK(ctx, p->Find("slot") && p->Find("slot")->IsNumber() &&
                         p->Find("slot")->numberValue == static_cast<double>(s + 1));
        GKE2E_CHECK(ctx, p->Find("x") && p->Find("x")->IsNumber());
        GKE2E_CHECK(ctx, p->Find("y") && p->Find("y")->IsNumber());
        GKE2E_CHECK(ctx, p->Find("z") && p->Find("z")->IsNumber());
        GKE2E_CHECK(ctx, p->Find("yawDegrees") && p->Find("yawDegrees")->IsNumber());
        GKE2E_CHECK(ctx, p->Find("hasMotion") && p->Find("hasMotion")->IsBool() && p->Find("hasMotion")->boolValue);
        GKE2E_CHECK(ctx, p->Find("hasFacial") && p->Find("hasFacial")->IsBool() &&
                         p->Find("hasFacial")->boolValue == (s == 0));
        if (p->Find("x") && p->Find("x")->IsNumber()) {
            GKE2E_CHECK_NEAR(ctx, p->Find("x")->numberValue, static_cast<double>(s) * 0.5, 1e-4);
        }
        if (p->Find("y") && p->Find("y")->IsNumber()) {
            GKE2E_CHECK_NEAR(ctx, p->Find("y")->numberValue, (s == 0) ? 0.0 : 0.1, 1e-4);
        }
        if (p->Find("z") && p->Find("z")->IsNumber()) {
            GKE2E_CHECK_NEAR(ctx, p->Find("z")->numberValue, (s == 0) ? 0.0 : -0.25, 1e-4);
        }
        if (p->Find("yawDegrees") && p->Find("yawDegrees")->IsNumber()) {
            GKE2E_CHECK_NEAR(ctx, p->Find("yawDegrees")->numberValue,
                             (s == 0) ? 0.0 : static_cast<double>(s) * 15.0, 1e-4);
        }
    }

    const auto* actorsObj = res->Find("actors");
    const auto* actorsArr = actorsObj ? actorsObj->Find("actors") : nullptr;
    GKE2E_REQUIRE(ctx, actorsArr && actorsArr->IsArray() && actorsArr->Size() == 1);
    CheckJsonActorMetadata(ctx, actorsArr->At(0), "hski", "花海咲季", "咲季");

    const auto* cam = res->Find("camera");
    const auto* camSettings = cam ? cam->Find("settings") : nullptr;
    const auto* followIdx = camSettings ? camSettings->Find("followSlotIndex") : nullptr;
    GKE2E_CHECK(ctx, followIdx && followIdx->IsNumber());

    const auto* fm = res->Find("formation");
    GKE2E_REQUIRE(ctx, fm && fm->IsObject());
    GKE2E_CHECK(ctx, fm->Find("active") && fm->Find("active")->IsBool());
    GKE2E_CHECK(ctx, fm->Find("isGroupSong") && fm->Find("isGroupSong")->IsBool());
    GKE2E_CHECK(ctx, fm->Find("trackPositionCount") && fm->Find("trackPositionCount")->IsNumber());
    GKE2E_CHECK(ctx, fm->Find("slotCount") && fm->Find("slotCount")->IsNumber() &&
                     fm->Find("slotCount")->numberValue >= 1.0);
    GKE2E_CHECK(ctx, fm->Find("selectedSlotIndex") && fm->Find("selectedSlotIndex")->IsNumber() &&
                     fm->Find("selectedSlotIndex")->numberValue >= 0.0);
    const auto* slots = fm->Find("slots");
    GKE2E_REQUIRE(ctx, slots && slots->IsArray() && slots->Size() >= 1);
    const auto* s0 = slots->At(0);
    GKE2E_REQUIRE(ctx, s0 && s0->IsObject());
    GKE2E_CHECK(ctx, s0->Find("slotIndex") && s0->Find("slotIndex")->IsNumber() &&
                     s0->Find("slotIndex")->numberValue == 0.0);
    GKE2E_CHECK(ctx, s0->Find("occupied") && s0->Find("occupied")->IsBool());
    GKE2E_CHECK(ctx, s0->Find("actorToken") && s0->Find("actorToken")->IsNumber());
    GKE2E_CHECK(ctx, s0->Find("character") && s0->Find("character")->IsString());
    GKE2E_CHECK(ctx, s0->Find("shortName") && s0->Find("shortName")->IsString());
    GKE2E_CHECK(ctx, s0->Find("displayName") && s0->Find("displayName")->IsString());
    GKE2E_CHECK(ctx, s0->Find("isOverflow") && s0->Find("isOverflow")->IsBool());
    GKE2E_CHECK(ctx, s0->Find("hasTrackMotion") && s0->Find("hasTrackMotion")->IsBool());
    GKE2E_CHECK(ctx, s0->Find("sourceSlotIndex") && s0->Find("sourceSlotIndex")->IsNumber());
    GKE2E_CHECK(ctx, s0->Find("placementDirty") && s0->Find("placementDirty")->IsBool());
    GKE2E_CHECK(ctx, s0->Find("hasActorBoneOverrides") && s0->Find("hasActorBoneOverrides")->IsBool());

    const auto* off0 = s0->Find("offset");
    GKE2E_REQUIRE(ctx, off0 && off0->IsObject());
    GKE2E_CHECK(ctx, off0->Find("x") && off0->Find("x")->IsNumber());
    GKE2E_CHECK(ctx, off0->Find("y") && off0->Find("y")->IsNumber());
    GKE2E_CHECK(ctx, off0->Find("z") && off0->Find("z")->IsNumber());
    GKE2E_CHECK(ctx, off0->Find("yawDegrees") && off0->Find("yawDegrees")->IsNumber());

    const auto* fw0 = s0->Find("facialWeights");
    GKE2E_REQUIRE(ctx, fw0 && fw0->IsObject());
    GKE2E_CHECK(ctx, fw0->Find("overall") && fw0->Find("overall")->IsNumber());
    GKE2E_CHECK(ctx, fw0->Find("brow") && fw0->Find("brow")->IsNumber());
    GKE2E_CHECK(ctx, fw0->Find("eye") && fw0->Find("eye")->IsNumber());
    GKE2E_CHECK(ctx, fw0->Find("mouth") && fw0->Find("mouth")->IsNumber());
}

void Test_T1_F13_03(gke2e::CaseContext& ctx) {
    TempDirGuard tmp("t1_f13_03");
    BridgeFixture f;
    f.core.configRoot = Utf8FromPath(tmp.path);
    const std::string pendingReason = CheckM4HelloCapabilities(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    const auto png512 = MakeSyntheticRgbaPng(512, 512, true);
    GKE2E_REQUIRE(ctx, WriteFileBytes(tmp.path / "portraits" / "hski.png", png512));

    int objA = 1;
    f.core.PublishSceneActors({{&objA, 11, "hski", "花海咲季", "chr_hski_face_00", ""}});
    const uint64_t tok = f.core.sceneActorRegistry.Entries()[0].token;

    const auto byChar = f.ReqJson("actors.getPortrait", "{\"character\":\"hski\"}");
    GKE2E_REQUIRE(ctx, f.IsOk(byChar));
    const auto* resChar = byChar.Find("result");
    GKE2E_REQUIRE(ctx, resChar != nullptr);
    GKE2E_CHECK(ctx, resChar->Find("cached") && resChar->Find("cached")->boolValue);
    const auto* uriVal = resChar->Find("dataUri");
    GKE2E_REQUIRE(ctx, uriVal && uriVal->IsString());
    std::string decMime;
    std::vector<uint8_t> decBytes;
    GKE2E_CHECK(ctx, cover::DecodeDataUri(uriVal->stringValue, decMime, decBytes));
    GKE2E_CHECK_EQ(ctx, decMime, "image/png");

    const auto byTok = f.ReqJson("actors.getPortrait", "{\"token\":" + std::to_string(tok) + "}");
    GKE2E_CHECK(ctx, f.IsOk(byTok));

    // Exercise formation.* RPCs, formation.changed event, info.json disk save, and camera.set followSlotIndex
    const fs::path motionsDir = tmp.path / "motions";
    const fs::path trackDir = motionsDir / "grp-song";
    GKE2E_REQUIRE(ctx, gkmotion::e2e::fixtures::write_synthetic_group_track(
        trackDir, 2, "Group Song", "Author", true, false));
    f.core.motionsRoot = Utf8FromPath(motionsDir);
    f.core.initialPositionPath = Utf8FromPath(tmp.path / "initial_position.json");
    std::string scanErr;
    GKE2E_REQUIRE_MSG(ctx, ScanMotionLibrary(f.core.motionsRoot, f.core.motions, scanErr), scanErr);
    f.core.libraryRevision = 1;

    int obj0 = 10, obj1 = 11, obj2 = 12;
    f.core.PublishSceneActors({
        {&obj0, 201, "hski", "花海咲季", "chr_hski_face_00", ""},
        {&obj1, 202, "ttmr", "月村手毬", "chr_ttmr_face_00", ""},
        {&obj2, 203, "fktn", "藤田琴音", "chr_fktn_face_00", ""},
    });
    const auto& actorEntries = f.core.sceneActorRegistry.Entries();
    GKE2E_REQUIRE_EQ(ctx, actorEntries.size(), size_t{3});
    const uint64_t tok0 = actorEntries[0].token;
    const uint64_t tok1 = actorEntries[1].token;

    const auto selTrack = f.ReqJson("library.select", "{\"key\":\"grp-song\",\"autoplay\":false,\"revision\":1}");
    GKE2E_REQUIRE(ctx, f.IsOk(selTrack));
    f.core.ProcessPendingGuiCommands();
    GKE2E_REQUIRE_EQ(ctx, f.core.loadedMotionKey, "grp-song");

    f.bridge->OnUiTick();
    f.events.clear();

    // 1. formation.selectSlot
    GKE2E_REQUIRE(ctx, f.IsOk(f.ReqJson("formation.selectSlot", "{\"slotIndex\":1}")));
    f.core.ProcessPendingGuiCommands();
    {
        const auto st = f.ReqJson("state.get");
        GKE2E_REQUIRE(ctx, f.IsOk(st));
        const auto* fm = st.Find("result") ? st.Find("result")->Find("formation") : nullptr;
        GKE2E_REQUIRE(ctx, fm && fm->Find("selectedSlotIndex"));
        GKE2E_CHECK_EQ(ctx, fm->Find("selectedSlotIndex")->numberValue, 1.0);
    }

    // 2. formation.setSlotOffset + formation.changed event
    GKE2E_REQUIRE(ctx, f.IsOk(f.ReqJson(
        "formation.setSlotOffset",
        "{\"slotIndex\":1,\"x\":0.5,\"y\":0.1,\"z\":-0.25,\"yawDegrees\":15.0}")));
    f.core.ProcessPendingGuiCommands();
    f.bridge->OnUiTick();
    {
        jsonlite::Value fmEv;
        GKE2E_REQUIRE(ctx, FindLastEventPayload(f.events, "formation.changed", fmEv));
        GKE2E_CHECK(ctx, fmEv.Find("selectedSlotIndex") && fmEv.Find("selectedSlotIndex")->numberValue == 1.0);
        const auto* evSlots = fmEv.Find("slots");
        GKE2E_REQUIRE(ctx, evSlots && evSlots->IsArray() && evSlots->Size() >= 2);
        const auto* off1 = evSlots->At(1)->Find("offset");
        GKE2E_REQUIRE(ctx, off1 && off1->IsObject());
        GKE2E_CHECK_NEAR(ctx, off1->Find("x")->numberValue, 0.5, 1e-4);
        GKE2E_CHECK_NEAR(ctx, off1->Find("y")->numberValue, 0.1, 1e-4);
        GKE2E_CHECK_NEAR(ctx, off1->Find("z")->numberValue, -0.25, 1e-4);
        GKE2E_CHECK_NEAR(ctx, off1->Find("yawDegrees")->numberValue, 15.0, 1e-4);
    }

    // 3. formation.saveSlotOffset (slot 0 -> global, slot 1 -> info.json)
    GKE2E_CHECK(ctx, f.IsOk(f.ReqJson("formation.saveSlotOffset", "{\"slotIndex\":0}")));
    f.core.ProcessPendingGuiCommands();
    GKE2E_REQUIRE(ctx, f.IsOk(f.ReqJson("formation.saveSlotOffset", "{\"slotIndex\":1}")));
    f.core.ProcessPendingGuiCommands();
    {
        jsonlite::Value info;
        std::string infoErr;
        GKE2E_REQUIRE_MSG(ctx, ParseTrackInfoJsonFile(trackDir / "info.json", info, infoErr), infoErr);
        const auto* positions = info.Find("positions");
        GKE2E_REQUIRE(ctx, positions && positions->IsArray() && positions->Size() >= 2);
        const auto* p1 = positions->At(1);
        GKE2E_REQUIRE(ctx, p1 && p1->IsObject());
        GKE2E_CHECK_EQ(ctx, p1->IntPath("slot", 0), 2);
        GKE2E_CHECK_NEAR(ctx, p1->FloatPath("x", 0.0f), 0.5, 1e-4);
        GKE2E_CHECK_NEAR(ctx, p1->FloatPath("y", 0.0f), 0.1, 1e-4);
        GKE2E_CHECK_NEAR(ctx, p1->FloatPath("z", 0.0f), -0.25, 1e-4);
        GKE2E_CHECK_NEAR(ctx, p1->FloatPath("yaw_degrees", 0.0f), 15.0, 1e-4);
    }

    // 4. formation.swapSlots (actors swap, offset stays with slot 1)
    GKE2E_REQUIRE(ctx, f.IsOk(f.ReqJson("formation.swapSlots", "{\"fromSlotIndex\":0,\"toSlotIndex\":1}")));
    f.core.ProcessPendingGuiCommands();
    {
        const auto st = f.ReqJson("state.get");
        GKE2E_REQUIRE(ctx, f.IsOk(st));
        const auto* slots = st.Find("result")->Find("formation")->Find("slots");
        GKE2E_REQUIRE(ctx, slots && slots->IsArray() && slots->Size() >= 3);
        GKE2E_CHECK_EQ(ctx, static_cast<uint64_t>(slots->At(0)->Find("actorToken")->numberValue), tok1);
        GKE2E_CHECK_EQ(ctx, static_cast<uint64_t>(slots->At(1)->Find("actorToken")->numberValue), tok0);
        GKE2E_CHECK_NEAR(ctx, slots->At(1)->Find("offset")->Find("x")->numberValue, 0.5, 1e-4);
    }

    // 5. formation.setSlotSource (overflow slot 2 reuses slot 1)
    GKE2E_REQUIRE(ctx, f.IsOk(f.ReqJson("formation.setSlotSource", "{\"slotIndex\":2,\"sourceSlotIndex\":1}")));
    f.core.ProcessPendingGuiCommands();
    {
        const auto st = f.ReqJson("state.get");
        GKE2E_REQUIRE(ctx, f.IsOk(st));
        const auto* slots = st.Find("result")->Find("formation")->Find("slots");
        GKE2E_REQUIRE(ctx, slots && slots->IsArray() && slots->Size() >= 3);
        GKE2E_CHECK_EQ(ctx, slots->At(2)->Find("sourceSlotIndex")->numberValue, 1.0);
    }

    // 6. camera.set followSlotIndex (-1 and 0) + camera.changed
    f.events.clear();
    GKE2E_REQUIRE(ctx, f.IsOk(f.ReqJson("camera.set", "{\"settings\":{\"followSlotIndex\":-1}}")));
    f.core.ProcessPendingGuiCommands();
    {
        const auto st = f.ReqJson("state.get");
        GKE2E_REQUIRE(ctx, f.IsOk(st));
        GKE2E_CHECK_EQ(ctx, st.Find("result")->Find("camera")->Find("settings")->Find("followSlotIndex")->numberValue, -1.0);
    }
    GKE2E_REQUIRE(ctx, f.IsOk(f.ReqJson("camera.set", "{\"settings\":{\"followSlotIndex\":0}}")));
    f.core.ProcessPendingGuiCommands();
    f.bridge->OnUiTick();
    {
        const auto st = f.ReqJson("state.get");
        GKE2E_REQUIRE(ctx, f.IsOk(st));
        GKE2E_CHECK_EQ(ctx, st.Find("result")->Find("camera")->Find("settings")->Find("followSlotIndex")->numberValue, 0.0);
        jsonlite::Value camEv;
        GKE2E_CHECK(ctx, FindLastEventPayload(f.events, "camera.changed", camEv));
    }
}

void Test_T1_F13_04(gke2e::CaseContext& ctx) {
    TempDirGuard tmp("t1_f13_04");
    const auto vmdA = gkmotion::e2e::fixtures::make_body_vmd(4, true, 1);
    const auto vmdB = gkmotion::e2e::fixtures::make_body_vmd(4, false, 2);
    const auto vmdC = gkmotion::e2e::fixtures::make_body_vmd(4, false, 3);
    const auto camVmd = gkmotion::e2e::fixtures::make_camera_vmd(4, 45);
    const auto wav = gkmotion::e2e::fixtures::make_pcm_wav_bytes(0.5);

    const fs::path pA = tmp.path / "01_a.vmd";
    const fs::path pB = tmp.path / "02_b.vmd";
    const fs::path pC = tmp.path / "03_c.vmd";
    const fs::path pCam = tmp.path / "cam.vmd";
    const fs::path pWav = tmp.path / "song.wav";
    GKE2E_REQUIRE(ctx, WriteFileBytes(pC, vmdC));
    GKE2E_REQUIRE(ctx, WriteFileBytes(pA, vmdA));
    GKE2E_REQUIRE(ctx, WriteFileBytes(pB, vmdB));
    GKE2E_REQUIRE(ctx, WriteFileBytes(pCam, camVmd));
    GKE2E_REQUIRE(ctx, WriteFileBytes(pWav, wav));

    BridgeFixture f;
    const std::string params = "{\"paths\":[" +
        JString(Utf8FromPath(pC)) + "," + JString(Utf8FromPath(pA)) + "," +
        JString(Utf8FromPath(pB)) + "," + JString(Utf8FromPath(pCam)) + "," + JString(Utf8FromPath(pWav)) + "]}";
    const auto resp = f.ReqJson("library.probeFiles", params);
    GKE2E_REQUIRE(ctx, f.IsOk(resp));
    const auto* res = resp.Find("result");
    const auto* cls = res ? res->Find("classification") : nullptr;
    GKE2E_REQUIRE(ctx, cls != nullptr);

    const auto* mc = cls->Find("motionCandidates");
    const auto* pos = cls->Find("positions");
    if (!mc && !pos) {
        GKE2E_PENDING(ctx, "Multi-VMD library.probeFiles classification pending M4.");
    }

    const auto* camC = cls->Find("cameraCandidate");
    const auto* audC = cls->Find("audioCandidate");
    GKE2E_REQUIRE(ctx, camC && camC->IsString());
    GKE2E_REQUIRE(ctx, audC && audC->IsString());
    GKE2E_CHECK(ctx, MotionPathEqualsUtf8(camC->stringValue, Utf8FromPath(pCam)));
    GKE2E_CHECK(ctx, MotionPathEqualsUtf8(audC->stringValue, Utf8FromPath(pWav)));

    std::vector<std::string> sortedMotionPaths;
    if (mc) {
        GKE2E_REQUIRE(ctx, mc->IsArray() && mc->Size() == 3);
        for (size_t i = 0; i < mc->Size(); ++i) {
            const auto* item = mc->At(i);
            GKE2E_REQUIRE(ctx, item && item->IsString());
            sortedMotionPaths.push_back(item->stringValue);
        }
    } else {
        GKE2E_REQUIRE(ctx, pos->IsArray() && pos->Size() == 3);
        for (size_t i = 0; i < pos->Size(); ++i) {
            const auto* item = pos->At(i);
            GKE2E_REQUIRE(ctx, item != nullptr);
            if (item->IsString()) {
                sortedMotionPaths.push_back(item->stringValue);
            } else {
                GKE2E_REQUIRE(ctx, item->IsObject());
                const jsonlite::Value* pVal = item->Find("motionCandidate");
                if (!pVal) pVal = item->Find("motionPath");
                if (!pVal) pVal = item->Find("motionFile");
                if (!pVal) pVal = item->Find("path");
                GKE2E_REQUIRE(ctx, pVal && pVal->IsString());
                sortedMotionPaths.push_back(pVal->stringValue);
            }
        }
    }
    GKE2E_REQUIRE_EQ(ctx, sortedMotionPaths.size(), size_t{3});
    GKE2E_CHECK(ctx, MotionPathEqualsUtf8(sortedMotionPaths[0], Utf8FromPath(pA)));
    GKE2E_CHECK(ctx, MotionPathEqualsUtf8(sortedMotionPaths[1], Utf8FromPath(pB)));
    GKE2E_CHECK(ctx, MotionPathEqualsUtf8(sortedMotionPaths[2], Utf8FromPath(pC)));
}

void Test_T2_F13_01(gke2e::CaseContext& ctx) {
    BridgeFixture f;
    // Malformed JSON / wrong version / non-object params / unknown method
    const auto malformed = ParseJson(f.bridge->HandleMessageForTest("{not json"));
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(malformed), "invalid_params");
    const auto badVer = ParseJson(f.bridge->HandleMessageForTest(
        "{\"v\":2,\"id\":1,\"type\":\"request\",\"method\":\"bridge.ping\"}"));
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(badVer), "invalid_params");
    const auto badParams = ParseJson(f.bridge->HandleMessageForTest(
        "{\"v\":1,\"id\":2,\"type\":\"request\",\"method\":\"bridge.ping\",\"params\":42}"));
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(badParams), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("no.such.method")), "unknown_method");

    // OnShutdown emits bridge.closed, bumps sessionId, resets seq=1 on reattach
    const uint64_t sid1 = f.bridge->SessionId();
    f.bridge->OnUiTick();
    f.events.clear();
    f.bridge->OnShutdown();
    GKE2E_CHECK(ctx, f.bridge->SessionId() > sid1);
    GKE2E_CHECK(ctx, !f.events.empty());

    std::vector<std::string> events2;
    f.bridge->SetEventSink(
        [](void* ud, const char* json) { static_cast<std::vector<std::string>*>(ud)->push_back(json ? json : ""); },
        &events2);
    f.bridge->OnShow();
    f.bridge->OnUiTick();
    GKE2E_REQUIRE(ctx, !events2.empty());
    const auto ev0 = ParseJson(events2[0]);
    GKE2E_CHECK(ctx, ev0.Find("seq") && ev0.Find("seq")->numberValue == 1);

    // Invalidate permanently closes bridge without invoking sink
    events2.clear();
    f.bridge->Invalidate("shutdown");
    GKE2E_CHECK(ctx, events2.empty());
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("bridge.ping")), "not_allowed");
}

void Test_T2_F13_02(gke2e::CaseContext& ctx) {
    BridgeFixture f;
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("library.probeFiles", "{\"paths\":[]}")), "invalid_params");

    std::string tooMany = "{\"paths\":[";
    for (int i = 0; i < 257; ++i) {
        if (i) tooMany += ",";
        tooMany += "\"f" + std::to_string(i) + ".vmd\"";
    }
    tooMany += "]}";
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("library.probeFiles", tooMany)), "invalid_params");

    TempDirGuard tmp("t2_f13_02");
    const fs::path pGood = tmp.path / "good.vmd";
    const fs::path pZero = tmp.path / "zero.vmd";
    GKE2E_REQUIRE(ctx, WriteFileBytes(pGood, gkmotion::e2e::fixtures::make_body_vmd(3, false, 1)));
    GKE2E_REQUIRE(ctx, WriteFileBytes(pZero, {}));
    const auto resp = f.ReqJson("library.probeFiles",
                                "{\"paths\":[" + JString(pGood.string()) + "," + JString(pZero.string()) + "]}");
    GKE2E_REQUIRE(ctx, f.IsOk(resp));
    const auto* res = resp.Find("result");
    const auto* probes = res ? res->Find("probes") : nullptr;
    GKE2E_REQUIRE(ctx, probes && probes->IsArray() && probes->Size() == 2);
    GKE2E_CHECK(ctx, probes->At(0)->Find("valid")->boolValue == true);
    GKE2E_CHECK(ctx, probes->At(1)->Find("valid")->boolValue == false);
}

void Test_T2_F13_03(gke2e::CaseContext& ctx) {
    BridgeFixture f;
    const std::string pendingReason = CheckM4HelloCapabilities(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("actors.getPortrait", "{}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("actors.getPortrait", "{\"character\":\"../hski\"}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("actors.getPortrait", "{\"character\":\"a\\\\b\"}")), "invalid_params");

    const auto npcRes = f.ReqJson("actors.getPortrait", "{\"character\":\"uncached_npc\"}");
    if (f.IsOk(npcRes)) {
        const auto* res = npcRes.Find("result");
        const auto* uri = res ? res->Find("dataUri") : nullptr;
        GKE2E_CHECK(ctx, !uri || uri->IsNull() || (uri->IsString() && uri->stringValue.empty()));
    }
}

void Test_T2_F13_04(gke2e::CaseContext& ctx) {
    BridgeFixture f;
    const std::string pendingReason = CheckM4HelloCapabilities(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.selectSlot", "{\"slotIndex\":-1}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.selectSlot", "{\"slotIndex\":999}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.swapSlots", "{\"fromSlotIndex\":0,\"toSlotIndex\":0}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.swapSlots", "{\"fromSlotIndex\":-1,\"toSlotIndex\":0}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.swapSlots", "{\"fromSlotIndex\":0,\"toSlotIndex\":999}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.setSlotOffset", "{\"slotIndex\":0,\"x\":999.0,\"y\":0,\"z\":0,\"yawDegrees\":0}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.setSlotOffset", "{\"slotIndex\":-1,\"x\":0,\"y\":0,\"z\":0,\"yawDegrees\":0}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.setSlotOffset", "{\"slotIndex\":999,\"x\":0,\"y\":0,\"z\":0,\"yawDegrees\":0}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.setSlotOffset", "{\"slotIndex\":0}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.saveSlotOffset", "{\"slotIndex\":-1}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.saveSlotOffset", "{\"slotIndex\":999}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.saveSlotOffset", "{}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.setSlotSource", "{\"slotIndex\":-1,\"sourceSlotIndex\":0}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.setSlotSource", "{\"slotIndex\":999,\"sourceSlotIndex\":0}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.setSlotSource", "{\"slotIndex\":0,\"sourceSlotIndex\":-1}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.setSlotSource", "{\"slotIndex\":0,\"sourceSlotIndex\":999}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.setSlotSource", "{}")), "invalid_params");

    f.core.injectionPolicy.SetSceneAllowed(false);
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.selectSlot", "{\"slotIndex\":0}")), "not_allowed");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.setSlotOffset", "{\"slotIndex\":0,\"x\":0,\"y\":0,\"z\":0,\"yawDegrees\":0}")), "not_allowed");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("formation.saveSlotOffset", "{\"slotIndex\":0}")), "not_allowed");
}

void Test_T2_F13_05(gke2e::CaseContext& ctx) {
    BridgeFixture f;
    const std::string pendingReason = CheckM4HelloCapabilities(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    const auto state = f.ReqJson("state.get");
    const auto* cam = state.Find("result") ? state.Find("result")->Find("camera") : nullptr;
    const auto* settings = cam ? cam->Find("settings") : nullptr;
    if (!settings || !settings->Find("followSlotIndex")) {
        GKE2E_PENDING(ctx, "CameraSettings.followSlotIndex pending M3/M4.");
    }
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("camera.set", "{\"settings\":{\"followSlotIndex\":-2}}")), "invalid_params");
    GKE2E_CHECK_EQ(ctx, f.ErrorCode(f.ReqJson("camera.set", "{\"settings\":{\"followSlotIndex\":1.5}}")), "invalid_params");
}

// -----------------------------------------------------------------------------
// Tier 3 — Pairwise Cross-Feature Tests (F5xF6, F6xF13, F5xF13)
// -----------------------------------------------------------------------------
void Test_T3_F5F6_01(gke2e::CaseContext& ctx) {
    GKE2E_REQUIRE_HEADER(ctx, GK_E2E_HAS_PORTRAIT_CACHE, "GkPortraitCache.hpp");
    BridgeFixture f;
    const std::string pendingReason = CheckActorsPortraitCapability(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    TempDirGuard tmp("t3_f5f6_01");
    f.core.configRoot = Utf8FromPath(tmp.path);
    const auto pngHski = MakeSyntheticRgbaPng(512, 512, true);
    GKE2E_REQUIRE(ctx, !pngHski.empty());
    GKE2E_REQUIRE(ctx, WriteFileBytes(tmp.path / "portraits" / "hski.png", pngHski));

    int a1 = 1, a2 = 2;
    f.core.PublishSceneActors({
        {&a1, 10, DetectIdolCharacterCode("chr_hski_face_00"), "花海咲季", "chr_hski_face_00", ""},
        {&a2, 20, DetectIdolCharacterCode("custom_npc_00"), "CustomNPC", "custom_npc_00", ""},
    });
    const auto& regEntries = f.core.sceneActorRegistry.Entries();
    GKE2E_REQUIRE_EQ(ctx, regEntries.size(), size_t{2});
    GKE2E_CHECK_EQ(ctx, regEntries[0].character, "hski");
    GKE2E_CHECK_EQ(ctx, regEntries[1].character, "");
    const uint64_t tokIdol = regEntries[0].token;
    const uint64_t tokNpc = regEntries[1].token;

    const auto snap = f.ReqJson("state.get");
    GKE2E_REQUIRE(ctx, f.IsOk(snap));
    const auto* arr = snap.Find("result")->Find("actors")->Find("actors");
    GKE2E_REQUIRE(ctx, arr && arr->IsArray() && arr->Size() == 2);
    CheckJsonActorMetadata(ctx, arr->At(0), "hski", "花海咲季", "咲季");
    CheckJsonActorMetadata(ctx, arr->At(1), "", "CustomNPC", "CustomNPC");

    const auto idolResp = f.ReqJson("actors.getPortrait", "{\"token\":" + std::to_string(tokIdol) + "}");
    GKE2E_REQUIRE(ctx, f.IsOk(idolResp));
    const auto* idolRes = idolResp.Find("result");
    GKE2E_REQUIRE(ctx, idolRes && idolRes->IsObject());
    GKE2E_CHECK(ctx, idolRes->Find("character") && idolRes->Find("character")->stringValue == "hski");
    GKE2E_CHECK(ctx, idolRes->Find("cached") && idolRes->Find("cached")->boolValue);
    const auto* uri = idolRes->Find("dataUri");
    GKE2E_REQUIRE(ctx, uri && uri->IsString());
    std::string decMime;
    std::vector<uint8_t> decBytes;
    GKE2E_REQUIRE(ctx, cover::DecodeDataUri(uri->stringValue, decMime, decBytes));
    GKE2E_CHECK_EQ(ctx, decMime, "image/png");
    GKE2E_CHECK(ctx, decBytes == pngHski);

    const auto npcResp = f.ReqJson("actors.getPortrait", "{\"token\":" + std::to_string(tokNpc) + "}");
    CheckNoPortraitDataReturned(ctx, f, npcResp);
}

void Test_T3_F6F13_01(gke2e::CaseContext& ctx) {
    BridgeFixture f;
    int a1 = 1, a2 = 2, a3 = 3;
    f.core.PublishSceneActors({
        {&a1, 101, "hski", "花海咲季", "chr_hski_face", ""},
        {&a2, 102, "ttmr", "月村手毬", "chr_ttmr_face", "hski | GakuActors[1] | preset:Center"},
        {&a3, 103, "",     "CustomNPC", "", ""},
    });
    f.bridge->OnUiTick();
    GKE2E_CHECK(ctx, !f.events.empty());

    const std::string pendingReason = CheckM4HelloCapabilities(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    jsonlite::Value actorsEv;
    GKE2E_REQUIRE(ctx, FindLastEventPayload(f.events, "actors.changed", actorsEv));
    const auto* evArr = actorsEv.Find("actors");
    GKE2E_REQUIRE(ctx, evArr && evArr->IsArray() && evArr->Size() == 3);
    CheckJsonActorMetadata(ctx, evArr->At(0), "hski", "花海咲季", "咲季");
    CheckJsonActorMetadata(ctx, evArr->At(1), "ttmr", "月村手毬", "");
    CheckJsonActorMetadata(ctx, evArr->At(2), "", "CustomNPC", "CustomNPC");

    const auto snap = f.ReqJson("state.get");
    GKE2E_REQUIRE(ctx, f.IsOk(snap));
    const auto* arr = snap.Find("result")->Find("actors")->Find("actors");
    GKE2E_REQUIRE(ctx, arr && arr->IsArray() && arr->Size() == 3);
    CheckJsonActorMetadata(ctx, arr->At(0), "hski", "花海咲季", "咲季");
    CheckJsonActorMetadata(ctx, arr->At(1), "ttmr", "月村手毬", "");
    CheckJsonActorMetadata(ctx, arr->At(2), "", "CustomNPC", "CustomNPC");
}

void Test_T3_F5F13_01(gke2e::CaseContext& ctx) {
    TempDirGuard tmp("t3_f5f13_01");
    BridgeFixture f;
    f.core.configRoot = Utf8FromPath(tmp.path);

    const auto png = MakeSyntheticRgbaPng(512, 512, true);
    GKE2E_REQUIRE(ctx, WriteFileBytes(tmp.path / "portraits" / "ttmr.png", png));

    const std::string pendingReason = CheckM4HelloCapabilities(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx, pendingReason);
    }

    int a1 = 1;
    f.core.sceneActorRegistry.Publish({{&a1, 10, "ttmr", "月村手毬", "", ""}});
    const uint64_t tok = f.core.sceneActorRegistry.Entries()[0].token;

    const auto resp = f.ReqJson("actors.getPortrait", "{\"token\":" + std::to_string(tok) + "}");
    GKE2E_REQUIRE(ctx, f.IsOk(resp));
    const auto* res = resp.Find("result");
    GKE2E_CHECK_EQ(ctx, res->Find("character")->stringValue, "ttmr");
    std::string mime;
    std::vector<uint8_t> decoded;
    GKE2E_REQUIRE(ctx, cover::DecodeDataUri(res->Find("dataUri")->stringValue, mime, decoded));
    GKE2E_CHECK_EQ(ctx, mime, "image/png");
    GKE2E_CHECK(ctx, decoded == png);
}

// -----------------------------------------------------------------------------
// Tier 4 — Real-World Offline Octo -> WIC 512px Cache -> SceneActors -> Bridge
// -----------------------------------------------------------------------------
void Test_T4_F5F6F13_01(gke2e::CaseContext& ctx) {
    const fs::path gameRoot = GetGameRoot();
    const fs::path manifestPath = gameRoot / "octo" / "pdb" / "400" / "705100" / "octocacheevai";
    GKE2E_REQUIRE_MSG(ctx, fs::is_regular_file(manifestPath),
                      "Offline Octo manifest missing: " + manifestPath.string());

    // 1. Real Win32 CNG decrypt + protobuf parse + XOR UnityFS verification on f:\Games\gakumas
    const auto rawManifest = ReadFileBytes(manifestPath);
    GKE2E_REQUIRE(ctx, !rawManifest.empty());
    std::vector<uint8_t> pb;
    std::string err;
    GKE2E_REQUIRE_MSG(ctx, DecryptOctoManifestBytes(rawManifest, pb, err), err);

    std::unordered_map<std::string, OctoBundleEntry> byName;
    size_t totalBundles = 0;
    GKE2E_REQUIRE(ctx, ParseOctoDatabaseBundles(pb, byName, totalBundles));

    static constexpr const char* kVerifyBundles[] = {
        "img_chr_hski_00-bustup", "img_chr_ttmr_00-bustup", "img_chr_fktn_00-bustup",
        "img_chr_nasr_00-bustup", "img_chr_hkne_00-audition", "img_adv_speaker_krnh_000-000",
        "img_chr_amao_00-thumb-circle"
    };
    for (const char* bname : kVerifyBundles) {
        const auto it = byName.find(bname);
        GKE2E_REQUIRE_MSG(ctx, it != byName.end(), std::string("Bundle missing in Octo manifest: ") + bname);
        const auto& e = it->second;
        const fs::path bundlePath = OctoBundlePathForEntry(gameRoot, e);
        GKE2E_REQUIRE_MSG(ctx, fs::is_regular_file(bundlePath), "Bundle missing: " + bundlePath.string());
        auto rawBundle = ReadFileBytes(bundlePath);
        uint64_t totalSz = 0;
        GKE2E_CHECK_MSG(ctx, DeobfuscateOctoBundleHeader(rawBundle, bname, totalSz),
                        std::string("XOR/UnityFS mismatch for ") + bname);
    }

    // 2. Real extracted RGBA PNGs from octo_extract_helper.py -> WIC 512px PNG cache
    const fs::path extractedDir = GetOctoExtractedDir();
    TempDirGuard tmp("t4_f5f6f13_01");
    const fs::path portraitsDir = tmp.path / "portraits";

    struct PortraitSpec {
        const char* code;
        const char* extractedFile;
        bool expectAlpha;
        bool expectDownscaledTo512;
    };
    static constexpr PortraitSpec kSpecs[] = {
        {"hski", "hski.png",       true,  true},
        {"ttmr", "ttmr.png",       true,  true},
        {"fktn", "fktn.png",       true,  true},
        {"nasr", "nasr.png",       true,  true},
        {"hkne", "hkne.png",       false, false},
        {"krnh", "krnh.png",       false, false},
        {"amao", "amao_thumb.png", true,  false},
    };

    std::unordered_map<std::string, std::vector<uint8_t>> cachedPngs;
    for (const auto& spec : kSpecs) {
        const fs::path rawPngPath = extractedDir / spec.extractedFile;
        GKE2E_REQUIRE_MSG(ctx, fs::is_regular_file(rawPngPath),
                          "Extracted Octo PNG missing (run octo_extract_helper.py): " + rawPngPath.string());
        const auto rawPng = ReadFileBytes(rawPngPath);
        GKE2E_REQUIRE(ctx, !rawPng.empty());

        std::vector<uint8_t> resized;
        std::string mime;
        GKE2E_REQUIRE(ctx, cover::ResizeAndEncodeCover(rawPng.data(), rawPng.size(), 512, resized, mime, true));
        GKE2E_CHECK_EQ(ctx, mime, "image/png");
        const PngInfo info = InspectPngHeader(resized);
        GKE2E_CHECK(ctx, info.valid);
        GKE2E_CHECK(ctx, info.width <= 512u && info.height <= 512u);
        if (spec.expectDownscaledTo512) {
            GKE2E_CHECK_EQ(ctx, info.width, 512u);
            GKE2E_CHECK_EQ(ctx, info.height, 512u);
        }
        if (std::string_view(spec.code) == "amao") {
            GKE2E_CHECK_EQ(ctx, info.width, 256u);
            GKE2E_CHECK_EQ(ctx, info.height, 256u);
        }
        if (spec.expectAlpha) {
            GKE2E_CHECK(ctx, PngHasNonOpaqueAlpha(resized));
        }
        const fs::path cachedPath = portraitsDir / (std::string(spec.code) + ".png");
        GKE2E_REQUIRE(ctx, WriteFileBytes(cachedPath, resized));
        cachedPngs[spec.code] = std::move(resized);
    }

    // 3. SceneActorRegistry + Bridge state.get & actors.getPortrait
    BridgeFixture f;
    f.core.configRoot = Utf8FromPath(tmp.path);
    int a1 = 1, a2 = 2, a3 = 3, a4 = 4, a5 = 5, a6 = 6, a7 = 7, aNpc = 8;
    f.core.PublishSceneActors({
        {&a1,   101, "hski",       "花海咲季",  "chr_hski_face_00", ""},
        {&a2,   102, "ttmr",       "月村手毬",  "chr_ttmr_face_00", ""},
        {&a3,   103, "fktn",       "藤田琴音",  "chr_fktn_face_00", ""},
        {&a4,   104, "nasr",       "根绪亚纱里", "chr_nasr_face_00", ""},
        {&a5,   105, "hkne",       "冰渡香名江", "chr_hkne_face_00", ""},
        {&a6,   106, "krnh",       "贺阳燐羽",  "chr_krnh_face_00", ""},
        {&a7,   107, "amao",       "有村麻央",  "chr_amao_face_00", ""},
        {&aNpc, 108, "custom_npc", "自定义NPC",  "npc_custom",       ""},
    });
    GKE2E_REQUIRE_EQ(ctx, f.core.sceneActorRegistry.Entries().size(), size_t{8});
    const auto stateResp = f.ReqJson("state.get");
    GKE2E_REQUIRE(ctx, f.IsOk(stateResp));

    const std::string pendingReason = CheckM4HelloCapabilities(f);
    if (!pendingReason.empty()) {
        GKE2E_PENDING(ctx,
            "Octo CNG decrypt, XOR UnityFS (bustup/audition/speaker/thumb-circle), WIC 512px PNG cache & SceneActorRegistry PASS; " +
            pendingReason);
    }

    const auto* stateActors = stateResp.Find("result")->Find("actors")->Find("actors");
    GKE2E_REQUIRE(ctx, stateActors && stateActors->IsArray() && stateActors->Size() == 8);
    CheckJsonActorMetadata(ctx, stateActors->At(6), "amao", "有村麻央", "");
    CheckJsonActorMetadata(ctx, stateActors->At(7), "custom_npc", "自定义NPC", "自定义NPC");

    for (const auto& spec : kSpecs) {
        const auto resp = f.ReqJson("actors.getPortrait",
                                    std::string("{\"character\":\"") + spec.code + "\"}");
        GKE2E_REQUIRE(ctx, f.IsOk(resp));
        const auto* res = resp.Find("result");
        GKE2E_REQUIRE(ctx, res != nullptr);
        GKE2E_CHECK(ctx, res->Find("cached") && res->Find("cached")->boolValue);
        const auto* uri = res->Find("dataUri");
        GKE2E_REQUIRE(ctx, uri && uri->IsString());
        std::string decMime;
        std::vector<uint8_t> decBytes;
        GKE2E_REQUIRE(ctx, cover::DecodeDataUri(uri->stringValue, decMime, decBytes));
        GKE2E_CHECK_EQ(ctx, decMime, "image/png");
        GKE2E_CHECK(ctx, decBytes == cachedPngs[spec.code]);
    }

    const auto npcResp = f.ReqJson("actors.getPortrait", "{\"character\":\"custom_npc\"}");
    CheckNoPortraitDataReturned(ctx, f, npcResp);
}

} // namespace

int main(int argc, char** argv) {
    gke2e::Runner runner;

    // Tier 1: F5 (6), F6 (6), F13 C++ (4) = 16 C++ (+ 2 TS = 18 Tier 1)
    runner.Add("T1-F5-01", 1, "F5", "WIC ResizeAndEncodeCover 1024->512 RGBA PNG preserves alpha (forcePng=true)", Test_T1_F5_01);
    runner.Add("T1-F5-02", 1, "F5", "WIC ResizeAndEncodeCover aspect ratio, no-upscale & maxDim=0->512", Test_T1_F5_02);
    runner.Add("T1-F5-03", 1, "F5", "WIC ToDataUri & DecodeDataUri PNG round-trip parity", Test_T1_F5_03);
    runner.Add("T1-F5-04", 1, "F5", "Win32 CNG AES-128-CBC decrypt + MD5 + XOR UnityFS on all 14 core idols", Test_T1_F5_04);
    runner.Add("T1-F5-05", 1, "F5", "GkPortraitCache header & disk cache path contract (<configRoot>/portraits/{code}.png)", Test_T1_F5_05);
    runner.Add("T1-F5-06", 1, "F5", "GkPortraitCache 4-tier bundle fallback order resolution", Test_T1_F5_06);

    runner.Add("T1-F6-01", 1, "F6", "Idol localization (LookupIdolChineseName, FormatIdolDisplayName, shortName probe)", Test_T1_F6_01);
    runner.Add("T1-F6-02", 1, "F6", "DetectIdolCharacterCode & GakuActorsPresetName extraction", Test_T1_F6_02);
    runner.Add("T1-F6-03", 1, "F6", "FormatSceneActorLabel priority chain & 1-based suffix", Test_T1_F6_03);
    runner.Add("T1-F6-04", 1, "F6", "SceneActorRegistry::Publish monotonic tokens & stable order on reorder", Test_T1_F6_04);
    runner.Add("T1-F6-05", 1, "F6", "SceneActorRegistry::Publish epoch semantics, Resolve & Clear", Test_T1_F6_05);
    runner.Add("T1-F6-06", 1, "F6", "GuiSceneActor character/displayName/shortName metadata probes", Test_T1_F6_06);

    runner.Add("T1-F13-01", 1, "F13", "GakuWebPanelServiceV1 ABI, GAKU_E_SIZE negotiation & motion.hello M4 capabilities", Test_T1_F13_01);
    runner.Add("T1-F13-02", 1, "F13", "state.get group LibraryEntry.positions[], SceneActor.character/shortName, followSlotIndex", Test_T1_F13_02);
    runner.Add("T1-F13-03", 1, "F13", "actors.getPortrait by character/token & formation.selectSlot RPCs", Test_T1_F13_03);
    runner.Add("T1-F13-04", 1, "F13", "Multi-VMD library.probeFiles classification (3 body VMDs + camera + audio)", Test_T1_F13_04);

    // Tier 2: F5 (6), F6 (6), F13 C++ (5) = 17 C++ (+ 1 TS = 18 Tier 2)
    runner.Add("T2-F5-01", 2, "F5", "WIC ResizeAndEncodeCover rejects null, empty, >64MiB & corrupt bytes", Test_T2_F5_01);
    runner.Add("T2-F5-02", 2, "F5", "WIC DecodeDataUri rejects non-image MIME, malformed base64 & >16MiB URI", Test_T2_F5_02);
    runner.Add("T2-F5-03", 2, "F5", "Octo CNG manifest decryptor rejects wrong magic, unaligned, corrupt & bad MD5", Test_T2_F5_03);
    runner.Add("T2-F5-04", 2, "F5", "Octo XOR de-obfuscation rejects short, corrupt & non-UnityFS bundles", Test_T2_F5_04);
    runner.Add("T2-F5-05", 2, "F5", "GkPortraitCache rejects path-traversal & invalid character codes", Test_T2_F5_05);
    runner.Add("T2-F5-06", 2, "F5", "GkPortraitCache handles corrupt cached PNG & total 4-tier miss safely", Test_T2_F5_06);

    runner.Add("T2-F6-01", 2, "F6", "DetectIdolCharacterCode rejects ambiguous multi-idol & non-delimited strings", Test_T2_F6_01);
    runner.Add("T2-F6-02", 2, "F6", "GakuActorsPresetName boundary lengths, case & control-char rejection", Test_T2_F6_02);
    runner.Add("T2-F6-03", 2, "F6", "SceneActorRegistry::Publish rejects null/0-id/duplicates & handles respawn", Test_T2_F6_03);
    runner.Add("T2-F6-04", 2, "F6", "SceneActorRegistry::Resolve rejects stale epoch, unknown token & cleared state", Test_T2_F6_04);
    runner.Add("T2-F6-05", 2, "F6", "NPC/custom/empty character label & shortName fallback", Test_T2_F6_05);
    runner.Add("T2-F6-06", 2, "F6", "ActorsJsonLocked JSON escaping (CJK/quotes/controls) & mixed-case code boundary", Test_T2_F6_06);

    runner.Add("T2-F13-01", 2, "F13", "Bridge wire framing errors, OnShutdown session rollover & Invalidate permanence", Test_T2_F13_01);
    runner.Add("T2-F13-02", 2, "F13", "library.probeFiles empty/oversized arrays & corrupt 0-byte VMD isolation", Test_T2_F13_02);
    runner.Add("T2-F13-03", 2, "F13", "actors.getPortrait security/path-traversal & uncached NPC miss", Test_T2_F13_03);
    runner.Add("T2-F13-04", 2, "F13", "formation.* out-of-range indices, offset bounds & not_allowed policy guard", Test_T2_F13_04);
    runner.Add("T2-F13-05", 2, "F13", "camera.set rejects invalid followSlotIndex (< -1)", Test_T2_F13_05);

    // Tier 3: C++ (3) (+ 1 TS = 4 Tier 3)
    runner.Add("T3-F5F6-01", 3, "F5", "Pairwise F5xF6: SceneActorRegistry idol detection -> GkPortraitCache disk lookup", Test_T3_F5F6_01);
    runner.Add("T3-F6F13-01", 3, "F6", "Pairwise F6xF13: SceneActorRegistry -> Bridge actors.changed & state.get metadata", Test_T3_F6F13_01);
    runner.Add("T3-F5F13-01", 3, "F13", "Pairwise F5xF13: <configRoot>/portraits PNG -> Bridge actors.getPortrait Data URI", Test_T3_F5F13_01);

    // Tier 4: C++ (1) (+ 1 TS = 2 Tier 4)
    runner.Add("T4-F5F6F13-01", 4, "F5", "Offline Octo bustup/audition/speaker/thumb-circle -> WIC 512px PNG -> SceneActors -> Bridge", Test_T4_F5F6F13_01);

    return runner.Run(argc, argv);
}
