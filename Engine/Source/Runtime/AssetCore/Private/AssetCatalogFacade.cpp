#include "AssetRuntimeStateInternal.h"

namespace Durin::Asset
{
	auto GetAssetCatalogStore() -> FAssetCatalogStore&
	{
		return FAssetRuntimeState::Get().GetRegistry();
	}

	auto FindAssetExact(const FAssetPath& Path) -> FAssetCatalogEntry
	{
		return FAssetRuntimeState::Get().GetRegistry().FindAssetExact(Path);
	}

	auto ResolveAssetPath(
		const FAssetPath& Path,
		const FAssetPathResolveOptions& Options) -> FAssetPathResolveResult
	{
		return FAssetRuntimeState::Get().GetRegistry().ResolveAssetPath(Path, Options);
	}

	auto CaptureAssetCatalogSnapshot() -> FAssetCatalogSnapshot
	{
		return FAssetRuntimeState::Get().GetRegistry().CaptureSnapshot();
	}

	auto GetAssetCatalogRevision() -> uint64
	{
		return FAssetRuntimeState::Get().GetRegistry().GetRevision();
	}

	auto CaptureAssetReferenceIndex() -> FAssetReferenceIndex
	{
		return FAssetRuntimeState::Get().GetRegistry().GetReferenceIndex();
	}

	auto FindRedirectorsTo(const FAssetPath& Destination)
		-> std::vector<FAssetPath>
	{
		return FAssetRuntimeState::Get().GetRegistry().FindRedirectorsTo(Destination);
	}

	auto BuildCookReachability(
		std::span<const FAssetPath> Roots,
		std::vector<FAssetPath>& OutPackages) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetRegistry().BuildCookReachability(
			Roots, OutPackages);
	}

	auto FlushAssetCatalogSnapshotForTesting() -> void
	{
		FAssetRuntimeState::Get().GetRegistry().FlushPersistentSnapshot();
	}

	auto IsAssetCatalogSnapshotDirtyForTesting() -> bool
	{
		return FAssetRuntimeState::Get().GetRegistry().IsPersistentSnapshotDirty();
	}

	auto GetAssetCatalogCacheWarningForTesting() -> std::string
	{
		return FAssetRuntimeState::Get().GetRegistry().GetCacheWarning();
	}

	auto RefreshAssetCatalog(
		EAssetRegistryScanMode Mode) -> FAssetCatalogRefreshResult
	{
		return FAssetRuntimeState::Get().GetRegistry().RefreshMountedContent(Mode);
	}
}
