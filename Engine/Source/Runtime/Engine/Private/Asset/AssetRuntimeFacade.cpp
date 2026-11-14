#include "AssetRuntimeStateInternal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

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

	auto LoadPackage(const FPackagePath& Path, FAssetLoadReport* OutReport,
		std::optional<UnifiedError::FError>* OutError) -> DPackage*
	{
		if (OutError) OutError->reset();
		if (OutReport) *OutReport = {.RequestedPath = Path, .FinalPath = Path, .PackagePath = Path};
		auto Result = FAssetRuntimeState::Get().GetLoadService().LoadPackageDetailed(Path, OutReport);
		if (Result && !*Result)
			Result = std::unexpected(UnifiedError::FError{AssetLoadErrors::InvalidObjectGraph, "The package loader returned no package."});
		if (!Result && OutError)
		{
			*OutError = std::move(Result.error());
			(*OutError)->PushContext("requested package", Path.ToString());
		}
		return Result.value_or(nullptr);
	}

	auto LoadObject(const FSoftObjectPath& Path, const DClass* ExpectedClass,
		FAssetLoadReport* OutReport, std::optional<UnifiedError::FError>* OutError) -> DObject*
	{
		if (OutError) OutError->reset();
		auto Result = FAssetRuntimeState::Get().GetLoadService().LoadObjectDetailed(Path, ExpectedClass, OutReport);
		if (Result && !*Result)
			Result = std::unexpected(UnifiedError::FError{AssetLoadErrors::InvalidObjectGraph, "The object loader returned no object."});
		if (!Result && OutError)
		{
			*OutError = std::move(Result.error());
			(*OutError)->PushContext("requested object", Path.ToString());
		}
		return Result.value_or(nullptr);
	}

	auto LoadObject(const FTopLevelAssetPath& Path, const DClass* ExpectedClass,
		FAssetLoadReport* OutReport, std::optional<UnifiedError::FError>* OutError) -> DObject*
	{
		if (OutError) OutError->reset();
		if (OutReport) *OutReport = {};
		FSoftObjectPath ObjectPath;
		if (!FSoftObjectPath::TryCreate(Path, std::span<const std::string>{}, ObjectPath))
		{
			if (OutError) OutError->emplace(AssetLoadErrors::InvalidPath,
				"A top-level asset load requires a valid exact asset path.");
			return nullptr;
		}
		return LoadObject(ObjectPath, ExpectedClass, OutReport, OutError);
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
