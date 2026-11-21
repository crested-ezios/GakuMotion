#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef far
#define far
#endif
#ifndef near
#define near
#endif
#include <objbase.h>
#include <wincodec.h>
#undef near
#undef far
#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "windowscodecs.lib")
#endif
#endif

namespace GakuMotion::cover
{
	inline std::string LowerAscii(std::string_view s)
	{
		std::string out(s);
		for (char& ch : out)
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return out;
	}

	inline std::string EncodeBase64(const uint8_t* data, size_t size)
	{
		static constexpr char kAlphabet[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		if (!data || size == 0) return out;
		out.reserve(((size + 2) / 3) * 4);
		for (size_t i = 0; i < size; i += 3)
		{
			const uint32_t b0 = data[i];
			const bool has1 = (i + 1) < size;
			const bool has2 = (i + 2) < size;
			const uint32_t b1 = has1 ? data[i + 1] : 0u;
			const uint32_t b2 = has2 ? data[i + 2] : 0u;
			const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
			out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
			out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
			out.push_back(has1 ? kAlphabet[(triple >> 6) & 0x3F] : '=');
			out.push_back(has2 ? kAlphabet[triple & 0x3F] : '=');
		}
		return out;
	}

	inline std::string MimeFromPath(const std::filesystem::path& path)
	{
		const std::string ext = LowerAscii(path.extension().string());
		if (ext == ".png") return "image/png";
		if (ext == ".webp") return "image/webp";
		if (ext == ".bmp") return "image/bmp";
		if (ext == ".gif") return "image/gif";
		return "image/jpeg";
	}

	inline std::string ToDataUri(const std::vector<uint8_t>& bytes, std::string_view mime)
	{
		if (bytes.empty() || mime.empty()) return {};
		return "data:" + std::string(mime) + ";base64," + EncodeBase64(bytes.data(), bytes.size());
	}

	inline bool DecodeBase64(std::string_view input, std::vector<uint8_t>& outBytes)
	{
		outBytes.clear();
		if (input.empty()) return false;
		static const int kLookup[256] = {
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
			52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
			-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
			15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
			-1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
			41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
			-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
		};
		outBytes.reserve((input.size() / 4) * 3);
		uint32_t val = 0;
		int valb = -8;
		for (const unsigned char ch : input)
		{
			if (ch == '=') break;
			if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
			const int d = kLookup[ch];
			if (d == -1) { outBytes.clear(); return false; }
			val = (val << 6) + static_cast<uint32_t>(d);
			valb += 6;
			if (valb >= 0)
			{
				outBytes.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
				valb -= 8;
			}
		}
		return !outBytes.empty();
	}

	inline bool DecodeDataUri(std::string_view uri, std::string& outMime, std::vector<uint8_t>& outBytes)
	{
		outMime.clear();
		outBytes.clear();
		constexpr std::string_view kPrefix = "data:";
		constexpr std::string_view kBase64Tag = ";base64,";
		if (uri.size() > 16ULL * 1024ULL * 1024ULL) return false;
		if (uri.rfind(kPrefix, 0) != 0) return false;
		const size_t marker = uri.find(kBase64Tag, kPrefix.size());
		if (marker == std::string_view::npos) return false;
		const std::string mime = LowerAscii(uri.substr(kPrefix.size(), marker - kPrefix.size()));
		if (mime != "image/png" && mime != "image/jpeg" && mime != "image/jpg" &&
			mime != "image/webp" && mime != "image/bmp" && mime != "image/gif")
		{
			return false;
		}
		const std::string_view payload = uri.substr(marker + kBase64Tag.size());
		if (!DecodeBase64(payload, outBytes)) return false;
		outMime = (mime == "image/jpg") ? "image/jpeg" : mime;
		return true;
	}

#ifdef _WIN32
	namespace detail
	{
		template <class T>
		class SimpleComPtr
		{
		public:
			SimpleComPtr() = default;
			~SimpleComPtr() { Reset(); }
			SimpleComPtr(const SimpleComPtr&) = delete;
			SimpleComPtr& operator=(const SimpleComPtr&) = delete;

			void Reset()
			{
				if (ptr_)
				{
					ptr_->Release();
					ptr_ = nullptr;
				}
			}

			T** Put()
			{
				Reset();
				return &ptr_;
			}

			T* Get() const { return ptr_; }
			T* operator->() const { return ptr_; }
			explicit operator bool() const { return ptr_ != nullptr; }

		private:
			T* ptr_{nullptr};
		};

		class CoInitGuard
		{
		public:
			CoInitGuard()
			{
				const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				if (hr == S_OK || hr == S_FALSE) shouldUninit_ = true;
			}
			~CoInitGuard()
			{
				if (shouldUninit_) CoUninitialize();
			}
		private:
			bool shouldUninit_{false};
		};

		inline bool HasNonOpaqueAlpha(IWICImagingFactory* factory, IWICBitmapSource* source, uint32_t width, uint32_t height)
		{
			if (!factory || !source || width == 0 || height == 0) return false;
			// Limit alpha scan size to prevent huge memory allocations on malformed headers.
			if (static_cast<uint64_t>(width) * height > 4096ULL * 4096ULL) return false;

			SimpleComPtr<IWICFormatConverter> converter;
			if (FAILED(factory->CreateFormatConverter(converter.Put()))) return false;
			if (FAILED(converter->Initialize(
					source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
					nullptr, 0.0, WICBitmapPaletteTypeCustom)))
			{
				return false;
			}

			const UINT stride = width * 4u;
			std::vector<uint8_t> pixels(static_cast<size_t>(stride) * height);
			if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data())))
				return false;

