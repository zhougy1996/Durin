#include "Modules/ModuleManager.h"
#include "AssetForgeBuiltinsAssetFeatures.h"
#include "TerrainHeightmapAssetFeatures.h"

namespace Durin
{
	class FAssetForgeBuiltinsModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			StaticMeshBuildRegistration =
				FModuleStartup::RegisterFeature<IStaticMeshBuildFeature>(AssetFeatures);
			StaticMeshPostLoadRegistration =
				FModuleStartup::RegisterFeature<IStaticMeshPostLoadFeature>(AssetFeatures);
			Texture2DRegistration = FModuleStartup::RegisterFeature<ITexture2DPostLoadFeature>(AssetFeatures);
			TextureCubeRegistration = FModuleStartup::RegisterFeature<ITextureCubePostLoadFeature>(AssetFeatures);
			SaveReadinessRegistration = FModuleStartup::RegisterFeature<
				IAssetSaveReadinessFeature>(AssetFeatures);
			require(StaticMeshBuildRegistration.IsValid());
			require(StaticMeshPostLoadRegistration.IsValid());
			require(Texture2DRegistration.IsValid());
			require(TextureCubeRegistration.IsValid());
			require(SaveReadinessRegistration.IsValid());
			TerrainFeatures = std::make_unique<AssetForge::Builtins::FTerrainHeightmapAssetFeatures>();
			require(TerrainFeatures->SetOperationGroup(
				FModuleStartup::CreateAsyncOperationGroup("TerrainDerivedDataLoads")));
			TerrainDerivedDataLoadRegistration = FModuleStartup::RegisterFeature<
				ITerrainHeightmapDerivedDataLoadFeature>(*TerrainFeatures);
			require(TerrainDerivedDataLoadRegistration.IsValid());
		}

		auto ShutdownModule() -> void override
		{
			TerrainFeatures->Shutdown();
		}

	private:
		AssetForge::Builtins::FAssetForgeBuiltinsAssetFeatures AssetFeatures;
		FModularFeatureRegistration StaticMeshBuildRegistration;
		FModularFeatureRegistration StaticMeshPostLoadRegistration;
		FModularFeatureRegistration Texture2DRegistration;
		FModularFeatureRegistration TextureCubeRegistration;
		FModularFeatureRegistration SaveReadinessRegistration;
		std::unique_ptr<AssetForge::Builtins::FTerrainHeightmapAssetFeatures> TerrainFeatures;
		FModularFeatureRegistration TerrainDerivedDataLoadRegistration;
	};

	IMPLEMENT_MODULE(FAssetForgeBuiltinsModule, AssetForgeBuiltins)
}
