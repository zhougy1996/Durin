#include "Asset/RegistryOperations.h"
#include "AssetTools/IAssetTools.h"
#include "AssetDeletionInternal.h"
#include "Asset/Load.h"
#include "Asset/MutationExtensions.h"
#include "Asset/PackageRemoval.h"
#include "AssetRegistry/Publication.h"
#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

namespace Durin
{
	namespace
	{
		auto Reject(std::optional<UnifiedError::FError>* OutError, UnifiedError::FError Error) -> bool
		{ if (OutError) *OutError = std::move(Error); return false; }
		auto WriteFailure(UnifiedError::FErrorId Code, std::string Message) -> FAssetWriteResult
		{ return {.Error = UnifiedError::FError{Code, std::move(Message)}};
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
		std::vector<const FStrongObjectPtr*> RetainedOwners;
		uint64 RegistryRevision = 0;
		FAssetReferenceStoreCapture ReferenceStores;
		std::vector<FAssetDeletionEntry> Entries;
		std::vector<FAssetDeletionWarning> Warnings;
		std::vector<FAssetDeletionBlocker> Blockers;
		std::vector<std::filesystem::path> PhysicalRoots;
		std::vector<std::filesystem::path> CompanionScope;
		bool bPrepared = false;
		bool bExecuted = false;
		bool bExecuting = false;
		struct FResident { FWeakObjectPtr Package; bool bWasDirty; std::vector<FObjectKey> Exports; };
		std::vector<FResident> Residents;
		FAssetRegistrySnapshot PreparedSnapshot;
		FAssetDeletionFileIdentities ConfirmedBytes;
		std::unordered_set<std::string> RemovedFiles;
		FAssetWriteResult LastResult;
		std::vector<std::filesystem::path> AffectedFiles;
		std::unordered_map<FPackagePath, std::vector<std::filesystem::path>> OutsideCompanions;

		auto ValidateFiles(const FAssetDeletionCommit& Commit, std::optional<UnifiedError::FError>* OutError) -> bool;
		auto RecordRemovedFiles() -> void;

		auto Prepare(std::span<const FPackagePath> Paths,
			std::span<const std::filesystem::path> Roots,
			std::vector<FAssetDeletionBlocker>& OutBlockers, std::optional<UnifiedError::FError>* OutError) -> bool;
		auto Validate(std::optional<UnifiedError::FError>* OutError) const -> bool;
		auto ValidateResidents(std::optional<UnifiedError::FError>* OutError) const -> bool;
		auto Delete(const FAssetDeletionCommit& Commit, std::vector<std::filesystem::path>& AffectedFiles) -> FAssetWriteResult;
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

	auto FAssetDeletionOperation::GetWriteResult() const -> const FAssetWriteResult&
	{
		static const FAssetWriteResult Unavailable{.Error = UnifiedError::FError{AssetWriteErrors::InvalidData, "No deletion operation is prepared."}};
		return State ? State->LastResult : Unavailable;
	}
	auto FAssetDeletionOperation::GetAffectedFiles() const -> std::span<const std::filesystem::path>
	{
		return State ? std::span<const std::filesystem::path>(State->AffectedFiles) : std::span<const std::filesystem::path>{};
	}
	auto FAssetDeletionOperation::GetFileIdentities() const -> const FAssetDeletionFileIdentities&
	{
		static const FAssetDeletionFileIdentities Empty;
		return State ? State->ConfirmedBytes : Empty;
	}
	auto FAssetDeletionOperation::Delete(const FAssetDeletionCommit& Commit, FFeedbackContext* Warn) -> bool
	{
		if (!State) { (Warn ? *Warn : FFeedbackContext::Get()).Error((GetWriteResult().Error ? GetWriteResult().Error->ToString() : std::string{})); return false; }
		if (State->bExecuted || State->bExecuting)
		{
			(Warn ? *Warn : FFeedbackContext::Get()).Error("The deletion operation has already executed.");
			return false;
		}
		State->bExecuting = true;
		struct FReset { bool& Flag; ~FReset() { Flag = false; } } Reset{State->bExecuting};
		State->AffectedFiles.clear();
		State->LastResult = State->Delete(Commit, State->AffectedFiles);
		if (!State->LastResult) (Warn ? *Warn : FFeedbackContext::Get()).Error((State->LastResult.Error ? State->LastResult.Error->ToString() : std::string{}));
		return static_cast<bool>(State->LastResult);
	}

	auto FAssetDeletionOperation::FState::Delete(
		const FAssetDeletionCommit& Commit, std::vector<std::filesystem::path>& AffectedFiles) -> FAssetWriteResult
	{
		if (!bPrepared || bExecuted)
			return WriteFailure(AssetWriteErrors::StaleData,
				"Only a prepared asset deletion job can execute.");
		if (!Commit.Delete)
			return WriteFailure(AssetWriteErrors::StaleData,
				"The asset deletion job has no destructive callback.");

		FAssetWriteResult Result;
		if (!Validate(&Result.Error)) return Result;
		if (GetAssetCatalogRevision() != RegistryRevision)
			return WriteFailure(AssetWriteErrors::StaleData,
				"The asset Registry changed after deletion confirmation.");
		if (!ValidateFiles(Commit, &Result.Error)) return Result;
		// The host supplies freshly verified bytes. Do not reopen guarded files or
		// repeat companion analysis; recheck callback-sensitive admission facts.
		if (!Entries.empty())
		{
			FAssetReferenceStoreCapture Stores;
			if (const auto Captured = CaptureAssetReferenceStores(Stores); !Captured)
				return {.Error = Captured.error()};
			if (Stores != ReferenceStores)
				return WriteFailure(AssetWriteErrors::StaleData, "External reference owners changed during file validation.");
		}
		if (!ValidateResidents(&Result.Error)) return Result;
		if (GetAssetCatalogRevision() != RegistryRevision)
			return WriteFailure(AssetWriteErrors::StaleData, "Deletion metadata changed during file validation.");
		std::vector<FPackageData> Packages;
		for (const FAssetDeletionEntry& Entry : Entries)
			if (FindPackageExact(Entry.RegistryEntry.PackagePath))
				Packages.push_back(Entry.RegistryEntry);
		if (auto Released = ReleasePackagesForRemoval(Packages, RegistryRevision, RetainedOwners, true); !Released)
			return {.Error = std::move(Released.error())};
		bExecuted = true;
		const FAssetWriteResult DeleteResult = Commit.Delete(AffectedFiles);
		RecordRemovedFiles();
		for (const auto& File : RemovedFiles) AffectedFiles.emplace_back(File);
		std::ranges::sort(AffectedFiles);
		AffectedFiles.erase(std::ranges::unique(AffectedFiles).begin(), AffectedFiles.end());
		if (!DeleteResult)
		{
			auto Failure = DeleteResult;
			if (Failure.Effect == EAssetWriteEffect::None && !AffectedFiles.empty())
				Failure.Effect = EAssetWriteEffect::PartiallyWritten;
			if (Failure.Effect != EAssetWriteEffect::None)
			{
				std::vector<FPackagePath> Paths;
				for (const FAssetDeletionEntry& Entry : Entries)
					Paths.push_back(Entry.RegistryEntry.PackagePath);
				FenceAssetRegistryProjection(Paths);
			}
			Failure.Error->PushContext("operation", "AssetDeletionFailed: deletion attempt failed; prepare a new operation before retrying.");
			return Failure;
		}
		if (Packages.empty()) return DeleteResult;
		if (auto Published = PublishPackageRemoval(Packages, RegistryRevision); !Published)
		{
			auto Error = std::move(Published.error());
			const auto Effect = Error.Is({"File", "IoError"}) ? EAssetWriteEffect::ContentUncertain
				: RemovedFiles.size() == ConfirmedBytes.size() ? EAssetWriteEffect::ContentCommittedProjectionPending
				: AffectedFiles.empty() ? EAssetWriteEffect::None : EAssetWriteEffect::PartiallyWritten;
			if (Effect != EAssetWriteEffect::None)
			{
				std::vector<FPackagePath> Paths;
				for (const FAssetDeletionEntry& Entry : Entries)
					Paths.push_back(Entry.RegistryEntry.PackagePath);
				FenceAssetRegistryProjection(Paths);
			}
			Error.PushContext("operation", Effect == EAssetWriteEffect::ContentCommittedProjectionPending
				? "Destructive deletion committed; Registry reconcile is required."
				: "Deletion could not publish catalog removal; prepare a new operation before retrying.");
			return {.Error = std::move(Error), .Effect = Effect};
		}
		return {};
	}

	// Validate the confirmed file identities before the single destructive attempt.
	auto FAssetDeletionOperation::FState::ValidateFiles(const FAssetDeletionCommit& Commit, std::optional<UnifiedError::FError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		ConfirmedBytes.clear();
		FAssetDeletionFileIdentities VerifiedFiles;
		if (Commit.ValidateFiles)
		{
			const bool Result = Commit.ValidateFiles(VerifiedFiles, OutError);
			if (!Result)
			{ if (OutError && !*OutError) OutError->emplace(AssetWriteErrors::InvalidData, "Deletion file validation was rejected."); return false; }
		}
		for (const auto& Entry : Entries)
		{
			std::vector<std::filesystem::path> Files = Entry.CompanionFiles;
			Files.push_back(Entry.RegistryEntry.PhysicalPath);
			for (const auto& File : Files)
			{
				const auto Key = std::filesystem::absolute(File).lexically_normal().generic_string();
				if (ConfirmedBytes.contains(Key)) continue;
				FXxHash128 Identity;
				if (Commit.ValidateFiles)
				{
					const auto Found = VerifiedFiles.find(Key);
					if (Found == VerifiedFiles.end())
						return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, "Host validation omitted a deletion participant."});
					Identity = Found->second;
				}
				else
				{
					auto Hashed = FFileHelper::HashFileXx128(File);
					if (!Hashed) return Reject(OutError, ToError(Hashed.error()));
					Identity = *Hashed;
				}
				ConfirmedBytes.emplace(Key, Identity);
			}
		}
		return true;
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

