#include "MultiOutputImport.h"
#include "ImportService.h"
#include "ImportRegistryInternal.h"

#include "AssetAuthoring.h"

namespace Durin::Asset
{
	namespace
	{
		auto AddDiagnostic(
			std::vector<FImportDiagnostic>& Diagnostics,
			EImportDiagnosticCategory Category,
			std::string_view Phase,
			std::string_view OutputIdentity,
			std::string_view Message) -> void
		{
			FImportDiagnostic Diagnostic{
				.Severity = EImportDiagnosticSeverity::Error,
				.Category = Category,
				.Phase = std::string(Phase),
				.SourceIdentity = "root",
				.OutputIdentity = std::string(OutputIdentity),
				.Message = std::string(Message)};
			if (Diagnostic.OutputIdentity.empty()) Diagnostic.OutputIdentity = "request";
			Diagnostic.Identity = GetImportDiagnosticIdentity(Diagnostic);
			Diagnostics.push_back(std::move(Diagnostic));
		}

		auto ToFrameworkDiagnostic(const FImportRecordDiagnostic& Diagnostic)
			-> FImportDiagnostic
		{
			return {
				.Severity = static_cast<EImportDiagnosticSeverity>(Diagnostic.Severity),
				.Category = static_cast<EImportDiagnosticCategory>(Diagnostic.Category),
				.Identity = Diagnostic.Identity,
				.Phase = Diagnostic.Phase,
				.SourceIdentity = Diagnostic.SourceIdentity,
				.OutputIdentity = Diagnostic.OutputIdentity,
				.Message = Diagnostic.Message};
		}

		auto ToRecordDiagnostic(const FImportDiagnostic& Diagnostic)
			-> FImportRecordDiagnostic
		{
			return {
				.Identity = GetImportDiagnosticIdentity(Diagnostic),
				.Severity = static_cast<uint8>(Diagnostic.Severity),
				.Category = static_cast<uint8>(Diagnostic.Category),
				.Phase = Diagnostic.Phase,
				.SourceIdentity = Diagnostic.SourceIdentity,
				.OutputIdentity = Diagnostic.OutputIdentity,
				.Message = Diagnostic.Message};
		}

		auto AbandonPrepared(FPreparedMultiOutputImport& Prepared) noexcept -> void
		{
			for (FPreparedMultiOutput& Output : Prepared.Outputs)
			{
				if (Output.Exchange)
				{
					Output.Exchange->Finalize();
					Output.Exchange.reset();
				}
				if (Output.Candidate)
				{
					Output.Candidate->Abandon();
					Output.Candidate.reset();
				}
			}
		}

		class FProgressBoundary
		{
		public:
			FProgressBoundary(IImportProgressReporter* InReporter, EImportPhase InPhase)
				: Reporter(InReporter), Phase(InPhase)
			{
				ReportImportProgress(Reporter, Phase, EImportProgressState::Started);
			}
			~FProgressBoundary()
			{
				if (!bFinished)
					ReportImportProgress(Reporter, Phase, EImportProgressState::Failed,
						"root", "request", 0, 0, "Import phase failed.");
			}
			auto Succeed(uint64 Completed = 1, uint64 Total = 1) -> void
			{
				if (bFinished) return;
				bFinished = true;
				ReportImportProgress(Reporter, Phase, EImportProgressState::Succeeded,
					"root", "request", Completed, Total);
			}
		private:
			IImportProgressReporter* Reporter = nullptr;
			EImportPhase Phase = EImportPhase::Validation;
			bool bFinished = false;
		};

		class FMultiDiagnosticFinalizer
		{
		public:
			explicit FMultiDiagnosticFinalizer(FMultiOutputExecutionResult& InResult)
				: Result(InResult) {}
			~FMultiDiagnosticFinalizer()
			{
				if (!Result.bSucceeded && Result.Diagnostics.empty()
					&& !Result.Message.empty())
				{
					AddDiagnostic(Result.Diagnostics,
						EImportDiagnosticCategory::ProviderFailure,
						"multi-output-execution", "request", Result.Message);
				}
				FinalizeImportDiagnostics(
					Result.Diagnostics, "multi-output-execution");
			}
		private:
			FMultiOutputExecutionResult& Result;
		};

		auto ToRecordPolicy(EImportOutputPolicy Policy) -> EImportRecordOutputPolicy
		{
			switch (Policy)
			{
			case EImportOutputPolicy::Reference: return EImportRecordOutputPolicy::Referenced;
			case EImportOutputPolicy::Detach: return EImportRecordOutputPolicy::Detached;
			default: return EImportRecordOutputPolicy::Managed;
			}
		}

		auto MakeTemporaryRecordPath(const FAssetPath& Path, FAssetPath& OutPath) -> bool
		{
			for (uint32 Suffix = 1; Suffix != 0; ++Suffix)
			{
				if (!FAssetPath::TryCreate(std::format(
					"{}_RecordCandidate_{}", Path.ToString(), Suffix), OutPath)) return false;
				if (!Asset::FindResidentPackage(OutPath)
					&& !Asset::FindAssetExact(OutPath)) return true;
			}
			return false;
		}

		auto ValidateProviderState(const FImportRecordPayload& State, std::string& OutError) -> bool
		{
			FImportRecordPayload Canonical;
			if (!MakeImportRecordPayload(State.SchemaId, State.SchemaVersion, State.Bytes,
				MaximumImportRecordProviderStateBytes, Canonical, OutError)) return false;
			if (Canonical != State)
			{
				OutError = "Multi-output provider-state hash does not match its bytes.";
				return false;
			}
			return true;
		}

