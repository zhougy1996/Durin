#include "Image/Image.h"

#include "Math/Color.h"

namespace Durin::Image
{
	namespace
	{
		auto Fail(std::string_view Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = Message;
			return false;
		}

		auto HalfToFloat(uint16 Half) -> float
		{
			const uint32 Sign = static_cast<uint32>(Half & 0x8000u) << 16;
			int32 Exponent = (Half >> 10) & 0x1fu;
			uint32 Mantissa = Half & 0x3ffu;
			uint32 Bits = 0;
			if (Exponent == 0)
			{
				if (Mantissa != 0)
				{
					Exponent = 1;
					while ((Mantissa & 0x400u) == 0) { Mantissa <<= 1; --Exponent; }
					Mantissa &= 0x3ffu;
					Bits = Sign | (static_cast<uint32>(Exponent + 112) << 23) | (Mantissa << 13);
				}
				else Bits = Sign;
			}
			else if (Exponent == 31)
				Bits = Sign | 0x7f800000u | (Mantissa << 13);
			else Bits = Sign | (static_cast<uint32>(Exponent + 112) << 23) | (Mantissa << 13);
			return std::bit_cast<float>(Bits);
		}

		auto FloatToHalf(float Value) -> uint16
		{
			const uint32 Bits = std::bit_cast<uint32>(Value);
			const uint16 Sign = static_cast<uint16>((Bits >> 16) & 0x8000u);
			const uint32 Mantissa = Bits & 0x7fffffu;
			const int32 Exponent = static_cast<int32>((Bits >> 23) & 0xffu) - 127 + 15;
			if (Exponent <= 0)
			{
				if (Exponent < -10) return Sign;
				const uint32 Shifted = (Mantissa | 0x800000u) >> (1 - Exponent);
				return static_cast<uint16>(Sign | ((Shifted + 0x1000u) >> 13));
			}
			if (Exponent >= 31)
				return static_cast<uint16>(Sign | (Mantissa == 0 ? 0x7c00u : 0x7e00u));
			return static_cast<uint16>(Sign | (static_cast<uint32>(Exponent) << 10)
				| ((Mantissa + 0x1000u) >> 13));
		}

		template<typename T>
		auto Load(const std::byte* Bytes) -> T
		{
			T Value;
			std::memcpy(&Value, Bytes, sizeof(Value));
			return Value;
		}

		template<typename T>
		auto Store(std::byte* Bytes, T Value) -> void
		{
			std::memcpy(Bytes, &Value, sizeof(Value));
		}

		auto DecodePixel(const std::byte* Pixel, ERawImageFormat Format,
			std::array<double, 4>& Out) -> bool
		{
			Out = {0.0, 0.0, 0.0, 1.0};
			switch (Format)
			{
			case ERawImageFormat::G8:
				Out[0] = Out[1] = Out[2] = std::to_integer<uint8>(Pixel[0]) / 255.0; return true;
			case ERawImageFormat::G16:
				Out[0] = Out[1] = Out[2] = Load<uint16>(Pixel) / 65535.0; return true;
			case ERawImageFormat::RG8:
				Out[0] = std::to_integer<uint8>(Pixel[0]) / 255.0;
				Out[1] = std::to_integer<uint8>(Pixel[1]) / 255.0; return true;
			case ERawImageFormat::RGBA8:
				for (size_t C = 0; C < 4; ++C) Out[C] = std::to_integer<uint8>(Pixel[C]) / 255.0; return true;
			case ERawImageFormat::RGBA16:
				for (size_t C = 0; C < 4; ++C) Out[C] = Load<uint16>(Pixel + C * 2) / 65535.0; return true;
			case ERawImageFormat::R16F: Out[0] = HalfToFloat(Load<uint16>(Pixel)); return true;
			case ERawImageFormat::RGBA16F:
				for (size_t C = 0; C < 4; ++C) Out[C] = HalfToFloat(Load<uint16>(Pixel + C * 2)); return true;
			case ERawImageFormat::R32F: Out[0] = Load<float>(Pixel); return true;
			case ERawImageFormat::RGBA32F:
				for (size_t C = 0; C < 4; ++C) Out[C] = Load<float>(Pixel + C * 4); return true;
			default: return false;
			}
		}

