#pragma once

#include "Asset/Deletion.h"
#include "AssetTools/AssetDeletion.h"
#include "ContentBrowser/ContentBrowserContracts.h"
#include "Editor/Transaction.h"
#include "Panels/ContentBrowserModel.h"

namespace Durin::Editor::ContentBrowser::Private
{
	using FEditorAssetMove = ::Durin::Editor::ContentBrowser::FAssetMove;
	// Classifies every preflight condition that can keep a deletion plan read-only.
	enum class EContentDeletionBlocker : uint8
	{
		InvalidSelection,
		UnsupportedMount,
		MountRoot,
		OutsideMount,
		ReadOnlyMount,
		SourceControlRestricted,
		CrossVolumeStaging,
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

	// Detects replacement and in-place modification without reading package payloads.
	// Directory digests cover the sorted relative path, kind, size, and timestamp of
	// every descendant; file digests cover normalized path, kind, size, and timestamp.
	struct FContentDeletionFingerprint
	{
		std::string PhysicalPath;
		EContentDeletionEntryKind Kind = EContentDeletionEntryKind::OrdinaryFile;
		uintmax_t FileSize = 0;
		int64 LastWriteTimeTicks = 0;
		FXxHash128 ByteIdentity;
		uint64 Digest = 0;
	};

	struct FContentDeletionRoot
	{
		std::string OriginalPath;
		EContentDeletionEntryKind Kind = EContentDeletionEntryKind::OrdinaryFile;
		FContentDeletionFingerprint Fingerprint;
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

	// The operation layer publishes this value only as shared_ptr<const ...>; the
	// confirmation modal and transaction therefore observe the same immutable scope.
	struct FContentDeletionPlan
	{
		uint64 RegistryRevision = 0;
		std::string DisplayName;
		std::string StagingVolumeRoot;
		FContentDeletionSummary Summary;
		std::vector<FContentDeletionRoot> MaximalRoots;
		std::vector<FContentDeletionFingerprint> Entries;
		std::vector<FContentDeletionBlocker> Blockers;
		std::vector<FContentDeletionWarning> Warnings;
		mutable FAssetDeletionOperation AssetOperation;

		auto CanExecute() const -> bool { return Blockers.empty(); }
	};

	using FContentDeletionPlanPtr = std::shared_ptr<const FContentDeletionPlan>;

	// Applying/Restoring are transient. Any failed compensation transitions to
	// RecoveryRequired and deliberately retains the owned staging directory.
	enum class EContentDeletionTransactionState : uint8
	{
		Restored,
		Applying,
		Applied,
		Restoring,
		RecoveryRequired,
	};

	enum class EContentDeletionJournalOperation : uint8
	{
		MoveToStaging,
		MoveToOriginal,
	};

	struct FContentDeletionJournalEntry
	{
		EContentDeletionJournalOperation Operation =
			EContentDeletionJournalOperation::MoveToStaging;
		std::string OriginalPath;
		std::string StagedPath;
		bool bCompleted = false;
		bool bCompensated = false;
	};

	enum class EContentDeletionMovePhase : uint8
	{
		Apply,
		Undo,
		CompensateApply,
		CompensateUndo,
	};

	// Narrow injection seams keep failure tests deterministic without weakening the
	// production ownership and path checks.
	struct FContentDeletionTransactionHooks
	{
		std::function<std::error_code(
			const std::filesystem::path&,
			const std::filesystem::path&,
			EContentDeletionMovePhase)> Rename;
	};

	class FContentDeletionTransaction final : public ::Durin::Editor::ITransactionCustomChange
	{
	public:
		explicit FContentDeletionTransaction(
			FContentDeletionPlanPtr InPlan,
			FContentDeletionTransactionHooks InHooks = {});
		~FContentDeletionTransaction() override;

		auto GetDescription() const -> std::string_view override;
		auto GetDetails(::Durin::Editor::ETransactionOperation Operation) const
			-> std::string override;
		auto MutatesMountedContent() const -> bool override { return true; }
		auto GetOwningModule() const -> std::string_view override { return "ContentBrowser"; }
		auto Undo() -> bool override;
		auto Redo() -> bool override;
		auto GetState() const -> EContentDeletionTransactionState { return State; }
		auto GetStagingRoot() const -> const std::filesystem::path&
		{
			return StagingRoot;
		}
		auto GetJournal() const -> std::span<const FContentDeletionJournalEntry>
		{
			return Journal;
		}

