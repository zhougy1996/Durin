#include "Modules/ModuleManager.h"
#include "AssetForgeBuiltinsAssetFeatures.h"
#include "AssetForgeBuiltinsProviders.h"
#include "TerrainHeightmapAssetFeatures.h"

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
			StaticMeshBuildRegistration =
				FModuleStartup::RegisterFeature<IStaticMeshBuildFeature>(AssetFeatures);
			StaticMeshPostLoadRegistration =
				FModuleStartup::RegisterFeature<IStaticMeshPostLoadFeature>(AssetFeatures);
			StaticMeshSourceMutationRegistration =
				FModuleStartup::RegisterFeature<IStaticMeshSourceMutationFeature>(AssetFeatures);
			Texture2DRegistration = FModuleStartup::RegisterFeature<ITexture2DPostLoadFeature>(AssetFeatures);
			Texture2DRecoveryRegistration = FModuleStartup::RegisterFeature<
				ITexture2DImportRecoveryFeature>(AssetFeatures);
			TextureCubeRegistration = FModuleStartup::RegisterFeature<ITextureCubePostLoadFeature>(AssetFeatures);
			VolumeTextureRegistration = FModuleStartup::RegisterFeature<
				IVolumeTextureImportRecoveryFeature>(AssetFeatures);
			AuthoringReadinessRegistration = FModuleStartup::RegisterFeature<
				IAssetAuthoringReadinessFeature>(AssetFeatures);
			require(StaticMeshBuildRegistration.IsValid());
			require(StaticMeshPostLoadRegistration.IsValid());
			require(StaticMeshSourceMutationRegistration.IsValid());
			require(Texture2DRegistration.IsValid());
			require(Texture2DRecoveryRegistration.IsValid());
			require(TextureCubeRegistration.IsValid());
			require(VolumeTextureRegistration.IsValid());
			require(AuthoringReadinessRegistration.IsValid());
			TerrainFeatures = std::make_unique<AssetForge::Builtins::FTerrainHeightmapAssetFeatures>();
			require(TerrainFeatures->SetOperationGroup(
				FModuleStartup::CreateAsyncOperationGroup("TerrainDerivedDataLoads")));
			TerrainDerivedDataLoadRegistration = FModuleStartup::RegisterFeature<
				ITerrainHeightmapDerivedDataLoadFeature>(*TerrainFeatures);
			TerrainSourceMutationRegistration = FModuleStartup::RegisterFeature<
				ITerrainHeightmapSourceMutationFeature>(*TerrainFeatures);
			require(TerrainDerivedDataLoadRegistration.IsValid());
			require(TerrainSourceMutationRegistration.IsValid());
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
		AssetForge::Builtins::FAssetForgeBuiltinsAssetFeatures AssetFeatures;
		FModularFeatureRegistration StaticMeshBuildRegistration;
		FModularFeatureRegistration StaticMeshPostLoadRegistration;
		FModularFeatureRegistration StaticMeshSourceMutationRegistration;
		FModularFeatureRegistration Texture2DRegistration;
		FModularFeatureRegistration Texture2DRecoveryRegistration;
		FModularFeatureRegistration TextureCubeRegistration;
		FModularFeatureRegistration VolumeTextureRegistration;
		FModularFeatureRegistration AuthoringReadinessRegistration;
		std::unique_ptr<AssetForge::Builtins::FTerrainHeightmapAssetFeatures> TerrainFeatures;
		FModularFeatureRegistration TerrainDerivedDataLoadRegistration;
		FModularFeatureRegistration TerrainSourceMutationRegistration;
		FModuleOwnedCallbackRegistration ImportRegistryCallbackRegistration;
	};

	IMPLEMENT_MODULE(FAssetForgeBuiltinsModule, AssetForgeBuiltins)
}
