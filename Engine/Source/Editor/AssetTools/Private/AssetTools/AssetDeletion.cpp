#include "AssetTools/IAssetTools.h"
#include "AssetDeletionInternal.h"
#include "AssetOperationResultInternal.h"
#include "Asset/Load.h"
#include "Asset/MutationExtensions.h"
#include "Asset/PackageRemoval.h"
#include "AssetRegistry/Publication.h"
#include "DObject/Package.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

namespace Durin
{
	namespace
	{
		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		auto AppendReferenceStoreWarnings(std::span<const FPackagePath> Paths,
			const FAssetReferenceStoreCapture& Capture,
			std::vector<FAssetDeletionWarning>& Warnings) -> void
		{
			for (const auto& Snapshot : Capture.Stores)
				for (const FPackagePath& Path : Paths)
				{
					std::vector<std::string> Occurrences;
					for (const auto& Occurrence : Snapshot.Occurrences)
						if (Occurrence.TargetPath == Path)
							Occurrences.push_back(std::format("{}:{} ({})",
								Occurrence.ProviderId, Occurrence.StableId, Occurrence.DisplayRoute));
					std::ranges::sort(Occurrences);
					if (Occurrences.empty()) continue;
					const size_t Count = Occurrences.size();
					Warnings.push_back({.AssetPath = Path,
						.ExternalOccurrences = std::move(Occurrences),
						.Details = std::format(
							"Deleting {} leaves {} persistent external owner occurrence(s) dangling. Run Fix Up Redirectors or update those owners before confirming.",
							Path.ToString(), Count)});
				}
		}
	}

	struct FAssetDeletionOperation::FState
	{
		uint64 RegistryRevision = 0;
		FAssetReferenceStoreCapture ReferenceStores;
		std::vector<FAssetDeletionEntry> Entries;
		std::vector<FAssetDeletionWarning> Warnings;
		std::vector<FAssetDeletionBlocker> Blockers;
		std::vector<std::filesystem::path> PhysicalRoots;
		bool bPrepared = false;
		bool bDeleted = false;
		bool bForwardPending = false;
		FAssetRegistrySnapshot RecoverySnapshot;
		std::unordered_map<std::string, FXxHash128> ConfirmedBytes;
		std::unordered_set<std::string> RemovedFiles;
		std::unordered_map<FPackagePath, std::vector<std::filesystem::path>> OutsideCompanions;

		auto CaptureRecoveryState() -> FAssetResult;
		auto ValidateRecoveryState() -> FAssetResult;
		auto RecordRemovedFiles() -> void;

		auto Prepare(std::span<const FPackagePath> Paths,
			std::span<const std::filesystem::path> Roots,
			std::vector<FAssetDeletionBlocker>& OutBlockers) -> FAssetResult;
		auto Validate(std::vector<FAssetDeletionBlocker>& OutBlockers) -> FAssetResult;
		auto Delete(const FAssetDeletionCommit& Commit) -> FAssetResult;
	};

	FAssetDeletionOperation::FAssetDeletionOperation() = default;
	FAssetDeletionOperation::~FAssetDeletionOperation() = default;
	FAssetDeletionOperation::FAssetDeletionOperation(FAssetDeletionOperation&&) noexcept = default;
	auto FAssetDeletionOperation::operator=(FAssetDeletionOperation&&) noexcept
		-> FAssetDeletionOperation& = default;

	auto FAssetDeletionOperation::GetRegistryRevision() const -> uint64
	{
		return State ? State->RegistryRevision : 0;
	}

	auto FAssetDeletionOperation::GetEntries() const
		-> std::span<const FAssetDeletionEntry>
	{
		return State ? std::span<const FAssetDeletionEntry>(State->Entries)
			: std::span<const FAssetDeletionEntry>{};
	}

	auto FAssetDeletionOperation::GetWarnings() const
		-> std::span<const FAssetDeletionWarning>
	{
		return State ? std::span<const FAssetDeletionWarning>(State->Warnings)
			: std::span<const FAssetDeletionWarning>{};
	}

	auto FAssetDeletionOperation::GetBlockers() const -> std::span<const FAssetDeletionBlocker>
	{
		return State ? std::span<const FAssetDeletionBlocker>(State->Blockers)
			: std::span<const FAssetDeletionBlocker>{};
	}

