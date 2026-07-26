#include "Modules/ModuleManager.h"

namespace Durin
{
	class FEngineAssetBuildModule final : public IModuleInterface
	{
	};

	IMPLEMENT_MODULE(FEngineAssetBuildModule, EngineAssetBuild)
}
