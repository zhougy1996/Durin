#include "AssetTools/IAssetTools.h"

#include "Asset/AssetOperations.h"
#include "AssetMaintenance/CanonicalResave.h"
#include "AssetMaintenance/CompatibilityAudit.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/Deletion.h"
#include "Asset/Load.h"
#include "Asset/MutationTypes.h"
#include "Asset/RedirectorFixup.h"
#include "Asset/Relocation.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "Editor/Transactor.h"

namespace Durin
{
	auto MakeRejectedAssetOperation(
		EAssetOperationKind Kind, std::string Message) -> FAssetOperationResult
	{
		return {
			.Kind = Kind,
			.State = EAssetOperationTerminalState::Rejected,
			.Message = std::move(Message)};
	}

	namespace
	{
		auto FromEngineResult(
			EAssetOperationKind Kind,
			const Asset::FAssetResult& Result,
			std::span<const FPackagePath> Affected = {}) -> FAssetOperationResult
		{
			FAssetOperationResult Operation{
				.Kind = Kind,
				.State = Result
					? EAssetOperationTerminalState::Completed
					: EAssetOperationTerminalState::Rejected,
				.Message = Result.Message};
			Operation.AffectedAssets.assign(Affected.begin(), Affected.end());
			return Operation;
		}

		auto Publish(
			const FPublishAssetOperation& Callback,
			FAssetOperationResult& Result) -> void
		{
			if (!Callback || !Result) return;
			Callback({
				.Kind = Result.Kind,
				.Phase = EAssetOperationPhase::Execute,
				.Persistence = Result.Persistence,
				.AffectedAssets = Result.AffectedAssets,
				.Warnings = Result.Warnings});
			Result.bPublished = true;
		}

		class FAssetMutationEditorTransaction final : public Editor::ITransactionCustomChange
		{
		public:
			FAssetMutationEditorTransaction(
				Asset::FAssetMutationTransaction InTransaction,
				std::string InSingular,
				std::string InPlural,
				size_t InSingularScopeSize)
				: Transaction(std::move(InTransaction))
				, Singular(std::move(InSingular))
				, Plural(std::move(InPlural))
				, SingularScopeSize(InSingularScopeSize)
			{
			}

			auto GetDescription() const -> std::string_view override
			{
				return Transaction.GetSummary().GetScope().size() == SingularScopeSize
					? Singular : Plural;
			}
			auto GetDetails(Editor::ETransactionOperation) const
				-> std::string override { return LastResult.Message; }
			auto MutatesMountedContent() const -> bool override { return true; }
			auto GetOwningModule() const -> std::string_view override { return "AssetTools"; }
			auto Undo() -> bool override
			{
				LastResult = Transaction.Undo();
				return static_cast<bool>(LastResult);
			}
			auto Redo() -> bool override
			{
				LastResult = Transaction.Redo();
				return static_cast<bool>(LastResult);
			}

		private:
			Asset::FAssetMutationTransaction Transaction;
			std::string Singular;
			std::string Plural;
			size_t SingularScopeSize = 1;
			Asset::FAssetResult LastResult;
		};

		auto CommitMutation(
			Asset::FAssetMutationTransaction Transaction,
			DTransactor& Transactions,
			EAssetOperationKind Kind,
			std::span<const FPackagePath> Affected,
			std::string Singular,
			std::string Plural,
			size_t SingularScopeSize = 1) -> FAssetOperationResult
		{
			const Asset::FAssetResult Committed = Transaction.Commit();
			if (!Committed) return FromEngineResult(Kind, Committed, Affected);
			if (!Transactions.CommitApplied(
					std::make_unique<FAssetMutationEditorTransaction>(
						std::move(Transaction), std::move(Singular), std::move(Plural),
						SingularScopeSize)))
				return MakeRejectedAssetOperation(
					Kind, "The committed asset change could not be retained in editor history.");
			FAssetOperationResult Result{
				.Kind = Kind,
				.Persistence = EAssetOperationPersistenceState::Persisted,
				.bPublished = true};
			Result.AffectedAssets.assign(Affected.begin(), Affected.end());
			return Result;
		}
	}