	auto FAssetDeletionOperation::Delete(const FAssetDeletionCommit& Commit) -> FAssetOperationResult
	{
		const FAssetResult Result = State ? State->Delete(Commit)
			: Error(EAssetError::StaleData, "The asset deletion operation is not prepared.");
		FAssetOperationResult Operation = AssetToolsPrivate::FromEngineResult(EAssetOperationKind::Delete, Result);
		if (State && State->bForwardPending
			&& Operation.State == EAssetOperationTerminalState::Rejected)
			Operation.State = EAssetOperationTerminalState::ForwardPending;
		for (const auto& Entry : GetEntries())
			Operation.AffectedAssets.push_back(Entry.RegistryEntry.PackagePath);
		for (const auto& Warning : GetWarnings())
			Operation.Warnings.push_back({Warning.AssetPath, Warning.Details});
		if (Result) Operation.Persistence = EAssetOperationPersistenceState::Persisted;
		return Operation;
	}

	auto FAssetDeletionOperation::FState::Delete(
		const FAssetDeletionCommit& Commit) -> FAssetResult
	{
		if (!bPrepared || bDeleted)
			return Error(EAssetError::StaleData,
				"Only a prepared asset deletion job can execute.");
		if (!Commit.Delete)
			return Error(EAssetError::StaleData,
				"The asset deletion job has no destructive callback.");

		std::vector<FAssetDeletionBlocker> Blockers;
		FAssetResult Result =
			bForwardPending ? ValidateRecoveryState() : Validate(Blockers);
		if (!Result) return Result;
		if (!Blockers.empty())
			return Error(EAssetError::InUse, Blockers.front().Details);
		if (GetAssetCatalogRevision() != RegistryRevision)
			return Error(EAssetError::StaleData,
				"The asset Registry changed after deletion confirmation.");
		if (!bForwardPending)
		{
			Result = CaptureRecoveryState();
			if (!Result) return Result;
		}
		std::vector<FAssetData> Packages;
		for (const FAssetDeletionEntry& Entry : Entries)
			if (FindAssetExact(Entry.RegistryEntry.PackagePath))
				Packages.push_back(Entry.RegistryEntry);
		Result = ReleasePackagesForRemoval(Packages, RegistryRevision);
		if (!Result) return Result;
		const FAssetResult DeleteResult = Commit.Delete();
		RecordRemovedFiles();
		if (!DeleteResult)
		{
			bForwardPending = true;
			std::vector<FPackagePath> Paths;
			for (const FAssetDeletionEntry& Entry : Entries)
				Paths.push_back(Entry.RegistryEntry.PackagePath);
			FenceAssetRegistryProjection(Paths);
			return {
				.Error = DeleteResult.Error,
				.Message = std::format(
					"AssetDeletionForwardPending: deletion is irreversible; retry the remaining paths. {}",
					DeleteResult.Message),
				.Disposition = EAssetResultDisposition::ForwardPending,
				.DesiredDirection = "DeleteRemaining"};
		}
		Result = PublishPackageRemoval(Packages, RegistryRevision);
		if (!Result)
		{
			std::vector<FPackagePath> Paths;
			for (const FAssetDeletionEntry& Entry : Entries)
				Paths.push_back(Entry.RegistryEntry.PackagePath);
			FenceAssetRegistryProjection(Paths);
			bDeleted = true;
			bForwardPending = false;
			return {
				.Error = EAssetError::StaleData,
				.Message = std::format(
					"ContentCommittedProjectionPending: destructive deletion committed; Registry reconcile is required. {}",
					Result.Message),
				.Disposition = EAssetResultDisposition::ContentCommittedProjectionPending};
		}
		bDeleted = true;
		bForwardPending = false;
		return {};
	}

