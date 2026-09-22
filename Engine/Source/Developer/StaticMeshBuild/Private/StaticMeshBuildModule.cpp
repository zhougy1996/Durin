#include "Modules/ModuleManager.h"
#include "StaticMesh/StaticMeshBuildProvider.h"
#include "StaticMesh/StaticMeshBuildOperations.h"

namespace Durin
{
	// Retires admitted recipe calls before unloading the implementation.
	class FStaticMeshBuildModule final
		: public IModuleInterface
		, public IStaticMeshBuildProvider
	{
		FModularFeatureRegistration Registration;

		auto GetDescriptor() const -> FStaticMeshBuildProviderDescriptor override
		{
			return {.ProducerIdentity = "Durin.StaticMeshBuild",
				.RenderBuilderVersion = 4, .CollisionBuilderVersion = 2};
		}

		auto BuildRender(const FStaticMeshRecipeBuildRequest& Request,
			FStaticMeshRecipeBuildProduct& OutProduct,
			const FStaticMeshBuildExecutionControl& Control) -> std::expected<void, FStaticMeshRecipeError> override
		{
			return FStaticMeshBuildOperations::BuildRenderRecipe(Request, OutProduct, Control);
		}

		auto BuildCollision(const FStaticMeshCollisionRecipeRequest& Request,
			FStaticMeshCollisionRecipeProduct& OutProduct,
			const FStaticMeshBuildExecutionControl& Control) -> std::expected<void, FStaticMeshRecipeError> override
		{
			return FStaticMeshBuildOperations::BuildCollisionRecipe(Request, OutProduct, Control);
		}

		auto StartupModule() -> void override
		{
			Registration = FModuleStartup::RegisterFeature<IStaticMeshBuildProvider>(*this);
			checkf(Registration.IsValid(), "StaticMeshBuild could not register its recipe provider.");
		}

		auto ShutdownModule() -> void override
		{
			Registration.Reset();
		}
	};

	IMPLEMENT_MODULE(FStaticMeshBuildModule, StaticMeshBuild)
}
