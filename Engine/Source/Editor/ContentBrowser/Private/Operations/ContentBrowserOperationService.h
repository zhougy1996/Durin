#pragma once

#include "CoreMinimal.h"

#include "Asset/AssetWriteResult.h"
#include "Misc/FeedbackContext.h"
#include "AssetTools/AssetDeletion.h"
#include "AssetTools/AssetSave.h"
#include "AssetTools/MutationTypes.h"
#include "Asset/MutationExtensions.h"
#include "ContentBrowser/ContentBrowserContracts.h"
#include "Panels/ContentBrowserDataTypes.h"

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

	// Confirmation and execution share immutable scope. Only the service owns the
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

	// Narrow injection seam for deterministic destructive-deletion failure tests.
	struct FContentDeletionHooks
	{
		std::function<std::error_code(const std::filesystem::path&)> RemoveAll;
	};

	class FContentDeletionOperation final
	{
	public:
		explicit FContentDeletionOperation(
			FContentDeletionPlanPtr InPlan,
			FAssetDeletionOperation InAssetOperation);

		auto Execute(FContentDeletionHooks InHooks = {}) -> bool;
		auto HasStarted() const -> bool { return bStarted; }
		auto GetResult() const -> const FAssetWriteResult& { return Result; }

	private:
		auto ValidatePhysicalState(FAssetDeletionFileIdentities& OutIdentities) -> FAssetWriteResult;
		auto DeletePhysicalRoots(std::vector<std::filesystem::path>& RemovedPaths) -> FAssetWriteResult;

		FContentDeletionPlanPtr Plan;
		FAssetDeletionOperation AssetOperation;
		FAssetWriteResult Result;
		bool bStarted = false;
		FContentDeletionHooks Hooks;
	};
	// Owns filesystem and asset-registry mutations initiated by the content browser.
	class FContentBrowserOperationService
	{
	public:
		FContentBrowserOperationService(
			std::function<void(FContentChangeBatch)> InItemUpdates = {},
			std::function<bool()> InCanMutate = {});

		auto SetItemUpdateNotifier(std::function<void(FContentChangeBatch)> Notify) -> void
		{ ItemUpdates = std::move(Notify); }

		FContentBrowserOperationService(const FContentBrowserOperationService&) = delete;
		auto operator=(const FContentBrowserOperationService&) -> FContentBrowserOperationService& = delete;
		FContentBrowserOperationService(FContentBrowserOperationService&&) = delete;
		auto operator=(FContentBrowserOperationService&&) -> FContentBrowserOperationService& = delete;

		// Capability queries do not report diagnostics or authorize subsequent I/O.
		auto CanModify(std::string* OutErrorMsg = nullptr) const -> bool;
		auto CanSaveItem(const FPackagePath& Package, std::string* OutErrorMsg = nullptr) const -> bool;
		auto CanDuplicateItem(const FTopLevelAssetPath& Source, std::string* OutErrorMsg = nullptr) const -> bool;
		auto CanRenameItem(const FContentBrowserItem& Item, std::string_view NewName, std::string* OutErrorMsg = nullptr) const -> bool;
		auto CanMoveItem(const FContentBrowserItem& Item, std::string_view Destination, std::string* OutErrorMsg = nullptr) const -> bool;
		auto StopRequestAdmission() -> void { bAccepting = false; }
		// Diagnostics and committed item updates are independent of command return values.
		auto SetDiagnosticSink(FFeedbackContext::FMessageSink Sink) -> void { Diagnostics = std::move(Sink); }
		auto SaveItems(std::vector<FPackagePath> Packages, EAssetSaveMode Mode = EAssetSaveMode::LoadedDirtyPackage) -> bool;
		auto RenameItem(const FContentBrowserItem& Item, std::string_view NewName, FContentBrowserItem& OutNewItem) -> bool;
		auto DuplicateItem(const FContentBrowserItem& Item, FContentBrowserItem& OutNewItem) -> bool;
		auto DuplicateItem(const FTopLevelAssetPath& SourcePath, std::string_view DestinationDirectory, FContentBrowserItem& OutNewItem) -> bool;
		auto PrepareFolder(std::string_view PhysicalDirectory, FContentBrowserItem& OutNewItem) -> bool;
		auto CreateFolder(std::string_view PhysicalDirectory, std::string_view Name, FContentBrowserItem& OutNewItem) -> bool;
		auto MoveItems(std::span<const FContentBrowserItem> Items, std::string_view PhysicalDirectory) -> bool;
		auto CopyItems(std::span<const FContentBrowserItem> Items, std::string_view PhysicalDirectory) -> bool;
		auto FixUpRedirectorsInFolder(std::string_view VirtualDirectory) -> bool;
		auto FixUpRedirectors(std::span<const FPackagePath> Redirectors) -> bool;
		auto FixUpAllRedirectors() -> bool;
		auto BuildDeletionPlan(std::span<const FContentBrowserItem> Items) -> FContentDeletionPlanPtr;
		// Confirmation is replaced explicitly when stale. Physical completion succeeds
		// even if Registry reconciliation is pending; that failure is reported separately.
		auto DeleteItems(FContentDeletionPlanPtr& Confirmation, FContentDeletionHooks Hooks = {}) -> bool;
		auto DismissDeletion(FContentDeletionPlanPtr Confirmation) -> void;


	private:
		struct FContentMove
		{
			FContentBrowserItem Item;
			std::string Destination;
		};
		auto MakeMoveRequest(const FContentBrowserItem& Item, std::string_view Directory,
			std::optional<std::string_view> NewName, FContentMove& OutRequest) const -> FAssetWriteResult;
		auto ValidateMoveRequest(const FContentMove& Request) const -> FAssetWriteResult;
		auto MoveContent(std::span<const FContentMove> Requests, FContentChangeBatch& Changes) -> FAssetWriteResult;
		auto AnalyzeDeletion(std::span<const FContentBrowserItem> Items,
			FAssetDeletionOperation& OutOperation) const -> FContentDeletionPlanPtr;
		struct FDeletionSession
		{
			FContentDeletionPlanPtr Confirmation;
			std::vector<FContentBrowserItem> Request;
			std::unique_ptr<FContentDeletionOperation> Execution;
		};
		uint64 NextDeletionSession = 0;
		std::unordered_map<uint64, FDeletionSession> DeletionSessions;
		auto RenameItemImpl(const FContentBrowserItem& Item, std::string_view NewName, FContentBrowserItem& OutNewItem, FContentChangeBatch& Changes) -> FAssetWriteResult;
		auto CopyItemsImpl(std::span<const FContentBrowserItem> Items, std::string_view Directory, FContentChangeBatch& Changes) -> FAssetWriteResult;
		auto ValidateFolder(std::string_view Directory, std::string_view Name, FContentBrowserItem& OutNewItem) -> FAssetWriteResult;
		auto CollectRedirectors(std::string_view VirtualDirectory) const
			-> std::vector<FPackagePath>;

		auto ValidateMoves(std::span<const FAssetRelocationMapping> Moves) const -> FAssetWriteResult;
		auto NotifyItemsUpdated(FContentChangeBatch Changes) -> void;
		auto Complete(const FAssetWriteResult& Status, FContentChangeBatch Changes = {}) -> bool;
		auto CompleteMutation(const FAssetMutationResultDetails& Native, FContentChangeBatch Changes = {}) -> bool;
		auto ReportRecovery(const FAssetMutationResultDetails& Native) const -> void;
		auto Report(ELogLevel Level, std::string_view Message) const -> void;
		auto Feedback() const -> FFeedbackContext;
		FFeedbackContext::FMessageSink Diagnostics;
		std::function<bool()> MutationAllowed;
		bool bAccepting = true;
		std::function<void(FContentChangeBatch)> ItemUpdates;
	};
} // namespace Durin::Editor::ContentBrowser::Private