	// Recovery keeps the original safety scope even when some selected packages
	// have disappeared. Outside state must remain identical; new scope needs a new
	// user decision and can never be silently folded into destructive retry.
	auto FAssetDeletionOperation::FState::CaptureRecoveryState() -> FAssetResult
	{
		RecoverySnapshot = CaptureAssetRegistrySnapshot();
		ConfirmedBytes.clear();
		OutsideCompanions.clear();
		std::unordered_set<FPackagePath> Selected;
		for (const auto& Entry : Entries)
		{
			Selected.insert(Entry.RegistryEntry.PackagePath);
			std::vector<std::filesystem::path> Files = Entry.CompanionFiles;
			Files.push_back(Entry.RegistryEntry.PhysicalPath);
			for (const auto& File : Files)
			{
				FXxHash128 Identity;
				std::error_code ErrorCode;
				if (!FFileHelper::HashFileXx128(File, Identity, ErrorCode))
					return Error(EAssetError::IoError, "Could not capture deletion recovery byte identity.");
				ConfirmedBytes.emplace(File.generic_string(), Identity);
			}
		}
		for (const auto& [Path, Data] : RecoverySnapshot.Catalog.Assets)
		{
			if (Selected.contains(Path)) continue;
			std::vector<std::filesystem::path> Files;
			if (!AssetToolsPrivate::InspectAssetCompanionFilesForDeletion(Data, Files)) continue;
			OutsideCompanions.emplace(Path, std::move(Files));
		}
		return {};
	}

	auto FAssetDeletionOperation::FState::RecordRemovedFiles() -> void
	{
		for (const auto& [File, Identity] : ConfirmedBytes)
		{
			std::error_code ErrorCode;
			const auto Status = std::filesystem::symlink_status(File, ErrorCode);
			if ((!ErrorCode || ErrorCode == std::errc::no_such_file_or_directory)
				&& Status.type() == std::filesystem::file_type::not_found) RemovedFiles.insert(File);
		}
	}

	auto FAssetDeletionOperation::FState::ValidateRecoveryState() -> FAssetResult
	{
		if (GetAssetRuntimeConfiguration().IsCooked())
			return Error(EAssetError::ReadOnlyMode, "Cooked content cannot be deleted.");
		const auto Current = CaptureAssetRegistrySnapshot();
		std::unordered_set<FPackagePath> Selected;
		for (const auto& Entry : Entries) Selected.insert(Entry.RegistryEntry.PackagePath);
		for (const auto& [Path, Data] : Current.Catalog.Assets)
		{
			const auto Before = RecoverySnapshot.Catalog.Assets.find(Path);
			if (Before == RecoverySnapshot.Catalog.Assets.end() || !(Before->second == Data))
				return Error(EAssetError::StaleData, "Asset metadata changed during deletion recovery.");
		}
		for (const auto& [Path, Data] : RecoverySnapshot.Catalog.Assets)
			if (!Current.Catalog.Assets.contains(Path)
				&& (!Selected.contains(Path) || !RemovedFiles.contains(Data.PhysicalPath)))
				return Error(EAssetError::StaleData, "Unconfirmed catalog removal occurred during deletion recovery.");
		auto OutsideEdges = [&](const FAssetReferenceIndex& Index) {
			std::vector<FAssetPackageReferenceEdge> Edges;
			for (const auto& Edge : Index.GetEdges())
				if (!Selected.contains(Edge.SourcePackage)) Edges.push_back(Edge);
			return Edges;
		};
		if (OutsideEdges(Current.References) != OutsideEdges(RecoverySnapshot.References)
			|| Current.References.IsComplete() != RecoverySnapshot.References.IsComplete())
			return Error(EAssetError::InUse, "Reference warnings changed during deletion recovery.");
		FAssetReferenceStoreCapture Stores;
		if (!Entries.empty())
		{
			const auto Captured = CaptureAssetReferenceStores(Stores);
			if (!Captured) return Captured;
			if (Stores != ReferenceStores)
				return Error(EAssetError::InUse, "External reference owners changed during deletion recovery.");
		}
		std::unordered_map<FPackagePath, std::vector<std::filesystem::path>> Companions;
		for (const auto& [Path, Data] : Current.Catalog.Assets)
		{
			if (Selected.contains(Path)) continue;
			std::vector<std::filesystem::path> Files;
			if (!AssetToolsPrivate::InspectAssetCompanionFilesForDeletion(Data, Files)) continue;
			Companions.emplace(Path, std::move(Files));
		}
		if (Companions != OutsideCompanions)
			return Error(EAssetError::InUse, "Companion ownership changed during deletion recovery.");
		for (const auto& Entry : Entries)
			if (IsPackageLoading(Entry.RegistryEntry.PackagePath)
				|| (FindResidentPackage(Entry.RegistryEntry.PackagePath)
					&& FindResidentPackage(Entry.RegistryEntry.PackagePath)->IsDirty()))
				return Error(EAssetError::InUse, "A deletion participant is loading or dirty.");
		for (const auto& [File, Expected] : ConfirmedBytes)
		{
			std::error_code ErrorCode;
			const auto Status = std::filesystem::symlink_status(File, ErrorCode);
			if (RemovedFiles.contains(File))
			{
				if ((!ErrorCode || ErrorCode == std::errc::no_such_file_or_directory)
					&& Status.type() == std::filesystem::file_type::not_found) continue;
				return Error(EAssetError::InUse, "A deleted asset file was replaced.");
			}
			FXxHash128 Actual;
			if (ErrorCode || !std::filesystem::is_regular_file(Status)
				|| !FFileHelper::HashFileXx128(File, Actual, ErrorCode) || Actual != Expected)
				return Error(EAssetError::InUse, "Remaining asset bytes changed during deletion recovery.");
		}
		RegistryRevision = Current.Revision;
		return {};
	}

