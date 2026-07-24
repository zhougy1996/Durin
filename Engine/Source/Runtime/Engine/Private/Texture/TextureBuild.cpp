#include "Texture/TextureBuild.h"

#include "ImageDecoder.h"

namespace Durin::TextureBuild
{
	namespace
	{
		auto DecodeSRGB(uint8 Value) -> double
		{
			const double Encoded = static_cast<double>(Value) / 255.0;
			return Encoded <= 0.04045 ? Encoded / 12.92 : std::pow((Encoded + 0.055) / 1.055, 2.4);
		}

		auto EncodeUNorm(double Value) -> uint8
		{
			return static_cast<uint8>(std::clamp(Value, 0.0, 1.0) * 255.0 + 0.5);
		}

		auto EncodeSRGB(double Value) -> uint8
		{
			const double Linear = std::clamp(Value, 0.0, 1.0);
			return EncodeUNorm(Linear <= 0.0031308 ? Linear * 12.92 : 1.055 * std::pow(Linear, 1.0 / 2.4) - 0.055);
		}

		auto BuildNextMip(const FTexture2DMipData& Source, ETextureUsage Usage, bool bSRGB) -> FTexture2DMipData
		{
			FTexture2DMipData Result;
			Result.Width = std::max(Source.Width / 2, 1u);
			Result.Height = std::max(Source.Height / 2, 1u);
			Result.RowPitch = Result.Width * ChannelCount;
			Result.Pixels.resize(static_cast<size_t>(Result.RowPitch) * Result.Height);

			for (uint32 DestY = 0; DestY < Result.Height; ++DestY)
			{
				const uint32 BeginY = DestY * Source.Height / Result.Height;
				const uint32 EndY = (DestY + 1) * Source.Height / Result.Height;
				for (uint32 DestX = 0; DestX < Result.Width; ++DestX)
				{
					const uint32 BeginX = DestX * Source.Width / Result.Width;
					const uint32 EndX = (DestX + 1) * Source.Width / Result.Width;
					const uint32 SampleCount = (EndX - BeginX) * (EndY - BeginY);
					std::array<double, ChannelCount> Sum{};
					for (uint32 SourceY = BeginY; SourceY < EndY; ++SourceY)
					{
						for (uint32 SourceX = BeginX; SourceX < EndX; ++SourceX)
						{
							const size_t SourceOffset = static_cast<size_t>(SourceY) * Source.RowPitch + SourceX * ChannelCount;
							for (uint32 Channel = 0; Channel < ChannelCount; ++Channel)
							{
								const uint8 Value = Source.Pixels[SourceOffset + Channel];
								if (Usage == ETextureUsage::Color && bSRGB && Channel < 3) Sum[Channel] += DecodeSRGB(Value);
								else if (Usage == ETextureUsage::Normal && Channel < 3) Sum[Channel] += static_cast<double>(Value) / 127.5 - 1.0;
								else Sum[Channel] += static_cast<double>(Value) / 255.0;
							}
						}
					}

					const size_t DestOffset = static_cast<size_t>(DestY) * Result.RowPitch + DestX * ChannelCount;
					if (Usage == ETextureUsage::Normal)
					{
						double X = Sum[0] / SampleCount;
						double Y = Sum[1] / SampleCount;
						double Z = Sum[2] / SampleCount;
						const double LengthSquared = X * X + Y * Y + Z * Z;
						if (LengthSquared > std::numeric_limits<double>::epsilon())
						{
							const double InverseLength = 1.0 / std::sqrt(LengthSquared);
							X *= InverseLength;
							Y *= InverseLength;
							Z *= InverseLength;
						}
						else
						{
							X = 0.0;
							Y = 0.0;
							Z = 1.0;
						}
						Result.Pixels[DestOffset] = EncodeUNorm(X * 0.5 + 0.5);
						Result.Pixels[DestOffset + 1] = EncodeUNorm(Y * 0.5 + 0.5);
						Result.Pixels[DestOffset + 2] = EncodeUNorm(Z * 0.5 + 0.5);
					}
					else
					{
						for (uint32 Channel = 0; Channel < 3; ++Channel)
						{
							const double Average = Sum[Channel] / SampleCount;
							Result.Pixels[DestOffset + Channel] = Usage == ETextureUsage::Color && bSRGB ? EncodeSRGB(Average) : EncodeUNorm(Average);
						}
					}
					Result.Pixels[DestOffset + 3] = EncodeUNorm(Sum[3] / SampleCount);
				}
			}
			return Result;
		}
	}

