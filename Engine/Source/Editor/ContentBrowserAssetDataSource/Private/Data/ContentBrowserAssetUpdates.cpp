#include "AssetTools/ContentMutation.h"
#include "ContentBrowserAssetDataSource.h"
#include "Data/ContentBrowserNativeUpdates.h"
#include "AssetRegistry/Catalog.h"
#include "DObject/Package.h"
#include "Misc/MountPaths.h"
namespace Durin
{
	using namespace ContentBrowser::AssetDataSource::Private;
	auto DContentBrowserAssetDataSource::Initialize(bool AutoRegister) -> void
	{
		if (IsInitialized()) return;
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		Super::Initialize(false);
		(void)CaptureAssetQuerySnapshot();
		auto Invalidate = [Invalidated = FilterCacheInvalidated] { *Invalidated = true; };
		MountsInvalidated = FMountPaths::OnMountPointsChanged().AddLambda(Invalidate);
		PackagesInvalidated = OnAssetPackagesChanged().AddLambda(Invalidate);
		CatalogInvalidated = OnAssetCatalogChanged().AddLambda([Invalidate](const FContentChangeBatch&) { Invalidate(); });
		DirectoriesChanged = ContentStorage::OnContentDirectoriesChanged().AddLambda([Weak = TWeakObjectPtr<DContentBrowserAssetDataSource>(this)](const FContentChangeBatch& Batch, const DObject* Origin, ContentStorage::EContentDirectoryChangeReason Reason) {
			const auto* OriginSource = Cast<DContentBrowserDataSource>(Origin);
			if (auto* Source = Weak.Get(); Source && OriginSource && Source != OriginSource && Source->GetOwningEditor()
				&& Source->GetOwningEditor() == OriginSource->GetOwningEditor() && Source->IsActive())
			{
				TStrongObjectPtr<DContentBrowserDataSource> Lifetime(Source);
				QueueNativeCommittedUpdates(*Source, Batch, Reason == ContentStorage::EContentDirectoryChangeReason::Discovery);
			}
		});
		QueryChanged = OnAssetQueryChanged().AddLambda([Weak = TWeakObjectPtr<DContentBrowserAssetDataSource>(this)](const FContentChangeBatch& Batch) {
			if (auto* Source = Weak.Get(); Source && Source->IsActive()) { TStrongObjectPtr<DContentBrowserDataSource> Lifetime(Source); Source->ClearCachedFilterData(); QueueNativeCommittedUpdates(*Source, Batch, true); ContentStorage::PublishContentDirectoryChanges(Batch, Source, ContentStorage::EContentDirectoryChangeReason::Discovery); }
		});
		if (AutoRegister) RegisterDataSource();
	}
	auto DContentBrowserAssetDataSource::Shutdown() -> void
	{
		ContentStorage::OnContentDirectoriesChanged().Remove(DirectoriesChanged); DirectoriesChanged = {};
		OnAssetQueryChanged().Remove(QueryChanged); QueryChanged = {};
		FMountPaths::OnMountPointsChanged().Remove(MountsInvalidated); MountsInvalidated = {};
		OnAssetPackagesChanged().Remove(PackagesInvalidated); PackagesInvalidated = {};
		OnAssetCatalogChanged().Remove(CatalogInvalidated); CatalogInvalidated = {};
		DiscoveryCatalog.reset(); Super::Shutdown();
	}
}
