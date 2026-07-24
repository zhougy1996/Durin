#include "Texture/TextureBuild.h"

#include "ImageDecoder.h"

#include <bc7enc.h>
#include <rgbcx.h>

namespace Durin::TextureBuild
{
	namespace
	{
		constexpr uint32 BlockWidth = 4;
		constexpr uint32 CompressionLevel = 10;

		auto GatherTextureBlock(const FTexture2DMipData& Source, uint32 BlockX, uint32 BlockY,
			std::array<uint8, BlockWidth * BlockWidth * ChannelCount>& OutPixels) -> void
		{
			for (uint32 Y = 0; Y < BlockWidth; ++Y)
			{
				const uint32 SourceY = std::min(BlockY * BlockWidth + Y, Source.Height - 1);
				for (uint32 X = 0; X < BlockWidth; ++X)
				{
					const uint32 SourceX = std::min(BlockX * BlockWidth + X, Source.Width - 1);
					const size_t SourceOffset = static_cast<size_t>(SourceY) * Source.RowPitch + SourceX * ChannelCount;
					const size_t DestOffset = (Y * BlockWidth + X) * ChannelCount;
					std::memcpy(OutPixels.data() + DestOffset, Source.Pixels.data() + SourceOffset, ChannelCount);
				}
			}
		}

		auto CompressTextureMip(const FTexture2DMipData& Source, EPixelFormat Format,
			FTexture2DMipData& OutMip, std::string& OutError) -> bool
		{
			const FPixelFormatLayout Layout = GetPixelFormatLayout(Format, Source.Width, Source.Height);
			if (Layout.DataSize == 0 || Layout.RowPitch > std::numeric_limits<uint32>::max()
				|| Layout.DataSize > std::numeric_limits<size_t>::max())
			{
				OutError = "Compressed texture mip layout exceeds supported limits.";
				return false;
			}

			static std::once_flag EncoderInitFlag;
			std::call_once(EncoderInitFlag, [] {
				rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
				bc7enc_compress_block_init();
			});

			OutMip.Width = Source.Width;
			OutMip.Height = Source.Height;
			OutMip.RowPitch = static_cast<uint32>(Layout.RowPitch);
			OutMip.Pixels.resize(static_cast<size_t>(Layout.DataSize));

			bc7enc_compress_block_params BC7Params;
			bc7enc_compress_block_params_init(&BC7Params);
			if (!GetPixelFormatInfo(Format).bIsSRGB)
				bc7enc_compress_block_params_init_linear_weights(&BC7Params);

			std::array<uint8, BlockWidth * BlockWidth * ChannelCount> BlockPixels{};
			for (uint32 BlockY = 0; BlockY < Layout.BlocksHigh; ++BlockY)
			{
				for (uint32 BlockX = 0; BlockX < Layout.BlocksWide; ++BlockX)
				{
					GatherTextureBlock(Source, BlockX, BlockY, BlockPixels);
					uint8* DestBlock = OutMip.Pixels.data()
						+ static_cast<size_t>(BlockY) * OutMip.RowPitch
						+ static_cast<size_t>(BlockX) * GetPixelFormatInfo(Format).BytesPerBlock;
					switch (Format)
					{
					case EPixelFormat::BC1_UNORM:
					case EPixelFormat::BC1_UNORM_SRGB:
						// Four-color mode keeps opaque textures opaque when sampled.
						rgbcx::encode_bc1(CompressionLevel, DestBlock, BlockPixels.data(), false, false);
						break;
					case EPixelFormat::BC3_UNORM:
					case EPixelFormat::BC3_UNORM_SRGB:
						rgbcx::encode_bc3_hq(CompressionLevel, DestBlock, BlockPixels.data());
						break;
					case EPixelFormat::BC5_UNORM:
						rgbcx::encode_bc5_hq(DestBlock, BlockPixels.data());
						break;
					case EPixelFormat::BC7_UNORM:
					case EPixelFormat::BC7_UNORM_SRGB:
						bc7enc_compress_block(DestBlock, BlockPixels.data(), &BC7Params);
						break;
					default:
						OutError = std::format("Texture compression is unavailable for pixel format {}.",
							GetPixelFormatInfo(Format).Name);
						return false;
					}
				}
			}
			return true;
		}

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
		switch (Usage)
		{
		case ETextureUsage::Color:
			if (bHasTransparency) return bSRGB ? EPixelFormat::BC3_UNORM_SRGB : EPixelFormat::BC3_UNORM;
			return bSRGB ? EPixelFormat::BC1_UNORM_SRGB : EPixelFormat::BC1_UNORM;
		case ETextureUsage::Normal:
			return EPixelFormat::BC5_UNORM;
		case ETextureUsage::DataMask:
			return bSRGB ? EPixelFormat::BC7_UNORM_SRGB : EPixelFormat::BC7_UNORM;
		default:
			return EPixelFormat::Unknown;
		}
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
		std::vector<FTexture2DMipData> UncompressedMips;
		FTexture2DMipData& BaseMip = UncompressedMips.emplace_back();
		BaseMip.Pixels = SourceData.Pixels;
		BaseMip.Width = SourceData.Width;
		BaseMip.Height = SourceData.Height;
		BaseMip.RowPitch = SourceData.Width * ChannelCount;
		while (UncompressedMips.back().Width > 1 || UncompressedMips.back().Height > 1)
		{
			UncompressedMips.push_back(BuildNextMip(UncompressedMips.back(), Usage, bSRGB));
		}
		OutPlatformData.Mips.reserve(UncompressedMips.size());
		for (const FTexture2DMipData& UncompressedMip : UncompressedMips)
		{
			FTexture2DMipData& CompressedMip = OutPlatformData.Mips.emplace_back();
			if (!CompressTextureMip(UncompressedMip, OutPlatformData.PixelFormat, CompressedMip, OutError))
			{
				OutPlatformData = {};
				return false;
			}
		}
		if (OutPlatformData.IsValid()) return true;
		OutPlatformData = {};
		OutError = "Failed to build texture platform data.";
		return false;
	}
}