	auto FAssetDeletionOperation::FState::Prepare(
		std::span<const FPackagePath> Paths,
		std::span<const std::filesystem::path> PhysicalRoots,
		std::vector<FAssetDeletionBlocker>& OutBlockers) -> FAssetResult
	{
		auto& OutToken = *this;
		OutBlockers.clear();
		if (GetAssetRuntimeConfiguration().IsCooked())
			return Error(
				EAssetError::ReadOnlyMode,
				"Cooked runtime package mode does not permit asset deletion.");

		std::vector<FPackagePath> SortedPaths(Paths.begin(), Paths.end());
		std::ranges::sort(
			SortedPaths,
			[](const FPackagePath& A, const FPackagePath& B) {
				return A.GetView() < B.GetView();
			});
		SortedPaths.erase(
			std::unique(SortedPaths.begin(), SortedPaths.end()),
			SortedPaths.end());
		const std::unordered_set<FPackagePath> DeletionSet(
			SortedPaths.begin(), SortedPaths.end());
		OutToken.RegistryRevision = GetAssetCatalogRevision();

		OutToken.PhysicalRoots.reserve(PhysicalRoots.size());
		for (const std::filesystem::path& Root : PhysicalRoots)
			OutToken.PhysicalRoots.push_back(
				std::filesystem::absolute(Root).lexically_normal());

		auto AddBlocker = [&](EAssetDeletionBlocker Kind,
			const FPackagePath& AssetPath,
			const FPackagePath& RelatedAssetPath,
			std::filesystem::path PhysicalPath,
			std::string Details) {
			OutBlockers.push_back({
				.Kind = Kind,
				.AssetPath = AssetPath,
				.RelatedAssetPath = RelatedAssetPath,
				.PhysicalPath = std::move(PhysicalPath),
				.Details = std::move(Details)});
		};

		for (const FPackagePath& Path : SortedPaths)
		{
			const FAssetCatalogEntry Data = FindAssetExact(Path);
			if (!Path.IsValid() || !Data)
			{
				AddBlocker(
					EAssetDeletionBlocker::MissingAsset,
					Path,
					{},
					{},
					std::format("Asset {} was not found.", Path.ToString()));
				continue;
			}

			FAssetDeletionEntry Entry{
				.RegistryEntry = *Data,
				.bLoaded = FindResidentPackage(Path) != nullptr};
			if (IsPackageLoading(Path))
				AddBlocker(
					EAssetDeletionBlocker::LoadingPackage,
					Path,
					{},
					Data->PhysicalPath,
					"Asset is currently loading.");
			if (DPackage* Loaded = FindResidentPackage(Path);
				Loaded && Loaded->IsDirty())
				AddBlocker(
					EAssetDeletionBlocker::DirtyPackage,
					Path,
					{},
					Data->PhysicalPath,
					"Asset has unsaved changes.");

			const FAssetResult CompanionResult =
				AssetToolsPrivate::InspectAssetCompanionFilesForDeletion(
					*Data, Entry.CompanionFiles);
			if (!CompanionResult)
				AddBlocker(
					EAssetDeletionBlocker::CompanionInspectionFailed,
					Path,
					{},
					Data->PhysicalPath,
					CompanionResult.Message);
			OutToken.Entries.push_back(std::move(Entry));
		}

		// Deletion is closed over final targets and every alias that resolves to them.
		// This prevents an alias-only delete from silently invalidating authored old paths,
		// and prevents a target delete from leaving redirectors with no destination.
		std::unordered_map<FPackagePath, std::vector<FPackagePath>> RedirectorsByTarget;
		for (const auto& [AliasPath, AliasData] : CaptureAssetCatalogSnapshot().Assets)
		{
			if (AliasData.EntryKind != EAssetRegistryEntryKind::Redirector) continue;
			const FAssetPathResolveResult Resolution =
				Durin::ResolveAssetPath(AliasPath);
			if (!Resolution) continue;
			RedirectorsByTarget[Resolution.FinalPath].push_back(AliasPath);
		}
		for (auto& [AssetPath, Redirectors] : RedirectorsByTarget)
			std::ranges::sort(
				Redirectors,
				[](const FPackagePath& A, const FPackagePath& B) {
					return A.GetView() < B.GetView();
				});

		for (const FPackagePath& Path : SortedPaths)
		{
			const FAssetCatalogEntry Data = FindAssetExact(Path);
			if (!Data) continue;
			if (Data->EntryKind == EAssetRegistryEntryKind::Redirector)
			{
				const FAssetPathResolveResult Resolution =
					Durin::ResolveAssetPath(Path);
				if (!Resolution || !DeletionSet.contains(Resolution.FinalPath))
				{
					const FPackagePath Related = Resolution.FinalPath.IsValid()
						? Resolution.FinalPath
						: Data->RedirectDestination;
					AddBlocker(
						EAssetDeletionBlocker::RedirectorTargetNotSelected,
						Path,
						Related,
						Data->PhysicalPath,
						Resolution
							? std::format(
								"Redirector {} cannot be deleted alone. Select its final target {} and every alias to that target, or run Fix Up Redirectors.",
								Path.ToString(), Resolution.FinalPath.ToString())
							: std::format(
								"Redirector {} cannot be deleted because its target does not resolve. Repair the redirector before deleting it.",
								Path.ToString()));
				}
				continue;
			}

			const auto Found = RedirectorsByTarget.find(Path);
			if (Found == RedirectorsByTarget.end()) continue;
			std::vector<FPackagePath> SelectedRedirectors;
			for (const FPackagePath& Redirector : Found->second)
			{
				if (DeletionSet.contains(Redirector))
					SelectedRedirectors.push_back(Redirector);
				else
				{
					const FAssetCatalogEntry RedirectorData = FindAssetExact(Redirector);
					AddBlocker(
						EAssetDeletionBlocker::TargetRedirectorsNotSelected,
						Path,
						Redirector,
						RedirectorData
							? std::filesystem::path(RedirectorData->PhysicalPath)
							: std::filesystem::path{},
						std::format(
							"Asset {} still has redirector {}. Reveal redirectors and include every alias, or run Fix Up Redirectors before deleting the target.",
							Path.ToString(), Redirector.ToString()));
				}
			}
			if (!SelectedRedirectors.empty())
				OutToken.Warnings.push_back({
					.AssetPath = Path,
					.RedirectorPaths = std::move(SelectedRedirectors),
					.Details = std::format(
						"Deleting {} together with {} redirector(s) permanently invalidates every authored old path.",
						Path.ToString(), Found->second.size())});
		}

		const FAssetReferenceIndex ReferenceIndex = CaptureAssetReferenceIndex();
		for (const FPackagePath& Path : SortedPaths)
		{
			std::vector<FPackagePath> SoftReferencers;
			for (const FAssetPackageReferenceEdge& Edge :
				 ReferenceIndex.FindReferencers(Path))
			{
				if (Edge.Kind != EAssetReferenceKind::SoftObject
					|| DeletionSet.contains(Edge.SourcePackage))
					continue;
				SoftReferencers.push_back(Edge.SourcePackage);
			}
			std::ranges::sort(
				SoftReferencers,
				[](const FPackagePath& A, const FPackagePath& B) {
					return A.GetView() < B.GetView();
				});
			SoftReferencers.erase(
				std::unique(SoftReferencers.begin(), SoftReferencers.end()),
				SoftReferencers.end());
			if (!SoftReferencers.empty())
			{
				const size_t SoftReferencerCount = SoftReferencers.size();
				OutToken.Warnings.push_back({
					.AssetPath = Path,
					.SoftReferencerPaths = std::move(SoftReferencers),
					.Details = std::format(
						"Deleting {} leaves {} package(s) with dangling soft references. Review the referencers before confirming.",
						Path.ToString(),
						SoftReferencerCount)});
			}
		}
		if (!SortedPaths.empty() && !ReferenceIndex.IsComplete())
			OutToken.Warnings.push_back({
				.AssetPath = SortedPaths.front(),
				.Details = std::format(
					"The package reference index is incomplete ({} error{}), so soft-reference warning counts may be incomplete.",
					ReferenceIndex.GetErrors().size(),
					ReferenceIndex.GetErrors().size() == 1 ? "" : "s")});

		if (!SortedPaths.empty())
		{
			const FAssetResult Captured = CaptureAssetReferenceStores(ReferenceStores);
			if (!Captured)
				AddBlocker(EAssetDeletionBlocker::ReferenceStoreInspectionFailed,
					SortedPaths.front(), {}, {}, Captured.Message);
			else
				AppendReferenceStoreWarnings(SortedPaths, ReferenceStores, Warnings);
		}
		std::ranges::sort(
			OutToken.Warnings,
			[](const FAssetDeletionWarning& A,
				const FAssetDeletionWarning& B) {
				if (A.AssetPath.GetView() != B.AssetPath.GetView())
					return A.AssetPath.GetView() < B.AssetPath.GetView();
				return A.Details < B.Details;
			});

		for (const auto& [OtherPath, OtherData] : CaptureAssetCatalogSnapshot().Assets)
		{
			if (DeletionSet.contains(OtherPath)) continue;
			for (const FPackagePath& Dependency : OtherData.Dependencies)
			{
				if (!DeletionSet.contains(Dependency)) continue;
				// Redirect hard blockers have dedicated actionable closure diagnostics.
				if (OtherData.EntryKind == EAssetRegistryEntryKind::Redirector)
					continue;
				const bool bLoadedReference = FindResidentPackage(OtherPath) != nullptr;
				AddBlocker(
					bLoadedReference
						? EAssetDeletionBlocker::ExternalLoadedReference
						: EAssetDeletionBlocker::ExternalPersistentReference,
					Dependency,
					OtherPath,
					OtherData.PhysicalPath,
					std::format(
						"Asset {} is referenced by {}.",
						Dependency.ToString(),
						OtherPath.ToString()));
			}
		}

		std::unordered_map<std::string, std::vector<FPackagePath>> CompanionOwners;
		for (const auto& [OwnerPath, OwnerData] : CaptureAssetCatalogSnapshot().Assets)
		{
			std::vector<std::filesystem::path> Files;
			if (!AssetToolsPrivate::InspectAssetCompanionFilesForDeletion(
					OwnerData, Files))
				continue;
			for (const std::filesystem::path& File : Files)
				CompanionOwners[File.generic_string()].push_back(OwnerPath);
		}
		for (const FAssetDeletionEntry& Entry : OutToken.Entries)
		{
			for (const std::filesystem::path& File : Entry.CompanionFiles)
			{
				auto Owners = CompanionOwners[File.generic_string()];
				std::ranges::sort(
					Owners,
					[](const FPackagePath& A, const FPackagePath& B) {
						return A.GetView() < B.GetView();
					});
				Owners.erase(std::unique(Owners.begin(), Owners.end()), Owners.end());
				if (Owners.size() > 1)
					AddBlocker(
						EAssetDeletionBlocker::CompanionOwnershipConflict,
						Entry.RegistryEntry.PackagePath,
						{},
						File,
						"Companion file is claimed by multiple assets.");
				for (const FPackagePath& Owner : Owners)
					if (!DeletionSet.contains(Owner))
						AddBlocker(
							EAssetDeletionBlocker::ExternalCompanionOwner,
							Entry.RegistryEntry.PackagePath,
							Owner,
							File,
							std::format(
								"Companion file is owned by asset {} outside the deletion set.",
								Owner.ToString()));
			}
		}
		for (const auto& [PhysicalPath, Owners] : CompanionOwners)
		{
			const bool bInsidePhysicalRoot = std::ranges::any_of(
				PhysicalRoots,
				[&](const std::filesystem::path& Root) {
					const std::string NormalizedRoot =
						std::filesystem::absolute(Root).lexically_normal().generic_string();
					return PhysicalPath == NormalizedRoot
						|| FPaths::IsLexicalDescendantPath(
							PhysicalPath, NormalizedRoot, true);
				});
			if (!bInsidePhysicalRoot) continue;
			for (const FPackagePath& Owner : Owners)
				if (!DeletionSet.contains(Owner))
					AddBlocker(
						EAssetDeletionBlocker::ExternalCompanionOwner,
						Owner,
						Owner,
						PhysicalPath,
						std::format(
							"Selected content contains a companion owned by asset {} outside the deletion set.",
							Owner.ToString()));
		}

		std::ranges::sort(
			OutBlockers,
			[](const FAssetDeletionBlocker& A,
				const FAssetDeletionBlocker& B) {
				if (A.AssetPath.GetView() != B.AssetPath.GetView())
					return A.AssetPath.GetView() < B.AssetPath.GetView();
				if (A.RelatedAssetPath.GetView() != B.RelatedAssetPath.GetView())
					return A.RelatedAssetPath.GetView()
						< B.RelatedAssetPath.GetView();
				if (A.PhysicalPath != B.PhysicalPath)
					return A.PhysicalPath.generic_string()
						< B.PhysicalPath.generic_string();
				return A.Kind < B.Kind;
			});
		return {};
	}

