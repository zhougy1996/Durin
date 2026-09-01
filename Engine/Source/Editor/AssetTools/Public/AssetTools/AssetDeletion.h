#pragma once

#include "AssetTools/AssetOperation.h"

namespace Durin
{
	struct FAssetDeletionCommit;
}

namespace Durin
{
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

	struct FAssetDeletionBlocker
	{
		EAssetDeletionBlocker Kind = EAssetDeletionBlocker::MissingAsset;
		FPackagePath AssetPath;
		FPackagePath RelatedAssetPath;
		std::filesystem::path PhysicalPath;
		std::string Details;
	};

	struct FAssetDeletionEntry
	{
		FPackagePath AssetPath;
		std::string PhysicalPath;
		std::vector<std::filesystem::path> CompanionFiles;
		bool bLoaded = false;
	};

	struct FAssetDeletionRequest
	{
		std::vector<FPackagePath> AssetPaths;
		std::vector<std::filesystem::path> PhysicalRoots;
	};

	// Owns one opaque irreversible Engine deletion job while exposing immutable editor data.
	class FAssetDeletionOperation
	{
	public:
		ASSETTOOLS_API FAssetDeletionOperation();
		ASSETTOOLS_API ~FAssetDeletionOperation();
		FAssetDeletionOperation(const FAssetDeletionOperation&) = delete;
		auto operator=(const FAssetDeletionOperation&)
			-> FAssetDeletionOperation& = delete;
		ASSETTOOLS_API FAssetDeletionOperation(FAssetDeletionOperation&&) noexcept;
		ASSETTOOLS_API auto operator=(FAssetDeletionOperation&&) noexcept
			-> FAssetDeletionOperation&;

		auto GetEntries() const -> std::span<const FAssetDeletionEntry>
		{
			return Entries;
		}
		auto GetWarnings() const -> std::span<const FAssetOperationWarning>
		{
			return Warnings;
		}
		auto GetBlockers() const -> std::span<const FAssetDeletionBlocker>
		{
			return Blockers;
		}
		ASSETTOOLS_API auto Delete(
			const FAssetDeletionCommit& Commit)
			-> FAssetOperationResult;

	private:
		struct FState;
		std::shared_ptr<FState> State;
		std::vector<FAssetDeletionEntry> Entries;
		std::vector<FAssetOperationWarning> Warnings;
		std::vector<FAssetDeletionBlocker> Blockers;

		friend auto PrepareAssetDeletionWithEditorPolicy(
			const FAssetDeletionRequest& Request,
			FAssetDeletionOperation& OutOperation) -> FAssetOperationResult;
	};
}
