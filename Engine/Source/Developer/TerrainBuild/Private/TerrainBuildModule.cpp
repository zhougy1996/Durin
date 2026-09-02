#include "Modules/ModuleManager.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapBuildProvider.h"
#include "Terrain/TerrainWorldTile.h"

namespace Durin
{
	// Owns pure Terrain recipe providers for the loaded module generation.
	class FTerrainBuildModule final
		: public IModuleInterface
		, public ITerrainHeightmapBuildProvider
		, public ITerrainWorldBuildProvider
	{
		FModularFeatureRegistration HeightmapProviderRegistration;
		FModularFeatureRegistration WorldProviderRegistration;

		auto GetHeightmapDescriptor() const
			-> FTerrainHeightmapBuildProviderDescriptor override
		{
			return {.ProducerIdentity = "canonical-u16",
				.ProducerVersion = TerrainHeightmapImportedDataSchemaVersion};
		}

		auto Build(FTerrainHeightmapRecipeRequest Request,
			FTerrainHeightmapRecipeProduct& OutProduct,
			std::string& OutError) -> bool override
		{
			OutProduct = {};
			if (Request.ShouldCancel && Request.ShouldCancel())
			{
				OutError = "Terrain heightmap build was canceled.";
				return false;
			}
			if (!BuildTerrainHeightmapPayload(Request.Width, Request.Height,
				Request.Samples, OutProduct.Payload, OutError)) return false;
			if (Request.ShouldCancel && Request.ShouldCancel())
			{
				OutProduct = {};
				OutError = "Terrain heightmap build was canceled.";
				return false;
			}
			return true;
		}

		auto GetTerrainWorldDescriptor() const
			-> FTerrainWorldBuildProviderDescriptor override
		{
			return {.ProducerIdentity = "Durin.TerrainWorld",
				.BuilderVersion = TerrainWorldBuilderVersion,
				.ProductSchemaVersion = TerrainWorldSchemaVersion};
		}

		auto Build(FTerrainWorldRecipeRequest Request,
			FTerrainWorldRecipeProduct& OutProduct,
			ETerrainWorldOutcome& OutOutcome,
			std::string& OutError) -> bool override
		{
			OutProduct = {};
			return BuildTerrainWorldRecipe(
				std::move(Request), OutProduct, OutOutcome, OutError);
		}

		auto StartupModule() -> void override
		{
			HeightmapProviderRegistration = FModuleStartup::RegisterFeature<
				ITerrainHeightmapBuildProvider>(*this);
			checkf(HeightmapProviderRegistration.IsValid(),
				"TerrainBuild could not register its typed heightmap provider.");
			WorldProviderRegistration = FModuleStartup::RegisterFeature<
				ITerrainWorldBuildProvider>(*this);
			checkf(WorldProviderRegistration.IsValid(),
				"TerrainBuild could not register its typed world provider.");
		}

		auto ShutdownModule() -> void override
		{
			WorldProviderRegistration.Reset();
			HeightmapProviderRegistration.Reset();
		}
	};

	IMPLEMENT_MODULE(FTerrainBuildModule, TerrainBuild)
}
