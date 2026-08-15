#pragma once

#define DURIN_ASSETCORE_INTERNAL 1
#include "AssetRedirector.h"
#include "AssetMutation.h"
#include "AssetTestSupport.h"
#undef DURIN_ASSETCORE_INTERNAL

namespace Durin::Asset
{
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

	// Coordinates package persistence, loading, and registry consistency.
	class FAssetRuntimeState
	{
	public:
		ASSETCORE_API static auto Get() -> FAssetRuntimeState&;

		ASSETCORE_API auto CreateAsset(const FAssetPath& Path, DClass* Class, size_t Size, DObject*& OutAsset) -> FAssetResult;
		ASSETCORE_API auto CreateRedirector(
			const FAssetPath& RedirectorPath,
			const FAssetPath& DestinationPath,
			DAssetRedirector*& OutRedirector) -> FAssetResult;
		ASSETCORE_API auto LoadAsset(
			const FAssetPath& Path,
			DObject*& OutAsset,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		ASSETCORE_API auto LoadAsset(
			const FAssetPath& Path,
			const DClass* ExpectedClass,
			DObject*& OutAsset,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		ASSETCORE_API auto SavePackage(
			DPackage* Package,
			const FAssetPackageSaveOptions& Options = {}) -> FAssetResult;
		ASSETCORE_API auto AdmitAssetPackageToCatalog(
			const FAssetPath& Path) -> FAssetResult;
		ASSETCORE_API auto AnalyzeAssetRelocationBatch(
			std::span<const FAssetRelocationMapping> Mappings,
			FAssetRelocationBatchToken& OutToken) -> FAssetResult;
		ASSETCORE_API auto RevalidateAssetRelocationBatch(
			const FAssetRelocationBatchToken& Token) -> FAssetResult;
		ASSETCORE_API auto ApplyAssetRelocationBatch(
			const FAssetRelocationBatchToken& Token) -> FAssetResult;
		ASSETCORE_API auto RestoreAssetRelocationBatch(
			const FAssetRelocationBatchToken& Token) -> FAssetResult;
		ASSETCORE_API auto AnalyzeRedirectorFixup(
			std::span<const FAssetPath> Redirectors,
			EAssetRedirectorFixupMode Mode,
			FAssetRedirectorFixupPlan& OutPlan) -> FAssetResult;
		ASSETCORE_API auto RevalidateRedirectorFixup(
			const FAssetRedirectorFixupPlan& Plan) -> FAssetResult;
		ASSETCORE_API auto ApplyRedirectorFixup(
			const FAssetRedirectorFixupPlan& Plan) -> FAssetResult;
		ASSETCORE_API auto AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult;
		ASSETCORE_API auto AnalyzeAssetDeletionBatch(
			std::span<const FAssetPath> Paths,
			std::span<const std::filesystem::path> PhysicalRoots,
			FAssetDeletionBatchToken& OutToken,
			std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult;
		ASSETCORE_API auto RevalidateAssetDeletionBatch(
			const FAssetDeletionBatchToken& Token,
			std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult;
		ASSETCORE_API auto UnloadAssetDeletionBatch(
			const FAssetDeletionBatchToken& Token) -> FAssetResult;
		ASSETCORE_API auto ApplyAssetDeletionBatch(
			const FAssetDeletionBatchToken& Token) -> FAssetResult;
		// Commit-only half used after the editor transaction has already revalidated,
		// unloaded, and staged the exact token-owned physical roots.
		ASSETCORE_API auto RemoveAssetDeletionBatchRegistryProjection(
			const FAssetDeletionBatchToken& Token) -> FAssetResult;
		ASSETCORE_API auto RestoreAssetDeletionBatch(
			const FAssetDeletionBatchToken& Token) -> FAssetResult;
		ASSETCORE_API auto DeleteAsset(const FAssetPath& Path) -> FAssetResult;
		ASSETCORE_API auto FindLoadedPackage(const FAssetPath& Path) const -> DPackage*;
		ASSETCORE_API auto UnloadPackage(const FAssetPath& Path) -> FAssetResult;
		ASSETCORE_API auto CapturePackageLoadSnapshot() const -> FAssetPackageLoadSnapshot;
		ASSETCORE_API auto ReleasePackagesLoadedSince(
			const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult;
		// Commit-only migration seam. Callers must fully validate every entry before
		// publishing the complete selected bundle through this no-fail projection step.
		ASSETCORE_API auto PublishMigratedPackageRegistryEntries(
			std::span<FAssetData> Entries) -> void;
		// Reopens an empty manager after Shutdown().
		ASSETCORE_API auto Initialize() -> void;
		ASSETCORE_API auto StopAcceptingRequests() -> void;
		auto IsAcceptingRequests() const -> bool { return bAcceptingRequests; }
		ASSETCORE_API auto Shutdown() -> void;
		// Configuration is rejected after the first successful package load until shutdown.
		ASSETCORE_API auto ConfigurePackageLoadContext(FPackageLoadContext InContext) -> FAssetResult;
		auto GetPackageLoadContext() const -> const FPackageLoadContext& { return PackageLoadContext; }

		auto GetRegistry() -> FAssetCatalogStore& { return Registry; }
		auto GetRegistry() const -> const FAssetCatalogStore& { return Registry; }
		ASSETCORE_API auto FindDraftPackage(const FAssetPath& Path) const -> DPackage*;
		ASSETCORE_API auto DiscardDraftPackage(const FAssetPath& Path) -> FAssetResult;

	private:
		struct FPackageStore
		{
			using FMap = std::unordered_map<FAssetPath, DPackage*>;
			FMap Packages;

			auto begin() { return Packages.begin(); }
			auto begin() const { return Packages.begin(); }
			auto end() { return Packages.end(); }
			auto end() const { return Packages.end(); }
			auto find(const FAssetPath& Path) { return Packages.find(Path); }
			auto find(const FAssetPath& Path) const { return Packages.find(Path); }
			auto contains(const FAssetPath& Path) const -> bool { return Packages.contains(Path); }
			auto emplace(const FAssetPath& Path, DPackage* Package) { return Packages.emplace(Path, Package); }
			auto erase(const FAssetPath& Path) { return Packages.erase(Path); }
			auto erase(FMap::iterator It) { return Packages.erase(It); }
			auto empty() const -> bool { return Packages.empty(); }
			auto size() const -> size_t { return Packages.size(); }
			auto clear() -> void { Packages.clear(); }
		};

		struct FDraftStore
		{
			std::unordered_map<FAssetPath, DPackage*> Packages;
			auto begin() const { return Packages.begin(); }
			auto end() const { return Packages.end(); }

			auto Find(const FAssetPath& Path) const -> DPackage*
			{
				const auto It = Packages.find(Path);
				return It == Packages.end() ? nullptr : It->second;
			}
			auto Contains(const FAssetPath& Path) const -> bool { return Packages.contains(Path); }
			auto Add(const FAssetPath& Path, DPackage* Package) { return Packages.emplace(Path, Package); }
			auto Remove(const FAssetPath& Path) -> size_t { return Packages.erase(Path); }
			auto Empty() const -> bool { return Packages.empty(); }
			auto Size() const -> size_t { return Packages.size(); }
			auto Clear() -> void { Packages.clear(); }
		};

		FAssetRuntimeState();
		auto LoadAssetFromCatalog(
			const FAssetData& Data,
			const DClass* ExpectedClass,
			DObject*& OutAsset,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		auto LoadAssetExactForMigration(
			const FAssetPath& Path,
			const DClass* ExpectedClass,
			DObject*& OutAsset,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		auto LoadAssetFromPhysicalPath(
			const FAssetPath& Path,
			std::string_view PhysicalPath,
			const DClass* ExpectedClass,
			DObject*& OutAsset,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		auto LoadPackageInternal(
			const FAssetPath& Path,
			std::string_view PhysicalPath,
			DPackage*& OutPackage,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		auto ResolveSoftObjectInternal(
			FSoftObjectPtr& Reference,
			const DClass* ExpectedClass,
			ESoftObjectNullPolicy NullPolicy) -> FSoftObjectResolveResult;
		auto LoadSoftObjectInternal(
			FSoftObjectPtr& Reference,
			const DClass* ExpectedClass,
			DObject*& OutObject,
			ESoftObjectNullPolicy NullPolicy,
			FAssetLoadReport* OutReport) -> FAssetResult;
		auto IsPackageReferenced(const DPackage* Package) const -> bool;

		FAssetCatalogStore Registry;

		// Persistent loaded packages and unpublished authoring drafts have distinct owners.
		FPackageStore LoadedPackages;
		FDraftStore DraftPackages;

		// Tracks active loads to reject dependency cycles.
		std::unordered_set<FAssetPath> LoadingPackages;

		// Packages with unhandled compatibility data cannot be persisted without explicit consent.
		std::unordered_set<DPackage*> CompatibilityRiskPackages;

		// Outermost loads commit residency as one boundary.
		uint32 LoadDepth = 0;
		std::vector<FAssetPath> TransactionPackages;
		FPackageLoadContext PackageLoadContext;
		bool bPackageLoadStarted = false;
		bool bAcceptingRequests = true;

		friend ASSETCORE_API auto SavePackagesAtomically(
			std::span<DPackage* const>,
			const FAssetBundleSaveOptions&) -> FAssetResult;
		friend ASSETCORE_API auto LoadPackageForMigration(
			const FAssetPath&,
			DPackage*&,
			FAssetLoadReport*) -> FAssetResult;
		friend ASSETCORE_API auto ResolveSoftObject(
			FSoftObjectPtr&,
			const DClass*,
			ESoftObjectNullPolicy) -> FSoftObjectResolveResult;
		friend ASSETCORE_API auto LoadSoftObject(
			FSoftObjectPtr&,
			const DClass*,
			DObject*&,
			ESoftObjectNullPolicy,
			FAssetLoadReport*) -> FAssetResult;
	};

	ASSETCORE_API auto GetAssetCatalogStore() -> FAssetCatalogStore&;
}
