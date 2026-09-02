#pragma once

#include "Terrain/TerrainHeightmapBuildProvider.h"

namespace Durin
{
	enum class ETerrainHeightmapDerivedDataOrigin : uint8
	{
		CacheHit,
		Rebuilt
	};

	struct FTerrainHeightmapDerivedDataRequest
	{
		std::vector<uint16> Samples;
		uint32 Width = 0;
		uint32 Height = 0;
		// Metadata plus lazy canonical bulk, used instead of Samples on PostLoad.
		std::optional<FTerrainHeightmapImportedData> ImportedData;
		bool bPersistDerivedData = true;
		bool bQueryDerivedData = true;
		std::function<bool()> ShouldCancel;
	};

	struct FTerrainHeightmapDerivedDataResult
	{
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		FTerrainHeightmapImportedData ImportedData;
		FXxHash128 ImportedDataIdentity;
		std::string Key;
		ETerrainHeightmapDerivedDataOrigin Origin =
			ETerrainHeightmapDerivedDataOrigin::Rebuilt;
		FTerrainHeightmapBuildProviderDescriptor Descriptor;
		uint64 CacheReadNanoseconds = 0;
		uint64 CacheWriteNanoseconds = 0;
		uint64 PayloadBytes = 0;
		std::string Diagnostic;
	};

	ENGINE_API auto BuildTerrainHeightmapDerivedData(
		FTerrainHeightmapDerivedDataRequest Request,
		FTerrainHeightmapDerivedDataResult& OutResult,
		std::string& OutError) -> bool;
}
