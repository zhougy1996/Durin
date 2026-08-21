#include "AssetRuntimeStateInternal.h"

namespace Durin::Asset
{
	auto GetAssetCatalogStore() -> FAssetCatalogStore&
	{
		return FAssetRuntimeState::Get().GetCatalogStore();
	}

	auto FindAssetExact(const FAssetPath& Path) -> FAssetCatalogEntry
	{
		return FAssetRuntimeState::Get().GetCatalogStore().FindAssetExact(Path);
	}

	auto ResolveAssetPath(
		const FAssetPath& Path,
		const FAssetPathResolveOptions& Options) -> FAssetPathResolveResult
	{
		return FAssetRuntimeState::Get().GetCatalogStore().ResolveAssetPath(Path, Options);
	}

	auto CaptureAssetCatalogSnapshot() -> FAssetCatalogSnapshot
	{
		return FAssetRuntimeState::Get().GetCatalogStore().CaptureSnapshot();
	}

	auto GetAssetCatalogRevision() -> uint64
	{
		return FAssetRuntimeState::Get().GetCatalogStore().GetRevision();
	}

	auto CaptureAssetReferenceIndex() -> FAssetReferenceIndex
	{
		return FAssetRuntimeState::Get().GetCatalogStore().GetReferenceIndex();
	}

	auto FindRedirectorsTo(const FAssetPath& Destination)
		-> std::vector<FAssetPath>
	{
		return FAssetRuntimeState::Get().GetCatalogStore().FindRedirectorsTo(Destination);
	}

	auto BuildCookReachability(
		std::span<const FAssetPath> Roots,
		std::vector<FAssetPath>& OutPackages) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetCatalogStore().BuildCookReachability(
			Roots, OutPackages);
	}

	auto FlushAssetCatalogSnapshotForTesting() -> void
	{
		FAssetRuntimeState::Get().GetCatalogStore().FlushPersistentSnapshot();
	}

	auto IsAssetCatalogSnapshotDirtyForTesting() -> bool
	{
		return FAssetRuntimeState::Get().GetCatalogStore().IsPersistentSnapshotDirty();
	}

	auto GetAssetCatalogCacheWarningForTesting() -> std::string
	{
		return FAssetRuntimeState::Get().GetCatalogStore().GetCacheWarning();
	}

	auto RefreshAssetCatalog(
		EAssetRegistryScanMode Mode) -> FAssetCatalogRefreshResult
	{
		return FAssetRuntimeState::Get().GetCatalogStore().RefreshMountedContent(Mode);
	}
}
