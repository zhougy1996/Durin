#include "AssetRuntimeStateInternal.h"
#include "AssetRegistry/Scan.h"
#include "AssetRegistry/Publication.h"

namespace Durin
{
	auto GetAssetPublicationCoordinator() -> FAssetPublicationCoordinator&
	{
		static FAssetPublicationCoordinator Store;
		return Store;
	}

	auto FlushAssetCatalogSnapshotForTesting() -> void
	{
		FlushAssetRegistryCaches();
	}

	auto IsAssetCatalogSnapshotDirtyForTesting() -> bool
	{
		return IsAssetRegistryCacheDirty();
	}

	auto GetAssetCatalogCacheWarningForTesting() -> std::string
	{
		return GetAssetRegistryCacheWarning();
	}

}
