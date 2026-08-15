#include "Modules/ModuleManager.h"
#include "StandardAssetAuthoringFeatures.h"
#include "StandardAssetImportProviders.h"
#include "StandardTerrainAuthoringFeature.h"

namespace Durin
{
	class FStandardAssetImportModule final : public IModuleInterface
	{
	public:
			auto StartupModule() -> void override
		{
			ImportRegistryCallbackRegistration =
					FModuleStartup::CreateOwnedCallbackRegistration("AssetImportCore.Registries");
			require(ImportRegistryCallbackRegistration.IsValid());
			StaticMeshRegistration = FModuleStartup::RegisterFeature<IStaticMeshAuthoringFeature>(AuthoringFeatures);
			Texture2DRegistration = FModuleStartup::RegisterFeature<ITexture2DAuthoringFeature>(AuthoringFeatures);
			TextureCubeRegistration = FModuleStartup::RegisterFeature<ITextureCubeAuthoringFeature>(AuthoringFeatures);
			require(StaticMeshRegistration.IsValid());
			require(Texture2DRegistration.IsValid());
			require(TextureCubeRegistration.IsValid());
			TerrainFeatures = std::make_unique<Asset::Import::Standard::FStandardTerrainAuthoringFeature>();
			require(TerrainFeatures->SetOperationGroup(
				FModuleStartup::CreateAsyncOperationGroup("TerrainAuthoringLoads")));
			TerrainRegistration = FModuleStartup::RegisterFeature<ITerrainHeightmapAuthoringFeature>(*TerrainFeatures);
			require(TerrainRegistration.IsValid());
			std::string Error;
			requiref(Asset::Import::Standard::RegisterStandardAssetImportProviders(
				Error, ImportRegistryCallbackRegistration.GetGate()), "{}", Error);
		}

		auto ShutdownModule() -> void override
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
		FModuleOwnedCallbackRegistration ImportRegistryCallbackRegistration;
	};

	IMPLEMENT_MODULE(FStandardAssetImportModule, StandardAssetImport)
}
