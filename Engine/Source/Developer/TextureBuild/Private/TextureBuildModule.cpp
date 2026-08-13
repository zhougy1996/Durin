#include "Modules/ModuleManager.h"
#include "Texture/TextureBuildService.h"

namespace Durin
{
	class FTextureBuildModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			checkf(AssetBuild::InitializeTextureBuildService(),
				"TextureBuild could not register its authoring service.");
		}

		auto ShutdownModule() -> void override
		{
			AssetBuild::ShutdownTextureBuildService();
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
