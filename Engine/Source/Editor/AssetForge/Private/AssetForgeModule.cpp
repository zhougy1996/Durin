#include "AssetForge/ImportService.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	class FAssetForgeModule final : public IModuleInterface
	{
	public:
		auto ShutdownModule() -> void override
		{
			AssetForge::GetImportService().CloseAsyncAdmission();
			AssetForge::GetImportService().CancelAndDrainAllAsyncImports();
		}
	};

	IMPLEMENT_MODULE(FAssetForgeModule, AssetForge)
}
