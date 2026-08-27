#include "Terrain/TerrainHeightmapPostLoad.h"

#include "Modules/ModularFeatureInvocation.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view TerrainDerivedDataLoadAmbiguousMessage =
			"TerrainHeightmap derived-data load capability is ambiguous.";
		constexpr std::string_view TerrainDerivedDataLoadVisitorFailedMessage =
			"TerrainHeightmap derived-data load provider failed.";
	}

	auto WaitForTerrainHeightmapDerivedDataLoad(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<ITerrainHeightmapDerivedDataLoadFeature>(
			[&](ITerrainHeightmapDerivedDataLoadFeature& Feature) {
				return Feature.WaitForDerivedDataLoad(Heightmap, OutError);
			},
			{
				.Unavailable = "No TerrainHeightmap derived-data load wait policy is registered.",
				.Ambiguous = TerrainDerivedDataLoadAmbiguousMessage,
				.VisitorFailed = TerrainDerivedDataLoadVisitorFailedMessage},
			OutError);
	}

	auto InvokeTerrainHeightmapUncookedPostLoadHandler(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<ITerrainHeightmapDerivedDataLoadFeature>(
			[&](ITerrainHeightmapDerivedDataLoadFeature& Feature) {
				return Feature.PostLoadUncooked(Heightmap, OutError);
			},
			{
				.Unavailable = "No uncooked TerrainHeightmap load policy is registered.",
				.Ambiguous = TerrainDerivedDataLoadAmbiguousMessage,
				.VisitorFailed = TerrainDerivedDataLoadVisitorFailedMessage},
			OutError);
	}

}
