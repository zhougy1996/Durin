#include "Asset/AssetCompatibilityAudit.h"

#include "AssetSystem.h"

namespace Durin::Editor
{
	namespace
	{
		struct FNotice
		{
			uint64 Serial = 0;
			std::optional<Asset::FAssetPackageCompatibilityRecord> Record;
			std::optional<EAssetCompatibilityAuditState> TerminalState;
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
		return Text;
	}

	struct FAssetCompatibilityAuditModel::FMailbox
	{
		std::mutex Mutex;
		std::deque<FNotice> Notices;
	};

	FAssetCompatibilityAuditModel::FAssetCompatibilityAuditModel(FAssetCompatibilityProbe InProbe)
		: Probe(std::move(InProbe)), Mailbox(std::make_shared<FMailbox>())
	{
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
	}

	auto FAssetCompatibilityAuditModel::RunCurrentProjectAudit() -> bool
	{
		return RunAudit(Asset::GetAssetRegistry().GetAssets(),
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

		State = EAssetCompatibilityAuditState::Running;
		Cancellation = FTaskCancellationSource{};
		FTaskLaunchOptions Options;
		Options.CancellationToken = Cancellation.GetToken();
		const uint64 Serial = RequestSerial;
		const std::shared_ptr<FMailbox> WorkerMailbox = Mailbox;
		const FAssetCompatibilityProbe WorkerProbe = Probe;
		Task = LaunchCancelableTask("AssetCompatibility.Audit",
			[Serial, WorkerMailbox, WorkerProbe, Inputs = std::move(Inputs),
				Catalog = std::move(Catalog)](const FTaskCancellationToken& Token) mutable {
				auto Queue = [&](FNotice Notice) {
					std::lock_guard Lock(WorkerMailbox->Mutex);
					WorkerMailbox->Notices.push_back(std::move(Notice));
				};
				try
				{
					for (const auto& Input : Inputs)
					{
						if (Token.IsCancellationRequested())
						{
							Queue({.Serial = Serial, .TerminalState = EAssetCompatibilityAuditState::Cancelled});
							return;
						}
						auto Result = WorkerProbe(Input, Catalog,
							[&Token] { return Token.IsCancellationRequested(); });
						if (Result.Status == Asset::EAssetCompatibilityProbeStatus::Cancelled)
						{
							Queue({.Serial = Serial, .TerminalState = EAssetCompatibilityAuditState::Cancelled});
							return;
						}
						if (Result.Record)
							Queue({.Serial = Serial, .Record = std::move(Result.Record)});
					}
					Queue({.Serial = Serial, .TerminalState = EAssetCompatibilityAuditState::Completed});
				}
				catch (const std::exception& Exception)
				{
					Queue({.Serial = Serial, .TerminalState = EAssetCompatibilityAuditState::Failed,
						.Failure = Exception.what()});
				}
				catch (...)
				{
					Queue({.Serial = Serial, .TerminalState = EAssetCompatibilityAuditState::Failed,
						.Failure = "Asset compatibility audit failed with an unknown exception."});
				}
			}, Options);
		if (!Task.IsValid())
		{
			State = EAssetCompatibilityAuditState::Failed;
			Failure = "The task scheduler rejected the asset compatibility audit.";
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
			if (Task.IsValid()) (void)WaitTask(Task);
		}
		DrainMailbox();
	}

	auto FAssetCompatibilityAuditModel::ProjectChanged() -> void
	{
		CancelAndDrain();
		++RequestSerial;
		Records.clear();
		Progress = {};
		Failure.clear();
		State = EAssetCompatibilityAuditState::Idle;
	}

	auto FAssetCompatibilityAuditModel::Shutdown() -> void
	{
		if (!bAdmissionOpen) return;
		bAdmissionOpen = false;
		CancelAndDrain();
	}

	auto FAssetCompatibilityAuditModel::Tick(
		const std::unordered_map<FAssetPath, Asset::FAssetData>& Assets) -> void
	{
		DrainMailbox();
		Reconcile(Assets);
	}

	auto FAssetCompatibilityAuditModel::DrainMailbox() -> void
	{
		std::deque<FNotice> Notices;
		{
			std::lock_guard Lock(Mailbox->Mutex);
			Notices.swap(Mailbox->Notices);
		}
		for (FNotice& Notice : Notices)
		{
			if (Notice.Serial != RequestSerial) continue;
			if (Notice.Record)
			{
				Records.insert_or_assign(Notice.Record->PackagePath, std::move(*Notice.Record));
				Progress.Completed = std::min(Progress.Completed + 1, Progress.Total);
			}
			if (Notice.TerminalState)
			{
				State = *Notice.TerminalState;
				Failure = std::move(Notice.Failure);
			}
		}
		if (State == EAssetCompatibilityAuditState::Running && Task.IsValid() && Task.IsComplete())
		{
			const ETaskState TaskState = Task.GetState();
			if (TaskState == ETaskState::Canceled) State = EAssetCompatibilityAuditState::Cancelled;
			else if (TaskState == ETaskState::Failed)
			{
				State = EAssetCompatibilityAuditState::Failed;
				Failure = Task.GetDiagnostic();
			}
		}
	}

	auto FAssetCompatibilityAuditModel::Reconcile(
		const std::unordered_map<FAssetPath, Asset::FAssetData>& Assets) -> void
	{
		std::erase_if(Records, [&](const auto& Entry) { return !Assets.contains(Entry.first); });
		for (const auto& [Path, Data] : Assets)
		{
			auto It = Records.find(Path);
			if (It == Records.end())
			{
				Records.emplace(Path, MakeNotCheckedRecord(Data));
				continue;
			}
			auto& Record = It->second;
			if (Record.Inspection == Asset::EAssetCompatibilityInspection::NotChecked)
			{
				if (Record.PhysicalPath != Data.PhysicalPath
					|| Record.Fingerprint.FileSize != Data.FileSize
					|| Record.Fingerprint.LastWriteTimeTicks != Data.LastWriteTimeTicks)
					Record = MakeNotCheckedRecord(Data);
				continue;
			}
			Record.Freshness = Asset::IsAssetPackageCompatibilityRecordCurrent(
				Record, Data.FileSize, Data.LastWriteTimeTicks)
				? Asset::EAssetCompatibilityFreshness::Current
				: Asset::EAssetCompatibilityFreshness::Stale;
		}
	}

	auto FAssetCompatibilityAuditModel::GetPresentationRecords() const
		-> std::vector<Asset::FAssetPackageCompatibilityRecord>
	{
		std::vector<Asset::FAssetPackageCompatibilityRecord> Result;
		Result.reserve(Records.size());
		for (const auto& [Path, Record] : Records) Result.push_back(Record);
		std::ranges::sort(Result, {}, [](const auto& Record) { return Record.PackagePath.ToString(); });
		return Result;
	}

	auto FAssetCompatibilityAuditModel::FindRecord(const FAssetPath& Path) const
		-> const Asset::FAssetPackageCompatibilityRecord*
	{
		const auto It = Records.find(Path);
		return It == Records.end() ? nullptr : &It->second;
	}
}
