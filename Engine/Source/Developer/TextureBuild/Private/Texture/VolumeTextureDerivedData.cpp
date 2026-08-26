#include "Texture/VolumeTextureDerivedData.h"

#include "Serialization/Archive.h"

namespace Durin::Asset
{
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
			|| TargetPlatform != Asset::ECookTargetPlatform::Win64
			|| (TargetProfile != Asset::ECookTargetProfile::Game
				&& TargetProfile != Asset::ECookTargetProfile::EditorValidation))
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
		Ar << KeySchema << Dimension << SourceContentHash.HashLow
			<< SourceContentHash.HashHigh << Width << Height << Depth
			<< Format << Filter << BuilderVersion << SourcePayloadSchemaVersion
			<< Platform << Profile;
	}

	auto BuildVolumeTextureDerivedDataKeyBytes(
		const FVolumeTextureBuildKeyInput& Input, std::string& OutError)
		-> std::vector<std::byte>
	{
		std::vector<std::byte> Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
		const_cast<FVolumeTextureBuildKeyInput&>(Input).Serialize(Ar);
		OutError = Ar.HasError() ? Ar.GetFailure()->Message : std::string{};
		if (Ar.HasError()) Bytes.clear();
		return Bytes;
	}

	auto BuildVolumeTextureDerivedDataKey(
		const FVolumeTextureBuildKeyInput& Input, std::string& OutError)
		-> std::string
	{
		const std::vector<std::byte> Bytes = BuildVolumeTextureDerivedDataKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}
}
