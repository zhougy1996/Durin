#pragma once

#include "AssetSubsystemFwd.h"
#include "Asset/Load.h"
#include "AssetRegistryOperationsInternal.h"
#include "DObject/StrongObjectPtr.h"

namespace Durin
{
	// Owns incomplete load components and coordinates completed package residency.
	class FAssetLoadService
	{
	public:
		FAssetLoadService(
			FAssetRuntimeConfiguration& InRuntimeConfiguration,
			bool& bInAcceptingRequests)
			: RuntimeConfiguration(InRuntimeConfiguration)
			, bAcceptingRequests(bInAcceptingRequests)
		{
		}

		auto LoadPackage(
			const FPackagePath& Path,
			DPackage*& OutPackage,
			FAssetLoadReport* OutReport = nullptr) -> FAssetReadResult;
		auto LoadObject(
			const FObjectPath& Path,
			const DClass* ExpectedClass,
			DObject*& OutObject,
			FAssetLoadReport* OutReport = nullptr) -> FAssetReadResult;
		auto ResolveSoftObject(
			FSoftObjectPtr& Reference,
			const DClass* ExpectedClass,
			ESoftObjectNullPolicy NullPolicy) -> FSoftObjectResolveResult;
		auto LoadSoftObject(
			FSoftObjectPtr& Reference,
			const DClass* ExpectedClass,
			DObject*& OutObject,
			ESoftObjectNullPolicy NullPolicy,
			FAssetLoadReport* OutReport) -> FAssetReadResult;
		auto FindResidentPackage(const FPackagePath& Path) const -> DPackage*;
		auto UnloadPackage(
			const FPackagePath& Path,
			EAssetPackageUnloadPolicy Policy =
				EAssetPackageUnloadPolicy::RejectUnsaved) -> FAssetReadResult;
		auto ReleasePackages(std::span<const TWeakObjectPtr<DPackage>> Packages,
			std::span<const TWeakObjectPtr<DPackage>> IgnoreSavedDependencies = {}) -> FAssetReadResult;

		auto IsPackageLoading(const FPackagePath& Path) const -> bool
		{
			return PendingLoads.contains(Path);
		}

		auto IsIdle() const -> bool
		{
			return LoadDepth == 0 && PendingLoads.empty();
		}
		auto Reset() -> void
		{
			PendingLoads.clear();
			CompletionStack.clear();
			NextLoadIndex = 0;
			LoadDepth = 0;
		}

	private:
		enum class ELoadPhase : uint8
		{
			Constructing, Skeleton, ValuesRestored, Validated, PostLoading, Ready, Failed
		};
		struct FPendingLoad
		{
			FPackagePath Path;
			TStrongObjectPtr<DPackage> Package;
			ELoadPhase Phase = ELoadPhase::Constructing;
			uint64 Index = 0;
			uint64 LowLink = 0;
			bool bOwnResource = false;
			std::function<FAssetReadResult()> Validate;
			std::function<void()> PostLoad;
		};
		auto ResolveDependencyPackage(FPendingLoad& Owner, const FPackagePath& Path,
			DPackage*& OutPackage) -> FAssetReadResult;
		auto ResolveDependencyObject(FPendingLoad& Owner, const FObjectPath& Path,
			DObject*& OutObject) -> FAssetReadResult;
		auto CompleteComponent(FPendingLoad& Root) -> FAssetReadResult;
		auto DiscardIncomplete(uint64 FirstIndex) noexcept -> void;
		auto LoadPackageFromPhysicalPath(
			const FPackagePath& Path,
			std::string_view PhysicalPath,
			DPackage*& OutPackage,
			FAssetLoadReport* OutReport = nullptr) -> FAssetReadResult;
		auto LoadPackageInternal(
			const FPackagePath& Path,
			std::string_view PhysicalPath,
			DPackage*& OutPackage,
			FAssetLoadReport* OutReport = nullptr) -> FAssetReadResult;
		auto IsPackageReferenced(const DPackage* Package) const -> bool;

		FAssetRuntimeConfiguration& RuntimeConfiguration;
		bool& bAcceptingRequests;
		std::unordered_map<FPackagePath, std::shared_ptr<FPendingLoad>> PendingLoads;
		std::vector<std::shared_ptr<FPendingLoad>> CompletionStack;
		uint64 NextLoadIndex = 0;
		uint32 LoadDepth = 0;

		friend class FAssetMutationCoordinator;
	};

	// Coordinates persistent asset mutations across catalog, residency, and disk.
	class FAssetMutationCoordinator
	{
	public:
		FAssetMutationCoordinator(
			FAssetLoadService& InLoader,
			FAssetRuntimeConfiguration& InRuntimeConfiguration,
			bool& bInAcceptingRequests)
			: Loader(InLoader)
			, RuntimeConfiguration(InRuntimeConfiguration)
			, bAcceptingRequests(bInAcceptingRequests)
		{
		}

		auto SavePackage(DPackage* Package, EAssetPackageSaveMode Mode) -> FAssetWriteResult;
		auto SavePackagesAtomically(
			std::span<DPackage* const> Packages,
			const FAssetBundleSaveOptions& Options) -> FAssetWriteResult;
		auto AdmitAssetPackageToCatalog(const FPackagePath& Path) -> FAssetWriteResult;
		auto RelocateAssets(
			std::span<const FAssetRelocationMapping> Mappings,
			const std::function<void()>& BeforeCommit = {}) -> FAssetMutationResultDetails;
		auto PrepareAssetRelocationState(
			std::span<const FAssetRelocationMapping> Mappings,
			std::shared_ptr<FAssetRelocationState>& OutState) -> FAssetWriteResult;
		auto RevalidateAssetRelocation(
			const std::shared_ptr<FAssetRelocationState>& State) -> FAssetWriteResult;
		auto ApplyAssetRelocation(
			const std::shared_ptr<FAssetRelocationState>& State) -> FAssetWriteResult;
		auto FixUpRedirectors(
			std::span<const FPackagePath> Redirectors,
			EAssetRedirectorFixupMode Mode,
			const std::function<void()>& BeforeCommit = {}) -> FAssetMutationResultDetails;
		auto PrepareRedirectorFixupState(
			std::span<const FPackagePath> Redirectors,
			EAssetRedirectorFixupMode Mode,
			std::shared_ptr<FAssetRedirectorFixupState>& OutState) -> FAssetWriteResult;
		auto ValidateRedirectorFixupCommit(
			const std::shared_ptr<FAssetRedirectorFixupState>& State) -> FAssetWriteResult;
		auto CommitRedirectorFixup(
			const std::shared_ptr<FAssetRedirectorFixupState>& State) -> FAssetWriteResult;
		auto ReleasePackagesForRemoval(
			std::span<const FAssetData> Entries, uint64 ExpectedRevision) -> FAssetWriteResult;
		auto PublishPackageRemoval(
			std::span<const FAssetData> Entries, uint64 ExpectedRevision) -> FAssetWriteResult;

	private:
		auto ValidatePackageRemoval(
			std::span<const FAssetData> Entries, uint64 ExpectedRevision) -> FAssetWriteResult;
		auto FindResidentPackage(const FPackagePath& Path) const -> DPackage*
		{
			return Loader.FindResidentPackage(Path);
		}
		auto UnloadPackage(const FPackagePath& Path) -> FAssetReadResult
		{
			return Loader.UnloadPackage(Path);
		}

		FAssetLoadService& Loader;
		FAssetRuntimeConfiguration& RuntimeConfiguration;
		bool& bAcceptingRequests;
	};
}
