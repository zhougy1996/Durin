#include "Asset/AssetUpgradeAuditService.h"

#include "Editor/EditorNotification.h"
#include "Misc/Time.h"

namespace Durin
{
	namespace
	{
		auto MakeRegistryDependencies(Asset::FAssetRegistry& Registry)
			-> FAssetUpgradeAuditServiceDependencies
		{
			return {
				.GetRegistryRevision = [&Registry] {
					return Registry.GetRevision();
				},
				.SnapshotAssets = [&Registry] {
					std::vector<Asset::FAssetData> Assets;
					Assets.reserve(Registry.GetAssets().size());
					for (const auto& [Path, Data] : Registry.GetAssets()) Assets.push_back(Data);
					return Assets;
				},
				.AuditPackage = [](const Asset::FAssetData& Data, Asset::FAssetPackageAuditReport& Report) {
					return Asset::AuditAssetPackage(Data, Report);
				},
				.GetSeconds = [] {
					return FTime::Seconds();
				}};
		}
	}

	FAssetUpgradeAuditService::FAssetUpgradeAuditService(
		Asset::FAssetRegistry& Registry,
		uint32 InMaxPackagesPerTick,
		double InTimeBudgetMilliseconds)
		: FAssetUpgradeAuditService(
			MakeRegistryDependencies(Registry),
			InMaxPackagesPerTick,
			InTimeBudgetMilliseconds)
	{
	}

	FAssetUpgradeAuditService::FAssetUpgradeAuditService(
		FAssetUpgradeAuditServiceDependencies InDependencies,
		uint32 InMaxPackagesPerTick,
		double InTimeBudgetMilliseconds)
		: Dependencies(std::move(InDependencies))
		, MaxPackagesPerTick(std::max(1u, InMaxPackagesPerTick))
		, TimeBudgetMilliseconds(std::max(0.0, InTimeBudgetMilliseconds))
	{
		check(Dependencies.GetRegistryRevision);
		check(Dependencies.SnapshotAssets);
		check(Dependencies.AuditPackage);
		check(Dependencies.GetSeconds);
		PublishSnapshot();
	}

	FAssetUpgradeAuditService::~FAssetUpgradeAuditService()
	{
		Shutdown();
	}

	auto FAssetUpgradeAuditService::Start() -> void
	{
		if (State == EAssetUpgradeAuditServiceState::Shutdown) return;
		BeginSession();
	}

	auto FAssetUpgradeAuditService::Tick() -> void
	{
		if (State == EAssetUpgradeAuditServiceState::Idle
			|| State == EAssetUpgradeAuditServiceState::Shutdown)
			return;
		if (Dependencies.GetRegistryRevision() != RegistryRevision)
		{
			BeginSession();
			return;
		}
		if (State != EAssetUpgradeAuditServiceState::Auditing) return;

		const double SliceStartSeconds = Dependencies.GetSeconds();
		uint32 Processed = 0;
		while (NextPackageIndex < Queue.size() && Processed < MaxPackagesPerTick)
		{
			const double PackageStartSeconds = Dependencies.GetSeconds();
			Asset::FAssetPackageAuditReport Report = Session.Packages[NextPackageIndex];
			const Asset::FAssetResult Result =
				Dependencies.AuditPackage(Queue[NextPackageIndex], Report);
			Report.PackagePath = Queue[NextPackageIndex].PackagePath;
			Report.AuditDurationMilliseconds =
				std::max(0.0, (Dependencies.GetSeconds() - PackageStartSeconds) * 1000.0);
			if (!Result && Report.State == Asset::EAssetPackageAuditState::NotAudited)
			{
				Report.PackagePath = Queue[NextPackageIndex].PackagePath;
				Report.AssetClassName = Queue[NextPackageIndex].AssetClassName;
				Report.FormatVersion = Queue[NextPackageIndex].FormatVersion;
				Report.State = Asset::EAssetPackageAuditState::AuditFailed;
				Report.Diagnostic = Result.Message;
			}
			Session.Packages[NextPackageIndex] = std::move(Report);
			++NextPackageIndex;
			++Processed;

			const double ElapsedMilliseconds =
				(Dependencies.GetSeconds() - SliceStartSeconds) * 1000.0;
			if (ElapsedMilliseconds >= TimeBudgetMilliseconds) break;
		}

		if (NextPackageIndex == Queue.size())
			State = EAssetUpgradeAuditServiceState::Completed;
		PublishSnapshot();
	}

	auto FAssetUpgradeAuditService::Pause() -> void
	{
		if (State != EAssetUpgradeAuditServiceState::Auditing) return;
		State = EAssetUpgradeAuditServiceState::Paused;
		PublishSnapshot();
	}

