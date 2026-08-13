#include "AsyncImport.h"
#include "AssetSystem.h"
#include "ImportRecordIndex.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	class FAssetImportCoreModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			ReferenceStoreHandle = Asset::RegisterAssetReferenceStore(
				&Asset::Import::GetImportRecordIndex());
		}

		auto ShutdownModule() -> void override
		{
			Asset::UnregisterAssetReferenceStore(ReferenceStoreHandle);
			ReferenceStoreHandle = 0;
			Asset::Import::CloseAsyncImportAdmission();
			Asset::Import::CancelAndDrainAllAsyncImports();
		}

	private:
		Asset::FAssetReferenceStoreHandle ReferenceStoreHandle = 0;
	};

	IMPLEMENT_MODULE(FAssetImportCoreModule, AssetImportCore)
}
