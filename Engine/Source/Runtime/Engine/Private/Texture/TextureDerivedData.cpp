#include "Texture/TextureDerivedData.h"
#include "TextureDerivedDataKey.h"

#include "Serialization/Archive.h"
#include "Serialization/BoundedPayloadSerialization.h"
#include "Texture/TextureCube.h"
#include "Texture/TexturePayloadContainer.h"

#if DURIN_WITH_EDITOR
#include "DerivedDataCache/DerivedDataCache.h"
#endif

namespace Durin
{
	namespace
	{
		auto MakeDerivedDataKey(
			std::string_view BucketName, std::span<const std::byte> Bytes)
			-> FCacheKeyProxy
		{
#if DURIN_WITH_EDITOR
			return FCacheKeyProxy(DerivedData::FCacheKey::FromHash(
				DerivedData::FCacheBucket::FromString(BucketName),
				FXxHash128::HashBuffer(Bytes)));
#else
			return {};
#endif
		}

		auto IsSupportedTarget(ECookTargetPlatform Platform, ECookTargetProfile Profile) -> bool
		{
			return Platform == ECookTargetPlatform::Win64
				&& (Profile == ECookTargetProfile::Game
					|| Profile == ECookTargetProfile::EditorValidation);
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

	auto FTexture2DBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		uint32 KeySchemaVersion = TextureDerivedDataKeySchemaVersion;
		uint32 Dimension = static_cast<uint32>(ETexturePayloadDimension::Texture2D);
		uint8 EncodedUsage = static_cast<uint8>(Usage);
		uint8 EncodedSRGB = bSRGB ? 1 : 0;
		uint8 EncodedCompressionQuality = static_cast<uint8>(CompressionQuality);
		uint8 EncodedAlphaMipMode = static_cast<uint8>(AlphaMipMode);
		uint32 EncodedAlphaCoverageThreshold = std::bit_cast<uint32>(AlphaCoverageThreshold);
		uint32 EncodedTargetPlatform = static_cast<uint32>(TargetPlatform);
		uint32 EncodedTargetProfile = static_cast<uint32>(TargetProfile);
		Ar << KeySchemaVersion << Dimension
			<< ImportedDataIdentity.HashLow << ImportedDataIdentity.HashHigh
			<< EncodedUsage << EncodedSRGB << EncodedCompressionQuality << EncodedAlphaMipMode
			<< MaximumResolution << EncodedAlphaCoverageThreshold
			<< BuilderVersion << PayloadSchemaVersion
			<< EncodedTargetPlatform << EncodedTargetProfile;
		if (Ar.IsLoading())
			Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
				"Texture2D build-key input is save-only.");
	}

