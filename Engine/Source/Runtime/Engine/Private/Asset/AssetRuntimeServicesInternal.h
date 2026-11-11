#pragma once

#include "CoreMinimal.h"

#include "AssetSubsystemFwd.h"
#include "Asset/Load.h"
#include "AssetRegistryOperationsInternal.h"
#include "DObject/StrongObjectPtr.h"

namespace Durin
{
	struct FAsyncPackageInput;
	class FAssetLoadRequestService;
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

		auto LoadPackageDetailed(
			const FPackagePath& Path,
			FAssetLoadReport* OutReport = nullptr) -> std::expected<DPackage*, FAssetReadError>;
		auto LoadObjectDetailed(
			const FSoftObjectPath& Path,
			const DClass* ExpectedClass,
			FAssetLoadReport* OutReport = nullptr) -> std::expected<DObject*, FAssetReadError>;
		auto FindLoadedObject(const FSoftObjectPath& Path, const DClass* ExpectedClass,
			DObject* Cached) -> DObject*;
		auto FindResidentPackage(const FPackagePath& Path) const -> DPackage*;
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
			bool bRestoring = false;
			std::unordered_set<DObject*> ExistingObjects;
			ELoadPhase Phase = ELoadPhase::Constructing;
			uint64 Index = 0;
			uint64 LowLink = 0;
			bool bOwnResource = false;
			std::function<FAssetReadResult()> Validate;
			std::function<void()> PostLoad;
		};
		auto ResolveDependencyPackage(FPendingLoad& Owner, const FPackagePath& Path,
			DPackage*& OutPackage) -> FAssetReadResult;
		auto ResolveDependencyObject(FPendingLoad& Owner, const FSoftObjectPath& Path,
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

		FAssetRuntimeConfiguration& RuntimeConfiguration;
		bool& bAcceptingRequests;
		std::unordered_map<FPackagePath, std::shared_ptr<FPendingLoad>> PendingLoads;
		std::vector<std::shared_ptr<FPendingLoad>> CompletionStack;
		uint64 NextLoadIndex = 0;
		uint32 LoadDepth = 0;
		const std::unordered_map<FPackagePath, FAsyncPackageInput>* AsyncInputs = nullptr;

		friend class FAssetMutationCoordinator;
		friend class FAssetLoadRequestService;
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
		auto SavePackages(
			std::span<DPackage* const> Packages,
			const FAssetPackageSaveArgs& Options) -> FAssetBatchSaveResult;
		auto AdmitAssetPackageToCatalog(const FPackagePath& Path) -> FAssetWriteResult;
		auto ReleasePackagesForRemoval(
			std::span<const FPackageData> Entries, uint64 ExpectedRevision) -> FAssetWriteResult;
		auto PublishPackageRemoval(
			std::span<const FPackageData> Entries, uint64 ExpectedRevision) -> FAssetWriteResult;

	private:
		auto ValidatePackageRemoval(
			std::span<const FPackageData> Entries, uint64 ExpectedRevision) -> FAssetWriteResult;
		auto FindResidentPackage(const FPackagePath& Path) const -> DPackage*
		{
			return Loader.FindResidentPackage(Path);
		}

		FAssetLoadService& Loader;
		FAssetRuntimeConfiguration& RuntimeConfiguration;
		bool& bAcceptingRequests;
	};
}
