#include "Modules/ModuleManager.h"
#include "Texture/TextureBuildService.h"

namespace Durin
{
	class FTextureBuildModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			checkf(Asset::Build::InitializeTextureBuildService(),
				"TextureBuild could not register its authoring service.");
		}

		auto ShutdownModule() -> void override
		{
			Asset::Build::ShutdownTextureBuildService();
		}
	};

	IMPLEMENT_MODULE(FTextureBuildModule, TextureBuild)
}
