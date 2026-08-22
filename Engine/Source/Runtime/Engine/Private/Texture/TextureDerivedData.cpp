#include "Texture/TextureDerivedData.h"

#include "Serialization/Archive.h"
#include "Serialization/BoundedPayloadSerialization.h"
#include "Texture/TextureCube.h"
#include "Texture/TexturePayloadContainer.h"

namespace Durin
{
	namespace
	{
		auto IsSupportedTarget(Asset::ECookTargetPlatform Platform, Asset::ECookTargetProfile Profile) -> bool
		{
			return Platform == Asset::ECookTargetPlatform::Win64
				&& (Profile == Asset::ECookTargetProfile::Game
					|| Profile == Asset::ECookTargetProfile::EditorValidation);
		}

		auto ToStablePixelFormat(EPixelFormat Format, ETextureStablePixelFormat& OutFormat) -> bool
		{
			switch (Format)
			{
			case EPixelFormat::BC1_UNORM: OutFormat = ETextureStablePixelFormat::BC1_UNORM; return true;
			case EPixelFormat::BC1_UNORM_SRGB: OutFormat = ETextureStablePixelFormat::BC1_UNORM_SRGB; return true;
			case EPixelFormat::BC3_UNORM: OutFormat = ETextureStablePixelFormat::BC3_UNORM; return true;
			case EPixelFormat::BC3_UNORM_SRGB: OutFormat = ETextureStablePixelFormat::BC3_UNORM_SRGB; return true;
			case EPixelFormat::BC5_UNORM: OutFormat = ETextureStablePixelFormat::BC5_UNORM; return true;
			case EPixelFormat::BC7_UNORM: OutFormat = ETextureStablePixelFormat::BC7_UNORM; return true;
			case EPixelFormat::BC7_UNORM_SRGB: OutFormat = ETextureStablePixelFormat::BC7_UNORM_SRGB; return true;
			default: return false;
			}
		}

		auto FromStablePixelFormat(uint32 StableFormat, EPixelFormat& OutFormat) -> bool
		{
			switch (static_cast<ETextureStablePixelFormat>(StableFormat))
			{
			case ETextureStablePixelFormat::BC1_UNORM: OutFormat = EPixelFormat::BC1_UNORM; return true;
			case ETextureStablePixelFormat::BC1_UNORM_SRGB: OutFormat = EPixelFormat::BC1_UNORM_SRGB; return true;
			case ETextureStablePixelFormat::BC3_UNORM: OutFormat = EPixelFormat::BC3_UNORM; return true;
			case ETextureStablePixelFormat::BC3_UNORM_SRGB: OutFormat = EPixelFormat::BC3_UNORM_SRGB; return true;
			case ETextureStablePixelFormat::BC5_UNORM: OutFormat = EPixelFormat::BC5_UNORM; return true;
			case ETextureStablePixelFormat::BC7_UNORM: OutFormat = EPixelFormat::BC7_UNORM; return true;
			case ETextureStablePixelFormat::BC7_UNORM_SRGB: OutFormat = EPixelFormat::BC7_UNORM_SRGB; return true;
			default: return false;
			}
		}

		auto IsCompleteMipChain(const FTexturePlatformData& PlatformData) -> bool
		{
			return PlatformData.IsValid()
				&& PlatformData.Mips.size() <= MaximumTextureMipCount
				&& PlatformData.Mips.front().Width <= MaximumTexture2DDimension
				&& PlatformData.Mips.front().Height <= MaximumTexture2DDimension
				&& PlatformData.Mips.back().Width == 1
				&& PlatformData.Mips.back().Height == 1;
		}

		auto IsCompleteCubeMipChain(const FTextureCubePlatformData& PlatformData) -> bool
		{
			if (!PlatformData.IsValid()) return false;
			const FTexturePlatformData& Reference = PlatformData.Faces[0];
			if (Reference.Mips.size() > MaximumTextureMipCount
				|| Reference.Mips.front().Width > MaximumTextureCubeDimension
				|| Reference.Mips.front().Height > MaximumTextureCubeDimension
				|| Reference.Mips.back().Width != 1
				|| Reference.Mips.back().Height != 1) return false;
			for (const FTexturePlatformData& Face : PlatformData.Faces)
			{
				for (size_t MipIndex = 0; MipIndex < Face.Mips.size(); ++MipIndex)
				{
					const FTexture2DMipData& Mip = Face.Mips[MipIndex];
					if (!Mip.IsValid(PlatformData.PixelFormat)) return false;
					if (MipIndex > 0)
					{
						const FTexture2DMipData& Previous = Face.Mips[MipIndex - 1];
						if (Mip.Width != std::max(Previous.Width / 2, 1u)
							|| Mip.Height != std::max(Previous.Height / 2, 1u)) return false;
					}
				}
			}
			return true;
		}

	}

