#pragma once

#include "Asset/AssetDefinitions.h"

#include "AssetTools/AssetOperation.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/PackageInspection.h"

namespace Durin
{
	class DClass;
}

namespace Durin
{
	// Additional authored files owned by a package, discovered without loading objects.
	struct FAssetDeleteContribution
	{
		std::vector<std::filesystem::path> Files;
	};

	// Editor acceptance failures that prevent a confirmed selection from being deleted.
	enum class EAssetDeletionBlocker : uint8
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

	// Identifies the selected asset and the conflicting reference or physical participant.
	struct FAssetDeletionBlocker
	{
		EAssetDeletionBlocker Kind = EAssetDeletionBlocker::MissingAsset;
		FPackagePath AssetPath;
		FPackagePath RelatedAssetPath;
		std::filesystem::path PhysicalPath;
		std::string Details;
	};

	// Exact warning facts retained for confirmation and stale-operation revalidation.
	struct FAssetDeletionWarning
	{
		FPackagePath AssetPath;
		std::vector<FPackagePath> RedirectorPaths;
		std::vector<FPackagePath> SoftReferencerPaths;
		std::vector<std::string> ExternalOccurrences;
		std::string Details;

		auto operator==(const FAssetDeletionWarning&) const -> bool = default;
	};

	// Owns the expected catalog metadata and companion closure of one selected package.
	struct FAssetDeletionEntry
	{
		FAssetData RegistryEntry;
		std::vector<std::filesystem::path> CompanionFiles;
		bool bLoaded = false;
	};

	// Host-owned physical I/O; invoked after validation and batch residency release.
	struct FAssetDeletionCommit
	{
		std::function<FAssetResult()> Delete;
	};

	// Includes physical roots so companion ownership can be checked for mixed selections.
	struct FAssetDeletionRequest
	{
		std::vector<FPackagePath> AssetPaths;
		std::vector<std::filesystem::path> PhysicalRoots;
	};

	// Owns one confirmed irreversible editor deletion, including policy and execution state.
	class FAssetDeletionOperation
	{
	public:
		ASSETTOOLS_API FAssetDeletionOperation();
		ASSETTOOLS_API ~FAssetDeletionOperation();
		FAssetDeletionOperation(const FAssetDeletionOperation&) = delete;
		auto operator=(const FAssetDeletionOperation&) -> FAssetDeletionOperation& = delete;
		ASSETTOOLS_API FAssetDeletionOperation(FAssetDeletionOperation&&) noexcept;
		ASSETTOOLS_API auto operator=(FAssetDeletionOperation&&) noexcept -> FAssetDeletionOperation&;
		ASSETTOOLS_API auto GetRegistryRevision() const -> uint64;
		ASSETTOOLS_API auto GetEntries() const -> std::span<const FAssetDeletionEntry>;
		ASSETTOOLS_API auto GetWarnings() const -> std::span<const FAssetDeletionWarning>;
		ASSETTOOLS_API auto GetBlockers() const -> std::span<const FAssetDeletionBlocker>;
		// Rejects empty, moved-from, blocked, stale, and completed operations before I/O.
		// A callback failure is irreversible and returns ForwardPending with fenced paths.
		ASSETTOOLS_API auto Delete(const FAssetDeletionCommit& Commit) -> FAssetOperationResult;

	private:
		struct FState;
		std::unique_ptr<FState> State;

		friend auto PrepareAssetDeletionWithEditorPolicy(
			const FAssetDeletionRequest& Request,
			FAssetDeletionOperation& OutOperation) -> FAssetOperationResult;
	};

	using FAssetDeleteContributor = std::function<FAssetResult(
		const FAssetData&,
		const FAssetPackageInspection&,
		FAssetDeleteContribution&
	)>;
	using FAssetDeleteContributorHandle = uint64;
	ASSETTOOLS_API auto RegisterAssetDeleteContributor(
		DClass* Class,
		FAssetDeleteContributor Contributor) -> FAssetDeleteContributorHandle;

	ASSETTOOLS_API auto UnregisterAssetDeleteContributor(
		FAssetDeleteContributorHandle Handle
	) -> void;

	// Distinguishes ordinary files from uniquely or ambiguously claimed companions.
	enum class EAssetCompanionOwnershipState : uint8
	{
		Unclaimed,
		Owned,
		Ambiguous,
	};

	// Owned package identities claiming one normalized physical path.
	struct FAssetCompanionOwnership
	{
		EAssetCompanionOwnershipState State =
			EAssetCompanionOwnershipState::Unclaimed;
		std::vector<FPackagePath> Owners;
	};

	ASSETTOOLS_API auto QueryAssetCompanionOwnership(
		const std::filesystem::path& PhysicalPath,
		FAssetCompanionOwnership& OutOwnership
	) -> FAssetResult;

} // namespace Durin