			for (size_t i = 3; i < pixels.size(); i += 4)
			{
				if (pixels[i] < 254) return true;
			}
			return false;
		}
	} // namespace detail

	inline bool ResizeAndEncodeCover(
		const uint8_t* inputBytes,
		size_t inputSize,
		uint32_t maxDim,
		std::vector<uint8_t>& outBytes,
		std::string& outMime,
		bool forcePng = false)
	{
		outBytes.clear();
		outMime.clear();
		if (!inputBytes || inputSize == 0 || inputSize > 64ULL * 1024ULL * 1024ULL) return false;
		if (maxDim == 0) maxDim = 512;

		detail::CoInitGuard comGuard;
		detail::SimpleComPtr<IWICImagingFactory> factory;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(factory.Put()))))
		{
			return false;
		}

		detail::SimpleComPtr<IStream> inStream;
		if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, inStream.Put()))) return false;
		ULONG written = 0;
		if (FAILED(inStream->Write(inputBytes, static_cast<ULONG>(inputSize), &written)) || written != inputSize)
			return false;
		LARGE_INTEGER zero{};
		if (FAILED(inStream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;

		detail::SimpleComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromStream(inStream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.Put())))
			return false;

		detail::SimpleComPtr<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, frame.Put()))) return false;

		UINT srcW = 0, srcH = 0;
		if (FAILED(frame->GetSize(&srcW, &srcH)) || srcW == 0 || srcH == 0) return false;

		GUID containerGuid{};
		const bool isPngContainer =
			SUCCEEDED(decoder->GetContainerFormat(&containerGuid)) &&
			IsEqualGUID(containerGuid, GUID_ContainerFormatPng);

		UINT dstW = srcW;
		UINT dstH = srcH;
		if (srcW > maxDim || srcH > maxDim)
		{
			if (srcW >= srcH)
			{
				dstW = maxDim;
				dstH = (std::max)(1u, static_cast<UINT>(static_cast<uint64_t>(srcH) * maxDim / srcW));
			}
			else
			{
				dstH = maxDim;
				dstW = (std::max)(1u, static_cast<UINT>(static_cast<uint64_t>(srcW) * maxDim / srcH));
			}
		}

		detail::SimpleComPtr<IWICBitmapScaler> scaler;
		IWICBitmapSource* activeSource = frame.Get();
		if (dstW != srcW || dstH != srcH)
		{
			if (FAILED(factory->CreateBitmapScaler(scaler.Put()))) return false;
			if (FAILED(scaler->Initialize(frame.Get(), dstW, dstH, WICBitmapInterpolationModeFant)))
				return false;
			activeSource = scaler.Get();
		}

		const bool keepPng = forcePng || (isPngContainer && detail::HasNonOpaqueAlpha(factory.Get(), activeSource, dstW, dstH));

		detail::SimpleComPtr<IWICFormatConverter> converter;
		if (FAILED(factory->CreateFormatConverter(converter.Put()))) return false;
		const WICPixelFormatGUID targetPixFmt = keepPng ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
		if (FAILED(converter->Initialize(
				activeSource, targetPixFmt, WICBitmapDitherTypeNone,
				nullptr, 0.0, WICBitmapPaletteTypeCustom)))
		{
			return false;
		}

		detail::SimpleComPtr<IStream> outStream;
		if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, outStream.Put()))) return false;

		detail::SimpleComPtr<IWICBitmapEncoder> encoder;
		const GUID encoderContainer = keepPng ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg;
		if (FAILED(factory->CreateEncoder(encoderContainer, nullptr, encoder.Put()))) return false;
		if (FAILED(encoder->Initialize(outStream.Get(), WICBitmapEncoderNoCache))) return false;

		detail::SimpleComPtr<IWICBitmapFrameEncode> encodeFrame;
		detail::SimpleComPtr<IPropertyBag2> props;
		if (FAILED(encoder->CreateNewFrame(encodeFrame.Put(), props.Put()))) return false;

		if (!keepPng && props)
		{
			PROPBAG2 option{};
			option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
			VARIANT val{};
			VariantInit(&val);
			val.vt = VT_R4;
			val.fltVal = 0.88f;
			props->Write(1, &option, &val);
			VariantClear(&val);
		}

		if (FAILED(encodeFrame->Initialize(props.Get()))) return false;
		if (FAILED(encodeFrame->SetSize(dstW, dstH))) return false;
		WICPixelFormatGUID encodeFmt = targetPixFmt;
		if (FAILED(encodeFrame->SetPixelFormat(&encodeFmt))) return false;
		if (FAILED(encodeFrame->WriteSource(converter.Get(), nullptr))) return false;
		if (FAILED(encodeFrame->Commit())) return false;
		if (FAILED(encoder->Commit())) return false;

		HGLOBAL hGlobal = nullptr;
		if (FAILED(GetHGlobalFromStream(outStream.Get(), &hGlobal)) || !hGlobal) return false;
		STATSTG stat{};
		if (FAILED(outStream->Stat(&stat, STATFLAG_NONAME))) return false;
		const size_t encodedSize = static_cast<size_t>(stat.cbSize.QuadPart);
		if (encodedSize == 0) return false;

		const void* locked = GlobalLock(hGlobal);
		if (!locked) return false;
		outBytes.assign(static_cast<const uint8_t*>(locked), static_cast<const uint8_t*>(locked) + encodedSize);
		GlobalUnlock(hGlobal);

		outMime = keepPng ? "image/png" : "image/jpeg";
		return true;
	}

	inline std::string EncodeBgraTopDownToDataUri(
		const uint8_t* bgraPixels,
		uint32_t width,
		uint32_t height,
		uint32_t rowPitch,
		uint32_t maxDim = 1024)
	{
		if (!bgraPixels || width == 0 || height == 0 || rowPitch < width * 4u) return {};
		if (static_cast<uint64_t>(rowPitch) * height > 256ULL * 1024ULL * 1024ULL) return {};
		if (maxDim == 0) maxDim = 1024;

		detail::CoInitGuard comGuard;
		detail::SimpleComPtr<IWICImagingFactory> factory;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(factory.Put()))))
		{
			return {};
		}

		detail::SimpleComPtr<IWICBitmap> bitmap;
		if (FAILED(factory->CreateBitmapFromMemory(
				width, height, GUID_WICPixelFormat32bppBGRA, rowPitch,
				rowPitch * height, const_cast<BYTE*>(bgraPixels), bitmap.Put())))
		{
			return {};
		}

		UINT dstW = width;
		UINT dstH = height;
		if (width > maxDim || height > maxDim)
		{
			if (width >= height)
			{
				dstW = maxDim;
				dstH = (std::max)(1u, static_cast<UINT>(static_cast<uint64_t>(height) * maxDim / width));
			}
			else
			{
				dstH = maxDim;
				dstW = (std::max)(1u, static_cast<UINT>(static_cast<uint64_t>(width) * maxDim / height));
			}
		}

		detail::SimpleComPtr<IWICBitmapScaler> scaler;
		IWICBitmapSource* activeSource = bitmap.Get();
		if (dstW != width || dstH != height)
		{
			if (FAILED(factory->CreateBitmapScaler(scaler.Put()))) return {};
			if (FAILED(scaler->Initialize(bitmap.Get(), dstW, dstH, WICBitmapInterpolationModeFant)))
				return {};
			activeSource = scaler.Get();
		}

		detail::SimpleComPtr<IWICFormatConverter> converter;
		if (FAILED(factory->CreateFormatConverter(converter.Put()))) return {};
		if (FAILED(converter->Initialize(
				activeSource, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
				nullptr, 0.0, WICBitmapPaletteTypeCustom)))
		{
			return {};
		}

		detail::SimpleComPtr<IStream> outStream;
		if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, outStream.Put()))) return {};

		detail::SimpleComPtr<IWICBitmapEncoder> encoder;
		if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.Put()))) return {};
		if (FAILED(encoder->Initialize(outStream.Get(), WICBitmapEncoderNoCache))) return {};

		detail::SimpleComPtr<IWICBitmapFrameEncode> encodeFrame;
		detail::SimpleComPtr<IPropertyBag2> props;
		if (FAILED(encoder->CreateNewFrame(encodeFrame.Put(), props.Put()))) return {};
		if (props)
		{
			PROPBAG2 option{};
			option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
			VARIANT val{};
			VariantInit(&val);
			val.vt = VT_R4;
			val.fltVal = 0.90f;
			props->Write(1, &option, &val);
			VariantClear(&val);
		}
		if (FAILED(encodeFrame->Initialize(props.Get()))) return {};
		if (FAILED(encodeFrame->SetSize(dstW, dstH))) return {};
		WICPixelFormatGUID encodeFmt = GUID_WICPixelFormat24bppBGR;
		if (FAILED(encodeFrame->SetPixelFormat(&encodeFmt))) return {};
		if (FAILED(encodeFrame->WriteSource(converter.Get(), nullptr))) return {};
		if (FAILED(encodeFrame->Commit())) return {};
		if (FAILED(encoder->Commit())) return {};

		HGLOBAL hGlobal = nullptr;
		if (FAILED(GetHGlobalFromStream(outStream.Get(), &hGlobal)) || !hGlobal) return {};
		STATSTG stat{};
		if (FAILED(outStream->Stat(&stat, STATFLAG_NONAME))) return {};
		const size_t encodedSize = static_cast<size_t>(stat.cbSize.QuadPart);
		if (encodedSize == 0) return {};

		const void* locked = GlobalLock(hGlobal);
		if (!locked) return {};
		std::vector<uint8_t> encoded(static_cast<const uint8_t*>(locked), static_cast<const uint8_t*>(locked) + encodedSize);
		GlobalUnlock(hGlobal);
		return ToDataUri(encoded, "image/jpeg");
	}
