#include "Terrain/TerrainHeightmapPostLoad.h"

namespace Durin
{
	namespace
	{
		template<typename F>
		auto InvokeTerrainFeature(F&& Visitor, std::string_view Unavailable, std::string& OutError) -> bool
		{
			const auto Result = FModularFeatureRegistry::Get().InvokeSingle<ITerrainHeightmapAuthoringFeature>(
				std::forward<F>(Visitor));
			if (Result.Status == EFeatureInvokeStatus::Invoked && Result.Value) return *Result.Value;
			if (Result.Status == EFeatureInvokeStatus::Unavailable) OutError = Unavailable;
			else if (Result.Status == EFeatureInvokeStatus::Ambiguous)
				OutError = "TerrainHeightmap authoring capability is ambiguous.";
			else if (Result.Status == EFeatureInvokeStatus::VisitorFailed)
				OutError = "TerrainHeightmap authoring provider failed.";
			return false;
		}
	}

	auto WaitForTerrainHeightmapAuthoringLoad(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
	{
		return InvokeTerrainFeature([&](ITerrainHeightmapAuthoringFeature& Feature) {
			return Feature.WaitForAuthoringLoad(Heightmap, OutError);
		}, "No TerrainHeightmap authoring-load wait policy is registered.", OutError);
	}

	auto InvokeTerrainHeightmapUncookedPostLoadHandler(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
	{
		return InvokeTerrainFeature([&](ITerrainHeightmapAuthoringFeature& Feature) {
			return Feature.PostLoadUncooked(Heightmap, OutError);
		}, "No uncooked TerrainHeightmap load policy is registered.", OutError);
	}

	auto InvokeTerrainHeightmapSourceChangeHandler(
		DTerrainHeightmap& Heightmap,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		return InvokeTerrainFeature([&](ITerrainHeightmapAuthoringFeature& Feature) {
			return Feature.ChangeSourceReference(Heightmap, SourceVirtualPath, OutError);
		}, "No TerrainHeightmap source-change policy is registered.", OutError);
	}
}
