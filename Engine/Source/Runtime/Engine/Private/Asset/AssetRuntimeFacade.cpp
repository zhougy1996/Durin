#include "AssetRuntimeStateInternal.h"
#include "Asset/PackageUnloading.h"

#include "DObject/Package.h"
#include "DObject/SoftObjectPathServices.h"
#include "Asset/PackageRemoval.h"

namespace Durin
{
	namespace
	{
		struct FSoftObjectPathRegistration
		{
			FSoftObjectPathRegistration()
			{
				Private::SetSoftObjectPathOperations({
					.Resolve = [](const FObjectPath& Path, const DClass* ExpectedClass, DObject* Cached) {
						return FAssetRuntimeState::Get().GetLoadService().FindLoadedObject(Path, ExpectedClass, Cached);
					},
					.Load = [](const FObjectPath& Path, const DClass* ExpectedClass) {
						return LoadObject(Path, ExpectedClass).value_or(nullptr);
					}});
			}
			~FSoftObjectPathRegistration() { Private::SetSoftObjectPathOperations({}); }
		} GSoftObjectPathRegistration;
	}

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

	auto SavePackage(DPackage* Package, EAssetPackageSaveMode Mode) -> FAssetWriteResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator().SavePackage(Package, Mode);
	}

	auto IsPackageLoading(const FPackagePath& Path) -> bool
	{
		return FAssetRuntimeState::Get().GetLoadService().IsPackageLoading(Path);
	}

	auto ReleasePackagesForRemoval(
		std::span<const FPackageData> Packages, uint64 ExpectedRevision) -> FAssetWriteResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator()
			.ReleasePackagesForRemoval(Packages, ExpectedRevision);
	}

	auto PublishPackageRemoval(
		std::span<const FPackageData> Packages, uint64 ExpectedRevision) -> FAssetWriteResult
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
		Durin::Private::InvalidateSoftObjectCaches();
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
