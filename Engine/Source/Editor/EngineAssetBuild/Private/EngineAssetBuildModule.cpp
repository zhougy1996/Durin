#include "Modules/ModuleManager.h"
#include "Authoring/AuthoringBuildService.h"
#include "Skeletal/SkeletalBuildOperations.h"
#include "SkeletalMesh/SkeletalAssetPostLoad.h"
#include "StaticMesh/StaticMeshAuthoring.h"
#include "StaticMesh/StaticMeshBuildOperations.h"

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
			checkf(AssetBuild::InitializeAuthoringBuildService(),
				"EngineAssetBuild could not initialize its authoring build service.");
			checkf(RegisterStaticMeshCollisionBuildHandler(
				&BuildStaticMeshCollision),
				"EngineAssetBuild could not register StaticMesh collision building.");
			checkf(RegisterSkeletalAssetUncookedPayloadLoaders(
				AssetBuild::LoadSkeletalMeshDerivedData,
				AssetBuild::LoadAnimationClipDerivedData),
				"EngineAssetBuild could not register skeletal DDC loading.");
		}

		auto ShutdownModule() -> void override
		{
			UnregisterSkeletalAssetUncookedPayloadLoaders();
			UnregisterStaticMeshCollisionBuildHandler();
			AssetBuild::ShutdownAuthoringBuildService();
		}
	};

	IMPLEMENT_MODULE(FEngineAssetBuildModule, EngineAssetBuild)
}
