#include "Modules/ModuleManager.h"
#include "AssetBuild/BuildFunction.h"
#include "Texture/TextureBuildService.h"

namespace Durin::Asset::Build
{
	TEXTUREBUILD_API auto InitializeTexture2DBuildFunction(
		FModuleOwnedCallbackGate Gate, std::string* OutError = nullptr) -> bool;
	TEXTUREBUILD_API auto ShutdownTexture2DBuildFunction() -> void;
}

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
			std::string Error;
			checkf(Asset::Build::InitializeTexture2DBuildFunction(
				BuildHostCallbackRegistration.GetGate(), &Error),
				"TextureBuild could not register Texture2D build function: {}", Error);
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
			Asset::Build::ShutdownTexture2DBuildFunction();
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
