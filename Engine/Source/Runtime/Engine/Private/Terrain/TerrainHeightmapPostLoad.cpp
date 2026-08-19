#include "Terrain/TerrainHeightmapPostLoad.h"

#include "Modules/ModularFeatureInvocation.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view TerrainAuthoringAmbiguousMessage =
			"TerrainHeightmap authoring capability is ambiguous.";
		constexpr std::string_view TerrainAuthoringVisitorFailedMessage =
			"TerrainHeightmap authoring provider failed.";
	}

	auto WaitForTerrainHeightmapAuthoringLoad(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<ITerrainHeightmapAuthoringFeature>(
			[&](ITerrainHeightmapAuthoringFeature& Feature) {
				return Feature.WaitForAuthoringLoad(Heightmap, OutError);
			},
			{
				.Unavailable = "No TerrainHeightmap authoring-load wait policy is registered.",
				.Ambiguous = TerrainAuthoringAmbiguousMessage,
				.VisitorFailed = TerrainAuthoringVisitorFailedMessage},
			OutError);
	}

	auto InvokeTerrainHeightmapUncookedPostLoadHandler(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<ITerrainHeightmapAuthoringFeature>(
			[&](ITerrainHeightmapAuthoringFeature& Feature) {
				return Feature.PostLoadUncooked(Heightmap, OutError);
			},
			{
				.Unavailable = "No uncooked TerrainHeightmap load policy is registered.",
				.Ambiguous = TerrainAuthoringAmbiguousMessage,
				.VisitorFailed = TerrainAuthoringVisitorFailedMessage},
			OutError);
	}

	auto InvokeTerrainHeightmapSourceChangeHandler(
		DTerrainHeightmap& Heightmap,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<ITerrainHeightmapAuthoringFeature>(
			[&](ITerrainHeightmapAuthoringFeature& Feature) {
				return Feature.ChangeSourceReference(Heightmap, SourceVirtualPath, OutError);
			},
			{
				.Unavailable = "No TerrainHeightmap source-change policy is registered.",
				.Ambiguous = TerrainAuthoringAmbiguousMessage,
				.VisitorFailed = TerrainAuthoringVisitorFailedMessage},
			OutError);
	}
}