	struct FAssetDeletionOperation::FState
	{
		Asset::FAssetDeletionTransaction Transaction;
	};

	FAssetDeletionOperation::FAssetDeletionOperation()
		: State(std::make_shared<FState>()) {}
	FAssetDeletionOperation::~FAssetDeletionOperation() = default;
	FAssetDeletionOperation::FAssetDeletionOperation(
		FAssetDeletionOperation&&) noexcept = default;
	auto FAssetDeletionOperation::operator=(FAssetDeletionOperation&&) noexcept
		-> FAssetDeletionOperation& = default;

	auto FAssetDeletionOperation::Commit(
		const Asset::FAssetDeletionPhysicalTransition& Transition)
		-> FAssetOperationResult
	{
		const Asset::FAssetResult EngineResult = State->Transaction.Commit(Transition);
		FAssetOperationResult Result = FromEngineResult(
			EAssetOperationKind::Delete, EngineResult);
		for (const FAssetDeletionEntry& Entry : Entries)
			Result.AffectedAssets.push_back(Entry.AssetPath);
		if (!EngineResult && State->Transaction.GetState()
			== Asset::EAssetMutationTransactionState::RecoveryRequired)
			Result.State = EAssetOperationTerminalState::RecoveryRequired;
		else if (EngineResult)
			Result.Persistence = EAssetOperationPersistenceState::Persisted;
		return Result;
	}

	auto FAssetDeletionOperation::Undo(
		const Asset::FAssetDeletionPhysicalTransition& Transition)
		-> FAssetOperationResult
	{
		const Asset::FAssetResult EngineResult = State->Transaction.Undo(Transition);
		FAssetOperationResult Result = FromEngineResult(
			EAssetOperationKind::Delete, EngineResult);
		if (!EngineResult && State->Transaction.GetState()
			== Asset::EAssetMutationTransactionState::RecoveryRequired)
			Result.State = EAssetOperationTerminalState::RecoveryRequired;
		return Result;
	}

	auto FAssetDeletionOperation::Redo(
		const Asset::FAssetDeletionPhysicalTransition& Transition)
		-> FAssetOperationResult
	{
		const Asset::FAssetResult EngineResult = State->Transaction.Redo(Transition);
		FAssetOperationResult Result = FromEngineResult(
			EAssetOperationKind::Delete, EngineResult);
		if (!EngineResult && State->Transaction.GetState()
			== Asset::EAssetMutationTransactionState::RecoveryRequired)
			Result.State = EAssetOperationTerminalState::RecoveryRequired;
		return Result;
	}

