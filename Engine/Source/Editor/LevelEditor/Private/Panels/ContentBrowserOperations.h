#pragma once

#include "Assets/EditorAssetMoveCoordinator.h"
#include "Editor/EditorTransaction.h"
#include "Panels/ContentBrowserModel.h"

namespace Durin
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
		Asset::FAssetDeletionBatchToken AssetBatch;

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
		UnloadAssetBatch,
		MoveToStaging,
		RemoveRegistryProjection,
		RestoreRegistryProjection,
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
		std::function<Asset::FAssetResult(
			const Asset::FAssetDeletionBatchToken&)> UnloadAssetBatch;
		std::function<Asset::FAssetResult(
			const Asset::FAssetDeletionBatchToken&)> RemoveRegistryProjection;
		std::function<Asset::FAssetResult(
			const Asset::FAssetDeletionBatchToken&)> RestoreAssetBatch;
	};

	class FContentDeletionTransaction final : public IEditorTransaction
	{
	public:
		explicit FContentDeletionTransaction(
			FContentDeletionPlanPtr InPlan,
			FContentDeletionTransactionHooks InHooks = {});
		~FContentDeletionTransaction() override;

		auto GetDescription() const -> std::string_view override;
		auto GetDetails(EEditorTransactionOperation Operation) const
			-> std::string override;
		auto MutatesContent() const -> bool override { return true; }
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
	};

	// Reports the post-operation focus requested by a successful content mutation.
	struct FContentBrowserOperationResult
	{
		Asset::FAssetResult Status;
		std::string FocusPhysicalPath;
		std::string RevealAssetPath;
		std::string OpenAssetClassName;

		explicit operator bool() const { return static_cast<bool>(Status); }
	};

	// Owns filesystem and asset-registry mutations initiated by the content browser.
	class FContentBrowserOperations
	{
	public:
		using FMoveAssets =
			std::function<Asset::FAssetResult(std::span<const FEditorAssetMove>)>;

		FContentBrowserOperations(
			FContentBrowserModel& InModel,
			FMoveAssets InMoveAssets
		);

		auto Rename(const FContentBrowserItem& Item, std::string_view NewName)
			-> FContentBrowserOperationResult;
		auto CreateFolder(std::string_view PhysicalDirectory)
			-> FContentBrowserOperationResult;
		auto CreateLevelAsset(std::string_view VirtualDirectory)
			-> FContentBrowserOperationResult;
		auto CreateMaterialAsset(
			std::string_view VirtualDirectory,
			bool bInstance) -> FContentBrowserOperationResult;
		auto Move(std::span<const FEditorAssetMove> Moves) -> Asset::FAssetResult;
		auto FixUpRedirectorsInFolder(std::string_view VirtualDirectory)
			-> Asset::FAssetResult;
		auto FixUpRedirectors(std::span<const FAssetPath> Redirectors)
			-> Asset::FAssetResult;
		auto FixUpAllRedirectors() -> Asset::FAssetResult;

		auto AnalyzeDeletion(
			std::span<const FContentBrowserItem> Items,
			const std::unordered_set<std::string>& Selection,
			std::vector<std::pair<std::string, Asset::FAssetDeleteAnalysis>>& Analyses,
			std::vector<std::pair<std::string, Asset::FAssetResult>>& Errors
		) const -> void;
		auto BuildDeletionPlan(
			std::span<const FContentBrowserItem> Items,
			const std::unordered_set<std::string>& Selection
		) const -> FContentDeletionPlanPtr;
		auto IsDeletionPlanCurrent(const FContentDeletionPlan& Plan) const -> bool;

		auto IsManagedCompanion(const FContentBrowserItem& Item) const -> bool;
		auto ShowInExplorer(std::string_view PhysicalPath) const -> void;
		auto CopyToClipboard(std::string_view Text) const -> void;

	private:
		auto RenameFolder(const FContentBrowserItem& Item, std::string_view NewName)
			-> Asset::FAssetResult;
		auto CollectRedirectors(std::string_view VirtualDirectory) const
			-> std::vector<FAssetPath>;

		FContentBrowserModel& Model;
		FMoveAssets MoveAssets;
	};
} // namespace Durin