		auto BuildRecordState(
			const FMultiOutputImportPlan& Plan,
			const std::unordered_map<std::string, FPreparedMultiOutput*>& PreparedByIdentity,
			FImportRecordState& OutState,
			std::string& OutError) -> bool
		{
			const FImportPlan& Generic = Plan.GetGenericPlan();
			OutState.ProviderId = std::string(Generic.GetProvider().GetProviderId());
			OutState.ProviderContractVersion = Generic.GetProvider().GetContractVersion();
			if (!MakeImportRecordPayload(Generic.GetSettings().SchemaId,
				Generic.GetSettings().SchemaVersion, Generic.GetSettings().Bytes,
				MaximumImportRecordSettingsBytes, OutState.Settings, OutError)) return false;
			OutState.ProviderState = Plan.GetProviderState();
			for (const FSourceSnapshotEntry& Source : Generic.GetSnapshot().GetSources())
			{
				OutState.Sources.push_back({
					.StableIdentity = Source.StableIdentity,
					.Role = Source.Role,
					.SourcePath = Source.SourcePath,
					.ContentHashLow = Source.ContentHash.HashLow,
					.ContentHashHigh = Source.ContentHash.HashHigh,
					.ByteCount = Source.ByteCount});
			}

			for (const FMultiOutputReconciliation& Entry : Plan.GetReconciliation())
			{
				std::string Fingerprint = Entry.PreviousAuthoredFingerprint;
				if (Entry.ProposedAction == EMultiOutputProposedAction::Create
					|| Entry.ProposedAction == EMultiOutputProposedAction::ReplaceManaged)
				{
					const auto PreparedIt = PreparedByIdentity.find(Entry.StableIdentity);
					if (PreparedIt == PreparedByIdentity.end() || !PreparedIt->second->Candidate)
					{
						OutError = std::format("Prepared output {} is missing.", Entry.StableIdentity);
						return false;
					}
					Fingerprint = PreparedIt->second->Candidate->GetAuthoredFingerprint();
					if (Fingerprint.empty())
					{
						OutError = std::format(
							"Prepared managed output {} has no authored fingerprint.",
							Entry.StableIdentity);
						return false;
					}
				}
				OutState.Outputs.push_back({
					.StableIdentity = Entry.StableIdentity,
					.Role = Entry.Role,
					.AssetPath = Entry.AssetPath,
					.AssetClassName = Entry.AssetClassName,
					.Policy = Entry.PersistedPolicy,
					.AuthoredFingerprint = std::move(Fingerprint)});
			}
			std::ranges::sort(OutState.Outputs, {}, &FImportRecordOutput::StableIdentity);

			if (DImportRecord* Existing = Plan.GetExistingRecord())
				OutState.DetachedTombstones = Existing->GetState().DetachedTombstones;
			uint64 Sequence = OutState.DetachedTombstones.empty()
				? 0 : OutState.DetachedTombstones.back().Sequence;
			for (const FMultiOutputReconciliation& Orphan : Plan.GetOrphans())
			{
				OutState.DetachedTombstones.push_back({
					.StableIdentity = Orphan.StableIdentity,
					.LastAssetPath = Orphan.AssetPath,
					.LastAuthoredFingerprint = Orphan.PreviousAuthoredFingerprint,
					.Sequence = ++Sequence});
			}
			if (OutState.DetachedTombstones.size() > MaximumImportRecordDetachedTombstones)
				OutState.DetachedTombstones.erase(
					OutState.DetachedTombstones.begin(),
					OutState.DetachedTombstones.begin()
						+ static_cast<ptrdiff_t>(OutState.DetachedTombstones.size()
							- MaximumImportRecordDetachedTombstones));
			for (const FImportWarningPreview& Warning : Plan.GetPreview().Warnings)
			{
				if (Warning.Change == EImportWarningChange::Resolved) continue;
				OutState.AcceptedDiagnostics.push_back(ToRecordDiagnostic(Warning.Diagnostic));
			}
			std::ranges::sort(OutState.AcceptedDiagnostics, {},
				&FImportRecordDiagnostic::Identity);
			OutState.PrimaryOutput = Plan.GetPrimaryOutput();
			if (OutState.PrimaryOutput.IsValid()
				&& std::ranges::none_of(OutState.Outputs, [&](const FImportRecordOutput& Output) {
					return Output.AssetPath == OutState.PrimaryOutput;
				})) OutState.PrimaryOutput = {};
			OutError.clear();
			return true;
		}
	}

	FPreparedMultiOutputImport::~FPreparedMultiOutputImport()
	{
		AbandonPrepared(*this);
	}