#else
	inline bool ResizeAndEncodeCover(
		const uint8_t*,
		size_t,
		uint32_t,
		std::vector<uint8_t>& outBytes,
		std::string& outMime,
		bool = false)
	{
		outBytes.clear();
		outMime.clear();
		return false;
	}

	inline std::string EncodeBgraTopDownToDataUri(
		const uint8_t*,
		uint32_t,
		uint32_t,
		uint32_t,
		uint32_t = 1024)
	{
		return {};
	}
#endif

	inline std::string BytesToThumbnailDataUri(
		const uint8_t* bytes,
		size_t size,
		std::string_view mimeHint = "image/jpeg",
		uint32_t maxDim = 512)
	{
		if (!bytes || size == 0) return {};
		std::vector<uint8_t> resized;
		std::string mime;
		if (ResizeAndEncodeCover(bytes, size, maxDim, resized, mime) && !resized.empty())
		{
			return ToDataUri(resized, mime);
		}
		// Fallback for already-small raw images if WIC codec failed.
		if (size <= 1536ULL * 1024ULL && !mimeHint.empty() && mimeHint.rfind("image/", 0) == 0)
		{
			std::vector<uint8_t> copy(bytes, bytes + size);
			return ToDataUri(copy, mimeHint);
		}
		return {};
	}

	inline std::string LoadCoverAsDataUri(const std::filesystem::path& coverPath, uint32_t maxDim = 512)
	{
		if (coverPath.empty()) return {};
		std::error_code ec;
		if (!std::filesystem::is_regular_file(coverPath, ec)) return {};
		const auto fsize = std::filesystem::file_size(coverPath, ec);
		if (ec || fsize == 0 || fsize > 32ULL * 1024ULL * 1024ULL) return {};

		std::ifstream in(coverPath, std::ios::binary);
		if (!in) return {};
		std::vector<uint8_t> raw(static_cast<size_t>(fsize));
		if (!in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size())))
			return {};

		return BytesToThumbnailDataUri(raw.data(), raw.size(), MimeFromPath(coverPath), maxDim);
	}

	inline void DeleteTrackCoverFiles(const std::filesystem::path& targetDir)
	{
		if (targetDir.empty()) return;
		std::error_code ec;
		for (const char* name : {"cover.jpg", "cover.jpeg", "cover.png", "cover.webp", "cover.bmp"})
		{
			std::filesystem::remove(targetDir / name, ec);
			ec.clear();
		}
	}

	inline bool SaveTrackCoverPng(
		const std::vector<uint8_t>& rawBytes,
		std::string_view mimeHint,
		const std::filesystem::path& targetDir,
		std::string& outFileName,
		std::string& outError,
		uint32_t maxDim = 512)
	{
		outFileName.clear();
		outError.clear();
		if (rawBytes.empty() || targetDir.empty())
		{
			outError = "Cover image payload or target directory is empty";
			return false;
		}

		std::vector<uint8_t> encoded;
		std::string mime;
		if (!ResizeAndEncodeCover(rawBytes.data(), rawBytes.size(), maxDim, encoded, mime, /*forcePng=*/true) || encoded.empty())
		{
			const bool isPngMagic = rawBytes.size() >= 8 &&
				rawBytes[0] == 0x89 && rawBytes[1] == 0x50 && rawBytes[2] == 0x4E && rawBytes[3] == 0x47;
			if (!isPngMagic && LowerAscii(mimeHint) != "image/png")
			{
				outError = "Failed to decode or encode cover image";
				return false;
			}
			encoded = rawBytes;
		}

		std::error_code ec;
		std::filesystem::create_directories(targetDir, ec);
		if (ec)
		{
			outError = "Cannot create track directory: " + ec.message();
			return false;
		}

		const auto tmpPath = targetDir / "cover.png.tmp";
		const auto finalPath = targetDir / "cover.png";
		{
			std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				outError = "Cannot open temporary cover file for writing";
				return false;
			}
			out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
			if (!out.good())
			{
				out.close();
				std::filesystem::remove(tmpPath, ec);
				outError = "Failed to write temporary cover file";
				return false;
			}
		}

		DeleteTrackCoverFiles(targetDir);
		std::filesystem::rename(tmpPath, finalPath, ec);
		if (ec)
		{
			std::filesystem::remove(tmpPath, ec);
			outError = "Failed to commit cover.png: " + ec.message();
			return false;
		}
		outFileName = "cover.png";
		return true;
	}

	inline bool SaveResizedCoverFile(
		const std::vector<uint8_t>& rawBytes,
		std::string_view mimeHint,
		const std::filesystem::path& targetDir,
		std::string& outFileName,
		uint32_t maxDim = 1024)
	{
		outFileName.clear();
		if (rawBytes.empty() || targetDir.empty()) return false;

		std::vector<uint8_t> encoded;
		std::string mime;
		if (!ResizeAndEncodeCover(rawBytes.data(), rawBytes.size(), maxDim, encoded, mime) || encoded.empty())
		{
			encoded = rawBytes;
			mime = LowerAscii(mimeHint);
		}

		outFileName = (mime == "image/png") ? "cover.png" : "cover.jpg";
		std::error_code ec;
		std::filesystem::create_directories(targetDir, ec);
		std::ofstream out(targetDir / outFileName, std::ios::binary | std::ios::trunc);
		if (!out) return false;
		out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
		return out.good();
	}
} // namespace GakuMotion::cover
