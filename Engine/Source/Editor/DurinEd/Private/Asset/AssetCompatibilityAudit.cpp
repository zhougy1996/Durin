#include "Asset/AssetCompatibilityAudit.h"

#include "AssetAuthoring.h"

namespace Durin::Editor
{
	namespace
	{
		auto LowerCopy(std::string_view Value) -> std::string
		{
			std::string Result(Value);
			std::ranges::transform(Result, Result.begin(), [](char Character) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
			});
			return Result;
		}

		auto ContainsSearch(std::string_view Value, const std::string& LowerSearch) -> bool
		{
			return LowerCopy(Value).find(LowerSearch) != std::string::npos;
		}

		struct FNotice
		{
			uint64 Serial = 0;
			std::optional<Asset::FAssetPackageCompatibilityRecord> Record;
		};

		struct FTerminalSummary
		{
			uint64 Serial = 0;
			uint64 ProcessedPackageCount = 0;
			EAssetCompatibilityAuditState State = EAssetCompatibilityAuditState::Completed;
			std::string Failure;
		};

		auto MakeNotCheckedRecord(const Asset::FAssetData& Data)
			-> Asset::FAssetPackageCompatibilityRecord
		{
			return {
				.PackagePath = Data.PackagePath,
				.PhysicalPath = Data.PhysicalPath,
				.Fingerprint = {
					.FileSize = Data.FileSize,
					.LastWriteTimeTicks = Data.LastWriteTimeTicks,
				},
				.Inspection = Asset::EAssetCompatibilityInspection::NotChecked,
				.Compatibility = Asset::EAssetPackageCompatibility::Unsupported,
				.Freshness = Asset::EAssetCompatibilityFreshness::Current,
			};
		}
	}

	auto MatchesAssetCompatibilityAuditFilter(
		const Asset::FAssetPackageCompatibilityRecord& Record,
		EAssetCompatibilityAuditFilter Filter) -> bool
	{
		switch (Filter)
		{
		case EAssetCompatibilityAuditFilter::All: return true;
		case EAssetCompatibilityAuditFilter::Issues: return !Record.Findings.empty();
		case EAssetCompatibilityAuditFilter::Incompatible:
			return Record.Compatibility == Asset::EAssetPackageCompatibility::Incompatible;
		case EAssetCompatibilityAuditFilter::Unsupported:
			return Record.Compatibility == Asset::EAssetPackageCompatibility::Unsupported
				&& Record.Inspection != Asset::EAssetCompatibilityInspection::NotChecked;
		case EAssetCompatibilityAuditFilter::Failed:
			return Record.Inspection == Asset::EAssetCompatibilityInspection::Failed;
		case EAssetCompatibilityAuditFilter::Stale:
			return Record.Freshness == Asset::EAssetCompatibilityFreshness::Stale;
		case EAssetCompatibilityAuditFilter::NotChecked:
			return Record.Inspection == Asset::EAssetCompatibilityInspection::NotChecked;
		}
		return true;
	}

	auto MatchesAssetCompatibilityAuditSearch(
		const Asset::FAssetPackageCompatibilityRecord& Record,
		std::string_view SearchText) -> bool
	{
		const std::string LowerSearch = LowerCopy(SearchText);
		if (LowerSearch.empty()) return true;
		if (ContainsSearch(Record.PackagePath.GetView(), LowerSearch)
			|| ContainsSearch(Record.PhysicalPath, LowerSearch)) return true;
		for (const auto& Finding : Record.Findings)
			if (ContainsSearch(Asset::AssetCompatibilityFindingCodeName(Finding.Code), LowerSearch)
				|| ContainsSearch(Finding.ObjectPath, LowerSearch)
				|| ContainsSearch(Finding.ClassIdentity, LowerSearch)
				|| ContainsSearch(Finding.DeclaringType, LowerSearch)
				|| ContainsSearch(Finding.FieldName, LowerSearch)
				|| ContainsSearch(Finding.Diagnostic, LowerSearch)) return true;
		for (const auto& Evidence : Record.CanonicalizationEvidence)
			if (ContainsSearch(Evidence.StoredIdentity, LowerSearch)
				|| ContainsSearch(Evidence.CurrentIdentity, LowerSearch)
				|| ContainsSearch(Evidence.LogicalPath, LowerSearch)) return true;
		for (const auto& Evidence : Record.DeprecatedRouteEvidence)
			if (ContainsSearch(Evidence.ObjectPath, LowerSearch)
				|| ContainsSearch(Evidence.DeclaringType, LowerSearch)
				|| ContainsSearch(Evidence.StoredFieldName, LowerSearch)
				|| ContainsSearch(Evidence.DeprecatedPropertyName, LowerSearch)
				|| std::ranges::any_of(Evidence.MigrationTargets, [&](const std::string& Target) {
					return ContainsSearch(Target, LowerSearch);
				})) return true;
		return false;
	}

	auto CountAssetCompatibilityAuditRecords(
		std::span<const Asset::FAssetPackageCompatibilityRecord> Records)
		-> FAssetCompatibilityAuditCounts
	{
		FAssetCompatibilityAuditCounts Counts;
		for (const auto& Record : Records)
		{
			Counts.Compatible += Record.Inspection == Asset::EAssetCompatibilityInspection::Ready
				&& Record.Compatibility == Asset::EAssetPackageCompatibility::Compatible;
			Counts.Incompatible += Record.Compatibility == Asset::EAssetPackageCompatibility::Incompatible;
			Counts.Unsupported += Record.Inspection != Asset::EAssetCompatibilityInspection::NotChecked
				&& Record.Compatibility == Asset::EAssetPackageCompatibility::Unsupported;
			Counts.Failed += Record.Inspection == Asset::EAssetCompatibilityInspection::Failed;
			Counts.Stale += Record.Freshness == Asset::EAssetCompatibilityFreshness::Stale;
			Counts.NotChecked += Record.Inspection == Asset::EAssetCompatibilityInspection::NotChecked;
		}
		return Counts;
	}

	auto FormatAssetCompatibilityAuditDiagnostics(
		const Asset::FAssetPackageCompatibilityRecord& Record) -> std::string
	{
		auto Inspection = [](Asset::EAssetCompatibilityInspection Value) -> std::string_view {
			switch (Value)
			{
			case Asset::EAssetCompatibilityInspection::NotChecked: return "Not checked";
			case Asset::EAssetCompatibilityInspection::Ready: return "Ready";
			case Asset::EAssetCompatibilityInspection::Failed: return "Failed";
			}
			return "Unknown";
		};
		auto Compatibility = [](Asset::EAssetPackageCompatibility Value) -> std::string_view {
			switch (Value)
			{
			case Asset::EAssetPackageCompatibility::Compatible: return "Compatible";
			case Asset::EAssetPackageCompatibility::Incompatible: return "Incompatible";
			case Asset::EAssetPackageCompatibility::Unsupported: return "Unsupported";
			}
			return "Unknown";
		};
		std::string Text = std::format("{}: {} / {} / {}", Record.PackagePath.ToString(),
			Inspection(Record.Inspection), Compatibility(Record.Compatibility),
			Record.Freshness == Asset::EAssetCompatibilityFreshness::Current ? "Current" : "Stale");
		for (const auto& Finding : Record.Findings)
			Text += std::format("\n[{}] {}", Asset::AssetCompatibilityFindingCodeName(Finding.Code), Finding.Diagnostic);
		for (const auto& Evidence : Record.CanonicalizationEvidence)
			Text += std::format("\n[CanonicalResaveRecommended] {}: {} -> {}",
				Evidence.LogicalPath, Evidence.StoredIdentity, Evidence.CurrentIdentity);
		for (const auto& Evidence : Record.DeprecatedRouteEvidence)
			Text += std::format("\n[CanonicalResaveRecommended] {}::{} uses deprecated route {}",
				Evidence.DeclaringType, Evidence.StoredFieldName, Evidence.DeprecatedPropertyName);
		return Text;
	}

	auto FormatAssetCompatibilityAuditReport(
		std::span<const Asset::FAssetPackageCompatibilityRecord> Records) -> std::string
	{
		std::string Report;
		for (const auto& Record : Records)
		{
			if (!Report.empty()) Report += "\n\n";
			Report += FormatAssetCompatibilityAuditDiagnostics(Record);
		}
		return Report;
	}

	struct FAssetCompatibilityAuditModel::FMailbox
	{
		std::mutex Mutex;
		std::deque<FNotice> Notices;
	};

	struct FAssetCompatibilityAuditModel::FPublicationLifetime
	{
		FAssetCompatibilityAuditModel* Model = nullptr;
	};

	FAssetCompatibilityAuditModel::FAssetCompatibilityAuditModel(FAssetCompatibilityProbe InProbe)
		: Probe(std::move(InProbe))
		, Mailbox(std::make_shared<FMailbox>())
		, PublicationLifetime(std::make_shared<FPublicationLifetime>())
	{
		PublicationLifetime->Model = this;
		if (!Probe)
		{
			Probe = [](const Asset::FAssetPackageCompatibilityProbeInput& Input,
				const Asset::FReflectionCompatibilityCatalog& Catalog,
				const Asset::FAssetCompatibilityCancellationCheck& IsCancelled) {
				return Asset::ProbeAssetPackageCompatibility(Input, Catalog, IsCancelled);
			};
		}
	}

	FAssetCompatibilityAuditModel::~FAssetCompatibilityAuditModel()
	{
		Shutdown();
		PublicationLifetime.reset();
	}

	auto FAssetCompatibilityAuditModel::RunCurrentProjectAudit() -> bool
	{
		return RunAudit(Asset::CaptureAssetCatalogSnapshot().Assets,
			Asset::FReflectionCompatibilityCatalog::Capture());
	}

	auto FAssetCompatibilityAuditModel::RunAudit(
		const std::unordered_map<FAssetPath, Asset::FAssetData>& Assets,
		Asset::FReflectionCompatibilityCatalog Catalog) -> bool
	{
		if (!bAdmissionOpen) return false;
		CancelAndDrain();
		Failure.clear();
		++RequestSerial;
		Generation.Advance();
		Progress = {.Completed = 0, .Total = Assets.size()};
		Records.clear();
		std::vector<Asset::FAssetPackageCompatibilityProbeInput> Inputs;
		Inputs.reserve(Assets.size());
		for (const auto& [Path, Data] : Assets)
		{
			Records.emplace(Path, MakeNotCheckedRecord(Data));
			Inputs.push_back({
				.PackagePath = Path,
				.PhysicalPath = Data.PhysicalPath,
				.ExpectedFileSize = Data.FileSize,
				.ExpectedLastWriteTimeTicks = Data.LastWriteTimeTicks,
			});
		}
		std::ranges::sort(Inputs, {}, [](const auto& Input) {
			return Input.PackagePath.ToString();
		});
		InvalidatePresentation();

		State = EAssetCompatibilityAuditState::Running;
		Cancellation = FTaskCancellationSource{};
		FTaskLaunchOptions Options;
		Options.CancellationToken = Cancellation.GetToken();
		const uint64 Serial = RequestSerial;
		const std::shared_ptr<FMailbox> WorkerMailbox = Mailbox;
		const FAssetCompatibilityProbe WorkerProbe = Probe;
		auto WorkerTask = LaunchCancelableTask<FTerminalSummary>("AssetCompatibility.Audit",
			[Serial, WorkerMailbox, WorkerProbe, Inputs = std::move(Inputs),
				Catalog = std::move(Catalog)](const FTaskCancellationToken& Token) mutable -> FTerminalSummary {
				auto Queue = [&](FNotice Notice) {
					std::lock_guard Lock(WorkerMailbox->Mutex);
					WorkerMailbox->Notices.push_back(std::move(Notice));
				};
				uint64 ProcessedPackageCount = 0;
				try
				{
					for (const auto& Input : Inputs)
					{
						if (Token.IsCancellationRequested())
						{
							return {.Serial = Serial, .ProcessedPackageCount = ProcessedPackageCount,
								.State = EAssetCompatibilityAuditState::Cancelled};
						}
						auto Result = WorkerProbe(Input, Catalog,
							[&Token] { return Token.IsCancellationRequested(); });
						if (Result.Status == Asset::EAssetCompatibilityProbeStatus::Cancelled)
						{
							return {.Serial = Serial, .ProcessedPackageCount = ProcessedPackageCount,
								.State = EAssetCompatibilityAuditState::Cancelled};
						}
						if (Result.Record)
						{
							Queue({.Serial = Serial, .Record = std::move(Result.Record)});
							++ProcessedPackageCount;
						}
					}
					return {.Serial = Serial, .ProcessedPackageCount = ProcessedPackageCount,
						.State = EAssetCompatibilityAuditState::Completed};
				}
				catch (const std::exception& Exception)
				{
					return {.Serial = Serial, .ProcessedPackageCount = ProcessedPackageCount,
						.State = EAssetCompatibilityAuditState::Failed, .Failure = Exception.what()};
				}
				catch (...)
				{
					return {.Serial = Serial, .ProcessedPackageCount = ProcessedPackageCount,
						.State = EAssetCompatibilityAuditState::Failed,
						.Failure = "Asset compatibility audit failed with an unknown exception."};
				}
			}, Options);
		Task = WorkerTask.GetTaskHandle();
		if (!WorkerTask.IsValid())
		{
			State = EAssetCompatibilityAuditState::Failed;
			Failure = "The task scheduler rejected the asset compatibility audit.";
			return false;
		}

		FTaskContinuationOptions TerminalOptions;
		TerminalOptions.Target = ETaskTarget::GameThreadDeferred;
		TerminalOptions.Priority = ETaskPriority::Normal;
		TerminalOptions.EstimatedPayloadBytes = sizeof(FTerminalSummary) + 512;
		TerminalOptions.GenerationToken = Generation.Capture();
		const std::weak_ptr<FPublicationLifetime> WeakLifetime = PublicationLifetime;
		TerminalTask = ThenOutcome(
			WorkerTask,
			"AssetCompatibility.PublishTerminal",
			[WeakLifetime, Serial](FTaskOutcome<FTerminalSummary> Outcome) {
				const std::shared_ptr<FPublicationLifetime> Lifetime = WeakLifetime.lock();
				if (!Lifetime || !Lifetime->Model) return;
				FAssetCompatibilityAuditModel& Model = *Lifetime->Model;
				if (Model.RequestSerial != Serial || !Model.bAdmissionOpen) return;
				Model.DrainMailbox();
				if (Outcome.State == ETaskState::Succeeded && Outcome.Result)
				{
					if (Outcome.Result->Serial != Serial) return;
					Model.State = Outcome.Result->State;
					Model.Failure = Outcome.Result->Failure;
				}
				else if (Outcome.State == ETaskState::Canceled)
				{
					Model.State = EAssetCompatibilityAuditState::Cancelled;
					Model.Failure.clear();
				}
				else
				{
					Model.State = EAssetCompatibilityAuditState::Failed;
					Model.Failure = Outcome.Diagnostic.empty()
						? "The asset compatibility audit worker failed."
						: Outcome.Diagnostic;
				}
				Model.InvalidatePresentation();
			},
			TerminalOptions
		);
		if (!TerminalTask.IsValid())
		{
			Cancellation.RequestCancellation();
			(void)CancelTask(Task);
			(void)WaitTask(Task);
			DrainMailbox();
			State = EAssetCompatibilityAuditState::Failed;
			Failure = "The task scheduler rejected terminal audit publication.";
			return false;
		}
		return true;
	}

	auto FAssetCompatibilityAuditModel::Cancel() -> bool
	{
		if (State != EAssetCompatibilityAuditState::Running) return false;
		Cancellation.RequestCancellation();
		(void)CancelTask(Task);
		return true;
	}

	auto FAssetCompatibilityAuditModel::CancelAndDrain() -> void
	{
		if (State == EAssetCompatibilityAuditState::Running)
		{
			Cancel();
			if (TerminalTask.IsValid()) (void)CancelTask(TerminalTask);
			if (Task.IsValid()) (void)WaitTask(Task);
		}
		DrainMailbox();
		if (State == EAssetCompatibilityAuditState::Running)
		{
			State = EAssetCompatibilityAuditState::Cancelled;
			InvalidatePresentation();
		}
	}

	auto FAssetCompatibilityAuditModel::ProjectChanged() -> void
	{
		CancelAndDrain();
		++RequestSerial;
		Generation.Advance();
		Records.clear();
		Progress = {};
		Failure.clear();
		State = EAssetCompatibilityAuditState::Idle;
		InvalidatePresentation();
	}

	auto FAssetCompatibilityAuditModel::Shutdown() -> void
	{
		if (!bAdmissionOpen) return;
		bAdmissionOpen = false;
		CancelAndDrain();
		Generation.Advance();
		if (PublicationLifetime) PublicationLifetime->Model = nullptr;
	}

	auto FAssetCompatibilityAuditModel::Tick() -> void
	{
		DrainMailbox();
	}

	auto FAssetCompatibilityAuditModel::ReconcileAssetCatalog(
		const std::unordered_map<FAssetPath, Asset::FAssetData>& Assets) -> void
	{
		Reconcile(Assets);
	}

	auto FAssetCompatibilityAuditModel::Tick(
		const std::unordered_map<FAssetPath, Asset::FAssetData>& Assets) -> void
	{
		Tick();
		ReconcileAssetCatalog(Assets);
	}

	auto FAssetCompatibilityAuditModel::DrainMailbox() -> void
	{
		std::deque<FNotice> Notices;
		{
			std::lock_guard Lock(Mailbox->Mutex);
			Notices.swap(Mailbox->Notices);
		}
		bool bPresentationChanged = false;
		for (FNotice& Notice : Notices)
		{
			if (Notice.Serial != RequestSerial) continue;
			if (Notice.Record)
			{
				Records.insert_or_assign(Notice.Record->PackagePath, std::move(*Notice.Record));
				Progress.Completed = std::min(Progress.Completed + 1, Progress.Total);
				bPresentationChanged = true;
			}
		}
		if (bPresentationChanged) InvalidatePresentation();
		const EAssetCompatibilityAuditState PriorState = State;
		if (State == EAssetCompatibilityAuditState::Running && TerminalTask.IsValid() && TerminalTask.IsComplete())
		{
			const FTaskDiagnostics Diagnostics = TerminalTask.GetDiagnostics();
			if (Diagnostics.State == ETaskState::Failed)
			{
				State = EAssetCompatibilityAuditState::Failed;
				Failure = Diagnostics.Diagnostic;
			}
			else if (Diagnostics.State == ETaskState::Canceled)
			{
				if (Diagnostics.TerminalReason == ETaskTerminalReason::DispatchRejected)
				{
					State = EAssetCompatibilityAuditState::Failed;
					Failure = Diagnostics.Diagnostic;
				}
				else
				{
					State = EAssetCompatibilityAuditState::Cancelled;
					Failure.clear();
				}
			}
		}
		if (State != PriorState) InvalidatePresentation();
	}

	auto FAssetCompatibilityAuditModel::Reconcile(
		const std::unordered_map<FAssetPath, Asset::FAssetData>& Assets) -> void
	{
		bool bPresentationChanged = std::erase_if(
			Records, [&](const auto& Entry) { return !Assets.contains(Entry.first); }) != 0;
		for (const auto& [Path, Data] : Assets)
		{
			auto It = Records.find(Path);
			if (It == Records.end())
			{
				Records.emplace(Path, MakeNotCheckedRecord(Data));
				bPresentationChanged = true;
				continue;
			}
			auto& Record = It->second;
			if (Record.Inspection == Asset::EAssetCompatibilityInspection::NotChecked)
			{
				if (Record.PhysicalPath != Data.PhysicalPath
					|| Record.Fingerprint.FileSize != Data.FileSize
					|| Record.Fingerprint.LastWriteTimeTicks != Data.LastWriteTimeTicks)
				{
					Record = MakeNotCheckedRecord(Data);
					bPresentationChanged = true;
				}
				continue;
			}
			const auto Freshness = Asset::IsAssetPackageCompatibilityRecordCurrent(
				Record, Data.FileSize, Data.LastWriteTimeTicks)
				? Asset::EAssetCompatibilityFreshness::Current
				: Asset::EAssetCompatibilityFreshness::Stale;
			if (Record.Freshness != Freshness)
			{
				Record.Freshness = Freshness;
				bPresentationChanged = true;
			}
		}
		if (bPresentationChanged) InvalidatePresentation();
	}

	auto FAssetCompatibilityAuditModel::GetPresentationRecords() const
		-> const std::vector<Asset::FAssetPackageCompatibilityRecord>&
	{
		if (CachedPresentationRevision != PresentationRevision)
		{
			PresentationRecords.clear();
			PresentationRecords.reserve(Records.size());
			for (const auto& [Path, Record] : Records) PresentationRecords.push_back(Record);
			std::ranges::sort(PresentationRecords, {}, [](const auto& Record) {
				return Record.PackagePath.GetView();
			});
			CachedPresentationRevision = PresentationRevision;
		}
		return PresentationRecords;
	}

	auto FAssetCompatibilityAuditModel::InvalidatePresentation() -> void
	{
		++PresentationRevision;
	}

	auto FAssetCompatibilityAuditModel::FindRecord(const FAssetPath& Path) const
		-> const Asset::FAssetPackageCompatibilityRecord*
	{
		const auto It = Records.find(Path);
		return It == Records.end() ? nullptr : &It->second;
	}
}
