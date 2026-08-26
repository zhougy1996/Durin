#include "AssetForge/Operations/ImportOperation.h"
#include "AssetForge/ImportService.h"
#include "Asset/Mutation.h"
#include "AssetForge/Persistence/ImportRecordIndex.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	class FAssetForgeModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			RegistryCallbacks =
				FModuleStartup::CreateOwnedCallbackRegistration("AssetCore.ReferenceStores");
			ReferenceStoreHandle = Asset::RegisterAssetReferenceStore(
				&AssetForge::GetImportRecordIndex(), RegistryCallbacks.GetGate());
		}

		auto ShutdownModule() -> void override
		{
			Asset::UnregisterAssetReferenceStore(ReferenceStoreHandle);
			ReferenceStoreHandle = 0;
			AssetForge::GetImportService().CloseAsyncAdmission();
			AssetForge::GetImportService().CancelAndDrainAllAsyncImports();
		}

	private:
		FModuleOwnedCallbackRegistration RegistryCallbacks;
		Asset::FAssetReferenceStoreHandle ReferenceStoreHandle = 0;
	};

	IMPLEMENT_MODULE(FAssetForgeModule, AssetForge)
}
