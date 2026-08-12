#include "Modules/ModuleManager.h"
#include "Texture/Texture2DAuthoringCoordinator.h"

namespace Durin
{
	class FEngineAssetBuildModule final : public IModuleInterface
	{
		auto StartupModule() -> void override
		{
			checkf(AssetBuild::InitializeTexture2DBuildCoordinator(),
				"EngineAssetBuild could not initialize its Texture2D coordinator.");
		}

		auto ShutdownModule() -> void override
		{
			AssetBuild::ShutdownTexture2DBuildCoordinator();
		}
	};

	IMPLEMENT_MODULE(FEngineAssetBuildModule, EngineAssetBuild)
}
