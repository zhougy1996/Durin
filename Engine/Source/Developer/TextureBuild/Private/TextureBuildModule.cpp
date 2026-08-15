#include "Modules/ModuleManager.h"
#include "Texture/TextureBuildService.h"

namespace Durin
{
	class FTextureBuildModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			BuildHostCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration("AssetBuildCore.BuildHost");
			BuildOperations = FModuleStartup::CreateAsyncOperationGroup("TextureBuild.Operations");
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

		auto ShutdownModule() -> void override
		{
			Asset::Build::ShutdownTextureBuildService();
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
