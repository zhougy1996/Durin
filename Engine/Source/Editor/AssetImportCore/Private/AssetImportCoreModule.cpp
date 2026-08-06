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
				&AssetImport::GetImportRecordIndex());
		}

		auto ShutdownModule() -> void override
		{
			Asset::UnregisterAssetReferenceStore(ReferenceStoreHandle);
			ReferenceStoreHandle = 0;
			AssetImport::CloseAsyncImportAdmission();
			AssetImport::CancelAndDrainAllAsyncImports();
		}

	private:
		Asset::FAssetReferenceStoreHandle ReferenceStoreHandle = 0;
	};

	IMPLEMENT_MODULE(FAssetImportCoreModule, AssetImportCore)
}
