#include "Texture/TextureBuilder.h"

#include "Math/Color.h"
#include "Threading/Task.h"

#include <bc7enc.h>
#include <rgbcx.h>

namespace Durin::TextureBuilder
{
	namespace
	{
		constexpr uint32 BlockWidth = 4;

		// Borrowed only within synchronous build helpers. The mip-chain FImages
		// retain shared immutable storage until all compression tasks have drained.
		struct FReadOnlyMip
		{
			FByteView Pixels;
			uint32 Width, Height, RowPitch;
			FReadOnlyMip(const Image::FImage& Image)
				: Pixels(Image.GetPixels()), Width(Image.GetInfo().Width),
				Height(Image.GetInfo().Height), RowPitch(Width * ChannelCount) {}
			FReadOnlyMip(const FTexture2DMipData& Mip)
				: Pixels(Mip.Pixels), Width(Mip.Width), Height(Mip.Height), RowPitch(Mip.RowPitch) {}
		};

		auto IsCancellationRequested(const FBuildExecutionControl* ExecutionControl) -> bool
		{
			return ExecutionControl && ExecutionControl->ShouldCancel
				&& ExecutionControl->ShouldCancel();
		}
		auto GatherTextureBlock(const FReadOnlyMip& Source, uint32 BlockX, uint32 BlockY,
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

		auto GetCompressionLevel(ETextureCompressionQuality Quality) -> uint32
		{
			switch (Quality)
			{
			case ETextureCompressionQuality::Low: return 4;
			case ETextureCompressionQuality::Normal: return 10;
			case ETextureCompressionQuality::High: return 18;
			default: return 10;
			}
		}

		auto CompressTextureMip(const FReadOnlyMip& Source, EPixelFormat Format,
			ETextureCompressionQuality Quality,
			FTexture2DMipData& OutMip,
			const FBuildExecutionControl* ExecutionControl) -> FTexture2DBuildResult
		{
			const FPixelFormatLayout Layout = GetPixelFormatLayout(Format, Source.Width, Source.Height);
			if (Layout.DataSize == 0 || Layout.RowPitch > std::numeric_limits<uint32>::max()
				|| Layout.DataSize > std::numeric_limits<size_t>::max())
			{
				return {ETexture2DBuildStatus::Failed,
					{.Code = ETexture2DBuildError::CompressedLayoutOverflow}};
			}

			if (Format != EPixelFormat::BC1_UNORM && Format != EPixelFormat::BC1_UNORM_SRGB
				&& Format != EPixelFormat::BC3_UNORM && Format != EPixelFormat::BC3_UNORM_SRGB
				&& Format != EPixelFormat::BC5_UNORM && Format != EPixelFormat::BC7_UNORM
				&& Format != EPixelFormat::BC7_UNORM_SRGB)
				return {ETexture2DBuildStatus::Failed, {.Code = ETexture2DBuildError::UnsupportedPixelFormat}};

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
			switch (Quality)
			{
			case ETextureCompressionQuality::Low:
				BC7Params.m_max_partitions = 16;
				BC7Params.m_try_least_squares = false;
				break;
			case ETextureCompressionQuality::Normal:
				break;
			case ETextureCompressionQuality::High:
				BC7Params.m_uber_level = 2;
				break;
			default:
				return {ETexture2DBuildStatus::Failed,
					{.Code = ETexture2DBuildError::InvalidCompressionQuality}};
			}
			const uint32 CompressionLevel = GetCompressionLevel(Quality);
			const uint32 AlphaSearchRadius = Quality == ETextureCompressionQuality::Low ? 1
				: Quality == ETextureCompressionQuality::High ? 5 : rgbcx::BC4_DEFAULT_SEARCH_RAD;

			// Rows write disjoint output ranges. Keep cancellation callbacks serialized:
			// callers are not required to provide a concurrently callable predicate.
			std::mutex CancellationMutex;
			bool bCancelled = false;
			auto ShouldCancel = [&] {
				std::lock_guard Lock(CancellationMutex);
				bCancelled = bCancelled || IsCancellationRequested(ExecutionControl);
				return bCancelled;
			};
			const bool bParallel = !ExecutionControl || ExecutionControl->bParallelCompression;
			// A 4096-block batching threshold, no more than eight chunks per mip.
			const uint64 RowsPerChunk = std::max<uint64>(
				(4096ull + Layout.BlocksWide - 1) / Layout.BlocksWide,
				(Layout.BlocksHigh + 7ull) / 8);
			const auto Compression = ParallelFor("Texture.CompressRows", Layout.BlocksHigh,
				[&](uint64 Row) {
				const uint32 BlockY = static_cast<uint32>(Row);
				if (ShouldCancel()) return;
				std::array<uint8, BlockWidth * BlockWidth * ChannelCount> BlockPixels{};
				for (uint32 BlockX = 0; BlockX < Layout.BlocksWide; ++BlockX)
				{
					if (BlockX != 0 && BlockX % CancellationBlockInterval == 0
						&& ShouldCancel())
					{
						return;
					}
					GatherTextureBlock(Source, BlockX, BlockY, BlockPixels);
					uint8* DestBlock = reinterpret_cast<uint8*>(OutMip.Pixels.data())
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
						rgbcx::encode_bc3_hq(CompressionLevel, DestBlock, BlockPixels.data(), AlphaSearchRadius);
						break;
					case EPixelFormat::BC5_UNORM:
						rgbcx::encode_bc5_hq(DestBlock, BlockPixels.data(), 0, 1, 4, AlphaSearchRadius);
						break;
					case EPixelFormat::BC7_UNORM:
					case EPixelFormat::BC7_UNORM_SRGB:
						bc7enc_compress_block(DestBlock, BlockPixels.data(), &BC7Params);
						break;
					default:
						break; // Format was validated before task admission.
					}
				}
			}, {.MinBatchSize = bParallel ? RowsPerChunk : std::numeric_limits<uint64>::max()});
			if (bCancelled)
				return {ETexture2DBuildStatus::Cancelled, {.Code = ETexture2DBuildError::Cancelled}};
			if (Compression.State != ETaskState::Succeeded)
				return {ETexture2DBuildStatus::Failed, {.Code = ETexture2DBuildError::CompressionTaskFailed, .TaskState = Compression.State}};
			return {ETexture2DBuildStatus::Succeeded, {}};
		}

