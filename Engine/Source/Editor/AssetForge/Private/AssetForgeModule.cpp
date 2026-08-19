#include "Modules/ModuleManager.h"
#include "AssetForgeAuthoringFeatures.h"
#include "AssetForgeProviders.h"
#include "TerrainAuthoringFeature.h"

namespace Durin
{
	class FAssetForgeModule final : public IModuleInterface
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
			TerrainFeatures = std::make_unique<Asset::Forge::FTerrainAuthoringFeature>();
			require(TerrainFeatures->SetOperationGroup(
				FModuleStartup::CreateAsyncOperationGroup("TerrainAuthoringLoads")));
			TerrainRegistration = FModuleStartup::RegisterFeature<ITerrainHeightmapAuthoringFeature>(*TerrainFeatures);
			require(TerrainRegistration.IsValid());
			std::string Error;
			requiref(Asset::Forge::RegisterAssetForgeProviders(
				Error, ImportRegistryCallbackRegistration.GetGate()), "{}", Error);
		}

		auto ShutdownModule() -> void override
		{
			TerrainFeatures->Shutdown();
			Asset::Forge::UnregisterAssetForgeProviders();
		}

	private:
		Asset::Forge::FAssetForgeAuthoringFeatures AuthoringFeatures;
		FModularFeatureRegistration StaticMeshRegistration;
		FModularFeatureRegistration Texture2DRegistration;
		FModularFeatureRegistration TextureCubeRegistration;
		std::unique_ptr<Asset::Forge::FTerrainAuthoringFeature> TerrainFeatures;
		FModularFeatureRegistration TerrainRegistration;
		FModuleOwnedCallbackRegistration ImportRegistryCallbackRegistration;
	};

	IMPLEMENT_MODULE(FAssetForgeModule, AssetForge)
}
