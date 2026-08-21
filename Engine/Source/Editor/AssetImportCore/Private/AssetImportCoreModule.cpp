#include "AsyncImport.h"
#include "ImportService.h"
#include "AssetAuthoring.h"
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
				&Asset::GetImportRecordIndex(), RegistryCallbacks.GetGate());
		}

		auto ShutdownModule() -> void override
		{
			Asset::UnregisterAssetReferenceStore(ReferenceStoreHandle);
			ReferenceStoreHandle = 0;
			Asset::GetImportService().CloseAsyncAdmission();
			Asset::GetImportService().CancelAndDrainAllAsyncImports();
		}

	private:
		FModuleOwnedCallbackRegistration RegistryCallbacks;
		Asset::FAssetReferenceStoreHandle ReferenceStoreHandle = 0;
	};

	IMPLEMENT_MODULE(FAssetImportCoreModule, AssetImportCore)
}
