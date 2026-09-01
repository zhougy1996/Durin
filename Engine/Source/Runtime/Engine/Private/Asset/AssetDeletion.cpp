#include "AssetRuntimeStateInternal.h"
#include "AssetDeletionInternal.h"
#include "AssetMutationReferenceInternal.h"

#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Misc/Paths.h"

namespace Durin
{
	namespace
	{
		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}
	}

	auto FAssetDeletionJob::GetRegistryRevision() const -> uint64
	{
		return State ? State->RegistryRevision : 0;
	}

	auto FAssetDeletionJob::GetEntries() const
		-> std::span<const FAssetDeletionBatchEntry>
	{
		return State ? std::span<const FAssetDeletionBatchEntry>(State->Entries)
			: std::span<const FAssetDeletionBatchEntry>{};
	}

	auto FAssetDeletionJob::GetWarnings() const
		-> std::span<const FAssetDeletionBatchWarning>
	{
		return State ? std::span<const FAssetDeletionBatchWarning>(State->Warnings)
			: std::span<const FAssetDeletionBatchWarning>{};
	}

	auto FAssetMutationCoordinator::AnalyzeAssetDeletion(
		const FPackagePath& Path,
		FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult
	{
		OutAnalysis = {};
		OutAnalysis.AssetPath = Path;
		const FAssetCatalogEntry Data = FindAssetExact(Path);
		if (!Path.IsValid() || !Data)
			return Error(EAssetError::NotFound, std::format(
				"Asset {} was not found.", Path.ToString()));
		OutAnalysis.bRedirector =
			Data->EntryKind == EAssetRegistryEntryKind::Redirector;
		OutAnalysis.RedirectDestination = Data->RedirectDestination;

		for (const auto& [OtherPath, OtherData] : CaptureAssetCatalogSnapshot().Assets)
		{
			if (OtherPath != Path
				&& std::ranges::find(OtherData.Dependencies, Path)
					!= OtherData.Dependencies.end())
				OutAnalysis.DirectReferencers.push_back(OtherPath);
		}
		std::ranges::sort(
			OutAnalysis.DirectReferencers,
			[](const FPackagePath& A, const FPackagePath& B) {
				return A.GetView() < B.GetView();
			});
		OutAnalysis.bLoaded = FindResidentPackage(Path) != nullptr;
		OutAnalysis.bLoading = LoadingPackages.contains(Path);

		const FAssetResult CompanionResult =
			AssetPrivate::InspectAssetCompanionFilesForDeletion(
				*Data, OutAnalysis.CompanionFiles);
		if (!CompanionResult)
		{
			OutAnalysis.Warning = std::format(
				"Could not determine companion files: {} Only the package file will be deleted.",
				CompanionResult.Message);
		}
		return {};
	}

	auto FAssetDeletionJob::Delete(
		const FAssetDeletionCommit& Commit) -> FAssetResult
	{
		if (!State || State->bDeleted)
			return Error(EAssetError::StaleData,
				"Only a prepared asset deletion job can execute.");
		if (!Commit.Delete)
			return Error(EAssetError::StaleData,
				"The asset deletion job has no destructive callback.");

		FAssetMutationCoordinator& Mutations =
			FAssetRuntimeState::Get().GetMutationCoordinator();
		std::vector<FAssetDeletionBatchBlocker> Blockers;
		FAssetResult Result =
			Mutations.ValidateAssetDeletionJob(*this, Blockers);
		if (!Result) return Result;
		if (!Blockers.empty())
			return Error(EAssetError::InUse, Blockers.front().Details);
		if (GetAssetCatalogRevision() != State->RegistryRevision)
			return Error(EAssetError::StaleData,
				"The asset Registry changed after deletion confirmation.");
		Result = Mutations.UnloadAssetDeletionJob(*this);
		if (!Result) return Result;
		const FAssetResult DeleteResult = Commit.Delete();
		if (!DeleteResult)
		{
			std::vector<FPackagePath> Paths;
			for (const FAssetDeletionBatchEntry& Entry : State->Entries)
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
		Result = Mutations.RemoveAssetDeletionRegistryProjection(*this);
		if (!Result)
		{
			std::vector<FPackagePath> Paths;
			for (const FAssetDeletionBatchEntry& Entry : State->Entries)
				Paths.push_back(Entry.RegistryEntry.PackagePath);
			FenceAssetRegistryProjection(Paths);
			State->bDeleted = true;
			return {
				.Error = EAssetError::StaleData,
				.Message = std::format(
					"ContentCommittedProjectionPending: destructive deletion committed; Registry reconcile is required. {}",
					Result.Message),
				.Disposition = EAssetResultDisposition::ContentCommittedProjectionPending};
		}
		State->bDeleted = true;
		return {};
	}

	auto FAssetMutationCoordinator::PrepareAssetDeletionJob(
		std::span<const FPackagePath> Paths,
		std::span<const std::filesystem::path> PhysicalRoots,
		FAssetDeletionJob& OutJob,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult
	{
		OutJob = {};
		OutJob.State = std::make_shared<FAssetDeletionJob::FState>();
		auto& OutToken = *OutJob.State;
		OutBlockers.clear();
		if (RuntimeConfiguration.IsCooked())
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
		OutToken.ReferenceStoreRevision =
			AssetPrivate::GetAssetReferenceStoreRevision();
		OutToken.PhysicalRoots.reserve(PhysicalRoots.size());
		for (const std::filesystem::path& Root : PhysicalRoots)
			OutToken.PhysicalRoots.push_back(
				std::filesystem::absolute(Root).lexically_normal());

		auto AddBlocker = [&](EAssetDeletionBatchBlocker Kind,
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
					EAssetDeletionBatchBlocker::MissingAsset,
					Path,
					{},
					{},
					std::format("Asset {} was not found.", Path.ToString()));
				continue;
			}

			FAssetDeletionBatchEntry Entry{
				.RegistryEntry = *Data,
				.bLoaded = FindResidentPackage(Path) != nullptr};
			if (LoadingPackages.contains(Path))
				AddBlocker(
					EAssetDeletionBatchBlocker::LoadingPackage,
					Path,
					{},
					Data->PhysicalPath,
					"Asset is currently loading.");
			if (DPackage* Loaded = FindResidentPackage(Path);
				Loaded && Loaded->IsDirty())
				AddBlocker(
					EAssetDeletionBatchBlocker::DirtyPackage,
					Path,
					{},
					Data->PhysicalPath,
					"Asset has unsaved changes.");

			const FAssetResult CompanionResult =
				AssetPrivate::InspectAssetCompanionFilesForDeletion(
					*Data, Entry.CompanionFiles);
			if (!CompanionResult)
				AddBlocker(
					EAssetDeletionBatchBlocker::CompanionInspectionFailed,
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
		for (auto& [TargetPath, Redirectors] : RedirectorsByTarget)
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
						EAssetDeletionBatchBlocker::RedirectorTargetNotSelected,
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
						EAssetDeletionBatchBlocker::TargetRedirectorsNotSelected,
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
					.TargetPath = Path,
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
					.TargetPath = Path,
					.SoftReferencerPaths = std::move(SoftReferencers),
					.Details = std::format(
						"Deleting {} leaves {} package(s) with dangling soft references. Review the referencers before confirming.",
						Path.ToString(),
						SoftReferencerCount)});
			}
		}
		if (!SortedPaths.empty() && !ReferenceIndex.IsComplete())
			OutToken.Warnings.push_back({
				.TargetPath = SortedPaths.front(),
				.Details = std::format(
					"The package reference index is incomplete ({} error{}), so soft-reference warning counts may be incomplete.",
					ReferenceIndex.GetErrors().size(),
					ReferenceIndex.GetErrors().size() == 1 ? "" : "s")});

		AssetPrivate::AppendRegisteredReferenceStoreDeletionProjection(
			SortedPaths, OutToken.Warnings, OutBlockers);
		std::ranges::sort(
			OutToken.Warnings,
			[](const FAssetDeletionBatchWarning& A,
				const FAssetDeletionBatchWarning& B) {
				if (A.TargetPath.GetView() != B.TargetPath.GetView())
					return A.TargetPath.GetView() < B.TargetPath.GetView();
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
						? EAssetDeletionBatchBlocker::ExternalLoadedReference
						: EAssetDeletionBatchBlocker::ExternalPersistentReference,
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
			if (!AssetPrivate::InspectAssetCompanionFilesForDeletion(
					OwnerData, Files))
				continue;
			for (const std::filesystem::path& File : Files)
				CompanionOwners[File.generic_string()].push_back(OwnerPath);
		}
		for (const FAssetDeletionBatchEntry& Entry : OutToken.Entries)
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
						EAssetDeletionBatchBlocker::CompanionOwnershipConflict,
						Entry.RegistryEntry.PackagePath,
						{},
						File,
						"Companion file is claimed by multiple assets.");
				for (const FPackagePath& Owner : Owners)
					if (!DeletionSet.contains(Owner))
						AddBlocker(
							EAssetDeletionBatchBlocker::ExternalCompanionOwner,
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
						EAssetDeletionBatchBlocker::ExternalCompanionOwner,
						Owner,
						Owner,
						PhysicalPath,
						std::format(
							"Selected content contains a companion owned by asset {} outside the deletion set.",
							Owner.ToString()));
		}

		std::ranges::sort(
			OutBlockers,
			[](const FAssetDeletionBatchBlocker& A,
				const FAssetDeletionBatchBlocker& B) {
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

	auto FAssetMutationCoordinator::ValidateAssetDeletionJob(
		const FAssetDeletionJob& Job,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult
	{
		if (!Job.State)
			return Error(EAssetError::StaleData,
				"The asset deletion job is empty.");
		const auto& Token = *Job.State;
		std::vector<FPackagePath> Paths;
		Paths.reserve(Token.Entries.size());
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
			Paths.push_back(Entry.RegistryEntry.PackagePath);

		FAssetDeletionJob CurrentJob;
		FAssetResult Result = PrepareAssetDeletionJob(
			Paths, Token.PhysicalRoots, CurrentJob, OutBlockers);
		if (!Result || !OutBlockers.empty()) return Result;
		const auto& Current = *CurrentJob.State;
		if (Current.Entries.size() != Token.Entries.size())
			return Error(EAssetError::InUse,
				"The asset deletion set changed after confirmation.");
		if (Current.ReferenceStoreRevision != Token.ReferenceStoreRevision)
			return Error(EAssetError::InUse,
				"Persistent asset-reference owners changed after deletion confirmation.");
		if (Current.Warnings != Token.Warnings)
			return Error(EAssetError::InUse,
				"Asset references changed after deletion confirmation.");
		for (size_t Index = 0; Index < Token.Entries.size(); ++Index)
		{
			const FAssetDeletionBatchEntry& Expected = Token.Entries[Index];
			const FAssetDeletionBatchEntry& Actual = Current.Entries[Index];
			if (!(Expected.RegistryEntry == Actual.RegistryEntry)
				|| Expected.CompanionFiles != Actual.CompanionFiles)
				return Error(EAssetError::InUse, std::format(
					"Asset {} changed after confirmation.",
					Expected.RegistryEntry.PackagePath.ToString()));
		}
		return {};
	}

	auto FAssetMutationCoordinator::UnloadAssetDeletionJob(
		const FAssetDeletionJob& Job) -> FAssetResult
	{
		const auto& Token = *Job.State;
		std::vector<DPackage*> Packages;
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
		{
			const FPackagePath& Path = Entry.RegistryEntry.PackagePath;
			if (LoadingPackages.contains(Path))
				return Error(EAssetError::InUse, std::format(
					"Asset {} is currently loading.", Path.ToString()));
			DPackage* Loaded = FindResidentPackage(Path);
			if (!Loaded) continue;
			if (Loaded->IsDirty())
				return Error(EAssetError::InUse, std::format(
					"Asset {} has unsaved changes.", Path.ToString()));
			Packages.push_back(Loaded);
		}
		for (DPackage* Package : Packages)
		{
			MarkObjectHierarchyAsGarbage(Package);
		}
		if (!Packages.empty()) CollectGarbage();
		return {};
	}

	auto FAssetMutationCoordinator::RemoveAssetDeletionRegistryProjection(
		const FAssetDeletionJob& Job) -> FAssetResult
	{
		const auto& Token = *Job.State;
		const uint64 ExpectedRevision = Token.RegistryRevision;
		FAssetPublicationState Prepared = Registry.CapturePreparedState();
		std::unordered_set<FPackagePath> DeletionSet;
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
			DeletionSet.insert(Entry.RegistryEntry.PackagePath);
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
		{
			const auto Current = Prepared.Assets.find(Entry.RegistryEntry.PackagePath);
			if (Current == Prepared.Assets.end() || !(Current->second == Entry.RegistryEntry))
				return Error(EAssetError::InUse, std::format(
					"Asset {} changed before registry removal.",
					Entry.RegistryEntry.PackagePath.ToString()));
		}
		for (const auto& [OtherPath, OtherData] : Prepared.Assets)
		{
			if (DeletionSet.contains(OtherPath)) continue;
			for (const FPackagePath& Dependency : OtherData.Dependencies)
				if (DeletionSet.contains(Dependency))
					return Error(EAssetError::InUse, std::format(
						"Asset {} gained external referencer {}.",
						Dependency.ToString(), OtherPath.ToString()));
		}
		FAssetRegistryDelta Delta{.ExpectedRevision = ExpectedRevision};
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
		{
			Delta.Removes.push_back(Entry.RegistryEntry.PackagePath);
			Delta.ReferenceInvalidations.push_back(Entry.RegistryEntry.PackagePath);
		}
		return Registry.PublishDelta(std::move(Delta));
	}

	auto FAssetMutationCoordinator::DeleteAssetForTesting(const FPackagePath& Path)
		-> FAssetResult
	{
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode,
				"Cooked runtime package mode does not permit asset deletion.");
		FAssetDeleteAnalysis Analysis;
		FAssetResult Result = AnalyzeAssetDeletion(Path, Analysis);
		if (!Result) return Result;
		if (Analysis.bRedirector)
			return Error(EAssetError::InUse, std::format(
				"Redirector {} cannot be deleted alone. Select its final target and every alias in one batch, or run Fix Up Redirectors.",
				Path.ToString()));
		if (!Analysis.DirectReferencers.empty())
			return Error(EAssetError::InUse, std::format(
				"Asset {} is referenced by {} asset(s).",
				Path.ToString(), Analysis.DirectReferencers.size()));
		if (Analysis.bLoading)
			return Error(EAssetError::InUse, "Asset is currently loading.");
		if (Analysis.bLoaded)
		{
			// Once persistent package references are gone, keeping the live
			// package must not require a restart.
			Result = UnloadPackage(Path);
			if (!Result) return Result;
		}

		const FAssetCatalogEntry Data = FindAssetExact(Path);
		if (!Data)
			return Error(EAssetError::NotFound, std::format(
				"Asset {} was not found.", Path.ToString()));
		std::vector<std::filesystem::path> Files;
		Files.emplace_back(Data->PhysicalPath);
		for (const std::filesystem::path& Companion : Analysis.CompanionFiles)
		{
			const std::filesystem::path Normalized =
				std::filesystem::absolute(Companion).lexically_normal();
			if (std::ranges::find(Files, Normalized) == Files.end())
				Files.push_back(Normalized);
		}

		const uint64 ExpectedRevision = GetAssetCatalogRevision();
		struct FStagedDeleteFile
		{
			std::filesystem::path Original;
			std::filesystem::path Staged;
			std::filesystem::path RecoveryCopy;
		};
		std::vector<FStagedDeleteFile> StagedFiles;
		auto Rollback = [&]() {
			std::error_code Ec;
			for (auto It = StagedFiles.rbegin(); It != StagedFiles.rend(); ++It)
			{
				Ec.clear();
				if (std::filesystem::exists(It->Staged))
					std::filesystem::rename(It->Staged, It->Original, Ec);
				else if (std::filesystem::exists(It->RecoveryCopy))
					std::filesystem::copy_file(
						It->RecoveryCopy, It->Original,
						std::filesystem::copy_options::overwrite_existing, Ec);
				std::filesystem::remove(It->RecoveryCopy, Ec);
			}
		};

		for (const std::filesystem::path& File : Files)
		{
			if (!std::filesystem::exists(File)) continue;
			std::filesystem::path Staged = File.string() + ".deletebak";
			std::error_code Ec;
			std::filesystem::remove(Staged, Ec);
			Ec.clear();
			std::filesystem::rename(File, Staged, Ec);
			if (Ec)
			{
				Rollback();
				return Error(EAssetError::IoError, std::format(
					"Failed to stage {} for deletion.", File.generic_string()));
			}
			const std::filesystem::path RecoveryCopy = Staged.string() + ".copy";
			Ec.clear();
			std::filesystem::remove(RecoveryCopy, Ec);
			Ec.clear();
			std::filesystem::copy_file(
				Staged, RecoveryCopy,
				std::filesystem::copy_options::overwrite_existing, Ec);
			if (Ec)
			{
				std::filesystem::rename(Staged, File, Ec);
				Rollback();
				return Error(EAssetError::IoError, std::format(
					"Failed to prepare rollback data for {}.",
					File.generic_string()));
			}
			StagedFiles.push_back({File, Staged, RecoveryCopy});
		}

		for (const FStagedDeleteFile& File : StagedFiles)
		{
			std::error_code Ec;
			if (!std::filesystem::remove(File.Staged, Ec) || Ec)
			{
				Rollback();
				return Error(EAssetError::IoError, std::format(
					"Failed to delete {}.", File.Original.generic_string()));
			}
		}
		FAssetRegistryDelta Delta{
			.ExpectedRevision = ExpectedRevision,
			.Removes = {Path},
			.ReferenceInvalidations = {Path}};
		if (FAssetResult PublishResult = Registry.PublishDelta(std::move(Delta));
			!PublishResult)
		{
			Rollback();
			return PublishResult;
		}
		for (const FStagedDeleteFile& File : StagedFiles)
		{
			std::error_code Ec;
			std::filesystem::remove(File.RecoveryCopy, Ec);
		}
		return {};
	}
}
