#pragma once

#include "EngineAPI.h"
#include "Asset/MutationTypes.h"
#include "Asset/PackageInspection.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class DClass;
}

namespace Durin::Asset
{
	struct FAssetDeleteContribution
	{
		std::vector<std::filesystem::path> Files;
	};

	struct FAssetDeleteAnalysis
	{
		FAssetPath AssetPath;
		std::vector<FAssetPath> DirectReferencers;
		std::vector<std::filesystem::path> CompanionFiles;
		FAssetPath RedirectDestination;
		std::string Warning;
		bool bLoaded = false;
		bool bLoading = false;
		bool bRedirector = false;

		auto CanDelete() const -> bool { return DirectReferencers.empty() && !bLoading; }
	};

	enum class EAssetDeletionBatchBlocker : uint8
	{
		MissingAsset,
		ExternalPersistentReference,
		ExternalLoadedReference,
		RedirectorTargetNotSelected,
		TargetRedirectorsNotSelected,
		LoadingPackage,
		DirtyPackage,
		ReferenceStoreInspectionFailed,
		CompanionInspectionFailed,
		CompanionOwnershipConflict,
		ExternalCompanionOwner,
	};

	struct FAssetDeletionBatchBlocker
	{
		EAssetDeletionBatchBlocker Kind = EAssetDeletionBatchBlocker::MissingAsset;
		FAssetPath AssetPath;
		FAssetPath RelatedAssetPath;
		std::filesystem::path PhysicalPath;
		std::string Details;
	};

	struct FAssetDeletionBatchWarning
	{
		FAssetPath TargetPath;
		std::vector<FAssetPath> RedirectorPaths;
		std::vector<FAssetPath> SoftReferencerPaths;
		std::vector<std::string> ExternalOccurrences;
		std::string Details;

		auto operator==(const FAssetDeletionBatchWarning&) const -> bool = default;
	};

	struct FAssetDeletionBatchEntry
	{
		FAssetData RegistryEntry;
		std::vector<std::filesystem::path> CompanionFiles;
		bool bLoaded = false;
	};

	struct FAssetDeletionPhysicalTransition
	{
		std::function<FAssetResult()> Stage;
		std::function<FAssetResult()> Restore;
		std::function<bool()> IsRecoveryRequired;
	};

	class FAssetDeletionTransaction
	{
	public:
		ENGINE_API auto GetRegistryRevision() const -> uint64;
		ENGINE_API auto GetEntries() const
			-> std::span<const FAssetDeletionBatchEntry>;
		ENGINE_API auto GetWarnings() const
			-> std::span<const FAssetDeletionBatchWarning>;
		ENGINE_API auto GetState() const -> EAssetMutationTransactionState;
		ENGINE_API auto Commit(const FAssetDeletionPhysicalTransition& Transition)
			-> FAssetResult;
		ENGINE_API auto Undo(const FAssetDeletionPhysicalTransition& Transition)
			-> FAssetResult;
		ENGINE_API auto Redo(const FAssetDeletionPhysicalTransition& Transition)
			-> FAssetResult;

	private:
		struct FState;
		std::shared_ptr<FState> State;

#if defined(DURIN_ENGINE_ASSET_INTERNAL)
		friend class FAssetMutationCoordinator;
#endif
	};

	using FAssetDeleteContributor = std::function<FAssetResult(
		const FAssetData&,
		const FAssetPackageInspection&,
		FAssetDeleteContribution&
	)>;
	using FAssetDeleteContributorHandle = uint64;
	ENGINE_API auto RegisterAssetDeleteContributor(
		DClass* Class,
		FAssetDeleteContributor Contributor,
		FModuleOwnedCallbackGate OwnerGate = {}
	) -> FAssetDeleteContributorHandle;
	ENGINE_API auto UnregisterAssetDeleteContributor(
		FAssetDeleteContributorHandle Handle
	) -> void;

	enum class EAssetCompanionOwnershipState : uint8
	{
		Unclaimed,
		Owned,
		Ambiguous,
	};

	struct FAssetCompanionOwnership
	{
		EAssetCompanionOwnershipState State =
			EAssetCompanionOwnershipState::Unclaimed;
		std::vector<FAssetPath> Owners;
	};

	ENGINE_API auto QueryAssetCompanionOwnership(
		const std::filesystem::path& PhysicalPath,
		FAssetCompanionOwnership& OutOwnership
	) -> FAssetResult;

	ENGINE_API auto AnalyzeAssetDeletion(
		const FAssetPath& Path,
		FAssetDeleteAnalysis& OutAnalysis
	) -> FAssetResult;
	ENGINE_API auto PrepareAssetDeletionTransaction(
		std::span<const FAssetPath> Paths,
		std::span<const std::filesystem::path> PhysicalRoots,
		FAssetDeletionTransaction& OutTransaction,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers
	) -> FAssetResult;
} // namespace Durin::Asset
