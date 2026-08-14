#include "AsyncImport.h"
#include "AssetSystem.h"
#include "ImportRecordIndex.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	class FAssetImportCoreModule final : public IModuleInterface
	{
	public:
		auto StartupModule(FModuleContext& Context) -> void override
		{
			RegistryCallbacks =
				Context.CreateOwnedCallbackRegistration("AssetCore.ReferenceStores");
			ReferenceStoreHandle = Asset::RegisterAssetReferenceStore(
				&Asset::Import::GetImportRecordIndex(), RegistryCallbacks.GetGate());
		}

		auto ShutdownModule(FModuleShutdownContext&) -> void override
		{
			Asset::UnregisterAssetReferenceStore(ReferenceStoreHandle);
			ReferenceStoreHandle = 0;
			Asset::Import::CloseAsyncImportAdmission();
			Asset::Import::CancelAndDrainAllAsyncImports();
		}

	private:
		FModuleOwnedCallbackRegistration RegistryCallbacks;
		Asset::FAssetReferenceStoreHandle ReferenceStoreHandle = 0;
	};

	IMPLEMENT_MODULE(FAssetImportCoreModule, AssetImportCore)
}
