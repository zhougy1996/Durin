#include "Texture/TextureMipBuilder.h"

#include "Math/Color.h"
#include "Texture/TextureBCEncoder.h"

namespace Durin::TextureMipBuilder
{
	namespace
	{
		struct FMutableMip
		{
			FByteBuffer Pixels;
			uint32 Width = 0, Height = 0, RowPitch = 0;
			operator FReadOnlyMipView() const
			{ return {Pixels, Width, Height, RowPitch}; }
		};

		auto BuildNextMip(
			const FReadOnlyMipView& Source,
			ETextureUsage Usage,
			bool bSRGB,
			FMutableMip& OutResult) -> void
		{
			FMutableMip Result;
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
								const uint8 Value = std::to_integer<uint8>(Source.Pixels[SourceOffset + Channel]);
								if (Usage == ETextureUsage::Color && bSRGB && Channel < 3) Sum[Channel] += ColorConvert::SRGB8ToLinear(Value);
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
						Result.Pixels[DestOffset] = static_cast<std::byte>(ColorConvert::QuantizeUNorm8(X * 0.5 + 0.5));
						Result.Pixels[DestOffset + 1] = static_cast<std::byte>(ColorConvert::QuantizeUNorm8(Y * 0.5 + 0.5));
						Result.Pixels[DestOffset + 2] = static_cast<std::byte>(ColorConvert::QuantizeUNorm8(Z * 0.5 + 0.5));
					}
					else
					{
						for (uint32 Channel = 0; Channel < 3; ++Channel)
						{
							const double Average = Sum[Channel] / SampleCount;
							Result.Pixels[DestOffset + Channel] = static_cast<std::byte>(
								Usage == ETextureUsage::Color && bSRGB ? ColorConvert::LinearToSRGB8(Average) : ColorConvert::QuantizeUNorm8(Average));
						}
					}
					Result.Pixels[DestOffset + 3] = static_cast<std::byte>(ColorConvert::QuantizeUNorm8(Sum[3] / SampleCount));
				}
			}
			OutResult = std::move(Result);
		}

		auto CalculateAlphaCoverage(
			const FReadOnlyMipView& Mip,
			float Threshold,
			double Scale,
			double& OutCoverage) -> void
		{
			const uint8 EncodedThreshold = ColorConvert::QuantizeUNorm8(Threshold);
			uint64 CoveredPixelCount = 0;
			for (uint32 Y = 0; Y < Mip.Height; ++Y)
			{
				for (uint32 X = 0; X < Mip.Width; ++X)
				{
					const size_t Offset = static_cast<size_t>(Y) * Mip.RowPitch + X * ChannelCount + 3;
					const uint8 AdjustedAlpha = ColorConvert::QuantizeUNorm8(
						static_cast<double>(std::to_integer<uint8>(Mip.Pixels[Offset])) / 255.0 * Scale);
					if (AdjustedAlpha >= EncodedThreshold) ++CoveredPixelCount;
				}
			}
			OutCoverage = static_cast<double>(CoveredPixelCount)
				/ (static_cast<uint64>(Mip.Width) * Mip.Height);
		}

		auto PreserveAlphaCoverage(
			FMutableMip& Mip,
			float Threshold,
			double TargetCoverage) -> void
		{
			double LowScale = 0.0;
			double HighScale = 1.0;
			double Coverage = 0.0;
			CalculateAlphaCoverage(Mip, Threshold, HighScale, Coverage);
			while (Coverage < TargetCoverage && HighScale < 256.0)
			{
				HighScale *= 2.0;
				CalculateAlphaCoverage(Mip, Threshold, HighScale, Coverage);
			}

			double BestScale = 1.0;
			CalculateAlphaCoverage(Mip, Threshold, 1.0, Coverage);
			double BestError = std::abs(Coverage - TargetCoverage);
			for (uint32 Iteration = 0; Iteration < 16; ++Iteration)
			{
				const double Scale = (LowScale + HighScale) * 0.5;
				CalculateAlphaCoverage(Mip, Threshold, Scale, Coverage);
				const double Error = std::abs(Coverage - TargetCoverage);
				if (Error < BestError)
				{
					BestError = Error;
					BestScale = Scale;
				}
				if (Coverage < TargetCoverage) LowScale = Scale;
				else HighScale = Scale;
			}

			for (uint32 Y = 0; Y < Mip.Height; ++Y)
			{
				for (uint32 X = 0; X < Mip.Width; ++X)
				{
					const size_t Offset = static_cast<size_t>(Y) * Mip.RowPitch + X * ChannelCount + 3;
					Mip.Pixels[Offset] = static_cast<std::byte>(ColorConvert::QuantizeUNorm8(
						static_cast<double>(std::to_integer<uint8>(Mip.Pixels[Offset])) / 255.0 * BestScale));
				}
			}
		}

		auto GenerateMipChain(const FBuildTextureMipsRequest& Request,
			FTexture2DBuildTimings& Metrics)
			-> std::expected<std::vector<Image::FImage>, std::string>
		{
			using FClock = std::chrono::steady_clock;
			const auto SourceMips = Request.SourceMips;
			const auto& Settings = Request.Settings;
			const auto Usage = Settings.Usage;
			const bool bSRGB = *Settings.bSRGB;
			const auto AlphaMipMode = Settings.AlphaMipMode;
			const auto AlphaCoverageThreshold = Settings.AlphaCoverageThreshold;
			const bool bHasTransparency = Request.PixelFormat == EPixelFormat::BC3_UNORM
				|| Request.PixelFormat == EPixelFormat::BC3_UNORM_SRGB;
			// FImage copies share the source allocation; no writable source copy is needed.
			std::vector<Image::FImage> UncompressedMips(SourceMips.begin(), SourceMips.end());
			const Image::FImage& BaseMip = UncompressedMips.front();
			const bool bPreserveAlphaCoverage = Usage == ETextureUsage::Color
				&& bHasTransparency && AlphaMipMode == ETextureAlphaMipMode::PreserveCoverage;
			double SourceAlphaCoverage = 0.0;
			if (bPreserveAlphaCoverage)
				CalculateAlphaCoverage(BaseMip, AlphaCoverageThreshold, 1.0, SourceAlphaCoverage);
			const FClock::time_point MipStart = FClock::now();
			while (SourceMips.size() == 1
				&& (UncompressedMips.back().GetInfo().Width > 1 || UncompressedMips.back().GetInfo().Height > 1))
			{
				FMutableMip NextMip;
				BuildNextMip(UncompressedMips.back(), Usage, bSRGB, NextMip);
				if (bPreserveAlphaCoverage)
					PreserveAlphaCoverage(NextMip, AlphaCoverageThreshold, SourceAlphaCoverage);
				auto ImageResult1 = Image::FImage::TryCreate({.Width = NextMip.Width, .Height = NextMip.Height,
					.Format = Image::ERawImageFormat::RGBA8,
					.GammaSpace = SourceMips.front().GetInfo().GammaSpace}, std::move(NextMip.Pixels));
				if (!ImageResult1)
				{
					return std::unexpected(std::string("Generated texture mip layout is invalid."));
				}
				auto FrozenMip = std::move(*ImageResult1);
				UncompressedMips.push_back(std::move(FrozenMip));
			}
			const FClock::time_point MipFinish = FClock::now();
			Metrics.MipGenerationNanoseconds = static_cast<uint64>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(MipFinish - MipStart).count());
			// Shared source bytes are already accounted as decoded input by the caller.
			for (size_t Index = SourceMips.size(); Index < UncompressedMips.size(); ++Index)
				Metrics.PeakIntermediateBytes += UncompressedMips[Index].GetPixels().size();
			return UncompressedMips;
		}
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

	auto AnalyzeTransparency(std::span<const Image::FImage> SourceMips) -> bool
	{
		bool bHasTransparency = false;
		// Supplied lower mips may contain alpha even when the base mip is opaque.
		for (const Image::FImage& Mip : SourceMips)
		{
			const auto Pixels = Mip.GetPixels();
			const uint32 Width = Mip.GetInfo().Width;
			for (uint32 Y = 0; Y < Mip.GetInfo().Height && !bHasTransparency; ++Y)
			{
				const size_t Row = static_cast<size_t>(Y) * Width * ChannelCount;
				for (uint32 X = 0; X < Width; ++X)
					if (Pixels[Row + static_cast<size_t>(X) * ChannelCount + 3] != std::byte{255})
					{
						bHasTransparency = true;
						break;
					}
			}
			if (bHasTransparency) break;
		}
		return bHasTransparency;
	}

	auto BuildTextureMips(const FBuildTextureMipsRequest& Request,
		const FBuildExecutionOptions* ExecutionOptions) -> std::expected<FTexture2DBuildOutput, std::string>
	{
		using FClock = std::chrono::steady_clock;
		const auto SourceMips = Request.SourceMips;
		const auto& Settings = Request.Settings;
		const auto MaxResolution = Settings.MaxResolution;
		const auto CompressionQuality = Settings.CompressionQuality;
		FTexture2DBuildOutput Product;
		auto& OutPlatformData = Product.PlatformData;
		// Metrics belong to the completed product.
		auto& Metrics = Product.Metrics;
		check(ValidateTexture2DSourceMips(SourceMips).has_value());
		check(ValidateTexture2DBuildSettings(Settings).has_value());
		check(Settings.bSRGB.has_value());
		OutPlatformData.PixelFormat = Request.PixelFormat;
		if (OutPlatformData.PixelFormat == EPixelFormat::Unknown)
		{
			return std::unexpected(std::string("Selected pixel format is unsupported by the texture encoder."));
		}
		auto Generated = GenerateMipChain(Request, Metrics);
		if (!Generated) return std::unexpected(Generated.error());
		auto UncompressedMips = std::move(*Generated);
		size_t FirstMipIndex = 0;
		if (MaxResolution > 0)
		{
			while (FirstMipIndex + 1 < UncompressedMips.size()
				&& (UncompressedMips[FirstMipIndex].GetInfo().Width > MaxResolution
					|| UncompressedMips[FirstMipIndex].GetInfo().Height > MaxResolution))
			{
				++FirstMipIndex;
			}
		}
		OutPlatformData.Mips.reserve(UncompressedMips.size() - FirstMipIndex);
		const FClock::time_point CompressionStart = FClock::now();
		for (size_t MipIndex = FirstMipIndex; MipIndex < UncompressedMips.size(); ++MipIndex)
		{
			FTexture2DMipData& CompressedMip = OutPlatformData.Mips.emplace_back();
			const std::expected<void, std::string> CompressionResult = CompressTextureMip(
				UncompressedMips[MipIndex], OutPlatformData.PixelFormat,
				CompressionQuality, CompressedMip, ExecutionOptions);
			if (!CompressionResult)
			{
				return std::unexpected(std::format("Mip {} compression failed: {}", MipIndex, CompressionResult.error()));
			}
		}
		Metrics.CompressionNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				FClock::now() - CompressionStart).count());

		if (OutPlatformData.IsValid())
		{
			return Product;
		}
		return std::unexpected(std::string("Failed to build texture platform data."));
	}
}
