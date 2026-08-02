#include "AsyncImport.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	class FAssetImportCoreModule final : public IModuleInterface
	{
	public:
		auto ShutdownModule() -> void override
		{
			AssetImport::CloseAsyncImportAdmission();
			AssetImport::CancelAndDrainAllAsyncImports();
		}
	};

	IMPLEMENT_MODULE(FAssetImportCoreModule, AssetImportCore)
}
