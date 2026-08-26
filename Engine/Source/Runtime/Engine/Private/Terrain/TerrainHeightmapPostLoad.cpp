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

	auto InvokeTerrainHeightmapSourceChangeHandler(
		DTerrainHeightmap& Heightmap,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		return Private::InvokeSingleModularFeature<ITerrainHeightmapSourceMutationFeature>(
			[&](ITerrainHeightmapSourceMutationFeature& Feature) {
				return Feature.ChangeSourceReference(Heightmap, SourceVirtualPath, OutError);
			},
			{
				.Unavailable = "No TerrainHeightmap source-change policy is registered.",
				.Ambiguous = "TerrainHeightmap source mutation capability is ambiguous.",
				.VisitorFailed = "TerrainHeightmap source mutation provider failed."},
			OutError);
	}
}
