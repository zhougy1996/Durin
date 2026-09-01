#pragma once

#include "Asset/Deletion.h"
#include "AssetTools/AssetDeletion.h"
#include "ContentBrowser/ContentBrowserContracts.h"
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
	// confirmation modal and destructive operation therefore observe the same immutable scope.
	struct FContentDeletionPlan
	{
		uint64 RegistryRevision = 0;
		std::string DisplayName;
		FContentDeletionSummary Summary;
		std::vector<FContentDeletionRoot> MaximalRoots;
		std::vector<FContentDeletionFingerprint> Entries;
		std::vector<FContentDeletionBlocker> Blockers;
		std::vector<FContentDeletionWarning> Warnings;
		mutable FAssetDeletionOperation AssetOperation;

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
			FContentDeletionHooks InHooks = {});

		auto Execute() -> bool;
		auto GetDetails() const -> const std::string& { return Details; }

	private:
		auto ValidatePhysicalState() -> bool;
		auto DeletePhysicalRoots() -> FAssetResult;
		auto Fail(std::string Message) -> bool;

		FContentDeletionPlanPtr Plan;
		FContentDeletionHooks Hooks;
		std::string Details;
	};

	// Reports the post-operation focus requested by a successful content mutation.
	struct FContentBrowserOperationResult
	{
		FAssetResult Status;
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
			std::function<FAssetResult(std::span<const FEditorAssetMove>)>;
		using FFixUpAssets =
			std::function<FAssetResult(std::span<const FPackagePath>)>;
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
			const FTopLevelAssetPath& SourcePath,
			std::string_view DestinationDirectory)
			-> FContentBrowserOperationResult;
		auto CreateFolder(std::string_view PhysicalDirectory)
			-> FContentBrowserOperationResult;
		auto Move(std::span<const FEditorAssetMove> Moves) -> FAssetResult;
		auto FixUpRedirectorsInFolder(std::string_view VirtualDirectory)
			-> FAssetResult;
		auto FixUpRedirectors(std::span<const FPackagePath> Redirectors)
			-> FAssetResult;
		auto FixUpAllRedirectors() -> FAssetResult;

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
			-> FAssetResult;
		auto CollectRedirectors(std::string_view VirtualDirectory) const
			-> std::vector<FPackagePath>;

		FContentBrowserModel& Model;
		FMoveAssets MoveAssets;
		FFixUpAssets FixUpAssets;
		std::function<void()> NotifyMountedContentMutation;
		FRemoveDirectory RemoveDirectory;
	};
} // namespace Durin::Editor::ContentBrowser::Private
