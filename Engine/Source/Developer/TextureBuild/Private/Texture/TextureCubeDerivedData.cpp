#include "Texture/TextureCubeDerivedData.h"

#include "Serialization/Archive.h"

namespace Durin::Asset
{
	namespace
	{
		auto IsSupportedTarget(
			Asset::ECookTargetPlatform Platform,
			Asset::ECookTargetProfile Profile) -> bool
		{
			return Platform == Asset::ECookTargetPlatform::Win64
				&& (Profile == Asset::ECookTargetProfile::Game
					|| Profile == Asset::ECookTargetProfile::EditorValidation);
		}
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

	auto BuildTextureCubeDerivedDataKeyBytes(
		const FTextureCubeBuildKeyInput& Input,
		std::string& OutError) -> std::vector<std::byte>
	{
		std::vector<std::byte> Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
		const_cast<FTextureCubeBuildKeyInput&>(Input).Serialize(Ar);
		OutError = Ar.HasError() ? Ar.GetFailure()->Message : std::string{};
		if (Ar.HasError()) Bytes.clear();
		return Bytes;
	}

	auto BuildTextureCubeDerivedDataKey(
		const FTextureCubeBuildKeyInput& Input,
		std::string& OutError) -> std::string
	{
		const std::vector<std::byte> Bytes = BuildTextureCubeDerivedDataKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}
}