	auto BuildTexture2DSerializedValue(
		const FTexturePlatformData& PlatformData,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutError.clear();
		if (!IsSupportedTarget(TargetPlatform, TargetProfile))
			return Fail("Texture payload target platform or profile is unsupported.", &OutError);
		if (!IsCompleteMipChain(PlatformData))
			return Fail("Texture payload requires a valid, complete, bounded mip chain.", &OutError);
		ETextureStablePixelFormat StableFormat;
		if (!ToStablePixelFormat(PlatformData.PixelFormat, StableFormat))
			return Fail("Texture payload pixel format has no stable serialized identifier.", &OutError);

		std::vector<TexturePayloadContainer::FBuildRecord> Records;
		Records.reserve(PlatformData.Mips.size());
		for (uint32 MipIndex = 0; MipIndex < PlatformData.Mips.size(); ++MipIndex)
		{
			const FTexture2DMipData& Mip = PlatformData.Mips[MipIndex];
			Records.push_back({
				.Record = {
					.Coordinate = 0,
					.MipIndex = MipIndex,
					.Width = Mip.Width,
					.Height = Mip.Height,
					.RowPitch = Mip.RowPitch},
				.Data = std::span<const uint8>(Mip.Pixels)});
		}
		return TexturePayloadContainer::Build({
			.ProducerVersion = Texture2DPayloadProducerVersion,
			.TargetPlatform = TargetPlatform,
			.TargetProfile = TargetProfile,
			.Dimension = ETexturePayloadDimension::Texture2D,
			.StableFormat = StableFormat,
			.SliceCount = 1,
			.MipCount = static_cast<uint32>(PlatformData.Mips.size())},
			Records, OutBytes, OutError);
	}

	auto ParseTexture2DSerializedValue(
		std::span<const uint8> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		FTexturePlatformData& OutPlatformData) -> FPayloadDecodeResult
	{
		auto Reject = [](EPayloadDecodeError Code, std::string Message) {
			return FPayloadDecodeResult{Code, std::move(Message)};
		};
		if (!IsSupportedTarget(ExpectedPlatform, ExpectedProfile))
			return Reject(EPayloadDecodeError::Incompatible,
				"Texture payload expected target is unsupported.");
		TexturePayloadContainer::FDecodedContainer Container;
		FPayloadDecodeResult Result = TexturePayloadContainer::Parse(
			Bytes, ExpectedPlatform, ExpectedProfile, Container);
		if (!Result) return Result;
		const TexturePayloadContainer::FDescriptor& Descriptor = Container.Descriptor;
		if (Descriptor.Dimension != ETexturePayloadDimension::Texture2D
			|| Descriptor.SliceCount != 1 || Descriptor.MipCount == 0
			|| Descriptor.MipCount > MaximumTextureMipCount
			|| Container.Records.size() != Descriptor.MipCount)
			return Reject(EPayloadDecodeError::Corrupt,
				"Texture2D payload header layout or counts are invalid.");

		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		if (!FromStablePixelFormat(static_cast<uint32>(Descriptor.StableFormat), PixelFormat))
			return Reject(EPayloadDecodeError::Incompatible,
				"Texture payload pixel format identifier is unsupported.");

		FTexturePlatformData Candidate;
		Candidate.PixelFormat = PixelFormat;
		Candidate.Mips.reserve(Descriptor.MipCount);
		for (uint32 MipIndex = 0; MipIndex < Descriptor.MipCount; ++MipIndex)
		{
			const TexturePayloadContainer::FRecord& Record = Container.Records[MipIndex];
			if (Record.Coordinate != 0 || Record.MipIndex != MipIndex || Record.LayerPitch != 0
				|| Record.Width == 0 || Record.Height == 0
				|| Record.Width > MaximumTexture2DDimension
				|| Record.Height > MaximumTexture2DDimension)
				return Reject(EPayloadDecodeError::Corrupt,
					"Texture payload subresource identity or dimensions are invalid.");
			if (MipIndex > 0)
			{
				const FTexture2DMipData& PreviousMip = Candidate.Mips.back();
				if (Record.Width != std::max(PreviousMip.Width / 2, 1u)
					|| Record.Height != std::max(PreviousMip.Height / 2, 1u))
					return Reject(EPayloadDecodeError::Corrupt,
						"Texture payload mip dimensions are not a complete progression.");
			}
			const FPixelFormatLayout Layout = GetPixelFormatLayout(
				PixelFormat, Record.Width, Record.Height);
			if (Record.RowPitch != Layout.RowPitch || Record.ByteCount != Layout.DataSize)
				return Reject(EPayloadDecodeError::Corrupt,
					"Texture payload subresource layout does not match its format.");

			FTexture2DMipData& Mip = Candidate.Mips.emplace_back();
			Mip.Width = Record.Width;
			Mip.Height = Record.Height;
			Mip.RowPitch = Record.RowPitch;
			const std::span<const uint8> Data = TexturePayloadContainer::GetData(Bytes, Record);
			Mip.Pixels.assign(Data.begin(), Data.end());
		}
		if (!IsCompleteMipChain(Candidate))
			return Reject(EPayloadDecodeError::Corrupt,
				"Texture payload mip chain is incomplete or invalid.");
		OutPlatformData = std::move(Candidate);
		return {};
	}

