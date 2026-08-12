#include "Modules/ModuleManager.h"
#include "StaticMesh/StaticMeshAuthoring.h"
#include "StaticMesh/StaticMeshBuildOperations.h"
#include "Texture/Texture2DAuthoringCoordinator.h"

namespace Durin
{
	class FEngineAssetBuildModule final : public IModuleInterface
	{
		static auto BuildStaticMeshCollision(
			const FStaticMeshRenderData& RenderData,
			const FStaticMeshSourceImportData& SourceImportData,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			FStaticMeshCollisionAuthoringProduct& OutProduct,
			std::string& OutError) -> bool
		{
			return AssetBuild::FStaticMeshBuildOperations::BuildCollisionProduct(
				RenderData, SourceImportData, Mode, Policy, OutProduct, OutError);
		}

		auto StartupModule() -> void override
		{
			checkf(AssetBuild::InitializeTexture2DBuildCoordinator(),
				"EngineAssetBuild could not initialize its Texture2D coordinator.");
			checkf(RegisterStaticMeshCollisionBuildHandler(
				&BuildStaticMeshCollision),
				"EngineAssetBuild could not register StaticMesh collision building.");
		}

		auto ShutdownModule() -> void override
		{
			UnregisterStaticMeshCollisionBuildHandler();
			AssetBuild::ShutdownTexture2DBuildCoordinator();
		}
	};

	IMPLEMENT_MODULE(FEngineAssetBuildModule, EngineAssetBuild)
}
