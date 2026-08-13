#include "Terrain/TerrainHeightmapBuildKey.h"

#include "Serialization/Archive.h"

namespace Durin::AssetBuild
{
	auto FTerrainHeightmapBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		if (Ar.IsLoading())
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
				"Terrain heightmap build-key input is save-only.");
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
		std::string RecipeName = "Durin.TerrainHeightmap.Builder.V1";
		uint32 SampleBits = 16;
		uint32 Orientation = 1;
		uint32 EncodedPlatform = static_cast<uint32>(TargetPlatform);
		uint32 EncodedProfile = static_cast<uint32>(TargetProfile);
		uint32 BaseRegionSize = TerrainHeightmapBaseRegionSize;
		Ar << KeySchemaVersion << RecipeName
			<< SourceContentHash.HashLow << SourceContentHash.HashHigh
			<< SampleBits << Orientation << BaseRegionSize
			<< BuilderVersion << PayloadSchemaVersion << EncodedPlatform << EncodedProfile;
	}

	auto BuildTerrainHeightmapDerivedDataKeyBytes(
		const FTerrainHeightmapBuildKeyInput& Input,
		std::string& OutError) -> std::vector<uint8>
	{
		std::vector<uint8> Bytes;
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
		const std::vector<uint8> Bytes = BuildTerrainHeightmapDerivedDataKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}
}