	auto FAssetDeletionOperation::FState::Validate(
		std::vector<FAssetDeletionBlocker>& OutBlockers) -> FAssetResult
	{
		const auto& Token = *this;
		std::vector<FPackagePath> Paths;
		Paths.reserve(Token.Entries.size());
		for (const FAssetDeletionEntry& Entry : Token.Entries)
			Paths.push_back(Entry.RegistryEntry.PackagePath);

		FState Current;
		FAssetResult Result = Current.Prepare(
			Paths, Token.PhysicalRoots, OutBlockers);
		if (!Result || !OutBlockers.empty()) return Result;
		if (Current.Entries.size() != Token.Entries.size())
			return Error(EAssetError::InUse,
				"The asset deletion set changed after confirmation.");
		if (Current.ReferenceStores != Token.ReferenceStores)
			return Error(EAssetError::InUse,
				"Persistent asset-reference owners changed after deletion confirmation.");
		if (Current.Warnings != Token.Warnings)
			return Error(EAssetError::InUse,
				"Asset references changed after deletion confirmation.");
		for (size_t Index = 0; Index < Token.Entries.size(); ++Index)
		{
			const FAssetDeletionEntry& Expected = Token.Entries[Index];
			const FAssetDeletionEntry& Actual = Current.Entries[Index];
			if (!(Expected.RegistryEntry == Actual.RegistryEntry)
				|| Expected.CompanionFiles != Actual.CompanionFiles)
				return Error(EAssetError::InUse, std::format(
					"Asset {} changed after confirmation.",
					Expected.RegistryEntry.PackagePath.ToString()));
		}
		return {};
	}

	auto PrepareAssetDeletionWithEditorPolicy(const FAssetDeletionRequest& Request,
		FAssetDeletionOperation& OutOperation) -> FAssetOperationResult
	{
		OutOperation = FAssetDeletionOperation{};
		OutOperation.State = std::make_unique<FAssetDeletionOperation::FState>();
		auto& State = *OutOperation.State;
		const FAssetResult Prepared = State.Prepare(Request.AssetPaths, Request.PhysicalRoots, State.Blockers);
		State.bPrepared = Prepared && State.Blockers.empty();
		FAssetOperationResult Result = AssetToolsPrivate::FromEngineResult(
			EAssetOperationKind::Delete, Prepared, Request.AssetPaths);
		for (const auto& Warning : State.Warnings)
			Result.Warnings.push_back({Warning.AssetPath, Warning.Details});
		if (!State.Blockers.empty()) Result.State = EAssetOperationTerminalState::Rejected;
		return Result;
	}
}