		auto EncodePixel(const std::array<double, 4>& Value, ERawImageFormat Format,
			std::byte* Pixel) -> bool
		{
			auto U8 = [](double V) { return static_cast<uint8>(std::clamp(V, 0.0, 1.0) * 255.0 + 0.5); };
			auto U16 = [](double V) { return static_cast<uint16>(std::clamp(V, 0.0, 1.0) * 65535.0 + 0.5); };
			switch (Format)
			{
			case ERawImageFormat::G8: Pixel[0] = static_cast<std::byte>(U8(Value[0])); return true;
			case ERawImageFormat::G16: Store(Pixel, U16(Value[0])); return true;
			case ERawImageFormat::RG8:
				Pixel[0] = static_cast<std::byte>(U8(Value[0])); Pixel[1] = static_cast<std::byte>(U8(Value[1])); return true;
			case ERawImageFormat::RGBA8:
				for (size_t C = 0; C < 4; ++C) Pixel[C] = static_cast<std::byte>(U8(Value[C])); return true;
			case ERawImageFormat::RGBA16:
				for (size_t C = 0; C < 4; ++C) Store(Pixel + C * 2, U16(Value[C])); return true;
			case ERawImageFormat::R16F: Store(Pixel, FloatToHalf(static_cast<float>(Value[0]))); return true;
			case ERawImageFormat::RGBA16F:
				for (size_t C = 0; C < 4; ++C) Store(Pixel + C * 2, FloatToHalf(static_cast<float>(Value[C]))); return true;
			case ERawImageFormat::R32F: Store(Pixel, static_cast<float>(Value[0])); return true;
			case ERawImageFormat::RGBA32F:
				for (size_t C = 0; C < 4; ++C) Store(Pixel + C * 4, static_cast<float>(Value[C])); return true;
			default: return false;
			}
		}
	}

	auto GetRawImageFormatInfo(ERawImageFormat Format) -> FRawImageFormatInfo
	{
		switch (Format)
		{
		case ERawImageFormat::G8: return {1, 1, 1, false};
		case ERawImageFormat::G16: return {1, 2, 2, false};
		case ERawImageFormat::RG8: return {2, 1, 2, false};
		case ERawImageFormat::RGBA8: return {4, 1, 4, false};
		case ERawImageFormat::RGBA16: return {4, 2, 8, false};
		case ERawImageFormat::R16F: return {1, 2, 2, true};
		case ERawImageFormat::RGBA16F: return {4, 2, 8, true};
		case ERawImageFormat::R32F: return {1, 4, 4, true};
		case ERawImageFormat::RGBA32F: return {4, 4, 16, true};
		default: return {};
		}
	}

	auto FImageInfo::GetByteSize(uint64& OutByteSize) const -> bool
	{
		OutByteSize = 0;
		const uint64 BytesPerPixel = GetRawImageFormatInfo(Format).BytesPerPixel;
		if (Width == 0 || Height == 0 || Depth == 0 || SliceCount == 0
			|| BytesPerPixel == 0 || GammaSpace > EImageGammaSpace::SRGB) return false;
		uint64 Size = Width;
		for (uint64 Factor : {static_cast<uint64>(Height), static_cast<uint64>(Depth),
			static_cast<uint64>(SliceCount), BytesPerPixel})
		{
			if (Size > MaximumRawImageBytes / Factor) return false;
			Size *= Factor;
		}
		OutByteSize = Size;
		return true;
	}

	auto FImageInfo::IsValid() const -> bool
	{
		uint64 Size = 0;
		return GetByteSize(Size) && Size > 0 && Size <= MaximumRawImageBytes;
	}

	FImageView::FImageView(FImageInfo InInfo, FSharedByteBuffer InPixels,
		uint64 InPixelOffset)
		: Info(InInfo), Pixels(std::move(InPixels)), PixelOffset(InPixelOffset)
	{
		uint64 Expected = 0;
		if (!Info.GetByteSize(Expected) || PixelOffset > Pixels.GetSize()
			|| Expected > Pixels.GetSize() - PixelOffset)
		{
			Info = {};
			Pixels = {};
			PixelOffset = 0;
			PixelSize = 0;
		}
		else PixelSize = Expected;
	}

	auto FImage::TryCreate(FImageInfo Info, FByteBuffer Pixels, FImage& OutImage,
		std::string* OutError) -> bool
	{
		return TryCreate(Info, FSharedByteBuffer::Take(std::move(Pixels)), OutImage, OutError);
	}

	auto FImage::TryCreate(FImageInfo Info, FSharedByteBuffer Pixels, FImage& OutImage,
		std::string* OutError) -> bool
	{
		uint64 Expected = 0;
		if (!Info.GetByteSize(Expected) || Expected != Pixels.GetSize())
			return Fail("Image metadata and pixel byte count do not match.", OutError);
		OutImage = FImage(FImageView(Info, std::move(Pixels)));
		if (OutError) OutError->clear();
		return true;
	}