	auto FAssetDeletionOperation::FState::Prepare(
		std::span<const FPackagePath> Paths,
		std::span<const std::filesystem::path> PhysicalRoots,
		std::vector<FAssetDeletionBlocker>& OutBlockers, std::optional<UnifiedError::FError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		auto& OutToken = *this;
		OutBlockers.clear();
		if (GetAssetRuntimeConfiguration().IsCooked())
			return Reject(OutError, UnifiedError::FError{
				AssetWriteErrors::ReadOnlyMode,
				"Cooked runtime package mode does not permit asset deletion."});

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
		PreparedSnapshot = CaptureAssetRegistrySnapshot();
		OutToken.RegistryRevision = PreparedSnapshot.Revision;

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
			const FPackageCatalogEntry Data = FindPackageExact(Path);
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

			if (auto* Loaded = FindResidentPackage(Path))
			{
				FResident Resident{FWeakObjectPtr(Loaded), Loaded->IsDirty()};
				for (auto* Export : Loaded->GetTopLevelAssets()) Resident.Exports.emplace_back(Export);
				Residents.push_back(std::move(Resident));
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

			std::optional<UnifiedError::FError> CompanionError;
			const bool CompanionResult =
				AssetToolsPrivate::InspectAssetCompanionFiles(
					*Data, Entry.CompanionFiles, &CompanionError);
			if (!CompanionResult)
				AddBlocker(
					EAssetDeletionBlocker::CompanionInspectionFailed,
					Path,
					{},
					Data->PhysicalPath,
					(CompanionError ? CompanionError->ToString() : std::string{}));
			OutToken.Entries.push_back(std::move(Entry));
		}
		FPackagePath BlockedPackage;
		if (const auto References = ValidatePackageRemovalReferences(SortedPaths, &BlockedPackage, RetainedOwners); !References)
			AddBlocker(EAssetDeletionBlocker::ExternalLoadedReference, BlockedPackage, {}, {}, References.error().ToString());
		CompanionScope = OutToken.PhysicalRoots;
		for (const auto& Entry : Entries)
			CompanionScope.insert(CompanionScope.end(), Entry.CompanionFiles.begin(), Entry.CompanionFiles.end());

		const FAssetReferenceIndex& ReferenceIndex = PreparedSnapshot.References;

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
			const auto Captured = CaptureAssetReferenceStores(ReferenceStores);
			if (!Captured)
				AddBlocker(EAssetDeletionBlocker::ReferenceStoreInspectionFailed,
					SortedPaths.front(), {}, {}, Captured.error().ToString());
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

		for (const auto& Path : SortedPaths)
			for (const auto& Edge : ReferenceIndex.FindReferencers(Path))
			{
				if (Edge.Kind == EAssetReferenceKind::SoftObject || DeletionSet.contains(Edge.SourcePackage)) continue;
				const auto* Other = PreparedSnapshot.Catalog.FindExact(Edge.SourcePackage);
				// Redirect hard blockers have dedicated actionable closure diagnostics.
				if (!Other || (!Other->TopLevelAssets.empty() && std::ranges::all_of(Other->TopLevelAssets, &FAssetData::IsRedirector))) continue;
				AddBlocker(FindResidentPackage(Edge.SourcePackage)
					? EAssetDeletionBlocker::ExternalLoadedReference : EAssetDeletionBlocker::ExternalPersistentReference,
					Path, Edge.SourcePackage, Other->PhysicalPath,
					std::format("Asset {} is referenced by {}.", Path.ToString(), Edge.SourcePackage.ToString()));
			}

		std::unordered_map<std::string, std::vector<FPackagePath>> CompanionOwners;
		for (const auto& Entry : Entries)
			for (const auto& File : Entry.CompanionFiles)
				CompanionOwners[File.generic_string()].push_back(Entry.RegistryEntry.PackagePath);
		for (const auto& [OwnerPath, OwnerData] : PreparedSnapshot.Catalog.Assets)
		{
			if (DeletionSet.contains(OwnerPath)) continue;
			if (!AssetToolsPrivate::MayOwnCompanionInRoots(OwnerData, CompanionScope)) continue;
			std::vector<std::filesystem::path> Files;
			if (!AssetToolsPrivate::InspectAssetCompanionFiles(
					OwnerData, Files))
				continue;
			for (const std::filesystem::path& File : Files)
				CompanionOwners[File.generic_string()].push_back(OwnerPath);
			OutsideCompanions.emplace(OwnerPath, std::move(Files));
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
		return true;
	}

	auto FAssetDeletionOperation::Validate(std::optional<UnifiedError::FError>* OutError) const -> bool
	{
		if (OutError) OutError->reset();
		if (!State || !State->bPrepared || State->bExecuted)
			return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, "The deletion confirmation is not available."});
		return State->Validate(OutError);
	}

