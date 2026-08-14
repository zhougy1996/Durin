#include "Modules/ModuleManager.h"
#include "StandardAssetAuthoringFeatures.h"
#include "StandardAssetImportProviders.h"
#include "StandardTerrainAuthoringFeature.h"

namespace Durin
{
	class FStandardAssetImportModule final : public IModuleInterface
	{
	public:
		auto StartupModule(FModuleContext& Context) -> void override
		{
			StaticMeshRegistration = Context.RegisterFeature<IStaticMeshAuthoringFeature>(AuthoringFeatures);
			Texture2DRegistration = Context.RegisterFeature<ITexture2DAuthoringFeature>(AuthoringFeatures);
			TextureCubeRegistration = Context.RegisterFeature<ITextureCubeAuthoringFeature>(AuthoringFeatures);
			require(StaticMeshRegistration.IsValid());
			require(Texture2DRegistration.IsValid());
			require(TextureCubeRegistration.IsValid());
			TerrainFeatures = std::make_unique<Asset::Import::Standard::FStandardTerrainAuthoringFeature>();
			require(TerrainFeatures->SetOperationGroup(
				Context.CreateAsyncOperationGroup("TerrainAuthoringLoads")));
			TerrainRegistration = Context.RegisterFeature<ITerrainHeightmapAuthoringFeature>(*TerrainFeatures);
			require(TerrainRegistration.IsValid());
			std::string Error;
			requiref(Asset::Import::Standard::RegisterStandardAssetImportProviders(Error), "{}", Error);
		}

		auto ShutdownModule(FModuleShutdownContext&) -> void override
		{
			TerrainFeatures->Shutdown();
			Asset::Import::Standard::UnregisterStandardAssetImportProviders();
		}

	private:
		Asset::Import::Standard::FStandardAssetAuthoringFeatures AuthoringFeatures;
		FModularFeatureRegistration StaticMeshRegistration;
		FModularFeatureRegistration Texture2DRegistration;
		FModularFeatureRegistration TextureCubeRegistration;
		std::unique_ptr<Asset::Import::Standard::FStandardTerrainAuthoringFeature> TerrainFeatures;
		FModularFeatureRegistration TerrainRegistration;
	};

	IMPLEMENT_MODULE(FStandardAssetImportModule, StandardAssetImport)
}