	auto FTextureCubeBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		if (Ar.IsLoading())
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
				"TextureCube build-key input is save-only.");
			return;
		}
		if (!IsSupportedTarget(TargetPlatform, TargetProfile))
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"TextureCube derived-data target is unsupported.");
			return;
		}
		if (!std::isfinite(ExposureEV)
			|| std::bit_cast<uint32>(ExposureEV) == 0x80000000u
			|| ExposureEV < -32.0f || ExposureEV > 32.0f)
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"TextureCube panorama exposure is not canonical.");
			return;
		}
		if (FaceDimension > MaximumTextureCubeDimension)
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded,
				"TextureCube requested face dimension exceeds the supported limit.");
			return;
		}

		uint32 KeySchemaVersion = TextureDerivedDataKeySchemaVersion;
		uint32 Dimension = static_cast<uint32>(ETexturePayloadDimension::TextureCube);
		uint32 EncodedLayout = static_cast<uint32>(SourceLayout);
		Ar << KeySchemaVersion << Dimension << EncodedLayout;
		switch (SourceLayout)
		{
		case ETextureCubeBuildSourceLayout::SixFaces:
			for (FXxHash128& Hash : FaceContentHashes)
				Ar << Hash.HashLow << Hash.HashHigh;
			break;
		case ETextureCubeBuildSourceLayout::EquirectangularPanorama:
			Ar << PanoramaContentHash.HashLow << PanoramaContentHash.HashHigh;
			{
				uint32 EncodedExposure = std::bit_cast<uint32>(ExposureEV);
				Ar << FaceDimension << EncodedExposure;
			}
			break;
		default:
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"TextureCube source layout is unsupported.");
			return;
		}
		uint8 EncodedSRGB = bSRGB ? 1 : 0;
		uint32 EncodedPlatform = static_cast<uint32>(TargetPlatform);
		uint32 EncodedProfile = static_cast<uint32>(TargetProfile);
		Ar << EncodedSRGB << BuilderVersion << PayloadSchemaVersion << ProjectionVersion
			<< EncodedPlatform << EncodedProfile;
	}

	auto FVolumeTextureBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		if (Ar.IsLoading())
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
				"Volume texture build-key input is save-only.");
			return;
		}
		if (Width == 0 || Height == 0 || Depth == 0
			|| Width > MaximumVolumeTextureDimension
			|| Height > MaximumVolumeTextureDimension
			|| Depth > MaximumVolumeTextureDimension
			|| SourcePayloadSchemaVersion != VolumeTextureSourcePayloadSchemaVersion
			|| !IsSupportedTarget(TargetPlatform, TargetProfile))
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"Volume texture derived-data key input is invalid.");
			return;
		}
		uint32 KeySchema = TextureDerivedDataKeySchemaVersion;
		uint32 Dimension = static_cast<uint32>(ETexturePayloadDimension::Texture3D);
		uint32 Format = static_cast<uint32>(Settings.OutputFormat);
		uint32 Filter = static_cast<uint32>(Settings.MipFilter);
		uint32 Platform = static_cast<uint32>(TargetPlatform);
		uint32 Profile = static_cast<uint32>(TargetProfile);
		Ar << KeySchema << Dimension << CanonicalSourceIdentity.HashLow
			<< CanonicalSourceIdentity.HashHigh << Width << Height << Depth
			<< Format << Filter << BuilderVersion << SourcePayloadSchemaVersion
			<< Platform << Profile;
	}

	auto BuildTexture2DDerivedDataKeyBytes(
		const FTexture2DBuildKeyInput& Input) -> FByteArray
	{
		FByteArray Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
		const_cast<FTexture2DBuildKeyInput&>(Input).Serialize(Ar);
		return Bytes;
	}

	auto BuildTexture2DDerivedDataKey(
		const FTexture2DBuildKeyInput& Input) -> FCacheKeyProxy
	{
		return MakeDerivedDataKey(
			Texture2DCacheBucket, BuildTexture2DDerivedDataKeyBytes(Input));
	}

	auto BuildTextureCubeDerivedDataKeyBytes(
		const FTextureCubeBuildKeyInput& Input, std::string& OutError) -> FByteArray
	{
		FByteArray Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
		const_cast<FTextureCubeBuildKeyInput&>(Input).Serialize(Ar);
		OutError = Ar.HasError() ? Ar.GetFailure()->Message : std::string{};
		if (Ar.HasError()) Bytes.clear();
		return Bytes;
	}

	auto BuildTextureCubeDerivedDataKey(
		const FTextureCubeBuildKeyInput& Input, std::string& OutError) -> FCacheKeyProxy
	{
		const FByteArray Bytes = BuildTextureCubeDerivedDataKeyBytes(Input, OutError);
		return Bytes.empty() ? FCacheKeyProxy{}
			: MakeDerivedDataKey(TextureCubeCacheBucket, Bytes);
	}

	auto BuildVolumeTextureDerivedDataKeyBytes(
		const FVolumeTextureBuildKeyInput& Input, std::string& OutError) -> FByteArray
	{
		FByteArray Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
		const_cast<FVolumeTextureBuildKeyInput&>(Input).Serialize(Ar);
		OutError = Ar.HasError() ? Ar.GetFailure()->Message : std::string{};
		if (Ar.HasError()) Bytes.clear();
		return Bytes;
	}

	auto BuildVolumeTextureDerivedDataKey(
		const FVolumeTextureBuildKeyInput& Input, std::string& OutError) -> FCacheKeyProxy
	{
		const FByteArray Bytes = BuildVolumeTextureDerivedDataKeyBytes(Input, OutError);
		return Bytes.empty() ? FCacheKeyProxy{}
			: MakeDerivedDataKey(VolumeTextureCacheBucket, Bytes);
	}

	auto BuildTexture2DSerializedValue(
		const FTexturePlatformData& PlatformData,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile,
		FByteArray& OutBytes,
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
				.Data = std::span<const std::byte>(Mip.Pixels)});
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
		std::span<const std::byte> Bytes,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FTexturePlatformData& OutPlatformData) -> FDecodeResult
	{
		auto Reject = [](EDecodeError Code, std::string Message) {
			return FDecodeResult{Code, std::move(Message)};
		};
		if (!IsSupportedTarget(ExpectedPlatform, ExpectedProfile))
			return Reject(EDecodeError::Incompatible,
				"Texture payload expected target is unsupported.");
		TexturePayloadContainer::FDecodedContainer Container;
		FDecodeResult Result = TexturePayloadContainer::Parse(
			Bytes, ExpectedPlatform, ExpectedProfile, Container);
		if (!Result) return Result;
		const TexturePayloadContainer::FDescriptor& Descriptor = Container.Descriptor;
		if (Descriptor.Dimension != ETexturePayloadDimension::Texture2D
			|| Descriptor.SliceCount != 1 || Descriptor.MipCount == 0
			|| Descriptor.MipCount > MaximumTextureMipCount
			|| Container.Records.size() != Descriptor.MipCount)
			return Reject(EDecodeError::Corrupt,
				"Texture2D payload header layout or counts are invalid.");

		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		if (!FromStablePixelFormat(static_cast<uint32>(Descriptor.StableFormat), PixelFormat))
			return Reject(EDecodeError::Incompatible,
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
				return Reject(EDecodeError::Corrupt,
					"Texture payload subresource identity or dimensions are invalid.");
			if (MipIndex > 0)
			{
				const FTexture2DMipData& PreviousMip = Candidate.Mips.back();
				if (Record.Width != std::max(PreviousMip.Width / 2, 1u)
					|| Record.Height != std::max(PreviousMip.Height / 2, 1u))
					return Reject(EDecodeError::Corrupt,
						"Texture payload mip dimensions are not a complete progression.");
			}
			const FPixelFormatLayout Layout = GetPixelFormatLayout(
				PixelFormat, Record.Width, Record.Height);
			if (Record.RowPitch != Layout.RowPitch || Record.ByteCount != Layout.DataSize)
				return Reject(EDecodeError::Corrupt,
					"Texture payload subresource layout does not match its format.");

			FTexture2DMipData& Mip = Candidate.Mips.emplace_back();
			Mip.Width = Record.Width;
			Mip.Height = Record.Height;
			Mip.RowPitch = Record.RowPitch;
			const std::span<const std::byte> Data = TexturePayloadContainer::GetData(Bytes, Record);
			Mip.Pixels.assign(Data.begin(), Data.end());
		}
		if (!IsCompleteMipChain(Candidate))
			return Reject(EDecodeError::Corrupt,
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
				FByteArray& Bytes, std::string& Error) {
				return BuildTexture2DSerializedValue(Value,
					Context.TargetPlatform, Context.TargetProfile, Bytes, Error);
			},
			[&](std::span<const std::byte> Bytes, FTexturePlatformData& Candidate) {
				return ParseTexture2DSerializedValue(Bytes,
					Context.TargetPlatform, Context.TargetProfile, Candidate);
			});
	}

	auto BuildTextureCubeSerializedValue(
		const FTextureCubePlatformData& PlatformData,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile,
		FByteArray& OutBytes,
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
					.Data = std::span<const std::byte>(Mip.Pixels)});
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
		std::span<const std::byte> Bytes,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FTextureCubePlatformData& OutPlatformData) -> FDecodeResult
	{
		auto Reject = [](EDecodeError Code, std::string Message) {
			return FDecodeResult{Code, std::move(Message)};
		};
		if (!IsSupportedTarget(ExpectedPlatform, ExpectedProfile))
			return Reject(EDecodeError::Incompatible,
				"Texture payload expected target is unsupported.");
		TexturePayloadContainer::FDecodedContainer Container;
		FDecodeResult Result = TexturePayloadContainer::Parse(
			Bytes, ExpectedPlatform, ExpectedProfile, Container);
		if (!Result) return Result;
		const TexturePayloadContainer::FDescriptor& Descriptor = Container.Descriptor;
		if (Descriptor.Dimension != ETexturePayloadDimension::TextureCube
			|| Descriptor.SliceCount != TextureCubeFaceCount || Descriptor.MipCount == 0
			|| Descriptor.MipCount > MaximumTextureMipCount
			|| Container.Records.size() != Descriptor.SliceCount * Descriptor.MipCount)
			return Reject(EDecodeError::Corrupt,
				"TextureCube payload header layout or counts are invalid.");

		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		if (!FromStablePixelFormat(static_cast<uint32>(Descriptor.StableFormat), PixelFormat))
			return Reject(EDecodeError::Incompatible,
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
				return Reject(EDecodeError::Corrupt,
					"TextureCube payload subresource identity or dimensions are invalid.");

			FTexturePlatformData& Face = Candidate.Faces[ExpectedSlice];
			Face.PixelFormat = PixelFormat;
			if (ExpectedMip > 0)
			{
				const FTexture2DMipData& PreviousMip = Face.Mips.back();
				if (Record.Width != std::max(PreviousMip.Width / 2, 1u)
					|| Record.Height != std::max(PreviousMip.Height / 2, 1u))
					return Reject(EDecodeError::Corrupt,
						"TextureCube payload mip dimensions are not a complete progression.");
			}
			else if (ExpectedSlice > 0)
			{
				const FTexture2DMipData& Reference = Candidate.Faces[0].Mips[0];
				if (Record.Width != Reference.Width || Record.Height != Reference.Height)
					return Reject(EDecodeError::Corrupt,
						"TextureCube payload face dimensions do not match.");
			}
			const FPixelFormatLayout Layout = GetPixelFormatLayout(
				PixelFormat, Record.Width, Record.Height);
			if (Record.RowPitch != Layout.RowPitch || Record.ByteCount != Layout.DataSize)
				return Reject(EDecodeError::Corrupt,
					"Texture payload subresource layout does not match its format.");
			if (ExpectedSlice > 0)
			{
				const FTexture2DMipData& Reference = Candidate.Faces[0].Mips[ExpectedMip];
				if (Record.Width != Reference.Width || Record.Height != Reference.Height
					|| Record.RowPitch != Reference.RowPitch
					|| Record.ByteCount != Reference.Pixels.size())
					return Reject(EDecodeError::Corrupt,
						"TextureCube payload faces have incompatible mip layouts.");
			}

			FTexture2DMipData& Mip = Face.Mips.emplace_back();
			Mip.Width = Record.Width;
			Mip.Height = Record.Height;
			Mip.RowPitch = Record.RowPitch;
			const std::span<const std::byte> Data = TexturePayloadContainer::GetData(Bytes, Record);
			Mip.Pixels.assign(Data.begin(), Data.end());
		}
		if (!IsCompleteCubeMipChain(Candidate))
			return Reject(EDecodeError::Corrupt,
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
				FByteArray& Bytes, std::string& Error) {
				return BuildTextureCubeSerializedValue(Value,
					Context.TargetPlatform, Context.TargetProfile, Bytes, Error);
			},
			[&](std::span<const std::byte> Bytes, FTextureCubePlatformData& Candidate) {
				return ParseTextureCubeSerializedValue(Bytes,
					Context.TargetPlatform, Context.TargetProfile, Candidate);
			});
	}
}
