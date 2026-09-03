#pragma once

#include "AssetSubsystemFwd.h"
#include "Asset/Load.h"
#include "AssetPublicationCoordinatorInternal.h"

namespace Durin
{
	// Owns load transactions and coordinates package construction and residency.
	class FAssetLoadService
	{
	public:
		FAssetLoadService(
			FAssetPublicationCoordinator& InCatalog,
			FAssetRuntimeConfiguration& InRuntimeConfiguration,
			bool& bInAcceptingRequests)
			: Registry(InCatalog)
			, RuntimeConfiguration(InRuntimeConfiguration)
			, bAcceptingRequests(bInAcceptingRequests)
		{
		}

		auto LoadPackage(
			const FPackagePath& Path,
			DPackage*& OutPackage,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		auto LoadObject(
			const FObjectPath& Path,
			const DClass* ExpectedClass,
			DObject*& OutObject,
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
		auto FindResidentPackage(const FPackagePath& Path) const -> DPackage*;
		auto UnloadPackage(
			const FPackagePath& Path,
			EAssetPackageUnloadPolicy Policy =
				EAssetPackageUnloadPolicy::RejectUnsaved) -> FAssetResult;
		auto CapturePackageLoadSnapshot() const -> FAssetPackageLoadSnapshot;
		auto ReleasePackagesLoadedSince(const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult;

		auto IsPackageLoading(const FPackagePath& Path) const -> bool
		{
			return LoadingPackages.contains(Path);
		}

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
		auto LoadPackageFromPhysicalPath(
			const FPackagePath& Path,
			std::string_view PhysicalPath,
			DPackage*& OutPackage,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		auto LoadPackageInternal(
			const FPackagePath& Path,
			std::string_view PhysicalPath,
			DPackage*& OutPackage,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		auto IsPackageReferenced(const DPackage* Package) const -> bool;

		FAssetPublicationCoordinator& Registry;
		FAssetRuntimeConfiguration& RuntimeConfiguration;
		bool& bAcceptingRequests;
		std::unordered_set<FPackagePath> LoadingPackages;
		uint32 LoadDepth = 0;
		std::vector<FPackagePath> TransactionPackages;

		friend class FAssetMutationCoordinator;
	};

	// Coordinates persistent asset mutations across catalog, residency, and disk.
	class FAssetMutationCoordinator
	{
	public:
		FAssetMutationCoordinator(
			FAssetPublicationCoordinator& InCatalog,
			FAssetLoadService& InLoader,
			FAssetRuntimeConfiguration& InRuntimeConfiguration,
			bool& bInAcceptingRequests)
			: Registry(InCatalog)
			, Loader(InLoader)
			, LoadingPackages(InLoader.LoadingPackages)
			, RuntimeConfiguration(InRuntimeConfiguration)
			, bAcceptingRequests(bInAcceptingRequests)
		{
		}

		auto SavePackage(DPackage* Package) -> FAssetResult;
		auto SavePackagesAtomically(
			std::span<DPackage* const> Packages,
			const FAssetBundleSaveOptions& Options) -> FAssetResult;
		auto AdmitAssetPackageToCatalog(const FPackagePath& Path) -> FAssetResult;
		auto PrepareAssetRelocationJob(
			std::span<const FAssetRelocationMapping> Mappings,
			FAssetRelocationSummary& OutSummary,
			FAssetMutationJob& OutJob) -> FAssetResult;
		auto PrepareAssetRelocationState(
			std::span<const FAssetRelocationMapping> Mappings,
			std::shared_ptr<FAssetRelocationState>& OutState) -> FAssetResult;
		auto RevalidateAssetRelocation(
			const std::shared_ptr<FAssetRelocationState>& State) -> FAssetResult;
		auto ApplyAssetRelocation(
			const std::shared_ptr<FAssetRelocationState>& State) -> FAssetResult;
		auto PrepareRedirectorFixupJob(
			std::span<const FPackagePath> Redirectors,
			EAssetRedirectorFixupMode Mode,
			FAssetRedirectorFixupSummary& OutSummary,
			FAssetMutationJob& OutJob) -> FAssetResult;
		auto PrepareRedirectorFixupState(
			std::span<const FPackagePath> Redirectors,
			EAssetRedirectorFixupMode Mode,
			std::shared_ptr<FAssetRedirectorFixupState>& OutState) -> FAssetResult;
		auto ValidateRedirectorFixupCommit(
			const std::shared_ptr<FAssetRedirectorFixupState>& State) -> FAssetResult;
		auto CommitRedirectorFixup(
			const std::shared_ptr<FAssetRedirectorFixupState>& State) -> FAssetResult;
		auto ReleasePackagesForRemoval(
			std::span<const FAssetData> Entries, uint64 ExpectedRevision) -> FAssetResult;
		auto PublishPackageRemoval(
			std::span<const FAssetData> Entries, uint64 ExpectedRevision) -> FAssetResult;

	private:
		auto ValidatePackageRemoval(
			std::span<const FAssetData> Entries, uint64 ExpectedRevision) -> FAssetResult;
		auto FindResidentPackage(const FPackagePath& Path) const -> DPackage*
		{
			return Loader.FindResidentPackage(Path);
		}
		auto UnloadPackage(const FPackagePath& Path) -> FAssetResult
		{
			return Loader.UnloadPackage(Path);
		}

		FAssetPublicationCoordinator& Registry;
		FAssetLoadService& Loader;
		std::unordered_set<FPackagePath>& LoadingPackages;
		FAssetRuntimeConfiguration& RuntimeConfiguration;
		bool& bAcceptingRequests;
	};
}
