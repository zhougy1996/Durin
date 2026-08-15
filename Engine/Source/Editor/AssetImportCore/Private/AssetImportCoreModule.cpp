#include "AsyncImport.h"
#include "ImportService.h"
#include "AssetMutation.h"
#include "ImportRecordIndex.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	class FAssetImportCoreModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			RegistryCallbacks =
				FModuleStartup::CreateOwnedCallbackRegistration("AssetCore.ReferenceStores");
			ReferenceStoreHandle = Asset::RegisterAssetReferenceStore(
				&Asset::Import::GetImportRecordIndex(), RegistryCallbacks.GetGate());
		}

		auto ShutdownModule() -> void override
		{
			Asset::UnregisterAssetReferenceStore(ReferenceStoreHandle);
			ReferenceStoreHandle = 0;
			Asset::Import::GetImportService().CloseAsyncAdmission();
			Asset::Import::GetImportService().CancelAndDrainAllAsyncImports();
		}

	private:
		FModuleOwnedCallbackRegistration RegistryCallbacks;
		Asset::FAssetReferenceStoreHandle ReferenceStoreHandle = 0;
	};

	IMPLEMENT_MODULE(FAssetImportCoreModule, AssetImportCore)
}
