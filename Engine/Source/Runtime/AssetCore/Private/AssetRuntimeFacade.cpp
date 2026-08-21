#include "AssetRuntimeStateInternal.h"

#include "DObject/Package.h"

namespace Durin::Asset
{
	auto LoadAsset(
		const FAssetPath& Path,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return FAssetRuntimeState::Get().LoadAsset(Path, OutAsset, OutReport);
	}

	auto LoadAsset(
		const FAssetPath& Path,
		const DClass* ExpectedClass,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return FAssetRuntimeState::Get().LoadAsset(
			Path, ExpectedClass, OutAsset, OutReport);
	}

	auto CreateAsset(
		const FAssetPath& Path,
		DClass* Class,
		size_t Size,
		DObject*& OutAsset) -> FAssetResult
	{
		return FAssetRuntimeState::Get().CreateAsset(Path, Class, Size, OutAsset);
	}

	auto CreateAssetRedirectorForTesting(
		const FAssetPath& RedirectorPath,
		const FAssetPath& DestinationPath,
		DAssetRedirector*& OutRedirector) -> FAssetResult
	{
		return FAssetRuntimeState::Get().CreateRedirector(
			RedirectorPath, DestinationPath, OutRedirector);
	}

	auto ResolveSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		ESoftObjectNullPolicy NullPolicy) -> FSoftObjectResolveResult
	{
		return FAssetRuntimeState::Get().ResolveSoftObjectInternal(
			Reference, ExpectedClass, NullPolicy);
	}

	auto LoadSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		DObject*& OutObject,
		ESoftObjectNullPolicy NullPolicy,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return FAssetRuntimeState::Get().LoadSoftObjectInternal(
			Reference, ExpectedClass, OutObject, NullPolicy, OutReport);
	}

	auto SavePackage(
		DPackage* Package,
		const FAssetPackageSaveOptions& Options) -> FAssetResult
	{
		return FAssetRuntimeState::Get().SavePackage(Package, Options);
	}

	auto PrepareAssetRelocationTransaction(
		std::span<const FAssetRelocationMapping> Mappings,
		FAssetMutationSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction) -> FAssetResult
	{
		return FAssetRuntimeState::Get().PrepareAssetRelocationTransaction(
			Mappings, OutSummary, OutTransaction);
	}

	auto PrepareRedirectorFixupTransaction(
		std::span<const FAssetPath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		FAssetRedirectorFixupSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction) -> FAssetResult
	{
		return FAssetRuntimeState::Get().PrepareRedirectorFixupTransaction(
			Redirectors, Mode, OutSummary, OutTransaction);
	}

	auto AnalyzeAssetDeletion(
		const FAssetPath& Path,
		FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult
	{
		return FAssetRuntimeState::Get().AnalyzeAssetDeletion(Path, OutAnalysis);
	}

	auto PrepareAssetDeletionTransaction(
		std::span<const FAssetPath> Paths,
		std::span<const std::filesystem::path> PhysicalRoots,
		FAssetDeletionTransaction& OutTransaction,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult
	{
		return FAssetRuntimeState::Get().PrepareAssetDeletionTransaction(
			Paths, PhysicalRoots, OutTransaction, OutBlockers);
	}

	auto DeleteAssetForTesting(const FAssetPath& Path) -> FAssetResult
	{
		return FAssetRuntimeState::Get().DeleteAssetForTesting(Path);
	}

	auto FindResidentPackage(const FAssetPath& Path) -> DPackage*
	{
		return FAssetRuntimeState::Get().FindResidentPackage(Path);
	}

	auto GetResidentPackagePublicationState(const FAssetPath& Path)
		-> std::optional<EAssetPackagePublicationState>
	{
		return FAssetRuntimeState::Get().GetResidentPackagePublicationState(Path);
	}

	auto UnloadPackage(
		const FAssetPath& Path,
		EAssetPackageUnloadPolicy Policy) -> FAssetResult
	{
		return FAssetRuntimeState::Get().UnloadPackage(Path, Policy);
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
		if (State.FindResidentPackage(Path) != Package)
			return {EAssetError::NotFound,
				"The package is not the resident package at its path."};
		return State.UnloadPackage(Path, Policy);
	}

	auto CapturePackageLoadSnapshot() -> FAssetPackageLoadSnapshot
	{
		return FAssetRuntimeState::Get().CapturePackageLoadSnapshot();
	}

	auto ReleasePackagesLoadedSince(
		const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult
	{
		return FAssetRuntimeState::Get().ReleasePackagesLoadedSince(Snapshot);
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