	private:
		struct FMove
		{
			std::filesystem::path Original;
			std::filesystem::path Staged;
		};

		auto EnsureStagingRoot() -> bool;
		auto ValidatePhysicalState(bool bApplied) -> bool;
		auto ValidateOriginalDestinations() -> bool;
		auto MovePath(
			const std::filesystem::path& Source,
			const std::filesystem::path& Destination,
			EContentDeletionMovePhase Phase) -> bool;
		auto CompensateMoves(
			size_t Count,
			bool bBackToOriginal,
			EContentDeletionMovePhase Phase) -> bool;
		auto StagePhysicalDeletion() -> Asset::FAssetResult;
		auto RestorePhysicalDeletion() -> Asset::FAssetResult;
		auto MakePhysicalTransition() -> Asset::FAssetDeletionPhysicalTransition;
		auto CleanupOwnedStagingRoot() -> void;
		auto Fail(std::string Message) -> bool;

		FContentDeletionPlanPtr Plan;
		FContentDeletionTransactionHooks Hooks;
		std::string Description;
		std::string Details;
		std::filesystem::path StagingRoot;
		std::filesystem::path MarkerPath;
		std::vector<FMove> Moves;
		std::vector<FContentDeletionJournalEntry> Journal;
		EContentDeletionTransactionState State =
			EContentDeletionTransactionState::Restored;
		bool bCommittedOnce = false;
	};

	// Reports the post-operation focus requested by a successful content mutation.
	struct FContentBrowserOperationResult
	{
		Asset::FAssetResult Status;
		std::string FocusPhysicalPath;
		std::string RevealAssetPath;
		std::string OpenAssetClassName;
		std::string Warning;

		explicit operator bool() const { return static_cast<bool>(Status); }
	};

	// Owns filesystem and asset-registry mutations initiated by the content browser.
	class FContentBrowserOperations
	{
	public:
		using FMoveAssets =
			std::function<Asset::FAssetResult(std::span<const FEditorAssetMove>)>;
		using FFixUpAssets =
			std::function<Asset::FAssetResult(std::span<const FPackagePath>)>;
		using FRemoveDirectory = std::function<bool(
			const std::filesystem::path&, std::error_code&)>;

		FContentBrowserOperations(
			FContentBrowserModel& InModel,
			FMoveAssets InMoveAssets,
			FRemoveDirectory InRemoveDirectory = {},
			FFixUpAssets InFixUpAssets = {},
			std::function<void()> InNotifyMountedContentMutation = {}
		);

		auto Rename(const FContentBrowserItem& Item, std::string_view NewName)
			-> FContentBrowserOperationResult;
		auto Duplicate(const FContentBrowserItem& Item)
			-> FContentBrowserOperationResult;
		auto Duplicate(
			const FPackagePath& SourcePath,
			std::string_view DestinationDirectory)
			-> FContentBrowserOperationResult;
		auto CreateFolder(std::string_view PhysicalDirectory)
			-> FContentBrowserOperationResult;
		auto Move(std::span<const FEditorAssetMove> Moves) -> Asset::FAssetResult;
		auto FixUpRedirectorsInFolder(std::string_view VirtualDirectory)
			-> Asset::FAssetResult;
		auto FixUpRedirectors(std::span<const FPackagePath> Redirectors)
			-> Asset::FAssetResult;
		auto FixUpAllRedirectors() -> Asset::FAssetResult;

		auto BuildDeletionPlan(
			std::span<const FContentBrowserItem> Items,
			const std::unordered_set<std::string>& Selection
		) const -> FContentDeletionPlanPtr;
		auto IsDeletionPlanCurrent(const FContentDeletionPlan& Plan) const -> bool;

		auto ShowInExplorer(std::string_view PhysicalPath) const -> void;
		auto CopyToClipboard(std::string_view Text) const -> void;

	private:
		auto RenameFolder(
			const FContentBrowserItem& Item,
			std::string_view NewName,
			std::string& OutWarning)
			-> Asset::FAssetResult;
		auto CollectRedirectors(std::string_view VirtualDirectory) const
			-> std::vector<FPackagePath>;

		FContentBrowserModel& Model;
		FMoveAssets MoveAssets;
		FFixUpAssets FixUpAssets;
		std::function<void()> NotifyMountedContentMutation;
		FRemoveDirectory RemoveDirectory;
	};
} // namespace Durin::Editor::ContentBrowser::Private