	auto IsValidUsage(ETextureUsage Usage) -> bool
	{
		return Usage == ETextureUsage::Color || Usage == ETextureUsage::Normal || Usage == ETextureUsage::DataMask;
	}

	auto GetDefaultSRGB(ETextureUsage Usage) -> bool
	{
		return Usage == ETextureUsage::Color;
	}

	auto SelectPixelFormat(ETextureUsage Usage, bool bSRGB, bool bHasTransparency) -> EPixelFormat
	{
		(void)Usage;
		(void)bHasTransparency;
		return bSRGB ? EPixelFormat::SRGBA8_UNORM : EPixelFormat::RGBA8_UNORM;
	}

	auto DecodeRGBA8(std::string_view PhysicalFilePath, FTextureSourceData& OutSourceData, std::string& OutError) -> bool
	{
		OutSourceData = {};
		Asset::FDecodedImage DecodedImage;
		if (!Asset::DecodeImageFromFile(PhysicalFilePath, DecodedImage, OutError)) return false;
		if (DecodedImage.Width > MaxDimension || DecodedImage.Height > MaxDimension)
		{
			OutError = std::format("Texture dimensions {}x{} exceed the {} pixel limit.",
				DecodedImage.Width, DecodedImage.Height, MaxDimension);
			return false;
		}
		OutSourceData.Pixels = std::move(DecodedImage.Pixels);
		OutSourceData.Width = DecodedImage.Width;
		OutSourceData.Height = DecodedImage.Height;
		OutSourceData.SourceChannelCount = DecodedImage.SourceChannelCount;
		OutSourceData.Format = ETextureSourceFormat::RGBA8;
		OutSourceData.bHasTransparency = DecodedImage.bHasTransparency;
		if (OutSourceData.IsValid()) return true;
		OutSourceData = {};
		OutError = "Decoded texture source data is invalid.";
		return false;
	}

	auto BuildMipChain(const FTextureSourceData& SourceData, ETextureUsage Usage, bool bSRGB,
		FTexturePlatformData& OutPlatformData, std::string& OutError) -> bool
	{
		OutPlatformData = {};
		if (!SourceData.IsValid())
		{
			OutError = "Texture source data is unavailable or invalid.";
			return false;
		}
		if (!IsValidUsage(Usage))
		{
			OutError = "Texture usage preset is invalid.";
			return false;
		}
		OutPlatformData.PixelFormat = SelectPixelFormat(Usage, bSRGB, SourceData.bHasTransparency);
		if (OutPlatformData.PixelFormat == EPixelFormat::Unknown)
		{
			OutError = "Selected pixel format is not supported by the current RHI backend.";
			return false;
		}
		FTexture2DMipData& BaseMip = OutPlatformData.Mips.emplace_back();
		BaseMip.Pixels = SourceData.Pixels;
		BaseMip.Width = SourceData.Width;
		BaseMip.Height = SourceData.Height;
		BaseMip.RowPitch = SourceData.Width * ChannelCount;
		while (OutPlatformData.Mips.back().Width > 1 || OutPlatformData.Mips.back().Height > 1)
		{
			OutPlatformData.Mips.push_back(BuildNextMip(OutPlatformData.Mips.back(), Usage, bSRGB));
		}
		if (OutPlatformData.IsValid()) return true;
		OutPlatformData = {};
		OutError = "Failed to build texture platform data.";
		return false;
	}
}