	auto BuildMultiOutputImportPreview(const FMultiOutputImportPlan& Plan) -> FImportPreview
	{
		FImportPreview Preview;
		for (const FSourceSnapshotEntry& Source
			: Plan.GetGenericPlan().GetSnapshot().GetSources())
		{
			Preview.Sources.push_back({
				.StableIdentity = Source.StableIdentity,
				.Role = Source.Role,
				.SourcePath = Source.SourcePath,
				.ByteCount = Source.ByteCount,
				.bEmbedded = Source.bEmbedded});
		}
		for (const FMultiOutputReconciliation& Entry : Plan.GetReconciliation())
		{
			const auto Generic = std::ranges::find(
				Plan.GetGenericPlan().GetOutputs(), Entry.StableIdentity,
				&FImportOutputPreview::StableIdentity);
			if (Generic == Plan.GetGenericPlan().GetOutputs().end()) continue;
			EImportPreviewAction Action = EImportPreviewAction::Create;
			switch (Entry.ProposedAction)
			{
			case EMultiOutputProposedAction::Create:
				Action = EImportPreviewAction::Create; break;
			case EMultiOutputProposedAction::ReplaceManaged:
				Action = EImportPreviewAction::Replace; break;
			case EMultiOutputProposedAction::Reference:
				Action = EImportPreviewAction::Reference; break;
			case EMultiOutputProposedAction::KeepDetached:
				Action = EImportPreviewAction::KeepDetached; break;
			case EMultiOutputProposedAction::ReportMissing:
				Action = EImportPreviewAction::Missing; break;
			case EMultiOutputProposedAction::RejectCollision:
				Action = EImportPreviewAction::Collision; break;
			case EMultiOutputProposedAction::ReportOrphan:
				Action = EImportPreviewAction::Orphan; break;
			}
			FImportOutputPreview Output = *Generic;
			Output.AssetPath = Entry.AssetPath;
			Preview.EstimatedCpuBytes += Output.EstimatedCpuBytes;
			Preview.EstimatedGpuBytes += Output.EstimatedGpuBytes;
			Preview.EstimatedDiskBytes += Output.EstimatedDiskBytes;
			Preview.Outputs.push_back({
				.Output = std::move(Output),
				.Action = Action,
				.bManaged = Entry.PersistedPolicy == EImportRecordOutputPolicy::Managed});
		}
		for (const FMultiOutputReconciliation& Entry : Plan.GetOrphans())
		{
			Preview.Outputs.push_back({
				.Output = {
					.StableIdentity = Entry.StableIdentity,
					.Role = Entry.Role,
					.AssetPath = Entry.AssetPath,
					.AssetClassName = Entry.AssetClassName,
					.Policy = EImportOutputPolicy::Detach},
				.Action = EImportPreviewAction::Orphan,
				.bManaged = false});
		}

		std::unordered_map<std::string, FImportDiagnostic> PriorWarnings;
		if (const DImportRecord* Existing = Plan.GetExistingRecord())
			for (const FImportRecordDiagnostic& Diagnostic : Existing->GetAcceptedDiagnostics())
				PriorWarnings.emplace(Diagnostic.Identity, ToFrameworkDiagnostic(Diagnostic));
		std::unordered_set<std::string> CurrentWarningIdentities;
		for (const FImportDiagnostic& Diagnostic : Plan.GetGenericPlan().GetDiagnostics())
		{
			if (Diagnostic.Severity == EImportDiagnosticSeverity::Error) continue;
			const std::string Identity = GetImportDiagnosticIdentity(Diagnostic);
			if (!CurrentWarningIdentities.insert(Identity).second) continue;
			const bool bAccepted = PriorWarnings.erase(Identity) != 0;
			Preview.Warnings.push_back({
				.Change = bAccepted ? EImportWarningChange::PreviouslyAccepted
					: EImportWarningChange::New,
				.Diagnostic = Diagnostic});
		}
		for (auto& [Identity, Diagnostic] : PriorWarnings)
		{
			(void)Identity;
			Preview.Warnings.push_back({
				.Change = EImportWarningChange::Resolved,
				.Diagnostic = std::move(Diagnostic)});
		}
		std::ranges::sort(Preview.Warnings, [](const FImportWarningPreview& A,
			const FImportWarningPreview& B) {
			return std::tuple{A.Change, GetImportDiagnosticIdentity(A.Diagnostic)}
				< std::tuple{B.Change, GetImportDiagnosticIdentity(B.Diagnostic)};
		});
		return Preview;
	}

