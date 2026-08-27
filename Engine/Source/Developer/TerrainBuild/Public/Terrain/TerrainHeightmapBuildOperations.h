#pragma once

#include "TerrainBuildAPI.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin::Asset
{
	struct FTerrainHeightmapBuildRequest
	{
		std::vector<uint16> Samples;
		uint32 Width = 0;
		uint32 Height = 0;
		uint64 SourceContentHashLow = 0;
		uint64 SourceContentHashHigh = 0;
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		ETerrainHeightmapSourceFormat SourceFormat = ETerrainHeightmapSourceFormat::Unknown;
		uint32 SourceProfileVersion = 0;
		bool bPersistDerivedData = true;
		bool bQueryDerivedData = true;
		std::function<bool()> ShouldCancel;
	};

	struct FTerrainHeightmapBuildProduct
	{
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		std::string DerivedDataKey;
		uint64 SourceContentHashLow = 0;
		uint64 SourceContentHashHigh = 0;
		std::string PersistenceDiagnostic;
	};

	struct FTerrainHeightmapPublicationContext
	{
		std::string SourceFilename;
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		ETerrainHeightmapSourceFormat SourceFormat = ETerrainHeightmapSourceFormat::Unknown;
		uint32 SourceProfileVersion = 0;
		bool bAdvanceRevision = true;
		bool bMarkPackageDirty = true;
	};

	struct FTerrainHeightmapDerivedDataLoadDiagnostics
	{
		uint64 QueryNanoseconds = 0;
		uint64 ReadNanoseconds = 0;
		uint64 DecodeNanoseconds = 0;
		bool bHit = false;
	};

	TERRAINBUILD_API auto BuildTerrainHeightmap(
		FTerrainHeightmapBuildRequest Request,
		FTerrainHeightmapBuildProduct& OutProduct,
		std::string& OutError) -> bool;
	TERRAINBUILD_API auto BuildTerrainHeightmapInto(
		DTerrainHeightmap& Heightmap,
		FTerrainHeightmapBuildRequest Request,
		const FTerrainHeightmapPublicationContext& Context,
		std::string& OutError) -> bool;
	TERRAINBUILD_API auto PublishTerrainHeightmapProduct(
		DTerrainHeightmap& Heightmap,
		FTerrainHeightmapBuildProduct Product,
		const FTerrainHeightmapPublicationContext& Context,
		std::string& OutError) -> bool;
	TERRAINBUILD_API auto MakeTerrainHeightmapDerivedDataKey(
		const FTerrainHeightmapSourceImportData& Source,
		std::string& OutError) -> std::string;
	TERRAINBUILD_API auto MakeTerrainHeightmapDerivedDataKey(
		const DTerrainHeightmap& Heightmap,
		std::string& OutError) -> std::string;
	TERRAINBUILD_API auto LoadTerrainHeightmapDerivedData(
		std::string_view Key,
		std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload,
		std::string& OutError,
		FTerrainHeightmapDerivedDataLoadDiagnostics* Diagnostics = nullptr) -> bool;

}
