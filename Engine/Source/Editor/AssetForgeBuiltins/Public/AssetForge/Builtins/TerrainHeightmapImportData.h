#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"
#include "Terrain/TerrainHeightmap.h"

#include "TerrainHeightmapImportData.gen.h"

namespace Durin::AssetForge::Builtins
{
	struct FTerrainHeightmapImportDataState : AssetImport::FAssetImportDataState
	{
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		ETerrainHeightmapSourceFormat SourceFormat =
			ETerrainHeightmapSourceFormat::Unknown;
		uint32 SourceProfileVersion = 0;

		auto operator==(const FTerrainHeightmapImportDataState&) const -> bool = default;
	};

	DCLASS()
	class DTerrainHeightmapImportData final : public AssetImport::DAssetImportData
	{
		GENERATED_BODY()

	public:
		ASSETFORGEBUILTINS_API explicit DTerrainHeightmapImportData(
			const FObjectInitializer& ObjectInitializer);
		auto GetDecoderId() const -> std::string_view { return DecoderId; }
		auto GetDecoderVersion() const -> uint32 { return DecoderVersion; }
		auto GetSourceFormat() const -> ETerrainHeightmapSourceFormat
		{
			return SourceFormat;
		}
		auto GetSourceProfileVersion() const -> uint32 { return SourceProfileVersion; }
		ASSETFORGEBUILTINS_API auto SetState(
			FTerrainHeightmapImportDataState State, std::string& OutError) -> bool;
		ASSETFORGEBUILTINS_API auto GetTerrainHeightmapState() const
			-> FTerrainHeightmapImportDataState;
		auto GetState() const -> AssetImport::FAssetImportDataState override
		{
			return GetTerrainHeightmapState();
		}
		ASSETFORGEBUILTINS_API auto Validate(std::string& OutError) const
			-> bool override;
		ASSETFORGEBUILTINS_API auto CloneToOwner(
			DObject* Owner, FName Name, std::string& OutError) const
			-> AssetImport::DAssetImportData* override;

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
