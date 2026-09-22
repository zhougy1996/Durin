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
				.RenderBuilderVersion = 4};
		}

		auto BuildRender(const FStaticMeshRecipeBuildRequest& Request,
			const FAssetBuildTaskContext& Control) -> std::expected<FStaticMeshRecipeBuildProduct, FStaticMeshRecipeError> override
		{
			return FStaticMeshBuildOperations::BuildRenderRecipe(Request, Control);
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
