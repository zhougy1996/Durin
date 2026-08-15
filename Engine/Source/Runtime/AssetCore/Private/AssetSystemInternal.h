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
		ASSETCORE_API auto PrepareAssetRelocationTransaction(
			std::span<const FAssetRelocationMapping> Mappings,
			FAssetMutationSummary& OutSummary,
			FAssetMutationTransaction& OutTransaction) -> FAssetResult;
		auto PrepareAssetRelocationState(
			std::span<const FAssetRelocationMapping> Mappings,
			std::shared_ptr<FAssetRelocationState>& OutState) -> FAssetResult;
		auto RevalidateAssetRelocation(
			const std::shared_ptr<FAssetRelocationState>& State) -> FAssetResult;
		auto ApplyAssetRelocation(
			const std::shared_ptr<FAssetRelocationState>& State) -> FAssetResult;
		auto RestoreAssetRelocation(
			const std::shared_ptr<FAssetRelocationState>& State) -> FAssetResult;
		ASSETCORE_API auto PrepareRedirectorFixupTransaction(
			std::span<const FAssetPath> Redirectors,
			EAssetRedirectorFixupMode Mode,
			FAssetRedirectorFixupSummary& OutSummary,
			FAssetMutationTransaction& OutTransaction) -> FAssetResult;
		auto PrepareRedirectorFixupState(
			std::span<const FAssetPath> Redirectors,
			EAssetRedirectorFixupMode Mode,
			std::shared_ptr<FAssetRedirectorFixupState>& OutState) -> FAssetResult;
		auto ValidateRedirectorFixupCommit(
			const std::shared_ptr<FAssetRedirectorFixupState>& State) -> FAssetResult;
		auto CommitRedirectorFixup(
			const std::shared_ptr<FAssetRedirectorFixupState>& State) -> FAssetResult;
		ASSETCORE_API auto AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult;
		ASSETCORE_API auto PrepareAssetDeletionTransaction(
			std::span<const FAssetPath> Paths,
			std::span<const std::filesystem::path> PhysicalRoots,
			FAssetDeletionTransaction& OutTransaction,
			std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult;
		auto ValidateAssetDeletionTransaction(
			const FAssetDeletionTransaction& Transaction,
			std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult;
		auto UnloadAssetDeletionTransaction(
			const FAssetDeletionTransaction& Transaction) -> FAssetResult;
		auto RemoveAssetDeletionRegistryProjection(
			const FAssetDeletionTransaction& Transaction) -> FAssetResult;
		auto RestoreAssetDeletionRegistryProjection(
			const FAssetDeletionTransaction& Transaction) -> FAssetResult;
		auto DeleteAssetForTesting(const FAssetPath& Path) -> FAssetResult;
		ASSETCORE_API auto FindResidentPackage(const FAssetPath& Path) const -> DPackage*;
		ASSETCORE_API auto GetResidentPackagePublicationState(
			const FAssetPath& Path) const
			-> std::optional<EAssetPackagePublicationState>;
		ASSETCORE_API auto UnloadPackage(
			const FAssetPath& Path,
			EAssetPackageUnloadPolicy Policy =
				EAssetPackageUnloadPolicy::RejectUnsaved) -> FAssetResult;
		ASSETCORE_API auto CapturePackageLoadSnapshot() const -> FAssetPackageLoadSnapshot;
		ASSETCORE_API auto ReleasePackagesLoadedSince(
			const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult;
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
	private:
		struct FResidentPackageEntry
		{
			DPackage* Package = nullptr;
			EAssetPackagePublicationState PublicationState =
				EAssetPackagePublicationState::Published;

			FResidentPackageEntry() = default;
			FResidentPackageEntry(
				DPackage* InPackage,
				EAssetPackagePublicationState InPublicationState =
					EAssetPackagePublicationState::Published)
				: Package(InPackage)
				, PublicationState(InPublicationState)
			{
			}

			auto operator->() const -> DPackage* { return Package; }
			operator DPackage*() const { return Package; }
			explicit operator bool() const { return Package != nullptr; }
		};

		struct FPackageStore
		{
			using FMap = std::unordered_map<FAssetPath, FResidentPackageEntry>;
			FMap Packages;

			auto begin() { return Packages.begin(); }
			auto begin() const { return Packages.begin(); }
			auto end() { return Packages.end(); }
			auto end() const { return Packages.end(); }
			auto find(const FAssetPath& Path) { return Packages.find(Path); }
			auto find(const FAssetPath& Path) const { return Packages.find(Path); }
			auto contains(const FAssetPath& Path) const -> bool { return Packages.contains(Path); }
			auto emplace(
				const FAssetPath& Path,
				DPackage* Package,
				EAssetPackagePublicationState PublicationState =
					EAssetPackagePublicationState::Published)
			{
				return Packages.emplace(
					Path, FResidentPackageEntry{Package, PublicationState});
			}
			auto erase(const FAssetPath& Path) { return Packages.erase(Path); }
			auto erase(FMap::iterator It) { return Packages.erase(It); }
			auto empty() const -> bool { return Packages.empty(); }
			auto size() const -> size_t { return Packages.size(); }
			auto clear() -> void { Packages.clear(); }
		};

		FAssetRuntimeState();
		auto LoadAssetFromCatalog(
			const FAssetData& Data,
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

		// One UE-style resident set; publication and dirty state are orthogonal.
		FPackageStore ResidentPackages;

		// Tracks active loads to reject dependency cycles.
		std::unordered_set<FAssetPath> LoadingPackages;

		// Outermost loads commit residency as one boundary.
		uint32 LoadDepth = 0;
		std::vector<FAssetPath> TransactionPackages;
		FPackageLoadContext PackageLoadContext;
		bool bPackageLoadStarted = false;
		bool bAcceptingRequests = true;

		friend ASSETCORE_API auto SavePackagesAtomically(
			std::span<DPackage* const>,
			const FAssetBundleSaveOptions&) -> FAssetResult;
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
