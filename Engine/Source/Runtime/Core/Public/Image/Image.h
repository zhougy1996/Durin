#pragma once

#include "CoreAPI.h"
#include "HAL/Platform.h"
#include "Serialization/SharedByteBuffer.h"

namespace Durin::Image
{
	inline constexpr uint64 MaximumRawImageBytes = 512ull * 1024ull * 1024ull;

	enum class ERawImageFormat : uint8
	{
		Invalid,
		G8,
		G16,
		RG8,
		RGBA8,
		RGBA16,
		R16F,
		RGBA16F,
		R32F,
		RGBA32F,
	};

	enum class EImageGammaSpace : uint8
	{
		Unknown,
		Linear,
		SRGB,
	};

	struct FRawImageFormatInfo
	{
		uint8 ChannelCount = 0;
		uint8 BytesPerChannel = 0;
		uint8 BytesPerPixel = 0;
		bool bFloatingPoint = false;
	};

	struct FImageInfo
	{
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 Depth = 1;
		uint32 SliceCount = 1;
		ERawImageFormat Format = ERawImageFormat::Invalid;
		EImageGammaSpace GammaSpace = EImageGammaSpace::Unknown;

		auto operator==(const FImageInfo&) const -> bool = default;
		CORE_API auto IsValid() const -> bool;
		CORE_API auto GetByteSize(uint64& OutByteSize) const -> bool;
	};

	// An immutable image view that retains ownership of its backing allocation.
	class FImageView
	{
	public:
		FImageView() = default;
		CORE_API FImageView(FImageInfo InInfo, FSharedByteBuffer InPixels,
			uint64 InPixelOffset = 0);

		auto IsValid() const -> bool { return Info.IsValid() && Pixels.GetSize() > 0; }
		auto GetInfo() const -> const FImageInfo& { return Info; }
		auto GetPixels() const -> std::span<const std::byte>
		{
			return Pixels.GetBytes().subspan(
				static_cast<size_t>(PixelOffset), static_cast<size_t>(PixelSize));
		}
		auto GetBuffer() const -> const FSharedByteBuffer& { return Pixels; }

	private:
		FImageInfo Info;
		FSharedByteBuffer Pixels;
		uint64 PixelOffset = 0;
		uint64 PixelSize = 0;
	};

	// Owns one tightly packed top-left-origin image value.
	class FImage
	{
	public:
		FImage() = default;
		CORE_API static auto TryCreate(FImageInfo Info, FByteArray Pixels,
			FImage& OutImage, std::string* OutError = nullptr) -> bool;
		CORE_API static auto TryCreate(FImageInfo Info, FSharedByteBuffer Pixels,
			FImage& OutImage, std::string* OutError = nullptr) -> bool;

		auto IsValid() const -> bool { return View.IsValid(); }
		auto GetInfo() const -> const FImageInfo& { return View.GetInfo(); }
		auto GetPixels() const -> std::span<const std::byte> { return View.GetPixels(); }
		auto GetView() const -> FImageView { return View; }
		void Reset() { View = {}; }

	private:
		explicit FImage(FImageView InView) : View(std::move(InView)) {}
		FImageView View;
	};

	struct FImageChannelAnalysis
	{
		uint8 MeaningfulChannelCount = 0;
		bool bHasTransparency = false;
		bool bAllFinite = true;
	};

	// Compatibility views used by existing codec clients while they migrate to
	// the common immutable FImage value.
	struct FDecodedImage
	{
		FByteArray Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint8 SourceChannelCount = 0;
		bool bHasTransparency = false;

		CORE_API auto ToImage(EImageGammaSpace GammaSpace,
			FImage& OutImage, std::string* OutError = nullptr) const -> bool;
	};

	struct FDecodedGrayscale16Image
	{
		std::vector<uint16> Samples;
		uint32 Width = 0;
		uint32 Height = 0;

		CORE_API auto ToImage(EImageGammaSpace GammaSpace,
			FImage& OutImage, std::string* OutError = nullptr) const -> bool;
	};

	struct FDecodedFloatImage
	{
		std::vector<float> Pixels;
		uint32 Width = 0;
		uint32 Height = 0;

		CORE_API auto ToImage(FImage& OutImage,
			std::string* OutError = nullptr) const -> bool;
	};

	CORE_API auto GetRawImageFormatInfo(ERawImageFormat Format)
		-> FRawImageFormatInfo;
	CORE_API auto ConvertImage(FImageView Source, ERawImageFormat DestinationFormat,
		EImageGammaSpace DestinationGamma, FImage& OutImage,
		std::string& OutError) -> bool;
	CORE_API auto AnalyzeImageChannels(FImageView Image,
		FImageChannelAnalysis& OutAnalysis, std::string& OutError) -> bool;
} // namespace Durin::Image
