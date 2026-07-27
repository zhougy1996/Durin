#include "Asset/AssetUpgradeAuditService.h"

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

	auto FAssetUpgradeAuditService::Shutdown() -> void
	{
		if (State == EAssetUpgradeAuditServiceState::Shutdown) return;
		State = EAssetUpgradeAuditServiceState::Shutdown;
		Queue.clear();
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
}