	auto DuplicateAssetWithEditorPolicy(const FAssetDuplicateRequest& Request)
		-> FAssetOperationResult
	{
		if (!Request.SourcePath.IsValid() || Request.DestinationDirectory.empty()
			|| !Request.ResolvePhysicalPackagePath)
			return MakeRejectedAssetOperation(EAssetOperationKind::Duplicate,
				"Asset duplication requires a source, destination, and path resolver.");
		const Asset::FTopLevelAssetCatalogEntry Source =
			Asset::FindTopLevelAssetExact(Request.SourcePath);
		if (!Source || Source->IsRedirector())
			return MakeRejectedAssetOperation(EAssetOperationKind::Duplicate,
				"The copied source is no longer an available real asset.");

		std::string Directory = Request.DestinationDirectory;
		if (!Directory.ends_with('/')) Directory.push_back('/');
		const std::string AssetName(Request.SourcePath.GetAssetName());
		FTopLevelAssetPath DestinationAssetPath;
		FPackagePath DestinationPackagePath;
		std::string DestinationPhysicalPath;
		for (uint32 Suffix = 0; Suffix <= 10000; ++Suffix)
		{
			const std::string CandidateName = Suffix == 0 ? AssetName
				: Suffix == 1 ? AssetName + "_Copy"
				: std::format("{}_Copy{}", AssetName, Suffix);
			FPackagePath CandidatePath;
			if (!FPackagePath::TryCreate(Directory + CandidateName, CandidatePath)) continue;
			std::string CandidatePhysical = Request.ResolvePhysicalPackagePath(CandidatePath);
			if (CandidatePhysical.empty() || Asset::FindAssetExact(CandidatePath)
				|| Asset::FindResidentPackage(CandidatePath)) continue;
			std::error_code Error;
			const bool bExists = std::filesystem::exists(CandidatePhysical, Error);
			if (Error)
				return MakeRejectedAssetOperation(EAssetOperationKind::Duplicate,
					std::format("Could not inspect the duplicate destination: {}", Error.message()));
			if (bExists) continue;
			if (!FTopLevelAssetPath::TryCreate(
				CandidatePath, CandidateName, DestinationAssetPath)) continue;
			DestinationPackagePath = std::move(CandidatePath);
			DestinationPhysicalPath = std::move(CandidatePhysical);
			break;
		}
		if (!DestinationAssetPath.IsValid())
			return MakeRejectedAssetOperation(EAssetOperationKind::Duplicate,
				"Could not find an available copy name in this folder.");

		DObject* DuplicatedAsset = nullptr;
		Asset::FAssetResult EngineResult = Asset::DuplicateAsset(
			Request.SourcePath, DestinationAssetPath, DuplicatedAsset);
		if (!EngineResult)
			return FromEngineResult(EAssetOperationKind::Duplicate, EngineResult);
		if (Request.bSave)
		{
			EngineResult = Asset::SavePackage(DuplicatedAsset->GetPackage());
			if (!EngineResult)
			{
				FAssetOperationResult Failure = FromEngineResult(
					EAssetOperationKind::Duplicate, EngineResult);
				const Asset::FAssetResult Cleanup = Asset::UnloadPackage(
					DestinationPackagePath,
					Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
				if (!Cleanup)
				{
					Failure.State = EAssetOperationTerminalState::RecoveryRequired;
					Failure.Message += std::format(
						" The unsaved duplicate could not be discarded: {}", Cleanup.Message);
				}
				return Failure;
			}
		}
		FAssetOperationResult Result{
			.Kind = EAssetOperationKind::Duplicate,
			.Persistence = Request.bSave
				? EAssetOperationPersistenceState::Persisted
				: EAssetOperationPersistenceState::Dirty,
			.AffectedAssets = {DestinationPackagePath},
			.Asset = DuplicatedAsset,
			.Package = DuplicatedAsset->GetPackage(),
			.PhysicalPath = std::move(DestinationPhysicalPath)};
		Publish(Request.Publish, Result);
		return Result;
	}

	auto SaveAssetsWithEditorPolicy(const FAssetSaveRequest& Request)
		-> FAssetOperationResult
	{
		if (Request.AssetPaths.empty())
			return MakeRejectedAssetOperation(
				EAssetOperationKind::Save, "No asset packages were selected for saving.");
		if (Request.Mode == EAssetSaveMode::CanonicalResave)
		{
			const Asset::FAssetPackageDiscoverySnapshot Snapshot =
				Asset::CaptureMountedAssetPackageSnapshot();
			if (Snapshot.Status != Asset::EAssetPackageSnapshotStatus::Completed)
				return MakeRejectedAssetOperation(EAssetOperationKind::Save,
					Snapshot.Error.empty() ? "Canonical-resave discovery did not complete."
						: Snapshot.Error);
			const Asset::FReflectionCompatibilityCatalog Catalog =
				Asset::FReflectionCompatibilityCatalog::Capture();
			std::vector<Asset::FAssetPackageCompatibilityProbeInput> Inputs;
			for (const FPackagePath& Path : Request.AssetPaths)
			{
				const auto Input = std::ranges::find(
					Snapshot.Packages, Path,
					&Asset::FAssetPackageCompatibilityProbeInput::PackagePath);
				if (Input == Snapshot.Packages.end())
					return MakeRejectedAssetOperation(EAssetOperationKind::Save, std::format(
						"Package {} is not in a content-writable mounted snapshot.", Path.ToString()));
				Inputs.push_back(*Input);
				Inputs.back().bIncludeNestedMigrationEvidence = true;
			}
			auto Audit = Asset::RunAssetCompatibilityAudit(Inputs, Catalog);
			if (Audit.Status != Asset::EAssetCompatibilityAuditStatus::Completed
				|| Audit.Records.size() != Inputs.size())
				return MakeRejectedAssetOperation(EAssetOperationKind::Save,
					"Selected packages could not be inspected completely.");
			Asset::FAssetCanonicalResaveSelection Selection{
				.Packages = Request.AssetPaths, .bAllowPlainResave = true};
			auto Plan = Asset::PlanAssetCanonicalResaves(Audit.Records, Selection);
			auto Applied = Asset::ApplyAssetCanonicalResaves(std::move(Plan), Catalog);
			if (Applied.Status != Asset::EAssetCanonicalResaveApplyStatus::Succeeded)
				return MakeRejectedAssetOperation(EAssetOperationKind::Save,
					Applied.Diagnostic.empty()
						? Asset::SerializeAssetCanonicalResaveApplyReport(Applied)
						: Applied.Diagnostic);
			FAssetOperationResult Result{
				.Kind = EAssetOperationKind::Save,
				.Persistence = EAssetOperationPersistenceState::Persisted,
				.AffectedAssets = Request.AssetPaths};
			Publish(Request.Publish, Result);
			return Result;
		}

		for (const FPackagePath& Path : Request.AssetPaths)
		{
			DPackage* Package = Asset::FindResidentPackage(Path);
			if (!Package || !Package->IsDirty())
				return MakeRejectedAssetOperation(EAssetOperationKind::Save,
					"Save Package is available only for a loaded package with authored changes.");
		}
		FAssetOperationResult Result{
			.Kind = EAssetOperationKind::Save,
			.Persistence = EAssetOperationPersistenceState::Persisted};
		for (const FPackagePath& Path : Request.AssetPaths)
		{
			const Asset::FAssetResult Saved =
				Asset::SavePackage(Asset::FindResidentPackage(Path));
			if (!Saved)
			{
				Result.State = EAssetOperationTerminalState::Rejected;
				Result.Persistence = Result.AffectedAssets.empty()
					? EAssetOperationPersistenceState::Dirty
					: EAssetOperationPersistenceState::PartiallyPersisted;
				Result.Message = Saved.Message;
				return Result;
			}
			Result.AffectedAssets.push_back(Path);
		}
		Publish(Request.Publish, Result);
		return Result;
	}

	auto RelocateAssetsWithEditorPolicy(const FAssetRelocationRequest& Request)
		-> FAssetOperationResult
	{
		if (!Request.Transactions)
			return MakeRejectedAssetOperation(EAssetOperationKind::Relocate,
				"Asset relocation requires editor transaction history.");
		if (Request.Transactions->HasPendingOperation())
			return MakeRejectedAssetOperation(EAssetOperationKind::Relocate,
				"Asset relocation is unavailable while another history operation is pending.");
		if (Request.Mappings.empty()) return {.Kind = EAssetOperationKind::Relocate};
		std::vector<Asset::FAssetRelocationMapping> Mappings;
		std::vector<FPackagePath> Affected;
		for (const FAssetRelocation& Mapping : Request.Mappings)
		{
			Mappings.push_back({Mapping.SourcePath, Mapping.DestinationPath});
			Affected.push_back(Mapping.SourcePath);
			Affected.push_back(Mapping.DestinationPath);
		}
		Asset::FAssetMutationSummary Summary;
		Asset::FAssetMutationTransaction Transaction;
		const Asset::FAssetResult Prepared = Asset::PrepareAssetRelocationTransaction(
			Mappings, Summary, Transaction);
		if (!Prepared)
			return FromEngineResult(EAssetOperationKind::Relocate, Prepared, Affected);
		return CommitMutation(
			std::move(Transaction), *Request.Transactions,
			EAssetOperationKind::Relocate, Affected,
			"Move Asset", "Move Assets", 2);
	}

	auto FixUpRedirectorsWithEditorPolicy(
		const FAssetRedirectorFixupRequest& Request) -> FAssetOperationResult
	{
		if (!Request.Transactions)
			return MakeRejectedAssetOperation(EAssetOperationKind::FixUpRedirectors,
				"Redirector fix-up requires editor transaction history.");
		if (Request.Transactions->HasPendingOperation())
			return MakeRejectedAssetOperation(EAssetOperationKind::FixUpRedirectors,
				"Redirector fix-up is unavailable while another history operation is pending.");
		if (Request.Redirectors.empty())
			return {.Kind = EAssetOperationKind::FixUpRedirectors};
		Asset::FAssetRedirectorFixupSummary Summary;
		Asset::FAssetMutationTransaction Transaction;
		const Asset::FAssetResult Prepared = Asset::PrepareRedirectorFixupTransaction(
			Request.Redirectors,
			Request.bDeleteRedirectors
				? Asset::EAssetRedirectorFixupMode::RewriteAndDelete
				: Asset::EAssetRedirectorFixupMode::RewriteOnly,
			Summary, Transaction);
		if (!Prepared)
			return FromEngineResult(
				EAssetOperationKind::FixUpRedirectors, Prepared, Request.Redirectors);
		return CommitMutation(
			std::move(Transaction), *Request.Transactions,
			EAssetOperationKind::FixUpRedirectors, Request.Redirectors,
			"Fix Up Redirector", "Fix Up Redirectors");
	}

	auto PrepareAssetDeletionWithEditorPolicy(
		const FAssetDeletionRequest& Request,
		FAssetDeletionOperation& OutOperation) -> FAssetOperationResult
	{
		OutOperation = FAssetDeletionOperation{};
		std::vector<Asset::FAssetDeletionBatchBlocker> EngineBlockers;
		const Asset::FAssetResult Prepared = Asset::PrepareAssetDeletionTransaction(
			Request.AssetPaths, Request.PhysicalRoots,
			OutOperation.State->Transaction, EngineBlockers);
		for (const Asset::FAssetDeletionBatchBlocker& Blocker : EngineBlockers)
			OutOperation.Blockers.push_back({
				.Kind = static_cast<EAssetDeletionBlocker>(Blocker.Kind),
				.AssetPath = Blocker.AssetPath,
				.RelatedAssetPath = Blocker.RelatedAssetPath,
				.PhysicalPath = Blocker.PhysicalPath,
				.Details = Blocker.Details});
		for (const Asset::FAssetDeletionBatchEntry& Entry :
			OutOperation.State->Transaction.GetEntries())
			OutOperation.Entries.push_back({
				.AssetPath = Entry.RegistryEntry.PackagePath,
				.PhysicalPath = Entry.RegistryEntry.PhysicalPath,
				.CompanionFiles = Entry.CompanionFiles,
				.bLoaded = Entry.bLoaded});
		for (const Asset::FAssetDeletionBatchWarning& Warning :
			OutOperation.State->Transaction.GetWarnings())
			OutOperation.Warnings.push_back({
				.AssetPath = Warning.TargetPath,
				.Details = Warning.Details});
		FAssetOperationResult Result = FromEngineResult(
			EAssetOperationKind::Delete, Prepared, Request.AssetPaths);
		Result.Warnings = OutOperation.Warnings;
		if (!OutOperation.Blockers.empty())
			Result.State = EAssetOperationTerminalState::Rejected;
		return Result;
	}
}
