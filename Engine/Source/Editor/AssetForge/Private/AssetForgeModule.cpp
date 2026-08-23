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
			Texture2DRecoveryRegistration = FModuleStartup::RegisterFeature<
				ITexture2DInterchangeRecoveryFeature>(AuthoringFeatures);
			TextureCubeRegistration = FModuleStartup::RegisterFeature<ITextureCubeAuthoringFeature>(AuthoringFeatures);
			VolumeTextureRegistration = FModuleStartup::RegisterFeature<
				IVolumeTextureInterchangeRecoveryFeature>(AuthoringFeatures);
			require(StaticMeshRegistration.IsValid());
			require(Texture2DRegistration.IsValid());
			require(Texture2DRecoveryRegistration.IsValid());
			require(TextureCubeRegistration.IsValid());
			require(VolumeTextureRegistration.IsValid());
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
		FModularFeatureRegistration Texture2DRecoveryRegistration;
		FModularFeatureRegistration TextureCubeRegistration;
		FModularFeatureRegistration VolumeTextureRegistration;
		std::unique_ptr<Asset::Forge::FTerrainAuthoringFeature> TerrainFeatures;
		FModularFeatureRegistration TerrainRegistration;
		FModuleOwnedCallbackRegistration ImportRegistryCallbackRegistration;
	};

	IMPLEMENT_MODULE(FAssetForgeModule, AssetForge)
}
