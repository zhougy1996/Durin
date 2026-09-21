#include "AssetRuntimeStateInternal.h"

#include "DObject/Package.h"
#include "Asset/PackageRemoval.h"

namespace Durin
{
	auto LoadPackage(
		const FPackagePath& Path,
		FAssetLoadReport* OutReport) -> std::expected<DPackage*, FAssetReadError>
	{
		return FAssetRuntimeState::Get().GetLoadService().LoadPackage(
			Path, OutReport);
	}

	auto LoadObject(
		const FObjectPath& Path,
		const DClass* ExpectedClass,
		FAssetLoadReport* OutReport) -> std::expected<DObject*, FAssetReadError>
	{
		return FAssetRuntimeState::Get().GetLoadService().LoadObject(
			Path, ExpectedClass, OutReport);
	}

	auto ResolveSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		ESoftObjectNullPolicy NullPolicy) -> FSoftObjectResolveResult
	{
		return FAssetRuntimeState::Get().GetLoadService().ResolveSoftObject(
			Reference, ExpectedClass, NullPolicy);
	}

	auto LoadSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		ESoftObjectNullPolicy NullPolicy,
		FAssetLoadReport* OutReport) -> std::expected<DObject*, FAssetReadError>
	{
		return FAssetRuntimeState::Get().GetLoadService().LoadSoftObject(
			Reference, ExpectedClass, NullPolicy, OutReport);
	}

	auto SavePackage(DPackage* Package, EAssetPackageSaveMode Mode) -> FAssetWriteResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator().SavePackage(Package, Mode);
	}

	auto IsPackageLoading(const FPackagePath& Path) -> bool
	{
		return FAssetRuntimeState::Get().GetLoadService().IsPackageLoading(Path);
	}

	auto ReleasePackagesForRemoval(
		std::span<const FAssetData> Packages, uint64 ExpectedRevision) -> FAssetWriteResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator()
			.ReleasePackagesForRemoval(Packages, ExpectedRevision);
	}

	auto PublishPackageRemoval(
		std::span<const FAssetData> Packages, uint64 ExpectedRevision) -> FAssetWriteResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator()
			.PublishPackageRemoval(Packages, ExpectedRevision);
	}

	auto FindResidentPackage(const FPackagePath& Path) -> DPackage*
	{
		return FAssetRuntimeState::Get().GetLoadService().FindResidentPackage(Path);
	}

	auto UnloadPackage(
		const FPackagePath& Path,
		EAssetPackageUnloadPolicy Policy) -> FAssetReadResult
	{
		return FAssetRuntimeState::Get().GetLoadService().UnloadPackage(Path, Policy);
	}

	auto UnloadPackage(
		DPackage* Package,
		EAssetPackageUnloadPolicy Policy) -> FAssetReadResult
	{
		FPackagePath Path;
		if (!Package || !Package->IsAssetPackage()
			|| !FPackagePath::TryCreate(Package->GetPackagePath(), Path))
			return {EAssetReadError::InvalidPackageType,
				"The package to unload is invalid."};
		FAssetRuntimeState& State = FAssetRuntimeState::Get();
		if (State.GetLoadService().FindResidentPackage(Path) != Package)
			return {EAssetReadError::NotFound,
				"The package is not the resident package at its path."};
		return State.GetLoadService().UnloadPackage(Path, Policy);
	}

	auto ShutdownAssetManager() -> void
	{
		FAssetRuntimeState::Get().Shutdown();
		InvalidateSoftObjectCaches();
	}

	auto InitializeAssetManager(
		FAssetRuntimeConfiguration Configuration) -> FAssetWriteResult
	{
		return FAssetRuntimeState::Get().Initialize(std::move(Configuration));
	}

	auto GetAssetRuntimeConfiguration() -> const FAssetRuntimeConfiguration&
	{
		return FAssetRuntimeState::Get().GetRuntimeConfiguration();
	}
}
