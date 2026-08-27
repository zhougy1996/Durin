#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"
#include "Terrain/TerrainHeightmap.h"

#include "TerrainHeightmapImportData.gen.h"

namespace Durin::AssetForge::Builtins
{
	// Loads authored packages that predate common TerrainHeightmap import data.
	DCLASS()
	class DTerrainHeightmapImportData final : public DAssetImportData
	{
		GENERATED_BODY()

	public:
		explicit DTerrainHeightmapImportData(const FObjectInitializer& ObjectInitializer)
			: Super(ObjectInitializer) {}

	private:
		DPROPERTY()
		std::string DecoderId;

		DPROPERTY()
		uint32 DecoderVersion = 0;

		DPROPERTY()
		ETerrainHeightmapSourceFormat SourceFormat = ETerrainHeightmapSourceFormat::Unknown;

		DPROPERTY()
		uint32 SourceProfileVersion = 0;
	};
}