	auto FAssetUpgradeAuditService::Resume() -> void
	{
		if (State != EAssetUpgradeAuditServiceState::Paused) return;
		State = EAssetUpgradeAuditServiceState::Auditing;
		PublishSnapshot();
	}

	auto FAssetUpgradeAuditService::Cancel() -> void
	{
		if (State != EAssetUpgradeAuditServiceState::Auditing
			&& State != EAssetUpgradeAuditServiceState::Paused)
			return;
		State = EAssetUpgradeAuditServiceState::Cancelled;
		PublishSnapshot();
	}

	auto FAssetUpgradeAuditService::Reaudit() -> void
	{
		if (State == EAssetUpgradeAuditServiceState::Shutdown) return;
		BeginSession();
	}

	auto FAssetUpgradeAuditService::MergeWorkspaceLoadReport(
		const Asset::FAssetLoadReport& Report) -> void
	{
		if (State == EAssetUpgradeAuditServiceState::Shutdown || !Report.PackagePath.IsValid())
			return;
		if (!Report.HasCompatibilityIssues() && !Report.HasNonUpgradeMutations()) return;
		if (State == EAssetUpgradeAuditServiceState::Idle)
		{
			const auto Existing = std::ranges::find(
				PendingWorkspaceReports, Report.PackagePath, &Asset::FAssetLoadReport::PackagePath);
			if (Existing == PendingWorkspaceReports.end()) PendingWorkspaceReports.push_back(Report);
			else *Existing = Report;
			return;
		}

		auto Existing = std::ranges::find(Session.Packages, Report.PackagePath,
			&Asset::FAssetPackageAuditReport::PackagePath);
		Asset::FAssetPackageAuditReport Merged;
		if (Existing != Session.Packages.end()) Merged = *Existing;
		Merged.PackagePath = Report.PackagePath;
		Merged.CompatibilityIssues = Report.CompatibilityIssues;
		Merged.Diagnostic = "Updated from the package report produced by its active workspace.";
		if (Report.HasNonUpgradeMutations())
			Merged.State = Asset::EAssetPackageAuditState::BlockedLoadMutation;
		else if (Report.HasRiskItems())
			Merged.State = Asset::EAssetPackageAuditState::RiskyUpgrade;
		else
			Merged.State = Asset::EAssetPackageAuditState::SafeUpgrade;

		if (Existing == Session.Packages.end()) Session.Packages.push_back(std::move(Merged));
		else *Existing = std::move(Merged);

		const auto Pending = std::ranges::find(Queue, Report.PackagePath, &Asset::FAssetData::PackagePath);
		if (Pending != Queue.end())
		{
			const size_t PendingIndex = static_cast<size_t>(std::distance(Queue.begin(), Pending));
			Queue.erase(Pending);
			if (PendingIndex < NextPackageIndex) --NextPackageIndex;
		}
		if (NextPackageIndex >= Queue.size()
			&& State == EAssetUpgradeAuditServiceState::Auditing)
			State = EAssetUpgradeAuditServiceState::Completed;
		PublishSnapshot();
	}

	auto FAssetUpgradeAuditService::InvalidatePackage(const FAssetPath& PackagePath) -> void
	{
		if (State == EAssetUpgradeAuditServiceState::Idle
			|| State == EAssetUpgradeAuditServiceState::Shutdown)
			return;
		const auto Existing = std::ranges::find(Session.Packages, PackagePath,
			&Asset::FAssetPackageAuditReport::PackagePath);
		if (Existing == Session.Packages.end()) return;
		Existing->State = Asset::EAssetPackageAuditState::Stale;
		Existing->Diagnostic = "The package changed after this audit result was published.";
		PublishSnapshot();
	}

	auto FAssetUpgradeAuditService::Shutdown() -> void
	{
		if (State == EAssetUpgradeAuditServiceState::Shutdown) return;
		State = EAssetUpgradeAuditServiceState::Shutdown;
		Queue.clear();
		PendingWorkspaceReports.clear();
		NextPackageIndex = 0;
		PublishSnapshot();
	}

	auto FAssetUpgradeAuditService::GetSnapshot() const
		-> std::shared_ptr<const FAssetUpgradeAuditSnapshot>
	{
		return PublishedSnapshot.load(std::memory_order_acquire);
	}