	auto FTexturePlatformData::Serialize(
		FArchive& Ar,
		const FTexturePlatformSerializationContext& Context) -> void
	{
		SerializeBoundedArchivePayload(
			Ar,
			*this,
			{MaximumTexturePayloadBytes, "Texture platform data"},
			[&](const FTexturePlatformData& Value,
				std::vector<uint8>& Bytes, std::string& Error) {
				return BuildTexture2DSerializedValue(Value,
					Context.TargetPlatform, Context.TargetProfile, Bytes, Error);
			},
			[&](std::span<const uint8> Bytes, FTexturePlatformData& Candidate) {
				return ParseTexture2DSerializedValue(Bytes,
					Context.TargetPlatform, Context.TargetProfile, Candidate);
			});
	}

	auto BuildTextureCubeSerializedValue(
		const FTextureCubePlatformData& PlatformData,
		Asset::ECookTargetPlatform TargetPlatform,
		Asset::ECookTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutError.clear();
		if (!IsSupportedTarget(TargetPlatform, TargetProfile))
			return Fail("Texture payload target platform or profile is unsupported.", &OutError);
		if (!IsCompleteCubeMipChain(PlatformData))
			return Fail("TextureCube payload requires six compatible, complete, bounded mip chains.", &OutError);
		ETextureStablePixelFormat StableFormat;
		if (!ToStablePixelFormat(PlatformData.PixelFormat, StableFormat))
			return Fail("Texture payload pixel format has no stable serialized identifier.", &OutError);

		const uint32 MipCount = static_cast<uint32>(PlatformData.Faces[0].Mips.size());
		const uint32 RecordCount = static_cast<uint32>(TextureCubeFaceCount) * MipCount;
		std::vector<TexturePayloadContainer::FBuildRecord> Records;
		Records.reserve(RecordCount);
		for (uint32 Slice = 0; Slice < TextureCubeFaceCount; ++Slice)
		{
			for (uint32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
			{
				const FTexture2DMipData& Mip = PlatformData.Faces[Slice].Mips[MipIndex];
				Records.push_back({
					.Record = {
						.Coordinate = Slice,
						.MipIndex = MipIndex,
						.Width = Mip.Width,
						.Height = Mip.Height,
						.RowPitch = Mip.RowPitch},
					.Data = std::span<const uint8>(Mip.Pixels)});
			}
		}
		return TexturePayloadContainer::Build({
			.ProducerVersion = TextureCubeBuilderVersion,
			.TargetPlatform = TargetPlatform,
			.TargetProfile = TargetProfile,
			.Dimension = ETexturePayloadDimension::TextureCube,
			.StableFormat = StableFormat,
			.SliceCount = TextureCubeFaceCount,
			.MipCount = MipCount}, Records, OutBytes, OutError);
	}

	auto ParseTextureCubeSerializedValue(
		std::span<const uint8> Bytes,
		Asset::ECookTargetPlatform ExpectedPlatform,
		Asset::ECookTargetProfile ExpectedProfile,
		FTextureCubePlatformData& OutPlatformData) -> FPayloadDecodeResult
	{
		auto Reject = [](EPayloadDecodeError Code, std::string Message) {
			return FPayloadDecodeResult{Code, std::move(Message)};
		};
		if (!IsSupportedTarget(ExpectedPlatform, ExpectedProfile))
			return Reject(EPayloadDecodeError::Incompatible,
				"Texture payload expected target is unsupported.");
		TexturePayloadContainer::FDecodedContainer Container;
		FPayloadDecodeResult Result = TexturePayloadContainer::Parse(
			Bytes, ExpectedPlatform, ExpectedProfile, Container);
		if (!Result) return Result;
		const TexturePayloadContainer::FDescriptor& Descriptor = Container.Descriptor;
		if (Descriptor.Dimension != ETexturePayloadDimension::TextureCube
			|| Descriptor.SliceCount != TextureCubeFaceCount || Descriptor.MipCount == 0
			|| Descriptor.MipCount > MaximumTextureMipCount
			|| Container.Records.size() != Descriptor.SliceCount * Descriptor.MipCount)
			return Reject(EPayloadDecodeError::Corrupt,
				"TextureCube payload header layout or counts are invalid.");

		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		if (!FromStablePixelFormat(static_cast<uint32>(Descriptor.StableFormat), PixelFormat))
			return Reject(EPayloadDecodeError::Incompatible,
				"Texture payload pixel format identifier is unsupported.");

		FTextureCubePlatformData Candidate;
		Candidate.PixelFormat = PixelFormat;
		for (uint32 RecordIndex = 0; RecordIndex < Container.Records.size(); ++RecordIndex)
		{
			const uint32 ExpectedSlice = RecordIndex / Descriptor.MipCount;
			const uint32 ExpectedMip = RecordIndex % Descriptor.MipCount;
			const TexturePayloadContainer::FRecord& Record = Container.Records[RecordIndex];
			if (Record.Coordinate != ExpectedSlice || Record.MipIndex != ExpectedMip
				|| Record.LayerPitch != 0 || Record.Width == 0 || Record.Height == 0
				|| Record.Width != Record.Height || Record.Width > MaximumTextureCubeDimension)
				return Reject(EPayloadDecodeError::Corrupt,
					"TextureCube payload subresource identity or dimensions are invalid.");

			FTexturePlatformData& Face = Candidate.Faces[ExpectedSlice];
			Face.PixelFormat = PixelFormat;
			if (ExpectedMip > 0)
			{
				const FTexture2DMipData& PreviousMip = Face.Mips.back();
				if (Record.Width != std::max(PreviousMip.Width / 2, 1u)
					|| Record.Height != std::max(PreviousMip.Height / 2, 1u))
					return Reject(EPayloadDecodeError::Corrupt,
						"TextureCube payload mip dimensions are not a complete progression.");
			}
			else if (ExpectedSlice > 0)
			{
				const FTexture2DMipData& Reference = Candidate.Faces[0].Mips[0];
				if (Record.Width != Reference.Width || Record.Height != Reference.Height)
					return Reject(EPayloadDecodeError::Corrupt,
						"TextureCube payload face dimensions do not match.");
			}
			const FPixelFormatLayout Layout = GetPixelFormatLayout(
				PixelFormat, Record.Width, Record.Height);
			if (Record.RowPitch != Layout.RowPitch || Record.ByteCount != Layout.DataSize)
				return Reject(EPayloadDecodeError::Corrupt,
					"Texture payload subresource layout does not match its format.");
			if (ExpectedSlice > 0)
			{
				const FTexture2DMipData& Reference = Candidate.Faces[0].Mips[ExpectedMip];
				if (Record.Width != Reference.Width || Record.Height != Reference.Height
					|| Record.RowPitch != Reference.RowPitch
					|| Record.ByteCount != Reference.Pixels.size())
					return Reject(EPayloadDecodeError::Corrupt,
						"TextureCube payload faces have incompatible mip layouts.");
			}

			FTexture2DMipData& Mip = Face.Mips.emplace_back();
			Mip.Width = Record.Width;
			Mip.Height = Record.Height;
			Mip.RowPitch = Record.RowPitch;
			const std::span<const uint8> Data = TexturePayloadContainer::GetData(Bytes, Record);
			Mip.Pixels.assign(Data.begin(), Data.end());
		}
		if (!IsCompleteCubeMipChain(Candidate))
			return Reject(EPayloadDecodeError::Corrupt,
				"TextureCube payload mip chains are incomplete or invalid.");
		OutPlatformData = std::move(Candidate);
		return {};
	}

	auto FTextureCubePlatformData::Serialize(
		FArchive& Ar,
		const FTexturePlatformSerializationContext& Context) -> void
	{
		SerializeBoundedArchivePayload(
			Ar,
			*this,
			{MaximumTexturePayloadBytes, "TextureCube platform data"},
			[&](const FTextureCubePlatformData& Value,
				std::vector<uint8>& Bytes, std::string& Error) {
				return BuildTextureCubeSerializedValue(Value,
					Context.TargetPlatform, Context.TargetProfile, Bytes, Error);
			},
			[&](std::span<const uint8> Bytes, FTextureCubePlatformData& Candidate) {
				return ParseTextureCubeSerializedValue(Bytes,
					Context.TargetPlatform, Context.TargetProfile, Candidate);
			});
	}
}
