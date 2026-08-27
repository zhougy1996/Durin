#include "AssetRuntimeStateInternal.h"

#include "DObject/Package.h"

namespace Durin::Asset
{
	auto LoadAsset(
		const FAssetPath& Path,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetLoadService().LoadAsset(
			Path, OutAsset, OutReport);
	}

	auto LoadAsset(
		const FAssetPath& Path,
		const DClass* ExpectedClass,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetLoadService().LoadAsset(
			Path, ExpectedClass, OutAsset, OutReport);
	}

	auto CreateAsset(
		const FAssetPath& Path,
		DClass* Class,
		size_t Size,
		DObject*& OutAsset) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetLoadService().CreateAsset(
			Path, Class, Size, OutAsset);
	}

	auto DuplicateAsset(
		const FAssetPath& SourcePath,
		const FAssetPath& DestinationPath,
		DObject*& OutAsset) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetLoadService().DuplicateAsset(
			SourcePath, DestinationPath, OutAsset);
	}

	auto CreateAssetRedirectorForTesting(
		const FAssetPath& RedirectorPath,
		const FAssetPath& DestinationPath,
		DAssetRedirector*& OutRedirector) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetLoadService().CreateRedirector(
			RedirectorPath, DestinationPath, OutRedirector);
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
		DObject*& OutObject,
		ESoftObjectNullPolicy NullPolicy,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetLoadService().LoadSoftObject(
			Reference, ExpectedClass, OutObject, NullPolicy, OutReport);
	}

	auto SavePackage(DPackage* Package) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator().SavePackage(Package);
	}

	auto PrepareAssetRelocationTransaction(
		std::span<const FAssetRelocationMapping> Mappings,
		FAssetMutationSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator()
			.PrepareAssetRelocationTransaction(
			Mappings, OutSummary, OutTransaction);
	}

	auto PrepareRedirectorFixupTransaction(
		std::span<const FAssetPath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		FAssetRedirectorFixupSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator()
			.PrepareRedirectorFixupTransaction(
			Redirectors, Mode, OutSummary, OutTransaction);
	}

	auto AnalyzeAssetDeletion(
		const FAssetPath& Path,
		FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator()
			.AnalyzeAssetDeletion(Path, OutAnalysis);
	}

	auto PrepareAssetDeletionTransaction(
		std::span<const FAssetPath> Paths,
		std::span<const std::filesystem::path> PhysicalRoots,
		FAssetDeletionTransaction& OutTransaction,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator()
			.PrepareAssetDeletionTransaction(
			Paths, PhysicalRoots, OutTransaction, OutBlockers);
	}

	auto DeleteAssetForTesting(const FAssetPath& Path) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator()
			.DeleteAssetForTesting(Path);
	}

	auto FindResidentPackage(const FAssetPath& Path) -> DPackage*
	{
		return FAssetRuntimeState::Get().GetLoadService().FindResidentPackage(Path);
	}

	auto AdoptCreatedPackage(DPackage* Package) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetLoadService()
			.AdoptCreatedPackage(Package);
	}

	auto GetResidentPackagePublicationState(const FAssetPath& Path)
		-> std::optional<EAssetPackagePublicationState>
	{
		return FAssetRuntimeState::Get().GetLoadService()
			.GetResidentPackagePublicationState(Path);
	}

	auto UnloadPackage(
		const FAssetPath& Path,
		EAssetPackageUnloadPolicy Policy) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetLoadService().UnloadPackage(Path, Policy);
	}

	auto UnloadPackage(
		DPackage* Package,
		EAssetPackageUnloadPolicy Policy) -> FAssetResult
	{
		FAssetPath Path;
		if (!Package || !Package->IsAssetPackage()
			|| !FAssetPath::TryCreate(Package->GetPackagePath(), Path))
			return {EAssetError::InvalidPackageType,
				"The package to unload is invalid."};
		FAssetRuntimeState& State = FAssetRuntimeState::Get();
		if (State.GetLoadService().FindResidentPackage(Path) != Package)
			return {EAssetError::NotFound,
				"The package is not the resident package at its path."};
		return State.GetLoadService().UnloadPackage(Path, Policy);
	}

	auto CapturePackageLoadSnapshot() -> FAssetPackageLoadSnapshot
	{
		return FAssetRuntimeState::Get().GetLoadService()
			.CapturePackageLoadSnapshot();
	}

	auto ReleasePackagesLoadedSince(
		const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetLoadService()
			.ReleasePackagesLoadedSince(Snapshot);
	}

	auto ShutdownAssetManager() -> void
	{
		FAssetRuntimeState::Get().Shutdown();
	}

	auto InitializeAssetManager(
		FAssetRuntimeConfiguration Configuration) -> FAssetResult
	{
		return FAssetRuntimeState::Get().Initialize(std::move(Configuration));
	}

	auto GetAssetRuntimeConfiguration() -> const FAssetRuntimeConfiguration&
	{
		return FAssetRuntimeState::Get().GetRuntimeConfiguration();
	}
}
