#include "Texture/TextureDerivedData.h"

#include "Serialization/Archive.h"
#include "Serialization/BoundedPayloadSerialization.h"
#include "Texture/TexturePayloadContainer.h"
#include "Texture/VolumeTexture.h"

namespace Durin
{
	namespace
	{
		auto FailVolume(std::string& OutError, std::string Message) -> bool
		{
			OutError = std::move(Message);
			return false;
		}

		auto IsVolumeTargetSupported(Asset::ECookTargetPlatform Platform,
			Asset::ECookTargetProfile Profile) -> bool
		{
			return Platform == Asset::ECookTargetPlatform::Win64
				&& (Profile == Asset::ECookTargetProfile::Game
					|| Profile == Asset::ECookTargetProfile::EditorValidation);
		}

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

	auto BuildVolumeTextureSerializedValue(
		const FVolumeTexturePlatformData& PlatformData,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		if (!IsVolumeTargetSupported(TargetPlatform, TargetProfile))
			return FailVolume(OutError, "Volume texture payload target is unsupported.");
		if (!PlatformData.IsValid() || PlatformData.Mips.size() > MaximumTextureMipCount)
			return FailVolume(OutError, "Volume texture payload requires a valid complete mip chain.");
		ETextureStablePixelFormat StableFormat;
		if (!ToVolumeStableFormat(PlatformData.PixelFormat, StableFormat))
			return FailVolume(OutError, "Volume texture format has no stable identifier.");

		std::vector<TexturePayloadContainer::FBuildRecord> Records;
		Records.reserve(PlatformData.Mips.size());
		for (uint32 MipIndex = 0; MipIndex < PlatformData.Mips.size(); ++MipIndex)
		{
			const FVolumeTextureMipData& Mip = PlatformData.Mips[MipIndex];
			Records.push_back({
				.Record = {
					.Coordinate = Mip.Depth,
					.MipIndex = MipIndex,
					.Width = Mip.Width,
					.Height = Mip.Height,
					.RowPitch = Mip.RowPitch,
					.LayerPitch = Mip.DepthPitch},
				.Data = std::span<const uint8>(Mip.Voxels)});
		}
		return TexturePayloadContainer::Build({
			.ProducerVersion = VolumeTextureBuilderVersion,
			.TargetPlatform = TargetPlatform,
			.TargetProfile = TargetProfile,
			.Dimension = ETexturePayloadDimension::Texture3D,
			.StableFormat = StableFormat,
			.SliceCount = 1,
			.MipCount = static_cast<uint32>(PlatformData.Mips.size())},
			Records, OutBytes, OutError);
	}

	auto ParseVolumeTextureSerializedValue(
		std::span<const uint8> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		FVolumeTexturePlatformData& OutPlatformData) -> FPayloadDecodeResult
	{
		auto Reject = [](EPayloadDecodeError Code, std::string Message) {
			return FPayloadDecodeResult{Code, std::move(Message)};
		};
		if (!IsVolumeTargetSupported(ExpectedPlatform, ExpectedProfile))
			return Reject(EPayloadDecodeError::Incompatible,
				"Volume texture expected target is unsupported.");
		TexturePayloadContainer::FDecodedContainer Container;
		FPayloadDecodeResult Result = TexturePayloadContainer::Parse(
			Bytes, ExpectedPlatform, ExpectedProfile, Container);
		if (!Result) return Result;
		const TexturePayloadContainer::FDescriptor& Descriptor = Container.Descriptor;
		if (Descriptor.Dimension != ETexturePayloadDimension::Texture3D
			|| Descriptor.SliceCount != 1 || Descriptor.MipCount == 0
			|| Descriptor.MipCount > MaximumTextureMipCount
			|| Container.Records.size() != Descriptor.MipCount)
			return Reject(EPayloadDecodeError::Corrupt,
				"Volume texture payload header layout is invalid.");
		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		if (!FromVolumeStableFormat(static_cast<uint32>(Descriptor.StableFormat), PixelFormat))
			return Reject(EPayloadDecodeError::Incompatible,
				"Volume texture stable format is unsupported.");

		FVolumeTexturePlatformData Candidate;
		Candidate.PixelFormat = PixelFormat;
		for (uint32 MipIndex = 0; MipIndex < Descriptor.MipCount; ++MipIndex)
		{
			const TexturePayloadContainer::FRecord& Record = Container.Records[MipIndex];
			if (Record.MipIndex != MipIndex || Record.Width == 0 || Record.Height == 0
				|| Record.Coordinate == 0 || Record.Width > MaximumVolumeTextureDimension
				|| Record.Height > MaximumVolumeTextureDimension
				|| Record.Coordinate > MaximumVolumeTextureDimension)
				return Reject(EPayloadDecodeError::Corrupt,
					"Volume texture mip identity or dimensions are invalid.");
			if (MipIndex > 0)
			{
				const FVolumeTextureMipData& Previous = Candidate.Mips.back();
				if (Record.Width != std::max(1u, Previous.Width / 2)
					|| Record.Height != std::max(1u, Previous.Height / 2)
					|| Record.Coordinate != std::max(1u, Previous.Depth / 2))
					return Reject(EPayloadDecodeError::Corrupt,
						"Volume texture mip progression is invalid.");
			}
			const FPixelFormatLayout Slice = GetPixelFormatLayout(
				PixelFormat, Record.Width, Record.Height);
			if (Record.LayerPitch == 0 || Slice.RowPitch != Record.RowPitch
				|| Slice.DataSize != Record.LayerPitch
				|| Record.Coordinate > std::numeric_limits<uint64>::max() / Record.LayerPitch
				|| Record.ByteCount != static_cast<uint64>(Record.LayerPitch) * Record.Coordinate)
				return Reject(EPayloadDecodeError::Corrupt,
					"Volume texture mip pitches do not match its format.");
			FVolumeTextureMipData& Mip = Candidate.Mips.emplace_back();
			Mip.Width = Record.Width;
			Mip.Height = Record.Height;
			Mip.Depth = Record.Coordinate;
			Mip.RowPitch = Record.RowPitch;
			Mip.DepthPitch = Record.LayerPitch;
			const std::span<const uint8> Data = TexturePayloadContainer::GetData(Bytes, Record);
			Mip.Voxels.assign(Data.begin(), Data.end());
		}
		if (!Candidate.IsValid())
			return Reject(EPayloadDecodeError::Corrupt,
				"Volume texture payload is incomplete or has trailing data.");
		OutPlatformData = std::move(Candidate);
		return {};
	}

	auto FVolumeTexturePlatformData::Serialize(FArchive& Ar,
		const FTexturePlatformSerializationContext& Context) -> void
	{
		SerializeBoundedArchivePayload(
			Ar,
			*this,
			{MaximumTexturePayloadBytes, "Volume texture platform data"},
			[&](const FVolumeTexturePlatformData& Value,
				std::vector<uint8>& Bytes, std::string& Error) {
				return BuildVolumeTextureSerializedValue(Value,
					Context.TargetPlatform, Context.TargetProfile, Bytes, Error);
			},
			[&](std::span<const uint8> Bytes, FVolumeTexturePlatformData& Candidate) {
				return ParseVolumeTextureSerializedValue(Bytes,
					Context.TargetPlatform, Context.TargetProfile, Candidate);
			});
	}
}
