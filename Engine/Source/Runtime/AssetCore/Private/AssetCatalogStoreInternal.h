#pragma once

#define DURIN_ASSETCORE_INTERNAL 1
#include "AssetRedirector.h"
#include "AssetMutation.h"
#include "AssetTestSupport.h"
#undef DURIN_ASSETCORE_INTERNAL

namespace Durin::Asset
{
	struct FAssetRelocationState;
	struct FAssetRedirectorFixupState;

	class FAssetCatalogStore
	{
	public:
		ASSETCORE_API auto ScanMountedContent(EAssetRegistryScanMode Mode = EAssetRegistryScanMode::Incremental) -> FAssetResult;
		ASSETCORE_API auto RefreshMountedContent(
			EAssetRegistryScanMode Mode = EAssetRegistryScanMode::Incremental)
			-> FAssetCatalogRefreshResult;
		ASSETCORE_API auto FlushPersistentSnapshot() -> void;
		ASSETCORE_API auto FindAssetExact(
			const FAssetPath& Path) const -> FAssetCatalogEntry;
		ASSETCORE_API auto CaptureSnapshot() const -> FAssetCatalogSnapshot;
		ASSETCORE_API auto ResolveAssetPath(
			const FAssetPath& Path,
			const FAssetPathResolveOptions& Options = {}) const -> FAssetPathResolveResult;
		ASSETCORE_API auto FindRedirectorsTo(const FAssetPath& Destination) const
			-> std::vector<FAssetPath>;
		auto GetAssets() const -> const std::unordered_map<FAssetPath, FAssetData>& { return Assets; }
		auto GetScanErrors() const -> const std::vector<FAssetResult>& { return ScanErrors; }
		auto GetLastScanStats() const -> const FAssetRegistryScanStats& { return LastScanStats; }
		auto GetCacheWarning() const -> const std::string& { return CacheWarning; }
		auto IsPersistentSnapshotDirty() const -> bool { return bPersistentSnapshotDirty; }
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetReferenceIndex() const -> const FAssetReferenceIndex& { return ReferenceIndex; }
		// Builds a final-real-path Cook closure from explicit and registered runtime
		// roots plus hard/soft dependencies. It never loads or mutates authored state.
		ASSETCORE_API auto BuildCookReachability(
			std::span<const FAssetPath> Roots,
			std::vector<FAssetPath>& OutPackages) const -> FAssetResult;

	private:
		auto FindAssetExactPointer(const FAssetPath& Path) const
			-> const FAssetData*;
		auto AddOrUpdate(FAssetData Data) -> void;
		auto Remove(const FAssetPath& Path) -> void;
		auto RefreshReferencesForAsset(const FAssetData& Data) -> bool;
		auto RemoveReferencesFromSource(const FAssetPath& Path) -> bool;
		auto RebuildRedirectorIndex() -> void;
		std::unordered_map<FAssetPath, FAssetData> Assets;
		std::unordered_map<FAssetPath, std::vector<FAssetPath>> RedirectorsByDestination;
		std::vector<FAssetResult> ScanErrors;
		FAssetRegistryScanStats LastScanStats;
		std::string CacheWarning;
		bool bPersistentSnapshotDirty = false;
		FAssetReferenceIndex ReferenceIndex;

		// Monotonically changes when the visible registry contents change.
		uint64 Revision = 1;

		friend class FAssetRuntimeState;
		friend ASSETCORE_API auto SavePackagesAtomically(
			std::span<DPackage* const>,
			const FAssetBundleSaveOptions&) -> FAssetResult;
	};

	ASSETCORE_API auto GetAssetCatalogStore() -> FAssetCatalogStore&;
}

