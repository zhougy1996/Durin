#include "Texture/VolumeTextureBuilder.h"

namespace Durin::Asset::Build::VolumeTextureBuilder
{
	namespace
	{
		struct FFormatLayout
		{
			EPixelFormat PixelFormat = EPixelFormat::Unknown;
			uint32 Channels = 0;
			uint32 BytesPerChannel = 0;
			bool bFloat = false;
		};

		auto GetLayout(EVolumeTextureFormat Format) -> FFormatLayout
		{
			switch (Format)
			{
			case EVolumeTextureFormat::R8_UNORM: return {EPixelFormat::R8_UNORM, 1, 1, false};
			case EVolumeTextureFormat::RG8_UNORM: return {EPixelFormat::RG8_UNORM, 2, 1, false};
			case EVolumeTextureFormat::RGBA8_UNORM: return {EPixelFormat::RGBA8_UNORM, 4, 1, false};
			case EVolumeTextureFormat::R16_FLOAT: return {EPixelFormat::R16_FLOAT, 1, 2, true};
			case EVolumeTextureFormat::RGBA16_FLOAT: return {EPixelFormat::RGBA16_FLOAT, 4, 2, true};
			default: return {};
			}
		}

		auto HalfToFloat(uint16 Half) -> float
		{
			const uint32 Sign = static_cast<uint32>(Half & 0x8000u) << 16;
			uint32 Exponent = (Half >> 10) & 0x1fu;
			uint32 Mantissa = Half & 0x3ffu;
			uint32 Bits = 0;
			if (Exponent == 0)
			{
				if (Mantissa == 0) Bits = Sign;
				else
				{
					int32 Shift = 0;
					while ((Mantissa & 0x400u) == 0) { Mantissa <<= 1; ++Shift; }
					Mantissa &= 0x3ffu;
					Bits = Sign | static_cast<uint32>(127 - 15 - Shift) << 23
						| Mantissa << 13;
				}
			}
			else if (Exponent == 31) Bits = Sign | 0x7f800000u | Mantissa << 13;
			else Bits = Sign | (Exponent + 112u) << 23 | Mantissa << 13;
			return std::bit_cast<float>(Bits);
		}

		auto FloatToHalf(float Value) -> uint16
		{
			const uint32 Bits = std::bit_cast<uint32>(Value);
			const uint32 Sign = (Bits >> 16) & 0x8000u;
			const uint32 Mantissa = Bits & 0x7fffffu;
			const int32 Exponent = static_cast<int32>((Bits >> 23) & 0xffu) - 127 + 15;
			if (Exponent <= 0)
			{
				if (Exponent < -10) return static_cast<uint16>(Sign);
				const uint32 Normalized = Mantissa | 0x800000u;
				const uint32 Shift = static_cast<uint32>(14 - Exponent);
				const uint32 Rounded = (Normalized + ((1u << (Shift - 1)) - 1u)
					+ ((Normalized >> Shift) & 1u)) >> Shift;
				return static_cast<uint16>(Sign | Rounded);
			}
			if (Exponent >= 31) return static_cast<uint16>(Sign | 0x7c00u);
			uint32 RoundedMantissa = Mantissa + 0xfffu + ((Mantissa >> 13) & 1u);
			uint32 EncodedExponent = static_cast<uint32>(Exponent);
			if (RoundedMantissa & 0x800000u)
			{
				RoundedMantissa = 0;
				if (++EncodedExponent >= 31) return static_cast<uint16>(Sign | 0x7c00u);
			}
			return static_cast<uint16>(Sign | EncodedExponent << 10
				| (RoundedMantissa >> 13));
		}

		auto ReadChannel(const uint8* Voxel, uint32 Channel,
			const FFormatLayout& Layout, float& OutValue) -> bool
		{
			if (!Layout.bFloat)
			{
				OutValue = static_cast<float>(Voxel[Channel]) / 255.0f;
				return true;
			}
			uint16 Half = 0;
			std::memcpy(&Half, Voxel + Channel * 2, sizeof(Half));
			OutValue = HalfToFloat(Half);
			return std::isfinite(OutValue);
		}

		auto WriteChannel(uint8* Voxel, uint32 Channel,
			const FFormatLayout& Layout, float Value) -> void
		{
			if (!Layout.bFloat)
			{
				Voxel[Channel] = static_cast<uint8>(std::floor(
					std::clamp(Value, 0.0f, 1.0f) * 255.0f + 0.5f));
				return;
			}
			const uint16 Half = FloatToHalf(Value);
			std::memcpy(Voxel + Channel * 2, &Half, sizeof(Half));
		}
	}