	auto FAssetUpgradeAuditService::BeginSession() -> void
	{
		RegistryRevision = Dependencies.GetRegistryRevision();
		Queue = Dependencies.SnapshotAssets();
		std::ranges::sort(Queue, {}, [](const Asset::FAssetData& Data) {
			return Data.PackagePath.ToString();
		});
		NextPackageIndex = 0;
		++Generation;
		State = Queue.empty()
			? EAssetUpgradeAuditServiceState::Completed
			: EAssetUpgradeAuditServiceState::Auditing;
		Session = {.RegistryRevision = RegistryRevision};
		Session.Packages.reserve(Queue.size());
		for (const Asset::FAssetData& Data : Queue)
		{
			Session.Packages.push_back({
				.PackagePath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.FormatVersion = Data.FormatVersion});
		}
		std::vector<Asset::FAssetLoadReport> WorkspaceReports =
			std::exchange(PendingWorkspaceReports, {});
		for (const Asset::FAssetLoadReport& Report : WorkspaceReports)
			MergeWorkspaceLoadReport(Report);
		PublishSnapshot();
	}

	auto FAssetUpgradeAuditService::PublishSnapshot() -> void
	{
		Session.RebuildProgressAndSort();
		PublishedSnapshot.store(
			std::make_shared<const FAssetUpgradeAuditSnapshot>(
				FAssetUpgradeAuditSnapshot{
					.Generation = Generation,
					.State = State,
					.Session = Session}),
			std::memory_order_release);
	}

	FAssetUpgradeAuditNotificationController::FAssetUpgradeAuditNotificationController(
		FAssetUpgradeAuditService& InService,
		FEditorNotificationManager& InNotificationManager,
		std::function<void()> InOpenUpgradeCenter)
		: Service(InService)
		, NotificationManager(InNotificationManager)
		, OpenUpgradeCenter(std::move(InOpenUpgradeCenter))
	{
	}

	auto FAssetUpgradeAuditNotificationController::Tick() -> void
	{
		const std::shared_ptr<const FAssetUpgradeAuditSnapshot> Snapshot = Service.GetSnapshot();
		if (!Snapshot || Snapshot->State == EAssetUpgradeAuditServiceState::Idle
			|| Snapshot->State == EAssetUpgradeAuditServiceState::Shutdown)
			return;
		const Asset::FAssetUpgradeSessionProgress& Progress = Snapshot->Session.Progress;
		const float Fraction = Progress.Total == 0
			? 1.0f
			: static_cast<float>(Progress.Completed) / static_cast<float>(Progress.Total);

		if (Snapshot->Generation != ObservedGeneration)
		{
			if (NotificationId) NotificationManager.Dismiss(*NotificationId);
			ObservedGeneration = Snapshot->Generation;
			ObservedCompleted = std::numeric_limits<uint64>::max();
			ObservedState = EAssetUpgradeAuditServiceState::Idle;
			NotificationId = NotificationManager.BeginProgress({
				.Message = std::format(
					"Checking project assets for upgrades... 0/{}", Progress.Total),
				.Progress = Fraction,
				.Action = FEditorNotificationAction{
					.Label = "Open Asset Upgrade Center",
					.Invoke = OpenUpgradeCenter},
				.Cancel = [this] { Service.Cancel(); }});
		}
		if (!NotificationId) return;
		if (Snapshot->State == ObservedState && Progress.Completed == ObservedCompleted) return;

		switch (Snapshot->State)
		{
		case EAssetUpgradeAuditServiceState::Auditing:
			NotificationManager.UpdateProgress(
				*NotificationId,
				Fraction,
				std::format(
					"Checking project assets for upgrades... {}/{}",
					Progress.Completed,
					Progress.Total));
			break;
		case EAssetUpgradeAuditServiceState::Paused:
			NotificationManager.UpdateProgress(
				*NotificationId, Fraction, "Project asset upgrade check paused.");
			break;
		case EAssetUpgradeAuditServiceState::Completed:
			NotificationManager.CompleteProgress(
				*NotificationId,
				std::format(
					"Asset upgrade check complete: {} safe, {} risky, {} blocked, {} failed.",
					Progress.Safe,
					Progress.Risky,
					Progress.Blocked,
					Progress.Failed));
			break;
		case EAssetUpgradeAuditServiceState::Cancelled:
			NotificationManager.CompleteProgress(
				*NotificationId,
				std::format(
					"Asset upgrade check cancelled after {}/{} packages.",
					Progress.Completed,
					Progress.Total));
			break;
		default:
			break;
		}
		ObservedState = Snapshot->State;
		ObservedCompleted = Progress.Completed;
	}

	auto FAssetUpgradeAuditNotificationController::Shutdown() -> void
	{
		if (NotificationId) NotificationManager.Dismiss(*NotificationId);
		NotificationId.reset();
	}
}
