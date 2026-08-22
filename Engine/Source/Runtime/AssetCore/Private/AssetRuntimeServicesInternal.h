#pragma once

#include "AssetCatalogStoreInternal.h"

namespace Durin::Asset
{
	// Owns the runtime package roots and their publication state.
	class FAssetResidencyStore
	{
	public:
		struct FEntry
		{
			DPackage* Package = nullptr;
			EAssetPackagePublicationState PublicationState =
				EAssetPackagePublicationState::Published;

			FEntry() = default;
			FEntry(
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

		using FMap = std::unordered_map<FAssetPath, FEntry>;
		auto begin() { return Packages.begin(); }
		auto begin() const { return Packages.begin(); }
		auto end() { return Packages.end(); }
		auto end() const { return Packages.end(); }
		auto find(const FAssetPath& Path) { return Packages.find(Path); }
		auto find(const FAssetPath& Path) const { return Packages.find(Path); }
		auto contains(const FAssetPath& Path) const -> bool
		{
			return Packages.contains(Path);
		}
		auto emplace(
			const FAssetPath& Path,
			DPackage* Package,
			EAssetPackagePublicationState PublicationState =
				EAssetPackagePublicationState::Published)
		{
			return Packages.emplace(Path, FEntry{Package, PublicationState});
		}
		auto erase(const FAssetPath& Path) { return Packages.erase(Path); }
		auto erase(FMap::iterator It) { return Packages.erase(It); }
		auto empty() const -> bool { return Packages.empty(); }
		auto size() const -> size_t { return Packages.size(); }
		auto clear() -> void { Packages.clear(); }

	private:
		FMap Packages;
	};

	// Owns load transactions and coordinates package construction and residency.
	class FAssetLoadService
	{
	public:
		FAssetLoadService(
			FAssetCatalogStore& InCatalog,
			FAssetResidencyStore& InResidency,
			FAssetRuntimeConfiguration& InRuntimeConfiguration,
			bool& bInAcceptingRequests)
			: Registry(InCatalog)
			, ResidentPackages(InResidency)
			, RuntimeConfiguration(InRuntimeConfiguration)
			, bAcceptingRequests(bInAcceptingRequests)
		{
		}

		auto CreateAsset(
			const FAssetPath& Path,
			DClass* Class,
			size_t Size,
			DObject*& OutAsset) -> FAssetResult;
		auto DuplicateAsset(
			const FAssetPath& SourcePath,
			const FAssetPath& DestinationPath,
			DObject*& OutAsset) -> FAssetResult;
		auto CreateRedirector(
			const FAssetPath& RedirectorPath,
			const FAssetPath& DestinationPath,
			DAssetRedirector*& OutRedirector) -> FAssetResult;
		auto LoadAsset(
			const FAssetPath& Path,
			DObject*& OutAsset,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		auto LoadAsset(
			const FAssetPath& Path,
			const DClass* ExpectedClass,
			DObject*& OutAsset,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		auto ResolveSoftObject(
			FSoftObjectPtr& Reference,
			const DClass* ExpectedClass,
			ESoftObjectNullPolicy NullPolicy) -> FSoftObjectResolveResult;
		auto LoadSoftObject(
			FSoftObjectPtr& Reference,
			const DClass* ExpectedClass,
			DObject*& OutObject,
			ESoftObjectNullPolicy NullPolicy,
			FAssetLoadReport* OutReport) -> FAssetResult;
		auto FindResidentPackage(const FAssetPath& Path) const -> DPackage*;
		auto GetResidentPackagePublicationState(const FAssetPath& Path) const
			-> std::optional<EAssetPackagePublicationState>;
		auto UnloadPackage(
			const FAssetPath& Path,
			EAssetPackageUnloadPolicy Policy =
				EAssetPackageUnloadPolicy::RejectUnsaved) -> FAssetResult;
		auto CapturePackageLoadSnapshot() const -> FAssetPackageLoadSnapshot;
		auto ReleasePackagesLoadedSince(const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult;

		auto IsIdle() const -> bool
		{
			return LoadDepth == 0 && LoadingPackages.empty()
				&& TransactionPackages.empty();
		}
		auto Reset() -> void
		{
			LoadingPackages.clear();
			TransactionPackages.clear();
			LoadDepth = 0;
		}

	private:
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
		auto IsPackageReferenced(const DPackage* Package) const -> bool;

		FAssetCatalogStore& Registry;
		FAssetResidencyStore& ResidentPackages;
		FAssetRuntimeConfiguration& RuntimeConfiguration;
		bool& bAcceptingRequests;
		std::unordered_set<FAssetPath> LoadingPackages;
		uint32 LoadDepth = 0;
		std::vector<FAssetPath> TransactionPackages;

		friend class FAssetMutationCoordinator;
	};

	// Coordinates persistent asset mutations across catalog, residency, and disk.
	class FAssetMutationCoordinator
	{
	public:
		FAssetMutationCoordinator(
			FAssetCatalogStore& InCatalog,
			FAssetResidencyStore& InResidency,
			FAssetLoadService& InLoader,
			FAssetRuntimeConfiguration& InRuntimeConfiguration,
			bool& bInAcceptingRequests)
			: Registry(InCatalog)
			, ResidentPackages(InResidency)
			, Loader(InLoader)
			, LoadingPackages(InLoader.LoadingPackages)
			, RuntimeConfiguration(InRuntimeConfiguration)
			, bAcceptingRequests(bInAcceptingRequests)
		{
		}

		auto SavePackage(
			DPackage* Package,
			const FAssetPackageSaveOptions& Options = {}) -> FAssetResult;
		auto SavePackagesAtomically(
			std::span<DPackage* const> Packages,
			const FAssetBundleSaveOptions& Options) -> FAssetResult;
		auto AdmitAssetPackageToCatalog(const FAssetPath& Path) -> FAssetResult;
		auto PrepareAssetRelocationTransaction(
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
		auto PrepareRedirectorFixupTransaction(
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
		auto AnalyzeAssetDeletion(
			const FAssetPath& Path,
			FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult;
		auto PrepareAssetDeletionTransaction(
			std::span<const FAssetPath> Paths,
			std::span<const std::filesystem::path> PhysicalRoots,
			FAssetDeletionTransaction& OutTransaction,
			std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult;
		auto ValidateAssetDeletionTransaction(
			const FAssetDeletionTransaction& Transaction,
			std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult;
		auto UnloadAssetDeletionTransaction(const FAssetDeletionTransaction& Transaction) -> FAssetResult;
		auto RemoveAssetDeletionRegistryProjection(const FAssetDeletionTransaction& Transaction) -> FAssetResult;
		auto RestoreAssetDeletionRegistryProjection(const FAssetDeletionTransaction& Transaction) -> FAssetResult;
		auto DeleteAssetForTesting(const FAssetPath& Path) -> FAssetResult;

	private:
		auto LoadAsset(const FAssetPath& Path, DObject*& OutAsset) -> FAssetResult
		{
			return Loader.LoadAsset(Path, OutAsset);
		}
		auto FindResidentPackage(const FAssetPath& Path) const -> DPackage*
		{
			return Loader.FindResidentPackage(Path);
		}
		auto UnloadPackage(const FAssetPath& Path) -> FAssetResult
		{
			return Loader.UnloadPackage(Path);
		}

		FAssetCatalogStore& Registry;
		FAssetResidencyStore& ResidentPackages;
		FAssetLoadService& Loader;
		std::unordered_set<FAssetPath>& LoadingPackages;
		FAssetRuntimeConfiguration& RuntimeConfiguration;
		bool& bAcceptingRequests;
	};
}
