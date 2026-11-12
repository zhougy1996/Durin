#pragma once

#include "CoreMinimal.h"

#include "Asset/AssetWriteResult.h"
#include "Misc/FeedbackContext.h"
#include "AssetTools/AssetDeletion.h"
#include "AssetTools/AssetSave.h"
#include "AssetTools/MutationTypes.h"
#include "Asset/MutationExtensions.h"
#include "ContentBrowser/ContentBrowserContracts.h"
#include "Operations/ContentDeletionOperation.h"

namespace Durin::ContentBrowser::Private
{
	// Owns mutation admission and confirmed deletion state for one request scope.
	class FContentBrowserRequestContext
	{
	public:
		explicit FContentBrowserRequestContext(const FContentBrowserHostContext& Host = {});
		static auto Combine(std::vector<std::shared_ptr<FContentBrowserRequestContext>> Contexts) -> std::shared_ptr<FContentBrowserRequestContext>;
		auto IsAcceptingRequests() const -> bool { return bAccepting; }

		FContentBrowserRequestContext(const FContentBrowserRequestContext&) = delete;
		auto operator=(const FContentBrowserRequestContext&) -> FContentBrowserRequestContext& = delete;
		FContentBrowserRequestContext(FContentBrowserRequestContext&&) = delete;
		auto operator=(FContentBrowserRequestContext&&) -> FContentBrowserRequestContext& = delete;

		// Capability queries do not report diagnostics or authorize subsequent I/O.
		auto CanModify(std::string* OutErrorMsg = nullptr) const -> bool;
		auto CanSaveItem(const FContentBrowserItem& Item, EAssetSaveMode Mode = EAssetSaveMode::LoadedDirtyPackage, std::string* OutErrorMsg = nullptr) const -> bool;
		auto CanDuplicateItem(const FContentBrowserItem& Item, std::string* OutErrorMsg = nullptr) const -> bool;
		auto CanRenameItem(const FContentBrowserItem& Item, std::string_view NewName, std::string* OutErrorMsg = nullptr) const -> bool;
		auto CanMoveItem(const FContentBrowserItem& Item, std::string_view Destination, std::string* OutErrorMsg = nullptr) const -> bool;
		auto CanCopyItem(const FContentBrowserItem& Item, std::string_view Destination, std::string* OutErrorMsg = nullptr) const -> bool;
		auto StopRequestAdmission() -> void { bAccepting = false; DeletionSessions.clear(); }
		// Diagnostics and committed item updates are independent of command return values.
		auto SetDiagnosticSink(FFeedbackContext::FMessageSink Sink) -> void { Diagnostics = std::move(Sink); }
		auto BulkSaveItems(std::span<const FContentBrowserItem> Items, EAssetSaveMode Mode = EAssetSaveMode::LoadedDirtyPackage) -> bool;
		auto RenameItem(const FContentBrowserItem& Item, std::string_view NewName, FContentBrowserItem& OutNewItem) -> bool;
		auto DuplicateItem(const FContentBrowserItem& Item, FContentBrowserItem& OutNewItem) -> bool;
		auto DuplicateItem(const FContentBrowserItem& Item, std::string_view PhysicalDirectory, FContentBrowserItem& OutNewItem) -> bool;
		auto CanCreateFolder(std::string_view PhysicalDirectory, std::string_view Name, std::string* OutErrorMsg = nullptr) const -> bool;
		auto PrepareFolder(std::string_view PhysicalDirectory, FContentBrowserItem& OutNewItem) -> bool;
		auto CreateFolder(std::string_view PhysicalDirectory, std::string_view Name, FContentBrowserItem& OutNewItem) -> bool;
		auto BulkMoveItems(std::span<const FContentBrowserItem> Items, std::string_view PhysicalDirectory) -> bool;
		auto BulkCopyItems(std::span<const FContentBrowserItem> Items, std::string_view PhysicalDirectory) -> bool;
		auto FixUpRedirectorsInFolder(std::string_view VirtualDirectory) -> bool;
		auto FixUpRedirectors(std::span<const FPackagePath> Redirectors) -> bool;
		auto FixUpAllRedirectors() -> bool;
		auto PrepareDeleteItems(std::span<const FContentBrowserItem> Items) -> FContentDeletionPlanPtr;
		// Confirmation is replaced explicitly when stale. Physical completion succeeds
		// even if Registry reconciliation is pending; that failure is reported separately.
		auto DeleteItems(FContentDeletionPlanPtr& Confirmation, FContentDeletionHooks Hooks = {}) -> bool;
		auto DismissDeletion(FContentDeletionPlanPtr Confirmation) -> void;

	private:
		auto CanDuplicateAsset(const FTopLevelAssetPath& Source, std::string* OutErrorMsg) const -> bool;
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
		auto ValidateFolder(std::string_view Directory, std::string_view Name, FContentBrowserItem& OutNewItem) const -> FAssetWriteResult;
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
} // namespace Durin::ContentBrowser::Private
