#include "Modules/ModuleManager.h"

namespace Durin
{
	class FAssetImportCoreModule final : public IModuleInterface
	{
	};

	IMPLEMENT_MODULE(FAssetImportCoreModule, AssetImportCore)
}