		auto BuildNextMip(
			const FReadOnlyMip& Source,
			ETextureUsage Usage,
			bool bSRGB,
			FTexture2DMipData& OutResult,
			const FBuildExecutionControl* ExecutionControl) -> bool
		{
			FTexture2DMipData Result;
			Result.Width = std::max(Source.Width / 2, 1u);
			Result.Height = std::max(Source.Height / 2, 1u);
			Result.RowPitch = Result.Width * ChannelCount;
			Result.Pixels.resize(static_cast<size_t>(Result.RowPitch) * Result.Height);

			for (uint32 DestY = 0; DestY < Result.Height; ++DestY)
			{
				if (DestY % CancellationScanlineInterval == 0
					&& IsCancellationRequested(ExecutionControl)) return false;
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
			return true;
		}

		auto CalculateAlphaCoverage(
			const FReadOnlyMip& Mip,
			float Threshold,
			double Scale,
			double& OutCoverage,
			const FBuildExecutionControl* ExecutionControl) -> bool
		{
			const uint8 EncodedThreshold = ColorConvert::QuantizeUNorm8(Threshold);
			uint64 CoveredPixelCount = 0;
			for (uint32 Y = 0; Y < Mip.Height; ++Y)
			{
				if (Y % CancellationScanlineInterval == 0
					&& IsCancellationRequested(ExecutionControl)) return false;
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
			return true;
		}

		auto PreserveAlphaCoverage(
			FTexture2DMipData& Mip,
			float Threshold,
			double TargetCoverage,
			const FBuildExecutionControl* ExecutionControl) -> bool
		{
			double LowScale = 0.0;
			double HighScale = 1.0;
			double Coverage = 0.0;
			if (!CalculateAlphaCoverage(
				Mip, Threshold, HighScale, Coverage, ExecutionControl)) return false;
			while (Coverage < TargetCoverage && HighScale < 256.0)
			{
				HighScale *= 2.0;
				if (!CalculateAlphaCoverage(
					Mip, Threshold, HighScale, Coverage, ExecutionControl)) return false;
			}

			double BestScale = 1.0;
			if (!CalculateAlphaCoverage(
				Mip, Threshold, 1.0, Coverage, ExecutionControl)) return false;
			double BestError = std::abs(Coverage - TargetCoverage);
			for (uint32 Iteration = 0; Iteration < 16; ++Iteration)
			{
				const double Scale = (LowScale + HighScale) * 0.5;
				if (!CalculateAlphaCoverage(
					Mip, Threshold, Scale, Coverage, ExecutionControl)) return false;
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
				if (Y % CancellationScanlineInterval == 0
					&& IsCancellationRequested(ExecutionControl)) return false;
				for (uint32 X = 0; X < Mip.Width; ++X)
				{
					const size_t Offset = static_cast<size_t>(Y) * Mip.RowPitch + X * ChannelCount + 3;
					Mip.Pixels[Offset] = static_cast<std::byte>(ColorConvert::QuantizeUNorm8(
						static_cast<double>(std::to_integer<uint8>(Mip.Pixels[Offset])) / 255.0 * BestScale));
				}
			}
			return true;
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

	auto BuildMipChain(std::span<const Image::FImage> SourceMips, ETextureUsage Usage, bool bSRGB,
		FTexturePlatformData& OutPlatformData, uint32 MaxResolution,
		ETextureCompressionQuality CompressionQuality, ETextureAlphaMipMode AlphaMipMode,
		float AlphaCoverageThreshold, const FBuildExecutionControl* ExecutionControl,
		std::optional<bool> TransparencyOverride) -> FTexture2DBuildResult
	{
		using FClock = std::chrono::steady_clock;
		auto IsCancelled = [ExecutionControl] {
			return ExecutionControl && ExecutionControl->ShouldCancel
				&& ExecutionControl->ShouldCancel();
		};
		if (ExecutionControl && ExecutionControl->Metrics)
			*ExecutionControl->Metrics = {};
		OutPlatformData = {};
		if (const auto Validation = ValidateTexture2DSourceMips(SourceMips); !Validation)
		{
			return {ETexture2DBuildStatus::Failed,
				{.Code = ETexture2DBuildError::InvalidInput, .InputCause = Validation.Error}};
		}
		if (!IsValidTextureUsage(Usage))
		{
			return {ETexture2DBuildStatus::Failed, {.Code = ETexture2DBuildError::InvalidUsage}};
		}
		if (!IsValidTextureCompressionQuality(CompressionQuality))
		{
			return {ETexture2DBuildStatus::Failed,
				{.Code = ETexture2DBuildError::InvalidCompressionQuality}};
		}
		if (!IsValidTextureAlphaMipMode(AlphaMipMode))
		{
			return {ETexture2DBuildStatus::Failed, {.Code = ETexture2DBuildError::InvalidAlphaMipMode}};
		}
		if (!IsValidTextureAlphaCoverageThreshold(AlphaCoverageThreshold))
		{
			return {ETexture2DBuildStatus::Failed,
				{.Code = ETexture2DBuildError::InvalidAlphaCoverageThreshold}};
		}
		bool bHasTransparency = TransparencyOverride.value_or(false);
		if (!TransparencyOverride)
		{
			// Supplied lower mips may contain alpha even when the base mip is opaque.
			for (const Image::FImage& Mip : SourceMips)
			{
				const auto Pixels = Mip.GetPixels();
				const uint32 Width = Mip.GetInfo().Width;
				for (uint32 Y = 0; Y < Mip.GetInfo().Height && !bHasTransparency; ++Y)
				{
					if (Y % CancellationScanlineInterval == 0 && IsCancelled())
						return {ETexture2DBuildStatus::Cancelled, {.Code = ETexture2DBuildError::Cancelled}};
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
		}
		OutPlatformData.PixelFormat = SelectPixelFormat(Usage, bSRGB, bHasTransparency);
		if (OutPlatformData.PixelFormat == EPixelFormat::Unknown)
		{
			return {ETexture2DBuildStatus::Failed,
				{.Code = ETexture2DBuildError::UnsupportedPixelFormat}};
		}
		// FImage copies share the source allocation; no writable source copy is needed.
		std::vector<Image::FImage> UncompressedMips(SourceMips.begin(), SourceMips.end());
		const Image::FImage& BaseMip = UncompressedMips.front();
		const bool bPreserveAlphaCoverage = Usage == ETextureUsage::Color
			&& bHasTransparency && AlphaMipMode == ETextureAlphaMipMode::PreserveCoverage;
		double SourceAlphaCoverage = 0.0;
		if (bPreserveAlphaCoverage
			&& !CalculateAlphaCoverage(
				BaseMip,
				AlphaCoverageThreshold,
				1.0,
				SourceAlphaCoverage,
				ExecutionControl))
		{
			OutPlatformData = {};
			return {ETexture2DBuildStatus::Cancelled, {.Code = ETexture2DBuildError::Cancelled}};
		}
		const FClock::time_point MipStart = FClock::now();
		while (SourceMips.size() == 1
			&& (UncompressedMips.back().GetInfo().Width > 1 || UncompressedMips.back().GetInfo().Height > 1))
		{
			if (IsCancelled())
			{
				OutPlatformData = {};
				return {ETexture2DBuildStatus::Cancelled, {.Code = ETexture2DBuildError::Cancelled}};
			}
			FTexture2DMipData NextMip;
			if (!BuildNextMip(
				UncompressedMips.back(), Usage, bSRGB, NextMip, ExecutionControl))
			{
				OutPlatformData = {};
				return {ETexture2DBuildStatus::Cancelled, {.Code = ETexture2DBuildError::Cancelled}};
			}
			if (bPreserveAlphaCoverage)
			{
				if (!PreserveAlphaCoverage(
					NextMip,
					AlphaCoverageThreshold,
					SourceAlphaCoverage,
					ExecutionControl))
				{
					OutPlatformData = {};
					return {ETexture2DBuildStatus::Cancelled, {.Code = ETexture2DBuildError::Cancelled}};
				}
			}
			auto ImageResult1 = Image::FImage::TryCreate({.Width = NextMip.Width, .Height = NextMip.Height,
				.Format = Image::ERawImageFormat::RGBA8,
				.GammaSpace = SourceMips.front().GetInfo().GammaSpace}, std::move(NextMip.Pixels));
			if (!ImageResult1)
			{
				OutPlatformData = {};
				return {ETexture2DBuildStatus::Failed, {.Code = ETexture2DBuildError::InvalidMipLayout}};
			}
			auto FrozenMip = std::move(*ImageResult1);
			UncompressedMips.push_back(std::move(FrozenMip));
		}
		const FClock::time_point MipFinish = FClock::now();
		if (ExecutionControl && ExecutionControl->Metrics)
		{
			FBuildMipChainMetrics& Metrics = *ExecutionControl->Metrics;
			Metrics.MipGenerationNanoseconds = static_cast<uint64>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(MipFinish - MipStart).count());
			// Shared source bytes are already accounted as decoded input by the caller.
			for (size_t Index = SourceMips.size(); Index < UncompressedMips.size(); ++Index)
				Metrics.PeakIntermediateBytes += UncompressedMips[Index].GetPixels().size();
		}
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
			if (IsCancelled())
			{
				OutPlatformData = {};
				return {ETexture2DBuildStatus::Cancelled, {.Code = ETexture2DBuildError::Cancelled}};
			}
			FTexture2DMipData& CompressedMip = OutPlatformData.Mips.emplace_back();
			const FTexture2DBuildResult CompressionResult = CompressTextureMip(
				UncompressedMips[MipIndex], OutPlatformData.PixelFormat,
				CompressionQuality, CompressedMip, ExecutionControl);
			if (!CompressionResult)
			{
				OutPlatformData = {};
				return CompressionResult;
			}
		}
		if (ExecutionControl && ExecutionControl->Metrics)
		{
			ExecutionControl->Metrics->CompressionNanoseconds = static_cast<uint64>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					FClock::now() - CompressionStart).count());
		}
		if (OutPlatformData.IsValid())
		{
			return {ETexture2DBuildStatus::Succeeded, {}};
		}
		OutPlatformData = {};
		return {ETexture2DBuildStatus::Failed,
			{.Code = ETexture2DBuildError::InvalidPlatformData}};
	}
}
