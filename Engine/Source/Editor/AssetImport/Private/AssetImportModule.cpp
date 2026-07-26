#include "Modules/ModuleManager.h"

namespace Durin
{
	class FAssetImportModule final : public IModuleInterface
	{
	};

	IMPLEMENT_MODULE(FAssetImportModule, AssetImport)
}
