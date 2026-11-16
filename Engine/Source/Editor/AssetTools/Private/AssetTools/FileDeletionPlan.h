#pragma once

#include "CoreMinimal.h"

namespace Durin::ContentStorage
{
	struct FFileDeletionInput { std::string Name, PhysicalPath; };

	// Classifies every preflight condition that can keep a deletion plan read-only.
	enum class EFileDeletionBlocker : uint8
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

	struct FFileDeletionBlocker
	{
		EFileDeletionBlocker Kind = EFileDeletionBlocker::InvalidSelection;
		std::string DisplayName;
		std::string PhysicalPath;
		std::string RelatedAssetPath;
		std::string Details;
	};

	enum class EFileDeletionEntryKind : uint8
	{
		Directory,
		AssetPackage,
		OrdinaryFile,
		ManagedCompanion,
		UnknownPackage,
	};

	// Complete file-byte identities detect in-place edits even when size and time
	// are preserved. Directory membership is validated against the captured entries.
	struct FFileDeletionFingerprint
	{
		std::string PhysicalPath;
		EFileDeletionEntryKind Kind = EFileDeletionEntryKind::OrdinaryFile;
		uintmax_t FileSize = 0;
		int64 LastWriteTimeTicks = 0;
		FXxHash128 ByteIdentity;
	};

	struct FFileDeletionRoot
	{
		std::string OriginalPath;
	};

	struct FFileDeletionSummary
	{
		uint64 AssetCount = 0;
		uint64 FileCount = 0;
		uint64 CompanionCount = 0;
		uint64 FolderCount = 0;
	};

	struct FFileDeletionWarning
	{
		std::string DisplayName;
		std::string Details;
	};

	// Immutable confirmation facts. Handle identity is checked against the active attempt.
	struct FFileDeletionPlan
	{
		std::string DisplayName;
		FFileDeletionSummary Summary;
		std::vector<FFileDeletionRoot> MaximalRoots;
		std::vector<FFileDeletionFingerprint> Entries;
		std::vector<FFileDeletionBlocker> Blockers;
		std::vector<FFileDeletionWarning> Warnings;

		auto CanExecute() const -> bool { return Blockers.empty(); }
	};

	using FFileDeletionPlanPtr = std::shared_ptr<const FFileDeletionPlan>;

}
