#include "Data/ContentBrowserDeletionOperation.h"
#include "Operations/ContentBrowserPaths.h"
#include "Operations/ContentBrowserPathInspection.h"
#include "Panels/ContentBrowserFilesystem.h"
#include "AssetTools/IAssetTools.h"
#include "AssetRegistry/Catalog.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::ContentBrowser::Private
{
	using ContentBrowserFilesystem::NormalizePath;
	namespace
	{
		auto MakeFingerprint(
			const std::filesystem::path& Path,
			EContentDeletionEntryKind Kind,
			FContentDeletionFingerprint& OutFingerprint,
			std::error_code& OutError) -> bool
		{
			OutError.clear();
			OutFingerprint = {};
			OutFingerprint.PhysicalPath = NormalizePath(Path.generic_string());
			OutFingerprint.Kind = Kind;
			if (Kind != EContentDeletionEntryKind::Directory)
			{
				OutFingerprint.FileSize = std::filesystem::file_size(Path, OutError);
				if (OutError) return false;
			}
			const auto WriteTime = std::filesystem::last_write_time(Path, OutError);
			if (OutError) return false;
			OutFingerprint.LastWriteTimeTicks =
				static_cast<int64>(WriteTime.time_since_epoch().count());
			if (Kind != EContentDeletionEntryKind::Directory)
			{
				auto Identity = FFileHelper::HashFileXx128(Path);
				if (!Identity)
				{
					OutError = Identity.error().NativeError;
					return false;
				}
				OutFingerprint.ByteIdentity = *Identity;
				const uintmax_t FinalSize = std::filesystem::file_size(Path, OutError);
				if (OutError) return false;
				const auto FinalWriteTime =
					std::filesystem::last_write_time(Path, OutError);
				if (OutError) return false;
				if (FinalSize != OutFingerprint.FileSize
					|| static_cast<int64>(FinalWriteTime.time_since_epoch().count())
						!= OutFingerprint.LastWriteTimeTicks)
				{
					OutError = std::make_error_code(std::errc::state_not_recoverable);
					return false;
				}
			}
			return true;
		}

	}
	auto FContentBrowserDeletionOperation::AnalyzeDeletion(
		std::span<const FContentBrowserNativeItem> Items,
		FAssetDeletionOperation& OutOperation) const
		-> FContentDeletionPlanPtr
	{
		auto Plan = std::make_shared<FContentDeletionPlan>();

		auto AddBlocker = [&](EContentDeletionBlocker Kind,
			std::string DisplayName,
			std::string PhysicalPath,
			std::string RelatedAssetPath,
			std::string Details) {
			Plan->Blockers.push_back({
				.Kind = Kind,
				.DisplayName = std::move(DisplayName),
				.PhysicalPath = std::move(PhysicalPath),
				.RelatedAssetPath = std::move(RelatedAssetPath),
				.Details = std::move(Details)});
		};

		if (Items.empty())
		{
			AddBlocker(
				EContentDeletionBlocker::InvalidSelection,
				"Selection", {}, {}, "No content is selected.");
			return Plan;
		}

		struct FSelectedRoot
		{
			const FContentBrowserNativeItem* Item = nullptr;
			std::string PhysicalPath;
			const FMountPoint* Mount = nullptr;
		};
		std::vector<FSelectedRoot> SelectedRoots;
		for (const FContentBrowserNativeItem& Item : Items)
		{
			const std::string PhysicalPath = NormalizePath(Item.PhysicalPath);
			const FContentBrowserPaths::FMountPath Resolved =
				FContentBrowserPaths::ResolveMountPath(PhysicalPath);
			if (!Resolved)
			{
				AddBlocker(
					EContentDeletionBlocker::OutsideMount,
					Item.Name,
					PhysicalPath,
					{},
					"Selected path is outside every mounted content root.");
				continue;
			}
			const FMountPoint* Mount = Resolved.Mount;
			if (AreSamePath(PhysicalPath, Mount->GetContentDir().generic_string()))
				AddBlocker(
					EContentDeletionBlocker::MountRoot,
					Item.Name,
					PhysicalPath,
					{},
					"A mounted content root cannot be deleted.");
			if (!Mount->bContentWritable)
				AddBlocker(
					EContentDeletionBlocker::ReadOnlyMount,
					Item.Name,
					PhysicalPath,
					{},
					"The selected mount is not content-writable.");
			SelectedRoots.push_back({&Item, PhysicalPath, Mount});
		}

		if (SelectedRoots.size() != Items.size())
			AddBlocker(
				EContentDeletionBlocker::InvalidSelection,
				"Selection",
				{},
				{},
				"One or more selected items are absent from the analyzed item set.");
		std::ranges::sort(
			SelectedRoots,
			[](const FSelectedRoot& A, const FSelectedRoot& B) {
				if (A.PhysicalPath.size() != B.PhysicalPath.size())
					return A.PhysicalPath.size() < B.PhysicalPath.size();
				return A.PhysicalPath < B.PhysicalPath;
			});
		std::vector<FSelectedRoot> MaximalRoots;
		for (const FSelectedRoot& Candidate : SelectedRoots)
		{
			if (!MaximalRoots.empty()
				&& Candidate.Mount != MaximalRoots.front().Mount)
				AddBlocker(
					EContentDeletionBlocker::UnsupportedMount,
					Candidate.Item->Name,
					Candidate.PhysicalPath,
					{},
					"One deletion plan cannot span multiple content mounts.");
			if (std::ranges::any_of(
					MaximalRoots,
					[&](const FSelectedRoot& Existing) {
						return FPaths::IsLexicalDescendantPath(
							Candidate.PhysicalPath, Existing.PhysicalPath, true);
					}))
				continue;
			MaximalRoots.push_back(Candidate);
		}
		if (MaximalRoots.size() == 1)
			Plan->DisplayName = MaximalRoots.front().Item->Name;
		else
			Plan->DisplayName = std::format("{} Items", MaximalRoots.size());

		const FAssetCatalogSnapshot Catalog =
			CaptureAssetCatalogSnapshot();
		std::unordered_map<std::string, const FPackageData*> AssetsByPhysicalPath;
		for (const auto& [Path, Data] : Catalog.Assets)
			AssetsByPhysicalPath.emplace(NormalizePath(Data.PhysicalPath), &Data);
		std::vector<FPackagePath> AssetPaths;
		std::vector<std::filesystem::path> PhysicalRoots;

		auto AddPhysicalEntry = [&](const std::filesystem::path& Physical,
			bool bDirectory) {
			const std::string Normalized = NormalizePath(Physical.generic_string());
			EContentDeletionEntryKind Kind = bDirectory
				? EContentDeletionEntryKind::Directory
				: EContentDeletionEntryKind::OrdinaryFile;
			if (!bDirectory && Physical.extension() == ".dasset")
			{
				if (const auto Asset = AssetsByPhysicalPath.find(Normalized);
					Asset != AssetsByPhysicalPath.end())
				{
					Kind = EContentDeletionEntryKind::AssetPackage;
					AssetPaths.push_back(Asset->second->PackagePath);
				}
				else
				{
					Kind = EContentDeletionEntryKind::UnknownPackage;
					AddBlocker(
						EContentDeletionBlocker::UnknownPackage,
						Physical.filename().generic_string(),
						Normalized,
						{},
						"Package file is not registered or has invalid metadata.");
				}
			}
			FContentDeletionFingerprint Fingerprint;
			std::error_code Ec;
			if (!MakeFingerprint(Physical, Kind, Fingerprint, Ec))
			{
				AddBlocker(
					EContentDeletionBlocker::InspectionFailed,
					Physical.filename().generic_string(),
					Normalized,
					{},
					std::format("Could not fingerprint content: {}", Ec.message()));
				return;
			}
			Plan->Entries.push_back(std::move(Fingerprint));
		};

		for (const FSelectedRoot& Root : MaximalRoots)
		{
			const std::filesystem::path Physical(Root.PhysicalPath);
			PhysicalRoots.push_back(Physical);
			std::error_code Ec;
			const bool bReparse = IsReparsePoint(Physical, Ec);
			if (Ec)
			{
				AddBlocker(
					EContentDeletionBlocker::InspectionFailed,
					Root.Item->Name,
					Root.PhysicalPath,
					{},
					std::format("Could not inspect selected path: {}", Ec.message()));
				continue;
			}
			if (bReparse)
			{
				AddBlocker(
					EContentDeletionBlocker::ReparsePoint,
					Root.Item->Name,
					Root.PhysicalPath,
					{},
					"Reparse points cannot be traversed or staged.");
				continue;
			}
			const bool bDirectory = std::filesystem::is_directory(Physical, Ec);
			if (Ec || (!bDirectory && !std::filesystem::is_regular_file(Physical, Ec)))
			{
				AddBlocker(
					EContentDeletionBlocker::InvalidSelection,
					Root.Item->Name,
					Root.PhysicalPath,
					{},
					"Selected content no longer exists as a supported file or directory.");
				continue;
			}
			AddPhysicalEntry(Physical, bDirectory);
			Plan->MaximalRoots.push_back({
				.OriginalPath = Root.PhysicalPath});
			if (!bDirectory) continue;

			for (std::filesystem::recursive_directory_iterator It(
					 Physical, std::filesystem::directory_options::none, Ec),
				 End;
				 !Ec && It != End;
				 It.increment(Ec))
			{
				std::error_code EntryEc;
				if (IsReparsePoint(It->path(), EntryEc))
				{
					AddBlocker(
						EContentDeletionBlocker::ReparsePoint,
						It->path().filename().generic_string(),
						NormalizePath(It->path().generic_string()),
						{},
						"Reparse points cannot be traversed or staged.");
					It.disable_recursion_pending();
					continue;
				}
				if (EntryEc)
				{
					AddBlocker(
						EContentDeletionBlocker::InspectionFailed,
						It->path().filename().generic_string(),
						NormalizePath(It->path().generic_string()),
						{},
						std::format("Could not inspect descendant: {}", EntryEc.message()));
					It.disable_recursion_pending();
					continue;
				}
				const bool bChildDirectory = It->is_directory(EntryEc);
				if (!EntryEc && (bChildDirectory || It->is_regular_file(EntryEc)))
					AddPhysicalEntry(It->path(), bChildDirectory);
				else if (EntryEc)
					AddBlocker(
						EContentDeletionBlocker::InspectionFailed,
						It->path().filename().generic_string(),
						NormalizePath(It->path().generic_string()),
						{},
						std::format("Could not classify descendant: {}", EntryEc.message()));
			}
			if (Ec)
				AddBlocker(
					EContentDeletionBlocker::InspectionFailed,
					Root.Item->Name,
					Root.PhysicalPath,
					{},
					std::format("Could not enumerate folder contents: {}", Ec.message()));
		}

		if (!DeleteAssets) AssetPaths.clear();
		if (!DeleteAssets || !DeleteFiles)
		{
			PhysicalRoots.clear();
			for (auto& Entry : Plan->Entries)
			{
				if (Entry.Kind == EContentDeletionEntryKind::OrdinaryFile)
				{
					FAssetCompanionOwnership Ownership;
					const auto Inspected = QueryAssetCompanionOwnership(Entry.PhysicalPath, Ownership);
					if (!Inspected || Ownership.State == EAssetCompanionOwnershipState::Ambiguous)
						AddBlocker(EContentDeletionBlocker::CompanionOwnershipConflict, Entry.PhysicalPath, Entry.PhysicalPath, {}, "Companion ownership cannot be established.");
					if (Ownership.State != EAssetCompanionOwnershipState::Unclaimed) Entry.Kind = EContentDeletionEntryKind::ManagedCompanion;
				}
				if ((DeleteAssets && Entry.Kind == EContentDeletionEntryKind::AssetPackage) || (DeleteFiles && Entry.Kind == EContentDeletionEntryKind::OrdinaryFile)) PhysicalRoots.emplace_back(Entry.PhysicalPath);
			}
		}
		std::ranges::sort(AssetPaths, [](const FPackagePath& A, const FPackagePath& B) {
			return A.GetView() < B.GetView();
		});
		AssetPaths.erase(std::unique(AssetPaths.begin(), AssetPaths.end()), AssetPaths.end());
		std::string AdmissionError;
		FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) { if (Level >= ELogLevel::Error) AdmissionError += Message; });
		const bool bPrepared = IAssetTools::Get().PrepareDeletion({.AssetPaths = AssetPaths, .PhysicalRoots = PhysicalRoots}, OutOperation, &Feedback);
		if (!bPrepared && OutOperation.GetBlockers().empty())
			AddBlocker(EContentDeletionBlocker::InspectionFailed, "Assets", {}, {}, AdmissionError);

		std::unordered_set<std::string> CompanionPaths;
		for (const FAssetDeletionEntry& Entry : OutOperation.GetEntries())
			for (const std::filesystem::path& Companion : Entry.CompanionFiles)
				CompanionPaths.insert(NormalizePath(Companion.generic_string()));
		for (FContentDeletionFingerprint& Entry : Plan->Entries)
			if (CompanionPaths.contains(Entry.PhysicalPath)
				&& Entry.Kind == EContentDeletionEntryKind::OrdinaryFile)
				Entry.Kind = EContentDeletionEntryKind::ManagedCompanion;

		for (const std::string& CompanionPath : CompanionPaths)
		{
			if (std::ranges::any_of(
					Plan->Entries,
					[&](const FContentDeletionFingerprint& Entry) {
						return Entry.PhysicalPath == CompanionPath;
					}))
				continue;
			std::error_code Ec;
			if (!std::filesystem::is_regular_file(CompanionPath, Ec))
			{
				if (Ec
					&& Ec != std::errc::no_such_file_or_directory
					&& Ec != std::errc::not_a_directory)
					AddBlocker(
						EContentDeletionBlocker::InspectionFailed,
						std::filesystem::path(CompanionPath).filename().generic_string(),
						CompanionPath,
						{},
						std::format("Could not classify asset companion: {}", Ec.message()));
				continue;
			}

			const FMountPoint* SelectedMount =
				MaximalRoots.empty() ? nullptr : MaximalRoots.front().Mount;
			std::filesystem::path SelectedReparsePoint;
			if (SelectedMount
				&& FPaths::IsLexicalDescendantPath(
					CompanionPath, SelectedMount->GetContentDir().generic_string(), true)
				&& FindReparsePointInPath(
					SelectedMount->GetContentDir().generic_string(),
					CompanionPath,
					SelectedReparsePoint,
					Ec))
			{
				AddBlocker(
					EContentDeletionBlocker::ReparsePoint,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format(
						"Asset companion path traverses reparse point {}.",
						SelectedReparsePoint.generic_string()));
				continue;
			}
			if (Ec)
			{
				AddBlocker(
					EContentDeletionBlocker::InspectionFailed,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format("Could not inspect asset companion: {}", Ec.message()));
				continue;
			}

			const FContentBrowserPaths::FMountPath Resolved =
				FContentBrowserPaths::ResolveMountPath(CompanionPath);
			if (!Resolved)
			{
				AddBlocker(
					EContentDeletionBlocker::OutsideMount,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					"Asset companion is outside every mounted content root.");
				continue;
			}
			const FMountPoint* CompanionMount = Resolved.Mount;
			bool bCompanionRootSafe = true;
			if (!SelectedMount || CompanionMount != SelectedMount)
			{
				AddBlocker(
					EContentDeletionBlocker::UnsupportedMount,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					"An external asset companion must belong to the selected content mount.");
				bCompanionRootSafe = false;
			}
			if (!CompanionMount->bContentWritable)
			{
				AddBlocker(
					EContentDeletionBlocker::ReadOnlyMount,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					"The asset companion's content mount is not content-writable.");
				bCompanionRootSafe = false;
			}
			std::filesystem::path ReparsePoint;
			const bool bReparse = FindReparsePointInPath(
				CompanionMount->GetContentDir().generic_string(), CompanionPath, ReparsePoint, Ec);
			if (Ec)
			{
				AddBlocker(
					EContentDeletionBlocker::InspectionFailed,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format("Could not inspect asset companion: {}", Ec.message()));
				continue;
			}
			if (bReparse)
			{
				AddBlocker(
					EContentDeletionBlocker::ReparsePoint,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format(
						"Asset companion path traverses reparse point {}.",
						ReparsePoint.generic_string()));
				bCompanionRootSafe = false;
			}
			if (!bCompanionRootSafe) continue;
			FContentDeletionFingerprint Fingerprint;
			if (!MakeFingerprint(
					CompanionPath,
					EContentDeletionEntryKind::ManagedCompanion,
					Fingerprint,
					Ec))
			{
				AddBlocker(
					EContentDeletionBlocker::InspectionFailed,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format("Could not fingerprint companion: {}", Ec.message()));
				continue;
			}
			Plan->Entries.push_back(Fingerprint);
			Plan->MaximalRoots.push_back({
				.OriginalPath = CompanionPath});
		}

		for (const FAssetDeletionBlocker& Blocker : OutOperation.GetBlockers())
		{
			EContentDeletionBlocker Kind = EContentDeletionBlocker::InspectionFailed;
			switch (Blocker.Kind)
			{
			case EAssetDeletionBlocker::ExternalPersistentReference:
			case EAssetDeletionBlocker::ExternalLoadedReference:
				Kind = EContentDeletionBlocker::ExternalReference;
				break;
			case EAssetDeletionBlocker::RedirectorTargetNotSelected:
				Kind = EContentDeletionBlocker::RedirectorTargetNotSelected;
				break;
			case EAssetDeletionBlocker::TargetRedirectorsNotSelected:
				Kind = EContentDeletionBlocker::TargetRedirectorsNotSelected;
				break;
			case EAssetDeletionBlocker::LoadingPackage:
				Kind = EContentDeletionBlocker::LoadingPackage;
				break;
			case EAssetDeletionBlocker::DirtyPackage:
				Kind = EContentDeletionBlocker::DirtyPackage;
				break;
			case EAssetDeletionBlocker::ReferenceStoreInspectionFailed:
				Kind = EContentDeletionBlocker::ReferenceStoreInspectionFailed;
				break;
			case EAssetDeletionBlocker::CompanionInspectionFailed:
				Kind = EContentDeletionBlocker::CompanionInspectionFailed;
				break;
			case EAssetDeletionBlocker::CompanionOwnershipConflict:
				Kind = EContentDeletionBlocker::CompanionOwnershipConflict;
				break;
			case EAssetDeletionBlocker::ExternalCompanionOwner:
				Kind = EContentDeletionBlocker::ExternalCompanionOwner;
				break;
			default:
				break;
			}
			AddBlocker(
				Kind,
				Blocker.AssetPath.ToString(),
				NormalizePath(Blocker.PhysicalPath.generic_string()),
				Blocker.RelatedAssetPath.ToString(),
				Blocker.Details);
		}
		for (const FAssetDeletionWarning& Warning : OutOperation.GetWarnings())
			Plan->Warnings.push_back({
				.DisplayName = Warning.AssetPath.ToString(),
				.Details = Warning.Details});

		std::ranges::sort(
			Plan->Entries,
			{},
			&FContentDeletionFingerprint::PhysicalPath);
		for (const FContentDeletionFingerprint& Entry : Plan->Entries)
			switch (Entry.Kind)
			{
			case EContentDeletionEntryKind::Directory:
				++Plan->Summary.FolderCount;
				break;
			case EContentDeletionEntryKind::AssetPackage:
				if (DeleteAssets) ++Plan->Summary.AssetCount;
				break;
			case EContentDeletionEntryKind::ManagedCompanion:
				if (DeleteAssets) ++Plan->Summary.CompanionCount;
				break;
			case EContentDeletionEntryKind::OrdinaryFile:
			case EContentDeletionEntryKind::UnknownPackage:
				if (DeleteFiles) ++Plan->Summary.FileCount;
				break;
			}
		std::ranges::sort(
			Plan->Blockers,
			[](const FContentDeletionBlocker& A, const FContentDeletionBlocker& B) {
				return std::tie(A.PhysicalPath, A.RelatedAssetPath, A.Kind, A.Details)
					< std::tie(B.PhysicalPath, B.RelatedAssetPath, B.Kind, B.Details);
			});
		Plan->Blockers.erase(
			std::unique(Plan->Blockers.begin(), Plan->Blockers.end(),
				[](const FContentDeletionBlocker& A,
					const FContentDeletionBlocker& B) {
					return A.Kind == B.Kind
						&& A.PhysicalPath == B.PhysicalPath
						&& A.RelatedAssetPath == B.RelatedAssetPath
						&& A.Details == B.Details;
				}),
			Plan->Blockers.end());
		return Plan;
	}

	FContentBrowserDeletionOperation::FContentBrowserDeletionOperation(const FContentBrowserOperationContext& Host, bool InDeleteAssets, bool InDeleteFiles)
		: Policy(Host), Diagnostics(Host.ReportDiagnostic), DeleteAssets(InDeleteAssets), DeleteFiles(InDeleteFiles) {}
	auto FContentBrowserDeletionOperation::CanModify(std::string* Error) const -> bool
	{
		if (Policy.CanMutateContent && !Policy.CanMutateContent()) { if (Error) *Error = "Content mutation is currently disabled."; return false; }
		return true;
	}
	auto FContentBrowserDeletionOperation::Report(ELogLevel Level, std::string_view Message) const -> void
	{
		if (Diagnostics) Diagnostics(Level, Message); else FFeedbackContext::Get().Serialize(Level, Message);
	}
	auto FContentBrowserDeletionOperation::NotifyItemsUpdated(FContentChangeBatch Changes) -> void
	{
		if (!Changes.bFullRefresh && Changes.Changes.empty()) return;
		if (Policy.NotifyScopedContentMutation) Policy.NotifyScopedContentMutation(std::move(Changes));
		else if (Policy.NotifyMountedContentMutation) Policy.NotifyMountedContentMutation();
	}
	auto FContentBrowserDeletionOperation::PrepareDeleteItems(std::span<const FContentBrowserNativeItem> Items) -> FContentDeletionPlanPtr
	{
		FAssetDeletionOperation AssetOperation;
		auto Plan = std::make_shared<FContentDeletionPlan>(*AnalyzeDeletion(Items, AssetOperation));
		Plan->SessionId = ++NextSession;
		CurrentPlan = Plan;
		CapturedRequest.assign(Items.begin(), Items.end());
		Execution = std::unique_ptr<FContentDeletionOperation>(new FContentDeletionOperation(Plan, std::move(AssetOperation), DeleteAssets, DeleteFiles));
		return Plan;
	}
	auto FContentBrowserDeletionOperation::DismissDeletion(FContentDeletionPlanPtr Plan) -> void
	{
		if (Plan == CurrentPlan) { CurrentPlan.reset(); Execution.reset(); CapturedRequest.clear(); }
	}
	auto FContentBrowserDeletionOperation::ValidateForExecution() -> FAssetWriteResult
	{
		std::string Error;
		if (!CanModify(&Error)) return {EAssetWriteError::ReadOnlyMode, Error};
		if (!Execution) return {EAssetWriteError::StaleData, "Deletion confirmation was consumed."};
		const auto Status = Execution->ValidateScope();
		if (Status) Execution->AllowPeerScopeChanges();
		return Status;
	}
	auto FContentBrowserDeletionOperation::DeleteItems(FContentDeletionPlanPtr& Confirmation, FContentDeletionHooks Hooks) -> TContentBrowserOperationResult<std::string>
	{
		TContentBrowserOperationResult<std::string> Result;
		auto Reject = [&](std::string Error) {
			Report(ELogLevel::Error, Error);
			if (Confirmation) for (const auto& Entry : Confirmation->Entries) Result.Add(Entry.PhysicalPath, EContentBrowserEffectStatus::Unattempted, Error);
			Result.bStopped = true;
			return Result;
		};
		std::string Error;
		if (!CanModify(&Error)) return Reject(Error);
		if (!Confirmation || Confirmation != CurrentPlan || !Execution) return Reject("Deletion confirmation was retired.");
		if (!Confirmation->CanExecute()) return Reject("Deletion is blocked.");
		for (const auto& Root : Confirmation->MaximalRoots)
		{
			const auto Mount = FContentBrowserPaths::ResolveMountPath(Root.OriginalPath);
			if (!Mount || !Mount.Mount->bContentWritable || AreSamePath(Root.OriginalPath, Mount.Mount->GetContentDir().generic_string())) return Reject("Deletion mount policy changed.");
			std::filesystem::path Reparse;
			std::error_code Ec;
			auto Parent = std::filesystem::path(Root.OriginalPath).parent_path();
			while (!std::filesystem::exists(Parent, Ec) && !Ec && Parent != Parent.parent_path()) Parent = Parent.parent_path();
			if (Ec || FindReparsePointInPath(Mount.Mount->GetContentDir().generic_string(), Parent, Reparse, Ec) || Ec) return Reject("Deletion ancestor changed or cannot be inspected.");
		}
		const auto CallerAdmission = Hooks.CanExecuteEffect;
		Hooks.CanExecuteEffect = [this, CallerAdmission] { return CanModify() && (!CallerAdmission || CallerAdmission()) && CanModify(); };
		const bool Succeeded = Execution->Execute(std::move(Hooks));
		const auto Native = Execution->GetResult();
		if (!Succeeded && !Execution->HasStarted())
		{
			if (Native.Error != EAssetWriteError::StaleData) return Reject(Native.Message);
			const auto Request = CapturedRequest;
			Confirmation = PrepareDeleteItems(Request);
			Report(ELogLevel::Warn, Native.Message + " Review and confirm the updated deletion plan.");
			Result.bStopped = true;
			for (const auto& Entry : Confirmation->Entries) Result.Add(Entry.PhysicalPath, EContentBrowserEffectStatus::Unattempted, "Confirm the replacement deletion plan before retrying.");
			return Result;
		}
		Result = Execution->GetEffects();
		FContentChangeBatch Changes;
		for (const auto& Effect : Result.Effects)
			if (Effect.Status == EContentBrowserEffectStatus::Committed)
			{
				const auto Entry = std::ranges::find(Confirmation->Entries, Effect.OriginalIdentity, &FContentDeletionFingerprint::PhysicalPath);
				Changes.Changes.push_back({EContentChangeKind::Removed, Effect.OriginalIdentity, {}, {}, {}, Entry != Confirmation->Entries.end() && Entry->Kind == EContentDeletionEntryKind::Directory});
			}
		if (!Native)
		{
			Report(Native.Effect == EAssetWriteEffect::ContentCommittedProjectionPending ? ELogLevel::Warn : ELogLevel::Error, Native.Message);
			if (Result.IsComplete()) Result.Add({}, EContentBrowserEffectStatus::Failed, Native.Message);
			Changes.bFullRefresh = true;
		}
		NotifyItemsUpdated(std::move(Changes));
		CurrentPlan.reset(); Execution.reset(); CapturedRequest.clear(); Confirmation.reset();
		return Result;
	}
}
