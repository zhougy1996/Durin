#pragma once

#include "CoreMinimal.h"

namespace Durin::Editor::ContentBrowser::Private
{
	// Classifies every preflight condition that can keep a deletion plan read-only.
	enum class EContentDeletionBlocker : uint8
	{
		InvalidSelection,
		UnsupportedMount,
		MountRoot,
		OutsideMount,
		ReadOnlyMount,
		SourceControlRestricted,
		ReparsePoint,
		UnknownPackage,
		ExternalReference,
		RedirectorTargetNotSelected,
		TargetRedirectorsNotSelected,
		LoadingPackage,
		DirtyPackage,
		ReferenceStoreInspectionFailed,
		CompanionInspectionFailed,
		CompanionOwnershipConflict,
		ExternalCompanionOwner,
		InspectionFailed,
		StalePlan,
	};

	struct FContentDeletionBlocker
	{
		EContentDeletionBlocker Kind = EContentDeletionBlocker::InvalidSelection;
		std::string DisplayName;
		std::string PhysicalPath;
		std::string RelatedAssetPath;
		std::string Details;
	};

	enum class EContentDeletionEntryKind : uint8
	{
		Directory,
		AssetPackage,
		OrdinaryFile,
		ManagedCompanion,
		UnknownPackage,
	};

	// Complete file-byte identities detect in-place edits even when size and time
	// are preserved. Directory membership is validated against the captured entries.
	struct FContentDeletionFingerprint
	{
		std::string PhysicalPath;
		EContentDeletionEntryKind Kind = EContentDeletionEntryKind::OrdinaryFile;
		uintmax_t FileSize = 0;
		int64 LastWriteTimeTicks = 0;
		FXxHash128 ByteIdentity;
	};

	struct FContentDeletionRoot
	{
		std::string OriginalPath;
	};

	struct FContentDeletionSummary
	{
		uint64 AssetCount = 0;
		uint64 FileCount = 0;
		uint64 CompanionCount = 0;
		uint64 FolderCount = 0;
	};

	struct FContentDeletionWarning
	{
		std::string DisplayName;
		std::string Details;
	};

	// Confirmation and execution share immutable scope. Only the data source owns the
	// move-only execution capability; SessionId is validated against its retained owner.
	struct FContentDeletionPlan
	{
		std::string DisplayName;
		FContentDeletionSummary Summary;
		std::vector<FContentDeletionRoot> MaximalRoots;
		std::vector<FContentDeletionFingerprint> Entries;
		std::vector<FContentDeletionBlocker> Blockers;
		std::vector<FContentDeletionWarning> Warnings;
		uint64 SessionId = 0;

		auto CanExecute() const -> bool { return Blockers.empty(); }
	};

	using FContentDeletionPlanPtr = std::shared_ptr<const FContentDeletionPlan>;

}
