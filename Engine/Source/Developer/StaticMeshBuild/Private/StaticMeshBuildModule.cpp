#include "StaticMeshBuildFunctionRegistry.h"
#include "Modules/ModuleManager.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshBuildOperations.h"

namespace Durin
{
	// Owns StaticMesh build-function and collision-provider registration.
	class FStaticMeshBuildModule final
		: public IModuleInterface
		, public IStaticMeshCollisionBuildFeature
	{
		FModularFeatureRegistration CollisionFeatureRegistration;
		FModuleOwnedCallbackRegistration BuildFunctionCallbackRegistration;

		auto BuildCollisionProduct(
			const FStaticMeshRenderData& RenderData,
			const FStaticMeshSourceImportData& SourceImportData,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			FStaticMeshCollisionBuildProduct& OutProduct,
			std::string& OutError) -> bool override
		{
			return Asset::FStaticMeshBuildOperations::BuildCollisionProduct(
				RenderData, SourceImportData, Mode, Policy, OutProduct, OutError);
		}

		auto StartupModule() -> void override
		{
			std::string Error;
			BuildFunctionCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration(
					"StaticMeshBuild.BuildFunctions");
			checkf(Asset::InitializeStaticMeshBuildFunctions(
				BuildFunctionCallbackRegistration.GetGate(), &Error),
				"StaticMeshBuild could not register its build functions: {}", Error);
			CollisionFeatureRegistration =
				FModuleStartup::RegisterFeature<IStaticMeshCollisionBuildFeature>(*this);
			checkf(CollisionFeatureRegistration.IsValid(),
				"StaticMeshBuild could not register StaticMesh collision building.");
		}

		auto ShutdownModule() -> void override
		{
			Asset::ShutdownStaticMeshBuildFunctions();
		}
	};

	IMPLEMENT_MODULE(FStaticMeshBuildModule, StaticMeshBuild)
}