	auto BuildMipChain(const FVolumeTextureSourceData& SourceData,
		const FVolumeTextureBuildSettings& Settings,
		FVolumeTexturePlatformData& OutPlatformData,
		std::string& OutError) -> bool
	{
		OutPlatformData = {};
		if (!SourceData.IsValid() || SourceData.Format != Settings.OutputFormat
			|| Settings.MipFilter != EVolumeTextureMipFilter::Box)
		{
			OutError = "Volume texture build requires valid source with matching output format and box filtering.";
			return false;
		}
		const FFormatLayout Layout = GetLayout(Settings.OutputFormat);
		const uint32 BytesPerVoxel = Layout.Channels * Layout.BytesPerChannel;
		FVolumeTexturePlatformData Candidate;
		Candidate.PixelFormat = Layout.PixelFormat;
		FVolumeTextureMipData& Base = Candidate.Mips.emplace_back();
		Base.Width = SourceData.Width;
		Base.Height = SourceData.Height;
		Base.Depth = SourceData.Depth;
		Base.RowPitch = Base.Width * BytesPerVoxel;
		Base.DepthPitch = Base.RowPitch * Base.Height;
		Base.Voxels.resize(SourceData.Voxels.size());
		std::ranges::transform(SourceData.Voxels, Base.Voxels.begin(),
			[](std::byte Byte) { return std::to_integer<uint8>(Byte); });
		for (size_t Offset = 0; Offset < Base.Voxels.size(); Offset += BytesPerVoxel)
			for (uint32 Channel = 0; Channel < Layout.Channels; ++Channel)
			{
				float Value = 0.0f;
				if (!ReadChannel(Base.Voxels.data() + Offset, Channel, Layout, Value))
				{
					OutError = "Volume texture float source contains a nonfinite channel.";
					return false;
				}
			}

		while (Candidate.Mips.back().Width != 1
			|| Candidate.Mips.back().Height != 1
			|| Candidate.Mips.back().Depth != 1)
		{
			const FVolumeTextureMipData& Previous = Candidate.Mips.back();
			FVolumeTextureMipData Next;
			Next.Width = std::max(1u, Previous.Width / 2);
			Next.Height = std::max(1u, Previous.Height / 2);
			Next.Depth = std::max(1u, Previous.Depth / 2);
			Next.RowPitch = Next.Width * BytesPerVoxel;
			Next.DepthPitch = Next.RowPitch * Next.Height;
			Next.Voxels.resize(static_cast<size_t>(Next.DepthPitch) * Next.Depth);
			for (uint32 Z = 0; Z < Next.Depth; ++Z)
				for (uint32 Y = 0; Y < Next.Height; ++Y)
					for (uint32 X = 0; X < Next.Width; ++X)
					{
						const uint32 X0 = X * 2, Y0 = Y * 2, Z0 = Z * 2;
						const uint32 X1 = std::min(X0 + 2, Previous.Width);
						const uint32 Y1 = std::min(Y0 + 2, Previous.Height);
						const uint32 Z1 = std::min(Z0 + 2, Previous.Depth);
						const uint32 SampleCount = (X1 - X0) * (Y1 - Y0) * (Z1 - Z0);
						uint8* Destination = Next.Voxels.data()
							+ static_cast<size_t>(Z) * Next.DepthPitch
							+ static_cast<size_t>(Y) * Next.RowPitch
							+ static_cast<size_t>(X) * BytesPerVoxel;
						for (uint32 Channel = 0; Channel < Layout.Channels; ++Channel)
						{
							double Sum = 0.0;
							for (uint32 SourceZ = Z0; SourceZ < Z1; ++SourceZ)
								for (uint32 SourceY = Y0; SourceY < Y1; ++SourceY)
									for (uint32 SourceX = X0; SourceX < X1; ++SourceX)
									{
										const uint8* Source = Previous.Voxels.data()
											+ static_cast<size_t>(SourceZ) * Previous.DepthPitch
											+ static_cast<size_t>(SourceY) * Previous.RowPitch
											+ static_cast<size_t>(SourceX) * BytesPerVoxel;
										float Value = 0.0f;
										ReadChannel(Source, Channel, Layout, Value);
										Sum += Value;
									}
							WriteChannel(Destination, Channel, Layout,
								static_cast<float>(Sum / SampleCount));
						}
					}
			Candidate.Mips.push_back(std::move(Next));
		}
		if (!Candidate.IsValid())
		{
			OutError = "Volume texture builder produced invalid platform data.";
			return false;
		}
		OutPlatformData = std::move(Candidate);
		OutError.clear();
		return true;
	}
}
