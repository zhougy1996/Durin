#include "Modules/ModuleManager.h"
#include "AssetForgeBuiltinsAuthoringFeatures.h"
#include "AssetForgeBuiltinsProviders.h"
#include "TerrainAuthoringFeature.h"

namespace Durin
{
	class FAssetForgeBuiltinsModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			ImportRegistryCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration("AssetForge.Registries");
			require(ImportRegistryCallbackRegistration.IsValid());
			StaticMeshRegistration = FModuleStartup::RegisterFeature<IStaticMeshAuthoringFeature>(AuthoringFeatures);
			Texture2DRegistration = FModuleStartup::RegisterFeature<ITexture2DPostLoadFeature>(AuthoringFeatures);
			Texture2DRecoveryRegistration = FModuleStartup::RegisterFeature<
				ITexture2DImportRecoveryFeature>(AuthoringFeatures);
			TextureCubeRegistration = FModuleStartup::RegisterFeature<ITextureCubeAuthoringFeature>(AuthoringFeatures);
			VolumeTextureRegistration = FModuleStartup::RegisterFeature<
				IVolumeTextureImportRecoveryFeature>(AuthoringFeatures);
			AuthoringReadinessRegistration = FModuleStartup::RegisterFeature<
				IAssetAuthoringReadinessFeature>(AuthoringFeatures);
			require(StaticMeshRegistration.IsValid());
			require(Texture2DRegistration.IsValid());
			require(Texture2DRecoveryRegistration.IsValid());
			require(TextureCubeRegistration.IsValid());
			require(VolumeTextureRegistration.IsValid());
			require(AuthoringReadinessRegistration.IsValid());
			TerrainFeatures = std::make_unique<AssetForge::Builtins::FTerrainAuthoringFeature>();
			require(TerrainFeatures->SetOperationGroup(
				FModuleStartup::CreateAsyncOperationGroup("TerrainAuthoringLoads")));
			TerrainRegistration = FModuleStartup::RegisterFeature<ITerrainHeightmapAuthoringFeature>(*TerrainFeatures);
			require(TerrainRegistration.IsValid());
			std::string Error;
			requiref(AssetForge::Builtins::RegisterAssetForgeBuiltinsProviders(
				Error, ImportRegistryCallbackRegistration.GetGate()), "{}", Error);
		}

		auto ShutdownModule() -> void override
		{
			TerrainFeatures->Shutdown();
			AssetForge::Builtins::UnregisterAssetForgeBuiltinsProviders();
		}

	private:
		AssetForge::Builtins::FAssetForgeBuiltinsAuthoringFeatures AuthoringFeatures;
		FModularFeatureRegistration StaticMeshRegistration;
		FModularFeatureRegistration Texture2DRegistration;
		FModularFeatureRegistration Texture2DRecoveryRegistration;
		FModularFeatureRegistration TextureCubeRegistration;
		FModularFeatureRegistration VolumeTextureRegistration;
		FModularFeatureRegistration AuthoringReadinessRegistration;
		std::unique_ptr<AssetForge::Builtins::FTerrainAuthoringFeature> TerrainFeatures;
		FModularFeatureRegistration TerrainRegistration;
		FModuleOwnedCallbackRegistration ImportRegistryCallbackRegistration;
	};

	IMPLEMENT_MODULE(FAssetForgeBuiltinsModule, AssetForgeBuiltins)
}
