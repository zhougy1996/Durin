#include "Texture/TextureDerivedData.h"

#include "Serialization/Archive.h"
#include "Texture/TexturePayloadContainer.h"
#include "Texture/VolumeTexture.h"

namespace Durin
{
	namespace
	{
		auto ToVolumeStableFormat(EPixelFormat Format,
			ETextureStablePixelFormat& OutFormat) -> bool
		{
			switch (Format)
			{
			case EPixelFormat::R8_UNORM: OutFormat = ETextureStablePixelFormat::R8_UNORM; return true;
			case EPixelFormat::RG8_UNORM: OutFormat = ETextureStablePixelFormat::RG8_UNORM; return true;
			case EPixelFormat::RGBA8_UNORM: OutFormat = ETextureStablePixelFormat::RGBA8_UNORM; return true;
			case EPixelFormat::R16_FLOAT: OutFormat = ETextureStablePixelFormat::R16_FLOAT; return true;
			case EPixelFormat::RGBA16_FLOAT: OutFormat = ETextureStablePixelFormat::RGBA16_FLOAT; return true;
			default: return false;
			}
		}

		auto FromVolumeStableFormat(uint32 StableFormat, EPixelFormat& OutFormat) -> bool
		{
			switch (static_cast<ETextureStablePixelFormat>(StableFormat))
			{
			case ETextureStablePixelFormat::R8_UNORM: OutFormat = EPixelFormat::R8_UNORM; return true;
			case ETextureStablePixelFormat::RG8_UNORM: OutFormat = EPixelFormat::RG8_UNORM; return true;
			case ETextureStablePixelFormat::RGBA8_UNORM: OutFormat = EPixelFormat::RGBA8_UNORM; return true;
			case ETextureStablePixelFormat::R16_FLOAT: OutFormat = EPixelFormat::R16_FLOAT; return true;
			case ETextureStablePixelFormat::RGBA16_FLOAT: OutFormat = EPixelFormat::RGBA16_FLOAT; return true;
			default: return false;
			}
		}
	}

	auto FVolumeTexturePlatformData::Serialize(FArchive& Ar) -> void
	{
		TexturePayloadContainer::FTargetContext Context;
		if (!TexturePayloadContainer::ResolveContext(Ar, Context)) return;
		auto Reject = [&](EArchiveFailureCode Code, std::string_view Message) {
			Ar.Fail(Code, Message);
		};
		if (Ar.HasError()) return;
		TexturePayloadContainer::FDescriptor Descriptor{
			.ProducerVersion = VolumeTextureBuilderVersion,
			.TargetPlatform = Context.TargetPlatform,
			.TargetProfile = Context.TargetProfile,
			.Dimension = ETexturePayloadDimension::Texture3D,
			.SliceCount = 1};
		std::vector<TexturePayloadContainer::FPayloadRecord> Records;
		if (Ar.IsSaving())
		{
			if (!IsValid() || Mips.size() > MaximumTextureMipCount)
				return Reject(EArchiveFailureCode::InvalidData,
					"Volume texture payload requires a valid complete mip chain.");
			if (!ToVolumeStableFormat(PixelFormat, Descriptor.StableFormat))
				return Reject(EArchiveFailureCode::UnsupportedType,
					"Volume texture format has no stable identifier.");
			Descriptor.MipCount = static_cast<uint32>(Mips.size());
			for (uint32 Index = 0; Index < Mips.size(); ++Index)
			{
				const auto& Mip = Mips[Index];
				Records.push_back({.Record = {.Coordinate = Mip.Depth, .MipIndex = Index,
					.Width = Mip.Width, .Height = Mip.Height, .RowPitch = Mip.RowPitch,
					.LayerPitch = Mip.DepthPitch}, .Data = Mip.Voxels});
			}
		}
		TexturePayloadContainer::Serialize(Ar, Descriptor, Records);
		if (Ar.HasError() || Ar.IsSaving()) return;
		if (Descriptor.Dimension != ETexturePayloadDimension::Texture3D
			|| Descriptor.SliceCount != 1)
			return Reject(EArchiveFailureCode::InvalidData, "Volume texture payload dimension is invalid.");
		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		if (!FromVolumeStableFormat(static_cast<uint32>(Descriptor.StableFormat), PixelFormat))
			return Reject(EArchiveFailureCode::UnsupportedType,
				"Volume texture stable format is unsupported.");

		Mips.clear();
		this->PixelFormat = PixelFormat;
		for (uint32 MipIndex = 0; MipIndex < Descriptor.MipCount; ++MipIndex)
		{
			const TexturePayloadContainer::FRecord& Record = Records[MipIndex].Record;
			if (Record.MipIndex != MipIndex || Record.Width == 0 || Record.Height == 0
				|| Record.Coordinate == 0 || Record.Width > MaximumVolumeTextureDimension
				|| Record.Height > MaximumVolumeTextureDimension
				|| Record.Coordinate > MaximumVolumeTextureDimension)
				return Reject(EArchiveFailureCode::InvalidData,
					"Volume texture mip identity or dimensions are invalid.");
			if (MipIndex > 0)
			{
				const FVolumeTextureMipData& Previous = Mips.back();
				if (Record.Width != std::max(1u, Previous.Width / 2)
					|| Record.Height != std::max(1u, Previous.Height / 2)
					|| Record.Coordinate != std::max(1u, Previous.Depth / 2))
					return Reject(EArchiveFailureCode::InvalidData,
						"Volume texture mip progression is invalid.");
			}
			const FPixelFormatLayout Slice = GetPixelFormatLayout(
				PixelFormat, Record.Width, Record.Height);
			if (Record.LayerPitch == 0 || Slice.RowPitch != Record.RowPitch
				|| Slice.DataSize != Record.LayerPitch
				|| Record.Coordinate > std::numeric_limits<uint64>::max() / Record.LayerPitch
				|| Record.ByteCount != static_cast<uint64>(Record.LayerPitch) * Record.Coordinate)
				return Reject(EArchiveFailureCode::InvalidData,
					"Volume texture mip pitches do not match its format.");
			FVolumeTextureMipData& Mip = Mips.emplace_back();
			Mip.Width = Record.Width;
			Mip.Height = Record.Height;
			Mip.Depth = Record.Coordinate;
			Mip.RowPitch = Record.RowPitch;
			Mip.DepthPitch = Record.LayerPitch;
			const FByteView Data = Records[MipIndex].Data;
			Mip.Voxels.assign(Data.begin(), Data.end());
		}
		if (!IsValid())
			return Reject(EArchiveFailureCode::InvalidData,
				"Volume texture payload is incomplete or has trailing data.");

	}

}