	auto ConvertImage(FImageView Source, ERawImageFormat DestinationFormat,
		EImageGammaSpace DestinationGamma, FImage& OutImage, std::string& OutError) -> bool
	{
		OutImage.Reset();
		if (!Source.IsValid() || GetRawImageFormatInfo(DestinationFormat).BytesPerPixel == 0
			|| DestinationGamma > EImageGammaSpace::SRGB)
		{
			OutError = "Image conversion input or destination is invalid.";
			return false;
		}
		const FImageInfo& SourceInfo = Source.GetInfo();
		FImageInfo DestinationInfo = SourceInfo;
		DestinationInfo.Format = DestinationFormat;
		DestinationInfo.GammaSpace = DestinationGamma;
		uint64 DestinationBytes = 0;
		if (!DestinationInfo.GetByteSize(DestinationBytes)
			|| DestinationBytes > std::numeric_limits<size_t>::max())
		{
			OutError = "Converted image exceeds the supported byte limit.";
			return false;
		}
		const auto SourceFormat = GetRawImageFormatInfo(SourceInfo.Format);
		const auto DestFormat = GetRawImageFormatInfo(DestinationFormat);
		const uint64 PixelCount = Source.GetPixels().size() / SourceFormat.BytesPerPixel;
		FByteBuffer Bytes(static_cast<size_t>(DestinationBytes));
		for (uint64 Index = 0; Index < PixelCount; ++Index)
		{
			std::array<double, 4> Pixel;
			if (!DecodePixel(Source.GetPixels().data() + Index * SourceFormat.BytesPerPixel,
				SourceInfo.Format, Pixel))
			{
				OutError = "Source image format cannot be converted.";
				return false;
			}
			if (SourceInfo.GammaSpace != DestinationGamma
				&& SourceInfo.GammaSpace != EImageGammaSpace::Unknown
				&& DestinationGamma != EImageGammaSpace::Unknown)
			{
				for (size_t Channel = 0; Channel < 3; ++Channel)
					Pixel[Channel] = SourceInfo.GammaSpace == EImageGammaSpace::SRGB
						? ColorConvert::SRGBToLinear(Pixel[Channel])
						: ColorConvert::LinearToSRGB(Pixel[Channel]);
			}
			if (!EncodePixel(Pixel, DestinationFormat,
				Bytes.data() + Index * DestFormat.BytesPerPixel))
			{
				OutError = "Destination image format cannot be encoded.";
				return false;
			}
		}
		return FImage::TryCreate(DestinationInfo, std::move(Bytes), OutImage, &OutError);
	}

	auto AnalyzeImageChannels(FImageView Image, FImageChannelAnalysis& OutAnalysis,
		std::string& OutError) -> bool
	{
		OutAnalysis = {};
		if (!Image.IsValid())
		{
			OutError = "Image channel analysis requires a valid image.";
			return false;
		}
		const auto Format = GetRawImageFormatInfo(Image.GetInfo().Format);
		OutAnalysis.MeaningfulChannelCount = Format.ChannelCount;
		const uint64 PixelCount = Image.GetPixels().size() / Format.BytesPerPixel;
		for (uint64 Index = 0; Index < PixelCount; ++Index)
		{
			std::array<double, 4> Pixel;
			DecodePixel(Image.GetPixels().data() + Index * Format.BytesPerPixel,
				Image.GetInfo().Format, Pixel);
			for (double Channel : Pixel) OutAnalysis.bAllFinite &= std::isfinite(Channel);
			if (Format.ChannelCount == 4 && Pixel[3] < 1.0) OutAnalysis.bHasTransparency = true;
		}
		OutError.clear();
		return true;
	}

	auto FDecodedImage::ToImage(EImageGammaSpace GammaSpace, FImage& OutImage,
		std::string* OutError) const -> bool
	{
		return FImage::TryCreate({.Width = Width, .Height = Height,
			.Format = ERawImageFormat::RGBA8, .GammaSpace = GammaSpace},
			FByteBuffer(Pixels), OutImage, OutError);
	}

	auto FDecodedGrayscale16Image::ToImage(EImageGammaSpace GammaSpace,
		FImage& OutImage, std::string* OutError) const -> bool
	{
		FByteBuffer Bytes(Samples.size() * sizeof(uint16));
		if (!Bytes.empty()) std::memcpy(Bytes.data(), Samples.data(), Bytes.size());
		return FImage::TryCreate({.Width = Width, .Height = Height,
			.Format = ERawImageFormat::G16, .GammaSpace = GammaSpace},
			std::move(Bytes), OutImage, OutError);
	}

	auto FDecodedFloatImage::ToImage(FImage& OutImage, std::string* OutError) const -> bool
	{
		const uint64 PixelCount = static_cast<uint64>(Width) * Height;
		if (PixelCount > std::numeric_limits<size_t>::max() / 4
			|| Pixels.size() != PixelCount * 3)
			return Fail("Decoded HDR image dimensions and pixels do not match.", OutError);
		FByteBuffer Bytes(static_cast<size_t>(PixelCount) * 4 * sizeof(float));
		for (size_t Index = 0; Index < static_cast<size_t>(PixelCount); ++Index)
		{
			std::memcpy(Bytes.data() + Index * 4 * sizeof(float),
				Pixels.data() + Index * 3, 3 * sizeof(float));
			Store(Bytes.data() + (Index * 4 + 3) * sizeof(float), 1.0f);
		}
		return FImage::TryCreate({.Width = Width, .Height = Height,
			.Format = ERawImageFormat::RGBA32F,
			.GammaSpace = EImageGammaSpace::Linear},
			std::move(Bytes), OutImage, OutError);
	}
} // namespace Durin::Image
