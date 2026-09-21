#pragma once

#include "AssetTools/AssetDeletion.h"
#include "AssetTools/AssetDuplicate.h"
#include "AssetTools/AssetSave.h"
#include "AssetTools/AssetMutation.h"
#include "ContentBrowser/ContentBrowserContracts.h"
#include "Panels/ContentBrowserDataTypes.h"
#include "Operations/ContentBrowserPaths.h"

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
	// are preserved. Directory digests include every descendant's identity.
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

	// Confirmation and execution share immutable scope. Only the service owns the
	// move-only execution capability; SessionId is validated against its retained owner.
	struct FContentDeletionPlan
	{
		uint64 RegistryRevision = 0;
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

		auto Execute(FContentDeletionHooks InHooks = {}) -> FAssetOperationResult;
		auto IsConfirmationCurrent() -> bool;
		auto HasStarted() const -> bool { return bStarted; }
		auto GetResult() const -> const FAssetOperationResult& { return Result; }
		auto GetDetails() const -> const std::string& { return Details; }

	private:
		auto ValidatePhysicalState(FAssetDeletionFileIdentities* OutIdentities = nullptr) -> bool;
		auto DeletePhysicalRoots(std::vector<std::filesystem::path>& RemovedPaths) -> FAssetWriteResult;
		auto Fail(std::string Message) -> bool;

		FContentDeletionPlanPtr Plan;
		FAssetDeletionOperation AssetOperation;
		FAssetOperationResult Result{.Kind = EAssetOperationKind::Delete};
		bool bStarted = false;
		FContentDeletionHooks Hooks;
		std::string Details;
	};

	// Reports the post-operation focus requested by a successful content mutation.
	struct FContentBrowserOperationResult
	{
		FContentBrowserOperationResult() = default;
		FContentBrowserOperationResult(FAssetReadResult InStatus) : Status(AssetWriteResultFromRead(InStatus)) {}
		FContentBrowserOperationResult(FAssetWriteResult InStatus) : Status(std::move(InStatus)) {}
		FContentBrowserOperationResult(EAssetWriteError Error, std::string Message)
			: Status{Error, std::move(Message)} {}
		FContentBrowserOperationResult(FAssetOperationResult InAssetResult)
			: AssetResult(std::move(InAssetResult))
		{
			Status.Message = AssetResult->Message;
		}
		FAssetWriteResult Status;
		std::optional<FAssetOperationResult> AssetResult;
		bool bContentChanged = false;
		FContentChangeBatch Changes;
		FContentDeletionPlanPtr ReplacementConfirmation;
		std::string FocusPhysicalPath;
		std::string RevealAssetPath;
		std::string OpenAssetClassName;
		std::string Warning;

		explicit operator bool() const { return AssetResult
			? AssetResult->State == EAssetOperationTerminalState::Completed : static_cast<bool>(Status); }
	};

	// Injects only the package capabilities used by browser operations.
	struct FContentBrowserAssetServices
	{
		std::function<FAssetOperationResult(const FAssetSaveRequest&)> SaveAssets;
		std::function<FAssetOperationResult(const FAssetDuplicateRequest&)> DuplicateAsset;
		std::function<FAssetOperationResult(const FAssetRelocationRequest&)> RelocateAssets;
		std::function<FAssetOperationResult(const FAssetRedirectorFixupRequest&)> FixUpRedirectors;
		std::function<FAssetOperationResult(const FAssetDeletionRequest&, FAssetDeletionOperation&)> PrepareDeletion;
		static auto Default() -> FContentBrowserAssetServices;
	};

	// Owns filesystem and asset-registry mutations initiated by the content browser.
	class FContentBrowserOperationService
	{
	public:
		using FMoveAssets =
			std::function<FContentBrowserOperationResult(std::span<const FEditorAssetMove>)>;
		using FFixUpAssets =
			std::function<FContentBrowserOperationResult(std::span<const FPackagePath>)>;
		using FRemoveDirectory = std::function<bool(
			const std::filesystem::path&, std::error_code&)>;

		FContentBrowserOperationService(
			FContentBrowserPaths InPaths = {},
			FMoveAssets InMoveAssets = {},
			FRemoveDirectory InRemoveDirectory = {},
			FFixUpAssets InFixUpAssets = {},
			std::function<void()> InNotifyMountedContentMutation = {},
			std::function<bool()> InCanMutate = {},
			FContentBrowserAssetServices InAssets = FContentBrowserAssetServices::Default()
		);

		auto SetScopedContentNotifier(std::function<void(FContentChangeBatch)> Notify) -> void
		{ NotifyScopedContentMutation = std::move(Notify); }

		FContentBrowserOperationService(const FContentBrowserOperationService&) = delete;
		auto operator=(const FContentBrowserOperationService&) -> FContentBrowserOperationService& = delete;
		FContentBrowserOperationService(FContentBrowserOperationService&&) = delete;
		auto operator=(FContentBrowserOperationService&&) -> FContentBrowserOperationService& = delete;

		auto Save(std::vector<FPackagePath> Packages, EAssetSaveMode Mode = EAssetSaveMode::LoadedDirtyPackage)
			-> FContentBrowserOperationResult;
		auto QueryMutation() const -> FAssetWriteResult;
		auto QuerySave(const FPackagePath& Package) const -> FAssetWriteResult;
		auto QueryDuplicate(const FTopLevelAssetPath& Source) const -> FAssetWriteResult;
		auto StopRequestAdmission() -> void { bAccepting = false; }

		auto Rename(const FContentBrowserItem& Item, std::string_view NewName)
			-> FContentBrowserOperationResult;
		auto Duplicate(const FContentBrowserItem& Item)
			-> FContentBrowserOperationResult;
		auto Duplicate(
			const FTopLevelAssetPath& SourcePath,
			std::string_view DestinationDirectory)
			-> FContentBrowserOperationResult;
		auto CreateFolder(std::string_view PhysicalDirectory)
			-> FContentBrowserOperationResult;
		auto Move(std::span<const FEditorAssetMove> Moves) -> FContentBrowserOperationResult;
		auto FixUpRedirectorsInFolder(std::string_view VirtualDirectory)
			-> FContentBrowserOperationResult;
		auto FixUpRedirectors(std::span<const FPackagePath> Redirectors)
			-> FContentBrowserOperationResult;
		auto FixUpAllRedirectors() -> FContentBrowserOperationResult;

		auto BuildDeletionPlan(std::span<const FContentBrowserItem> Items) -> FContentDeletionPlanPtr;
		auto ExecuteDeletion(FContentDeletionPlanPtr Confirmation, FContentDeletionHooks Hooks = {})
			-> FContentBrowserOperationResult;
		auto DismissDeletion(FContentDeletionPlanPtr Confirmation) -> void;
		auto IsDeletionPlanCurrent(const FContentDeletionPlan& Plan) const -> bool;


	private:
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
		auto RenameFolder(
			const FContentBrowserItem& Item,
			std::string_view NewName,
			std::string& OutWarning)
			-> FContentBrowserOperationResult;
		auto CollectRedirectors(std::string_view VirtualDirectory) const
			-> std::vector<FPackagePath>;

		auto ValidateMoves(std::span<const FEditorAssetMove> Moves) const -> FAssetWriteResult;
		auto Publish(FContentBrowserOperationResult Result) -> FContentBrowserOperationResult;
		FContentBrowserAssetServices Assets;
		std::function<bool()> CanMutate;
		bool bAccepting = true;
		mutable FContentBrowserPaths Paths;
		FMoveAssets MoveAssets;
		FFixUpAssets FixUpAssets;
		std::function<void()> NotifyMountedContentMutation;
		std::function<void(FContentChangeBatch)> NotifyScopedContentMutation;
		FRemoveDirectory RemoveDirectory;
	};
} // namespace Durin::Editor::ContentBrowser::Private
