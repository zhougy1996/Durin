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
					.Resolve = [](const FSoftObjectPath& Path, const DClass* ExpectedClass, DObject* Cached) {
						return FAssetRuntimeState::Get().GetLoadService().FindLoadedObject(Path, ExpectedClass, Cached);
					},
					.Load = [](const FSoftObjectPath& Path, const DClass* ExpectedClass) {
						return LoadObject(Path, ExpectedClass);
					}});
			}
			~FSoftObjectPathRegistration() { Private::SetSoftObjectPathOperations({}); }
		} GSoftObjectPathRegistration;
	}

	auto LoadPackage(const FPackagePath& Path, FAssetLoadReport* OutReport) -> DPackage*
	{
		return LoadPackageDetailed(Path, OutReport).value_or(nullptr);
	}

	auto LoadObject(const FSoftObjectPath& Path, const DClass* ExpectedClass,
		FAssetLoadReport* OutReport) -> DObject*
	{
		return LoadObjectDetailed(Path, ExpectedClass, OutReport).value_or(nullptr);
	}

	auto LoadObject(const FTopLevelAssetPath& Path, const DClass* ExpectedClass,
		FAssetLoadReport* OutReport) -> DObject*
	{
		return LoadObjectDetailed(Path, ExpectedClass, OutReport).value_or(nullptr);
	}

	auto LoadPackageDetailed(
		const FPackagePath& Path,
		FAssetLoadReport* OutReport) -> std::expected<DPackage*, FAssetReadError>
	{
		if (OutReport) *OutReport = {.RequestedPath = Path, .FinalPath = Path, .PackagePath = Path};
		auto Result = FAssetRuntimeState::Get().GetLoadService().LoadPackageDetailed(Path, OutReport);
		return Result;
	}

	auto LoadObjectDetailed(
		const FSoftObjectPath& Path,
		const DClass* ExpectedClass,
		FAssetLoadReport* OutReport) -> std::expected<DObject*, FAssetReadError>
	{
		return FAssetRuntimeState::Get().GetLoadService().LoadObjectDetailed(
			Path, ExpectedClass, OutReport);
	}

	auto SavePackage(DPackage* Package, const FAssetPackageSaveArgs& Args) -> FAssetWriteResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator().SavePackage(Package, Args.Mode);
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

	auto ShutdownAssetRuntime() -> void
	{
		FAssetRuntimeState::Get().Shutdown();
	}

	auto InitializeAssetRuntime(
		FAssetRuntimeConfiguration Configuration) -> FAssetWriteResult
	{
		return FAssetRuntimeState::Get().Initialize(std::move(Configuration));
	}

	auto GetAssetRuntimeConfiguration() -> const FAssetRuntimeConfiguration&
	{
		return FAssetRuntimeState::Get().GetRuntimeConfiguration();
	}
}
