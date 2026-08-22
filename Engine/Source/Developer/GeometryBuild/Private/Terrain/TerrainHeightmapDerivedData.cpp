#include "Terrain/TerrainHeightmapBuildKey.h"

#include "Serialization/Archive.h"

namespace Durin::Asset::Build
{
	auto FTerrainHeightmapBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		if (Ar.IsLoading())
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
				"Terrain heightmap build-key input is save-only.");
			return;
		}
		if (DecoderId.empty() || DecoderVersion == 0
			|| SourceFormat == ETerrainHeightmapSourceFormat::Unknown
			|| SourceProfileVersion == 0)
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"Terrain heightmap derived-data key requires an explicit decoder profile.");
			return;
		}
		if (TargetPlatform != Asset::ECookTargetPlatform::Win64
			|| (TargetProfile != Asset::ECookTargetProfile::Game
				&& TargetProfile != Asset::ECookTargetProfile::EditorValidation))
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"Terrain heightmap derived-data target is unsupported.");
			return;
		}
		uint32 KeySchemaVersion = TerrainHeightmapKeySchemaVersion;
		std::string RecipeName = "Durin.TerrainHeightmap.Builder.V2";
		uint32 SampleBits = 16;
		uint32 Orientation = 1;
		uint32 EncodedSourceFormat = static_cast<uint32>(SourceFormat);
		uint32 EncodedPlatform = static_cast<uint32>(TargetPlatform);
		uint32 EncodedProfile = static_cast<uint32>(TargetProfile);
		uint32 BaseRegionSize = TerrainHeightmapBaseRegionSize;
		Ar << KeySchemaVersion << RecipeName
			<< SourceContentHash.HashLow << SourceContentHash.HashHigh
			<< DecoderId << DecoderVersion << EncodedSourceFormat << SourceProfileVersion
			<< SampleBits << Orientation << BaseRegionSize
			<< BuilderVersion << PayloadSchemaVersion << EncodedPlatform << EncodedProfile;
	}

	auto BuildTerrainHeightmapDerivedDataKeyBytes(
		const FTerrainHeightmapBuildKeyInput& Input,
		std::string& OutError) -> std::vector<std::byte>
	{
		std::vector<std::byte> Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
		const_cast<FTerrainHeightmapBuildKeyInput&>(Input).Serialize(Ar);
		OutError = Ar.HasError() ? Ar.GetFailure()->Message : std::string{};
		if (Ar.HasError()) Bytes.clear();
		return Bytes;
	}

	auto BuildTerrainHeightmapDerivedDataKey(
		const FTerrainHeightmapBuildKeyInput& Input,
		std::string& OutError) -> std::string
	{
		const std::vector<std::byte> Bytes = BuildTerrainHeightmapDerivedDataKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}
}