	auto FImportService::CreateMultiOutputImportPlan(
		const FMultiOutputPlanRequest& Request,
		FImportRecordIndex& Index) -> FMultiOutputPlanResult
	{
		FMultiOutputPlanResult Result;
		if (!Request.RecordPath.IsValid() || !Request.GenericPlan.GetProvider()
			|| Request.GenericPlan.GetOutputs().empty())
		{
			Result.Message = "Multi-output planning requires a generic plan, outputs, and record path.";
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::InvalidRequest,
				"multi-output-plan", {}, Result.Message);
			return Result;
		}
		if (!ValidateProviderState(Request.ProviderState, Result.Message))
		{
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::InvalidRequest,
				"multi-output-plan", {}, Result.Message);
			return Result;
		}
		std::string IndexError;
		if (!Index.EnsureCurrent(IndexError))
		{
			Result.Message = std::move(IndexError);
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::ProviderFailure,
				"multi-output-plan", "record", Result.Message);
			return Result;
		}
		if (Request.ExistingRecord)
		{
			FAssetPath ExistingPath;
			std::string ValidationError;
			if (!Request.ExistingRecord->GetPackage()
				|| !FAssetPath::TryCreate(
					Request.ExistingRecord->GetPackage()->GetPackagePath(), ExistingPath)
				|| ExistingPath != Request.RecordPath
				|| !Request.ExistingRecord->Validate(ValidationError)
				|| Request.ExistingRecord->GetProviderId()
					!= Request.GenericPlan.GetProvider().GetProviderId()
				|| Request.ExistingRecord->GetProviderContractVersion()
					!= Request.GenericPlan.GetProvider().GetContractVersion())
			{
				Result.Message = ValidationError.empty()
					? "Existing import record does not match the selected provider or path."
					: ValidationError;
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::InvalidPlan,
					"multi-output-plan", {}, Result.Message);
				return Result;
			}
			if (Index.IsRecordConflicted(Request.RecordPath))
			{
				Result.Message = "Duplicate record identity or output managers prevent reimport.";
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::Collision,
					"multi-output-plan", {}, Result.Message);
				return Result;
			}
		}
		else if (Asset::FindAssetExact(Request.RecordPath)
			|| Asset::FindResidentPackage(Request.RecordPath))
		{
			const Asset::FAssetCatalogEntry Exact =
				Asset::FindAssetExact(Request.RecordPath);
			Result.Message = Exact
				&& Exact->EntryKind == Asset::EAssetRegistryEntryKind::Redirector
				? std::format(
					"Initial import-record destination is a redirector to {}. Run Fix Up Redirectors or choose another destination.",
					Exact->RedirectDestination.ToString())
				: "Initial import-record destination is occupied. Choose another destination or remove the existing asset.";
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::Collision,
				"multi-output-plan", {}, Result.Message);
			return Result;
		}

		std::unordered_map<std::string, const FImportRecordOutput*> PreviousByIdentity;
		if (Request.ExistingRecord)
			for (const FImportRecordOutput& Output : Request.ExistingRecord->GetOutputs())
				PreviousByIdentity.emplace(Output.StableIdentity, &Output);
		std::unordered_set<std::string> PlannedIdentities;
		for (const FImportOutputPreview& Preview : Request.GenericPlan.GetOutputs())
		{
			PlannedIdentities.insert(Preview.StableIdentity);
			const auto PreviousIt = PreviousByIdentity.find(Preview.StableIdentity);
			const FImportRecordOutput* Previous =
				PreviousIt == PreviousByIdentity.end() ? nullptr : PreviousIt->second;
			FMultiOutputReconciliation Entry{
				.StableIdentity = Preview.StableIdentity,
				.Role = Preview.Role,
				.AssetPath = Previous ? Previous->AssetPath : Preview.AssetPath,
				.ResolvedAssetPath = Previous ? Previous->AssetPath : Preview.AssetPath,
				.AssetClassName = Preview.AssetClassName,
				.PersistedPolicy = Previous ? Previous->Policy : ToRecordPolicy(Preview.Policy),
				.PreviousAuthoredFingerprint = Previous ? Previous->AuthoredFingerprint : std::string{}};
			const Asset::FAssetCatalogEntry Exact =
				Asset::FindAssetExact(Entry.AssetPath);
			const Asset::FAssetPathResolveResult Resolution =
				Asset::ResolveAssetPath(Entry.AssetPath);
			if (Resolution) Entry.ResolvedAssetPath = Resolution.FinalPath;
			const Asset::FAssetData* Occupant = Resolution && Resolution.FinalAssetData
				? &*Resolution.FinalAssetData
				: Exact.Data ? &*Exact.Data : nullptr;
			DPackage* Loaded = Asset::FindResidentPackage(Entry.ResolvedAssetPath);
			const bool bOccupied = Exact || Resolution || Loaded;
			const std::vector<FImportRecordManagement> Managers = Index.FindManagers(Entry.AssetPath);
			if (!Managers.empty()) Entry.ObservedManager = Managers.front().RecordPath;

			if (Entry.PersistedPolicy == EImportRecordOutputPolicy::Referenced)
			{
				Entry.ObservedState = bOccupied
					? EMultiOutputObservedState::Referenced : EMultiOutputObservedState::Missing;
				Entry.ProposedAction = bOccupied
					? EMultiOutputProposedAction::Reference : EMultiOutputProposedAction::ReportMissing;
			}
			else if (Entry.PersistedPolicy == EImportRecordOutputPolicy::Detached)
			{
				Entry.ObservedState = EMultiOutputObservedState::Detached;
				Entry.ProposedAction = EMultiOutputProposedAction::KeepDetached;
			}
			else if (!bOccupied)
			{
				Entry.ObservedState = Previous
					? EMultiOutputObservedState::Missing : EMultiOutputObservedState::Absent;
				Entry.ProposedAction = !Previous || Request.bRecreateMissingManagedOutputs
					? EMultiOutputProposedAction::Create
					: EMultiOutputProposedAction::ReportMissing;
			}
			else
			{
				const bool bClassMatches = !Occupant
					|| Occupant->AssetClassName == Entry.AssetClassName;
				const bool bManagedByThis = Previous && Managers.size() == 1
					&& Managers.front().RecordPath == Request.RecordPath;
				if (Previous && bClassMatches && bManagedByThis)
				{
					Entry.ObservedState = EMultiOutputObservedState::Managed;
					Entry.ProposedAction = EMultiOutputProposedAction::ReplaceManaged;
					if (Loaded) Entry.PackageEditRevision = Loaded->GetEditRevision();
				}
				else
				{
					Entry.ObservedState = EMultiOutputObservedState::Collision;
					Entry.ProposedAction = EMultiOutputProposedAction::RejectCollision;
					AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::Collision,
						"multi-output-plan", Entry.StableIdentity,
						Exact && Exact->EntryKind == Asset::EAssetRegistryEntryKind::Redirector
							? std::format(
								"Output path {} is a redirector to {}. Run Fix Up Redirectors or choose another destination.",
								Entry.AssetPath.ToString(), Exact->RedirectDestination.ToString())
							: std::format(
								"Output path {} is occupied by an unrelated asset or manager. Choose another destination or remove the existing asset.",
								Entry.AssetPath.ToString()));
				}
			}
			Result.Plan.Reconciliation.push_back(std::move(Entry));
		}

		if (Request.ExistingRecord)
			for (const FImportRecordOutput& Previous : Request.ExistingRecord->GetOutputs())
			{
				if (PlannedIdentities.contains(Previous.StableIdentity)) continue;
				Result.Plan.Orphans.push_back({
					.StableIdentity = Previous.StableIdentity,
					.Role = Previous.Role,
					.AssetPath = Previous.AssetPath,
					.ResolvedAssetPath = Previous.AssetPath,
					.AssetClassName = Previous.AssetClassName,
					.PersistedPolicy = Previous.Policy,
					.ObservedState = EMultiOutputObservedState::Orphan,
					.ProposedAction = EMultiOutputProposedAction::ReportOrphan,
					.PreviousAuthoredFingerprint = Previous.AuthoredFingerprint,
					.ObservedManager = Request.RecordPath});
			}

		std::ranges::sort(Result.Plan.Reconciliation, {},
			&FMultiOutputReconciliation::StableIdentity);
		std::ranges::sort(Result.Plan.Orphans, {},
			&FMultiOutputReconciliation::StableIdentity);
		Result.Plan.GenericPlan = Request.GenericPlan;
		Result.Plan.RecordPath = Request.RecordPath;
		Result.Plan.ExistingRecord = Request.ExistingRecord;
		Result.Plan.ProviderState = Request.ProviderState;
		Result.Plan.PrimaryOutput = Request.PrimaryOutput;
		Result.Plan.IndexRevision = Index.GetRevision();
		Result.Plan.AssetRegistryRevision = Asset::GetAssetCatalogRevision();
		if (Request.ExistingRecord)
		{
			Result.Plan.ExistingRecordFingerprint = Request.ExistingRecord->GetFingerprint();
			Result.Plan.ExistingRecordEditRevision =
				Request.ExistingRecord->GetPackage()->GetEditRevision();
		}
		Result.Plan.Preview = BuildMultiOutputImportPreview(Result.Plan);
		FinalizeImportDiagnostics(Result.Diagnostics, "multi-output-plan");
		Result.bSucceeded = true;
		return Result;
	}

	auto FImportService::ExecuteMultiOutputImport(
		const FMultiOutputImportPlan& Plan,
		FPreparedMultiOutputImport Prepared,
		FImportRecordIndex& Index,
		const FMultiOutputExecutionOptions& Options) -> FMultiOutputExecutionResult
	{
		FMultiOutputExecutionResult Result;
		FMultiDiagnosticFinalizer DiagnosticFinalizer(Result);
		Result.Provider = Prepared.GetProvider()
			? Prepared.GetProvider() : Plan.GetGenericPlan().GetProvider();
		FProgressBoundary ValidationProgress(Options.Progress, EImportPhase::Validation);
		std::unordered_map<std::string, FPreparedMultiOutput*> PreparedByIdentity;
		for (FPreparedMultiOutput& Output : Prepared.Outputs)
		{
			if (Options.IsCancellationRequested
				&& Options.IsCancellationRequested())
			{
				Result.Message = "Multi-output candidate validation was canceled.";
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::Canceled,
					"candidate-validation", Output.StableIdentity, Result.Message);
				AbandonPrepared(Prepared);
				return Result;
			}
			if (Output.StableIdentity.empty()
				|| !PreparedByIdentity.emplace(Output.StableIdentity, &Output).second
				|| !Output.Candidate || !Output.Candidate->GetAsset()
				|| !Output.Candidate->GetPackage()
				|| !Output.Candidate->Validate(Result.Diagnostics))
			{
				Result.Message = "Prepared multi-output candidates are incomplete, duplicated, or invalid.";
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::ValidationFailure,
					"candidate-validation", Output.StableIdentity, Result.Message);
				AbandonPrepared(Prepared);
				return Result;
			}
		}
		for (const FMultiOutputReconciliation& Entry : Plan.GetReconciliation())
		{
			if (Entry.ProposedAction == EMultiOutputProposedAction::RejectCollision)
			{
				Result.Message = "A rejected output collision prevents publication.";
				AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::Collision,
					"candidate-validation", Entry.StableIdentity, Result.Message);
				AbandonPrepared(Prepared);
				return Result;
			}
			if (Entry.ProposedAction != EMultiOutputProposedAction::Create
				&& Entry.ProposedAction != EMultiOutputProposedAction::ReplaceManaged) continue;
			const auto It = PreparedByIdentity.find(Entry.StableIdentity);
			if (It == PreparedByIdentity.end())
			{
				Result.Message = std::format("Candidate {} was not prepared.", Entry.StableIdentity);
				AbandonPrepared(Prepared);
				return Result;
			}
			FPreparedMultiOutput& Output = *It->second;
			if (Entry.ProposedAction == EMultiOutputProposedAction::Create)
			{
				FAssetPath CandidatePath;
				if (!Output.Candidate->IsNewAsset()
					|| !FAssetPath::TryCreate(
						Output.Candidate->GetPackage()->GetPackagePath(), CandidatePath)
					|| CandidatePath != Entry.AssetPath || Output.Exchange)
				{
					Result.Message = "Created output candidate is not an unpublished final-path package.";
					AbandonPrepared(Prepared);
					return Result;
				}
			}
			else if (!Output.ExistingTarget || !Output.Exchange || Output.Candidate->IsNewAsset())
			{
				Result.Message = "Managed replacement has no target or prepared typed exchange.";
				AbandonPrepared(Prepared);
				return Result;
			}
		}
		if (Options.IsCancellationRequested && Options.IsCancellationRequested())
		{
			Result.Message = "Multi-output candidate preparation was canceled.";
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::Canceled,
				"candidate-validation", "record", Result.Message);
			AbandonPrepared(Prepared);
			return Result;
		}

		FImportRecordState RecordState;
		if (!BuildRecordState(Plan, PreparedByIdentity, RecordState, Result.Message))
		{
			AbandonPrepared(Prepared);
			return Result;
		}

		DImportRecord* PublishedRecord = Plan.GetExistingRecord();
		DImportRecord* RecordCandidate = nullptr;
		FAssetPath RecordCandidatePath = Plan.GetRecordPath();
		if (PublishedRecord && !MakeTemporaryRecordPath(Plan.GetRecordPath(), RecordCandidatePath))
		{
			Result.Message = "Could not allocate a temporary import-record candidate path.";
			return Result;
		}
		const Asset::FAssetResult CreateRecord = CreateImportRecordAsset(RecordCandidatePath, RecordCandidate);
		if (!CreateRecord || !RecordCandidate)
		{
			Result.Message = CreateRecord.Message;
			AbandonPrepared(Prepared);
			return Result;
		}
		if (!RecordCandidate->SetState(std::move(RecordState), Result.Message))
		{
			(void)Asset::UnloadPackage(RecordCandidate->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			AbandonPrepared(Prepared);
			return Result;
		}
		if (Options.IsCancellationRequested && Options.IsCancellationRequested())
		{
			Result.Message = "Import-record candidate preparation was canceled.";
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::Canceled,
				"candidate-validation", "record", Result.Message);
			(void)Asset::UnloadPackage(RecordCandidate->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			AbandonPrepared(Prepared);
			return Result;
		}
		if (!PublishedRecord) PublishedRecord = RecordCandidate;
		ValidationProgress.Succeed(Prepared.Outputs.size(), Prepared.Outputs.size());
		FProgressBoundary PublicationProgress(Options.Progress, EImportPhase::Publication);

		std::string IndexError;
		if (!Index.EnsureCurrent(IndexError)) Result.Message = std::move(IndexError);
		std::lock_guard PublicationLock(GetImportPublicationMutex());
		bool bStale = !Result.Message.empty()
			|| Index.GetRevision() != Plan.IndexRevision
			|| Asset::GetAssetCatalogRevision() != Plan.AssetRegistryRevision
			|| Plan.GetGenericPlan().GetImporterRevision()
				!= GetImporterRevision();
		if (Plan.GetExistingRecord())
		{
			bStale = bStale || !Plan.GetExistingRecord()->GetPackage()
				|| Plan.GetExistingRecord()->GetPackage()->GetEditRevision()
					!= Plan.ExistingRecordEditRevision
				|| Plan.GetExistingRecord()->GetFingerprint()
					!= Plan.ExistingRecordFingerprint
				|| Index.IsRecordConflicted(Plan.GetRecordPath())
				|| Asset::FindResidentPackage(Plan.GetRecordPath())
					!= Plan.GetExistingRecord()->GetPackage();
		}
		else
		{
			bStale = bStale || Asset::FindAssetExact(Plan.GetRecordPath())
				|| Asset::FindResidentPackage(Plan.GetRecordPath()) != RecordCandidate->GetPackage();
		}
		for (const FMultiOutputReconciliation& Entry : Plan.GetReconciliation())
		{
			if (Entry.ProposedAction == EMultiOutputProposedAction::ReplaceManaged)
			{
				const FPreparedMultiOutput& Output = *PreparedByIdentity.at(Entry.StableIdentity);
				FAssetPath CurrentPath;
				bStale = bStale || !Output.ExistingTarget || !Output.ExistingTarget->GetPackage()
					|| !FAssetPath::TryCreate(
						Output.ExistingTarget->GetPackage()->GetPackagePath(), CurrentPath)
					|| CurrentPath != Entry.ResolvedAssetPath
					|| Output.ExistingTarget->GetClass()->GetQualifiedName().ToString()
						!= Entry.AssetClassName
					|| Output.ExistingTarget->GetPackage()->GetEditRevision()
						!= Entry.PackageEditRevision
					|| Asset::FindResidentPackage(Entry.ResolvedAssetPath)
						!= Output.ExistingTarget->GetPackage();
			}
			else if (Entry.ProposedAction == EMultiOutputProposedAction::Create)
			{
				const FPreparedMultiOutput& Output = *PreparedByIdentity.at(Entry.StableIdentity);
				bStale = bStale || Asset::FindAssetExact(Entry.AssetPath)
					|| Asset::FindResidentPackage(Entry.AssetPath)
						!= Output.Candidate->GetPackage();
			}
		}
		if (bStale)
		{
			Result.Message = Result.Message.empty()
				? "Import record, target, registry, index, manager, or provider changed after preview."
				: Result.Message;
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::StalePlan,
				"publication-preflight", {}, Result.Message);
			(void)Asset::UnloadPackage(RecordCandidate->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			AbandonPrepared(Prepared);
			return Result;
		}

		std::vector<std::pair<DPackage*, bool>> DirtyStates;
		std::vector<FPreparedMultiOutput*> CommittedOutputs;
		std::vector<DPackage*> Packages;
		for (const FMultiOutputReconciliation& Entry : Plan.GetReconciliation())
		{
			if (Entry.ProposedAction != EMultiOutputProposedAction::Create
				&& Entry.ProposedAction != EMultiOutputProposedAction::ReplaceManaged) continue;
			FPreparedMultiOutput& Output = *PreparedByIdentity.at(Entry.StableIdentity);
			DPackage* Package = Entry.ProposedAction == EMultiOutputProposedAction::Create
				? Output.Candidate->GetPackage() : Output.ExistingTarget->GetPackage();
			DirtyStates.emplace_back(Package, Package->IsDirty());
			if (Output.Exchange)
			{
				Output.Exchange->Commit();
				CommittedOutputs.push_back(&Output);
			}
			Packages.push_back(Package);
			Result.Outputs.push_back(Entry.ProposedAction == EMultiOutputProposedAction::Create
				? Output.Candidate->GetAsset() : Output.ExistingTarget);
		}

		const bool bRecordWasDirty = PublishedRecord->GetPackage()->IsDirty();
		bool bRecordExchanged = false;
		if (Plan.GetExistingRecord())
		{
			Plan.GetExistingRecord()->ExchangeImportedState(*RecordCandidate);
			bRecordExchanged = true;
		}
		Asset::FAssetResult FingerprintResult;
		FImportRecordState PublishedState = PublishedRecord->GetState();
		for (const FMultiOutputReconciliation& Entry : Plan.GetReconciliation())
		{
			if (Entry.ProposedAction != EMultiOutputProposedAction::Create
				&& Entry.ProposedAction != EMultiOutputProposedAction::ReplaceManaged) continue;
			FPreparedMultiOutput& Output = *PreparedByIdentity.at(Entry.StableIdentity);
			DPackage* Package = Entry.ProposedAction == EMultiOutputProposedAction::Create
				? Output.Candidate->GetPackage() : Output.ExistingTarget->GetPackage();
			std::string Fingerprint;
			std::string FingerprintError;
			if (!ComputeImportPackageFingerprint(Package, Fingerprint, FingerprintError))
			{
				FingerprintResult = {
					Asset::EAssetError::InvalidObjectGraph, std::move(FingerprintError)};
				break;
			}
			const auto RecordOutput = std::ranges::find(
				PublishedState.Outputs, Entry.StableIdentity,
				&FImportRecordOutput::StableIdentity);
			if (RecordOutput == PublishedState.Outputs.end())
			{
				FingerprintResult = {
					Asset::EAssetError::InvalidObjectGraph,
					"Published import record lost a prepared output identity."};
				break;
			}
			RecordOutput->AuthoredFingerprint = std::move(Fingerprint);
		}
		if (FingerprintResult)
		{
			std::string StateError;
			if (!PublishedRecord->SetState(std::move(PublishedState), StateError))
				FingerprintResult = {
					Asset::EAssetError::InvalidObjectGraph, std::move(StateError)};
		}
		Packages.push_back(PublishedRecord->GetPackage());
		std::unordered_set<DPackage*> Unique;
		std::erase_if(Packages, [&](DPackage* Package) {
			return !Package || !Unique.insert(Package).second;
		});
		Asset::FAssetBundleSaveOptions SaveOptions = Options.SaveOptions;
		SaveOptions.RootPackage = PublishedRecord->GetPackage();
		const Asset::FAssetResult Save = FingerprintResult
			? Asset::SavePackagesAtomically(Packages, SaveOptions)
			: FingerprintResult;
		if (!Save)
		{
			FProgressBoundary RestoreProgress(Options.Progress, EImportPhase::Restore);
			if (bRecordExchanged) Plan.GetExistingRecord()->ExchangeImportedState(*RecordCandidate);
			for (auto It = CommittedOutputs.rbegin(); It != CommittedOutputs.rend(); ++It)
				(*It)->Exchange->Reverse();
			for (const auto& [Package, bWasDirty] : DirtyStates)
				if (!bWasDirty) Package->ClearDirty();
			if (!bRecordWasDirty) PublishedRecord->GetPackage()->ClearDirty();
			bool bRestored = true;
			if (Plan.GetExistingRecord())
				bRestored = Plan.GetExistingRecord()->GetFingerprint()
					== Plan.ExistingRecordFingerprint;
			for (const FMultiOutputReconciliation& Entry : Plan.GetReconciliation())
			{
				if (Entry.ProposedAction != EMultiOutputProposedAction::ReplaceManaged) continue;
				const FPreparedMultiOutput& Output = *PreparedByIdentity.at(Entry.StableIdentity);
				std::string Fingerprint;
				std::string FingerprintError;
				bRestored = bRestored && ComputeImportPackageFingerprint(
					Output.ExistingTarget->GetPackage(), Fingerprint, FingerprintError)
					&& Fingerprint == Entry.PreviousAuthoredFingerprint;
			}
			if (bRestored) RestoreProgress.Succeed();
			else AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::RestoreFailure,
				"restore", {}, "One or more managed outputs did not restore their prior authored state.");
			Result.Message = Save.Message;
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::PublicationFailure,
				"package-publication", {}, Result.Message);
			(void)Asset::UnloadPackage(RecordCandidate->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			AbandonPrepared(Prepared);
			FinalizeImportDiagnostics(Result.Diagnostics, "package-publication");
			return Result;
		}

		if (Plan.GetExistingRecord())
			(void)Asset::UnloadPackage(RecordCandidate->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
		for (FPreparedMultiOutput& Output : Prepared.Outputs)
		{
			if (Output.Exchange)
			{
				Output.Exchange->Finalize();
				Output.Exchange.reset();
			}
			if (Output.Candidate && !Output.Candidate->IsNewAsset())
				Output.Candidate->Abandon();
			Output.Candidate.reset();
		}
		for (const FMultiOutputReconciliation& Orphan : Plan.GetOrphans())
			Result.Orphans.push_back(Orphan.AssetPath);
		Result.Record = PublishedRecord;
		Result.bSucceeded = true;
		PublicationProgress.Succeed();
		std::string RebuildError;
		if (!Index.Rebuild(RebuildError))
			Result.Diagnostics.push_back({
				.Severity = EImportDiagnosticSeverity::Warning,
				.Category = EImportDiagnosticCategory::ProviderFailure,
				.Phase = "index-publication",
				.SourceIdentity = "root",
				.OutputIdentity = "record",
				.Message = RebuildError});
		FinalizeImportDiagnostics(Result.Diagnostics, "publication");
		return Result;
	}

	struct FImportRecordHandlerRegistry::FImpl
	{
		struct FEntry
		{
			FModuleOwnedResourceLease RegistryResource;
			std::shared_ptr<IImportRecordHandler> Handler;
			FModuleOwnedCallbackGate OwnerGate;
		};
		mutable std::mutex Mutex;
		std::map<std::string, FEntry, std::less<>> Handlers;
		uint64 Revision = 1;
	};

	FImportRecordHandlerRegistry::FImportRecordHandlerRegistry()
		: Impl(std::make_unique<FImpl>()) {}
	FImportRecordHandlerRegistry::~FImportRecordHandlerRegistry() = default;

	auto FImportRecordHandlerRegistry::Register(
		std::shared_ptr<IImportRecordHandler> Handler,
		FModuleOwnedCallbackGate OwnerGate,
		std::string& OutError) -> bool
	{
		auto Invocation = OwnerGate.TryEnter();
		if (!Handler || !Invocation || Handler->GetProviderId().empty())
		{
			OutError = "Import-record handler identity is invalid.";
			return false;
		}
		const std::string Id(Handler->GetProviderId());
		std::lock_guard Lock(Impl->Mutex);
		auto Resource = OwnerGate.RetainResource();
		if (!Resource)
		{
			OutError = "Import-record handler owner is retiring.";
			return false;
		}
		if (!Impl->Handlers.emplace(Id, FImpl::FEntry{
			std::move(Resource), std::move(Handler), std::move(OwnerGate)}).second)
		{
			OutError = std::format("Import-record handler {} is already registered.", Id);
			return false;
		}
		++Impl->Revision;
		OutError.clear();
		return true;
	}

	auto FImportRecordHandlerRegistry::Unregister(std::string_view ProviderId) -> bool
	{
		std::lock_guard Lock(Impl->Mutex);
		if (Impl->Handlers.erase(std::string(ProviderId)) == 0) return false;
		++Impl->Revision;
		return true;
	}

	auto FImportRecordHandlerRegistry::Find(std::string_view ProviderId) const
		-> std::shared_ptr<const IImportRecordHandler>
	{
		std::lock_guard Lock(Impl->Mutex);
		const auto It = Impl->Handlers.find(ProviderId);
		if (It == Impl->Handlers.end()) return nullptr;
		auto Invocation = It->second.OwnerGate.TryEnter();
		auto Resource = It->second.OwnerGate.RetainResource();
		if (!Invocation || !Resource) return nullptr;
		struct FLease
		{
			FModuleOwnedResourceLease Resource;
			std::shared_ptr<IImportRecordHandler> Handler;
		};
		auto Lease = std::make_shared<FLease>(FLease{
			std::move(Resource), It->second.Handler});
		return std::shared_ptr<const IImportRecordHandler>(
			std::move(Lease), It->second.Handler.get());
	}

	auto FImportRecordHandlerRegistry::GetRevision() const -> uint64
	{
		std::lock_guard Lock(Impl->Mutex);
		return Impl->Revision;
	}

	auto FImportService::QueryImportRecordCapabilities(
		const FImportRecordInspection& Inspection) const -> FImportRecordCapabilitySet
	{
		if (Inspection.Record)
		{
			if (const std::shared_ptr<const IImportRecordHandler> Handler =
				FindImportRecordHandler(Inspection.Record->GetProviderId()))
				return Handler->QueryCapabilities(*Inspection.Record, Inspection);
		}
		FImportRecordCapabilitySet Result{
			.ProviderId = Inspection.Record
				? std::string(Inspection.Record->GetProviderId()) : std::string{}};
		for (const auto [Action, Label] : std::array{
			std::pair{EImportRecordAction::Reimport, std::string_view("Reimport")},
			std::pair{EImportRecordAction::RecreateMissingOutputs,
				std::string_view("Recreate Missing Outputs")},
			std::pair{EImportRecordAction::RepairManagedOutputs,
				std::string_view("Repair Managed Outputs")}})
		{
			FImportDiagnostic Diagnostic{
				.Severity = EImportDiagnosticSeverity::Error,
				.Category = EImportDiagnosticCategory::ProviderUnavailable,
				.Phase = "capability-query",
				.SourceIdentity = "root",
				.OutputIdentity = "record",
				.Message = "The persisted import-record provider handler is unavailable."};
			Diagnostic.Identity = GetImportDiagnosticIdentity(Diagnostic);
			Result.Capabilities.push_back({
				.Action = Action,
				.bAvailable = false,
				.Label = std::string(Label),
				.Diagnostics = {std::move(Diagnostic)}});
		}
		return Result;
	}

	auto FImportService::ExecuteImportRecordAction(
		DImportRecord& Record,
		EImportRecordAction Action,
		const FMultiOutputExecutionOptions& Options) -> FImportRecordActionResult
	{
		const std::shared_ptr<const IImportRecordHandler> Handler =
			FindImportRecordHandler(Record.GetProviderId());
		const FProviderLease Provider = FindProvider(Record.GetProviderId());
		if (!Handler || !Provider
			|| Provider.GetContractVersion() != Record.GetProviderContractVersion())
		{
			FImportRecordActionResult Result;
			Result.Message = "The persisted import-record provider is unavailable or incompatible.";
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::ProviderUnavailable,
				"record-action", "record", Result.Message);
			return Result;
		}
		FImportRecordActionResult Result = Handler->Execute(Record, Action, Options);
		Result.Provider = Provider;
		if (!Result && Result.Diagnostics.empty())
			AddDiagnostic(Result.Diagnostics, EImportDiagnosticCategory::ProviderFailure,
				"record-action", "record", Result.Message.empty()
					? "The import-record provider action failed." : Result.Message);
		FinalizeImportDiagnostics(Result.Diagnostics, "record-action", "root", "record");
		return Result;
	}
}
