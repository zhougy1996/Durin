#include "Modules/ModuleManager.h"

namespace Durin
{
	class FAssetRegistryModule final : public IModuleInterface
	{
	};

	IMPLEMENT_MODULE(FAssetRegistryModule, AssetRegistry)
}