	auto FAssetDeletionOperation::FState::Validate(std::optional<UnifiedError::FError>* OutError) const -> bool
	{
		if (OutError) OutError->reset();
		if (GetAssetRuntimeConfiguration().IsCooked())
			return Reject(OutError, UnifiedError::FError{AssetWriteErrors::ReadOnlyMode, "Cooked content cannot be deleted."});
		// Catalog publications include reference facts. An unchanged revision keeps
		// the confirmed alias closure and warnings valid; files and providers have
		// independent lifetimes and must still be checked below.
		if (GetAssetCatalogRevision() != RegistryRevision)
			return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, "The asset Registry changed after deletion confirmation."});
		for (const auto& Entry : Entries)
		{
			const auto& Path = Entry.RegistryEntry.PackagePath;
			const auto Current = FindPackageExact(Path);
			if (!Current || !(*Current == Entry.RegistryEntry))
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, "A deletion participant changed after confirmation."});
			const auto* Loaded = FindResidentPackage(Path);
			if (IsPackageLoading(Path))
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::InUse, "A deletion participant is loading or dirty."});
			std::vector<std::filesystem::path> Files;
			const bool Inspected = AssetToolsPrivate::InspectAssetCompanionFiles(*Current, Files, OutError);
			if (!Inspected) return Inspected;
			if (Files != Entry.CompanionFiles)
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, "Selected companion files changed after confirmation."});
		}
		std::vector<FPackagePath> Paths;
		for (const auto& Entry : Entries) Paths.push_back(Entry.RegistryEntry.PackagePath);
		if (const auto References = ValidatePackageRemovalReferences(Paths, nullptr, RetainedOwners); !References) return Reject(OutError, References.error());
		std::unordered_map<FPackagePath, std::vector<std::filesystem::path>> Companions;
		for (const auto& [Path, Data] : PreparedSnapshot.Catalog.Assets)
		{
			if (std::ranges::any_of(Entries, [&](const auto& Entry) {
				return Entry.RegistryEntry.PackagePath == Path;
			})) continue;
			if (!AssetToolsPrivate::MayOwnCompanionInRoots(Data, CompanionScope)) continue;
			std::vector<std::filesystem::path> Files;
			if (!AssetToolsPrivate::InspectAssetCompanionFiles(Data, Files)) continue;
			Companions.emplace(Path, std::move(Files));
		}
		if (Companions != OutsideCompanions)
			return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, "Companion ownership changed after confirmation."});
		if (!Entries.empty())
		{
			FAssetReferenceStoreCapture Stores;
			const auto Captured = CaptureAssetReferenceStores(Stores);
			if (!Captured) return Reject(OutError, Captured.error());
			if (Stores != ReferenceStores)
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, "External reference owners changed after confirmation."});
		}
		if (GetAssetCatalogRevision() != RegistryRevision)
			return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, "Deletion metadata changed during validation."});
		return ValidateResidents(OutError);
	}

	auto FAssetDeletionOperation::FState::ValidateResidents(std::optional<UnifiedError::FError>* OutError) const -> bool
	{
		for (const auto& Resident : Residents)
		{
			auto* Package = Cast<DPackage>(Resident.Package.Get());
			std::vector<FObjectKey> Exports;
			if (Package) for (auto* Export : Package->GetTopLevelAssets()) Exports.emplace_back(Export);
			if (!Package || Package->IsDirty() != Resident.bWasDirty || Exports != Resident.Exports)
				return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, "The live deletion selection or its unsaved changes changed after confirmation."});
		}
		return true;
	}

	auto PrepareAssetDeletionWithEditorPolicy(const FAssetDeletionRequest& Request,
		FAssetDeletionOperation& OutOperation, FFeedbackContext* Warn) -> bool
	{
		OutOperation = FAssetDeletionOperation{};
		OutOperation.State = std::make_unique<FAssetDeletionOperation::FState>();
		auto& State = *OutOperation.State;
		State.RetainedOwners = Request.RetainedOwners;
		std::optional<UnifiedError::FError> Error;
		const bool Prepared = State.Prepare(Request.AssetPaths, Request.PhysicalRoots, State.Blockers, &Error);
		State.bPrepared = Prepared && State.Blockers.empty();
		auto& Feedback = Warn ? *Warn : FFeedbackContext::Get();
		if (!Prepared && Error) Feedback.Error(Error->ToString());
		for (const auto& Blocker : State.Blockers) Feedback.Error(Blocker.Details);
		for (const auto& Warning : State.Warnings) Feedback.Warning(Warning.Details);
		return State.bPrepared;
	}
}
