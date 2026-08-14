#include "Modules/ModuleManager.h"
#include "Texture/TextureBuildService.h"

namespace Durin
{
	class FTextureBuildModule final : public IModuleInterface
	{
	public:
		auto StartupModule(FModuleContext& Context) -> void override
		{
			BuildHostCallbackRegistration =
				Context.CreateOwnedCallbackRegistration("AssetBuildCore.BuildHost");
			BuildOperations = Context.CreateAsyncOperationGroup("TextureBuild.Operations");
			require(BuildOperations.IsValid());
			Asset::Build::FTexture2DBuildCoordinatorConfig Config;
			Config.OwnerCancellationToken = BuildOperations.GetCancellationToken();
			Config.OwnerTaskScope = BuildOperations.GetTaskScope();
			checkf(Asset::Build::InitializeTextureBuildService(
				BuildHostCallbackRegistration.GetGate(), Config),
				"TextureBuild could not register its authoring service.");
		}

	private:
		FModuleOwnedCallbackRegistration BuildHostCallbackRegistration;
		FAsyncOperationGroup BuildOperations;

		auto ShutdownModule(FModuleShutdownContext&) -> void override
		{
			Asset::Build::ShutdownTextureBuildService();
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
