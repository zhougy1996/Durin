#include "VolumeTexturePreview.h"

namespace Durin::Editor::Texture
{
	namespace
	{
		auto Extract(std::span<const uint8> Voxels, uint32 Width, uint32 Height,
			uint32 Depth, uint32 RowPitch, uint32 DepthPitch,
			uint32 BytesPerVoxel, EVolumeTexturePreviewAxis Axis, uint32 SliceIndex)
			-> FVolumeTexturePreviewSlice
		{
			FVolumeTexturePreviewSlice Result;
			const uint32 SliceCount = Axis == EVolumeTexturePreviewAxis::XY ? Depth
				: Axis == EVolumeTexturePreviewAxis::XZ ? Height : Width;
			if (SliceCount == 0) return Result;
			SliceIndex = std::min(SliceIndex, SliceCount - 1);
			Result.Width = Axis == EVolumeTexturePreviewAxis::YZ ? Height : Width;
			Result.Height = Axis == EVolumeTexturePreviewAxis::XY ? Height : Depth;
			const uint64 OutputBytes = static_cast<uint64>(Result.Width) * Result.Height * 4;
			if (OutputBytes > 16ull * 1024ull * 1024ull) return {};
			Result.Pixels.resize(static_cast<size_t>(OutputBytes));
			for (uint32 V = 0; V < Result.Height; ++V)
			{
				for (uint32 U = 0; U < Result.Width; ++U)
				{
					const uint32 X = Axis == EVolumeTexturePreviewAxis::YZ ? SliceIndex : U;
					const uint32 Y = Axis == EVolumeTexturePreviewAxis::XY ? V
						: Axis == EVolumeTexturePreviewAxis::XZ ? SliceIndex : U;
					const uint32 Z = Axis == EVolumeTexturePreviewAxis::XY ? SliceIndex : V;
					const uint64 SourceOffset = static_cast<uint64>(Z) * DepthPitch
						+ static_cast<uint64>(Y) * RowPitch
						+ static_cast<uint64>(X) * BytesPerVoxel;
					const size_t DestinationOffset =
						(static_cast<size_t>(V) * Result.Width + U) * 4;
					if (BytesPerVoxel == 1)
					{
						const uint8 Value = Voxels[static_cast<size_t>(SourceOffset)];
						Result.Pixels[DestinationOffset + 0] = Value;
						Result.Pixels[DestinationOffset + 1] = Value;
						Result.Pixels[DestinationOffset + 2] = Value;
						Result.Pixels[DestinationOffset + 3] = 255;
					}
					else
					{
						std::copy_n(Voxels.data() + SourceOffset, 4,
							Result.Pixels.data() + DestinationOffset);
					}
				}
			}
			return Result;
		}
	}

	auto ExtractVolumeTexturePreviewSlice(const FVolumeTextureMipData& Mip,
		EPixelFormat Format, EVolumeTexturePreviewAxis Axis, uint32 SliceIndex)
		-> FVolumeTexturePreviewSlice
	{
		FVolumeTexturePreviewSlice Result;
		if (!Mip.IsValid(Format)
			|| (Format != EPixelFormat::R8_UNORM
				&& Format != EPixelFormat::RGBA8_UNORM)) return Result;
		const uint32 BytesPerVoxel = Format == EPixelFormat::R8_UNORM ? 1u : 4u;
		return Extract(Mip.Voxels, Mip.Width, Mip.Height, Mip.Depth,
			Mip.RowPitch, Mip.DepthPitch, BytesPerVoxel, Axis, SliceIndex);
	}

	auto ExtractVolumeTexturePreviewSlice(const FVolumeTextureSourceData& Source,
		EVolumeTexturePreviewAxis Axis, uint32 SliceIndex)
		-> FVolumeTexturePreviewSlice
	{
		if (!Source.IsValid()
			|| (Source.Format != EVolumeTextureFormat::R8_UNORM
				&& Source.Format != EVolumeTextureFormat::RGBA8_UNORM)) return {};
		const uint32 BytesPerVoxel = Source.Format == EVolumeTextureFormat::R8_UNORM
			? 1u : 4u;
		const auto Bytes = Source.GetVoxelBytes();
		return Extract({reinterpret_cast<const uint8*>(Bytes.data()), Bytes.size()},
			Source.Width, Source.Height, Source.Depth,
			Source.Width * BytesPerVoxel,
			Source.Width * Source.Height * BytesPerVoxel,
			BytesPerVoxel, Axis, SliceIndex);
	}
}
