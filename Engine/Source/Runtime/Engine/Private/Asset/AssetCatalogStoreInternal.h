#pragma once

#define DURIN_ENGINE_ASSET_INTERNAL 1
#include "Asset/Redirector.h"
#include "Asset/Mutation.h"
#include "Asset/Testing.h"
#undef DURIN_ENGINE_ASSET_INTERNAL

namespace Durin::Asset
{
	struct FAssetRelocationState;
	struct FAssetRedirectorFixupState;
	class FAssetLoadService;
	class FAssetMutationCoordinator;

	class FAssetCatalogStore
	{
	public:
		ENGINE_API auto ScanMountedContent(EAssetRegistryScanMode Mode = EAssetRegistryScanMode::Incremental) -> FAssetResult;
		ENGINE_API auto RefreshMountedContent(
			EAssetRegistryScanMode Mode = EAssetRegistryScanMode::Incremental)
			-> FAssetCatalogRefreshResult;
		ENGINE_API auto FlushPersistentSnapshot() -> void;
		ENGINE_API auto FindAssetExact(
			const FAssetPath& Path) const -> FAssetCatalogEntry;
		ENGINE_API auto CaptureSnapshot() const -> FAssetCatalogSnapshot;
		ENGINE_API auto ResolveAssetPath(
			const FAssetPath& Path,
			const FAssetPathResolveOptions& Options = {}) const -> FAssetPathResolveResult;
		ENGINE_API auto FindRedirectorsTo(const FAssetPath& Destination) const
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
		ENGINE_API auto BuildCookReachability(
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

		friend class FAssetLoadService;
		friend class FAssetMutationCoordinator;
	};

	ENGINE_API auto GetAssetCatalogStore() -> FAssetCatalogStore&;
}
