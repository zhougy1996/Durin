#include "Texture/TextureBCEncoder.h"

#include "Threading/Task.h"
#include <bc7enc.h>
#include <rgbcx.h>

namespace Durin::TextureBuilder
{
	namespace
	{
		constexpr uint32 BlockWidth = 4;

		auto IsCancellationRequested(const FBuildExecutionControl* Control) -> bool
		{
			return Control && Control->ShouldCancel && Control->ShouldCancel();
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
	}

	auto CompressTextureMip(const FReadOnlyMip& Source, EPixelFormat Format,
		ETextureCompressionQuality Quality,
		FTexture2DMipData& OutMip,
		const FBuildExecutionControl* ExecutionControl) -> std::expected<void, FTexture2DBuildError>
	{
		const FPixelFormatLayout Layout = GetPixelFormatLayout(Format, Source.Width, Source.Height);
		if (Layout.DataSize == 0 || Layout.RowPitch > std::numeric_limits<uint32>::max()
			|| Layout.DataSize > std::numeric_limits<size_t>::max())
		{
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::CompressedLayoutOverflow});
		}

		if (Format != EPixelFormat::BC1_UNORM && Format != EPixelFormat::BC1_UNORM_SRGB
			&& Format != EPixelFormat::BC3_UNORM && Format != EPixelFormat::BC3_UNORM_SRGB
			&& Format != EPixelFormat::BC5_UNORM && Format != EPixelFormat::BC7_UNORM
			&& Format != EPixelFormat::BC7_UNORM_SRGB)
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::UnsupportedPixelFormat});

		static std::once_flag EncoderInitFlag;
		std::call_once(EncoderInitFlag, [] {
			rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
			bc7enc_compress_block_init();
		});

		OutMip.Width = Source.Width;
		OutMip.Height = Source.Height;
		OutMip.RowPitch = static_cast<uint32>(Layout.RowPitch);
		FByteBuffer Pixels(static_cast<size_t>(Layout.DataSize));

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
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::InvalidInput,
				.InputCause = FTexture2DInputError{.Code = ETexture2DInputError::InvalidCompressionQuality,
					.Settings = {.CompressionQuality = Quality}}});
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
				uint8* DestBlock = reinterpret_cast<uint8*>(Pixels.data())
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
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::Cancelled});
		if (Compression.State != ETaskState::Succeeded)
			return std::unexpected(FTexture2DBuildError{.Code = ETexture2DBuildError::CompressionTaskFailed, .TaskState = Compression.State});
		OutMip.Pixels = FSharedByteBuffer::Take(std::move(Pixels));
		return {};
	}

}
