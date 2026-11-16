#pragma once

#include "CoreMinimal.h"
#include "AssetTools/ContentDeletion.h"
#include "AssetDeleteModel.h"
#include "AssetTools/NativeAssetDeletion.h"

namespace Durin::ContentStorage
{
	// Conditions that block model execution.
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

	// Model-owned confirmation facts. This snapshot never authorizes execution by itself.
	struct FFileDeletionSnapshot
	{
		std::string DisplayName;
		FFileDeletionSummary Summary;
		std::vector<FFileDeletionRoot> MaximalRoots;
		std::vector<FFileDeletionFingerprint> Entries;
		std::vector<FFileDeletionBlocker> Blockers;
		std::vector<FFileDeletionWarning> Warnings;

	};


}

namespace Durin::AssetToolsPrivate
{
	// Private model state, not a separately owned operation or confirmation service.
	struct FFileDeletionState
	{
		FFileDeletionState(const ContentStorage::FContentMutationPolicy&, bool Assets, bool Files);
		auto Analyze(std::span<const ContentStorage::FFileDeletionInput>, FAssetDeleteModel&, ObjectTools::EAllowCancelDuringDelete) -> void;
		auto CanDelete() const -> bool { return !Attempted && Snapshot.Blockers.empty(); }
		auto DoDelete() -> bool;
		auto ValidateScope(std::optional<UnifiedError::FError>* = nullptr) -> bool;
		auto Report(ELogLevel, std::string_view) const -> void;

		ContentStorage::FFileDeletionSnapshot Snapshot;
		ContentStorage::FContentMutationPolicy Policy;
		ContentStorage::FFileDeletionHooks Hooks;
		FAssetDeletionOperation AssetOperation;
		FAssetWriteResult Result;
		ContentStorage::FFileDeletionResult Effects;
		bool Attempted = false, Started = false, PeerScopeValidated = false;
		bool DeleteAssets = true, DeleteFiles = true;
		uint64 ContributorRevision = 0;
	private:
		auto Execute() -> bool;
		auto ValidatePhysicalState(FAssetDeletionFileIdentities&, bool OwnedOnly = false, std::optional<UnifiedError::FError>* = nullptr) -> bool;
		auto DeletePhysicalRoots(std::vector<std::filesystem::path>&) -> FAssetWriteResult;
	};

	// Internal construction and native-test inspection; callers execute through FAssetDeleteModel.
	struct ASSETTOOLS_API FAssetDeleteModelAccess
	{
		static auto Create(std::span<const ContentStorage::FFileDeletionInput>, const ContentStorage::FContentMutationPolicy& = {},
			bool Assets = true, bool Files = true, ObjectTools::EAllowCancelDuringDelete = ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed) -> std::unique_ptr<FAssetDeleteModel>;
		static auto GetState(FAssetDeleteModel& Model) -> FFileDeletionState&;
	};
}
