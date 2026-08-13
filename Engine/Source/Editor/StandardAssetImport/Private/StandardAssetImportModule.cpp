#include "Modules/ModuleManager.h"
#include "Authoring/AuthoringBuildService.h"
#include "StandardAssetImportProviders.h"

namespace Durin
{
	class FStandardAssetImportModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			checkf(AssetBuild::InitializeAuthoringBuildService(),
				"EngineAssetBuild authoring services are unavailable.");
			std::string Error;
			requiref(RegisterStandardAssetImportProviders(Error), "{}", Error);
		}

		auto ShutdownModule() -> void override
		{
			if (!AssetBuild::WaitForAuthoringBuildService(30.0))
				AssetBuild::ShutdownAuthoringBuildService();
			UnregisterStandardAssetImportProviders();
		}
	};

	IMPLEMENT_MODULE(FStandardAssetImportModule, StandardAssetImport)
}
