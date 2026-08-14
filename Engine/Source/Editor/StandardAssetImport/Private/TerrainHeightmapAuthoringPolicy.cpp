#include "TerrainHeightmapAuthoringPolicy.h"

#include "EncodedSourceSnapshot.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Terrain/TerrainHeightmapPostLoad.h"
#include "TerrainHeightmapBuildAdapter.h"
#include "TerrainHeightmapSourceTranslation.h"

namespace Durin::Asset::Import::Standard
{
	namespace
	{
		bool GTerrainHeightmapAuthoringPolicyRegistered = false;

		auto PostLoadTerrainHeightmap(
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
		{
			std::string Key = Asset::Build::MakeTerrainHeightmapDerivedDataKey(Heightmap, OutError);
			if (!Key.empty())
			{
				std::shared_ptr<const FTerrainHeightmapPayload> Payload;
				if (Asset::Build::LoadTerrainHeightmapDerivedData(Key, Payload, OutError))
				{
					const auto& Source = Heightmap.GetSourceImportData();
					Heightmap.PublishAuthoringCandidate(Source, 0, 0, std::move(Payload),
						std::move(Key), "Loaded terrain heightmap payload from DDC.",
						false, false, true);
					return true;
				}
			}
			FEncodedSourceSnapshot Snapshot;
			if (!CaptureEncodedSource(Heightmap.GetSourceImportData().SourcePath,
				Snapshot, OutError, MaximumTerrainHeightmapEncodedBytes)) return false;
			FTerrainHeightmapSourceData SourceData;
			return TranslateTerrainHeightmapSource(
				std::filesystem::path(Snapshot.SourcePath.Path).extension().generic_string(),
				Snapshot.GetBytes(), SourceData, OutError)
				&& BuildTerrainHeightmapFromSource(Heightmap, std::move(SourceData), Snapshot,
					OutError, false, false);
		}
	}

	auto RegisterTerrainHeightmapAuthoringPolicy() -> bool
	{
		if (GTerrainHeightmapAuthoringPolicyRegistered) return true;
		if (!RegisterTerrainHeightmapUncookedPostLoadHandler(PostLoadTerrainHeightmap))
			return false;
		if (!RegisterTerrainHeightmapSourceChangeHandler(
			ChangeTerrainHeightmapSourceReference))
		{
			UnregisterTerrainHeightmapUncookedPostLoadHandler();
			return false;
		}
		GTerrainHeightmapAuthoringPolicyRegistered = true;
		return true;
	}

	auto UnregisterTerrainHeightmapAuthoringPolicy() -> void
	{
		if (!GTerrainHeightmapAuthoringPolicyRegistered) return;
		UnregisterTerrainHeightmapSourceChangeHandler();
		UnregisterTerrainHeightmapUncookedPostLoadHandler();
		GTerrainHeightmapAuthoringPolicyRegistered = false;
	}
}
