#pragma once

#include "DurinEdAPI.h"
#include "AssetSystem.h"

namespace Durin
{
	enum class EAssetUpgradeAuditServiceState : uint8
	{
		Idle,
		Auditing,
		Paused,
		Completed,
		Cancelled,
		Shutdown
	};

	struct FAssetUpgradeAuditSnapshot
	{
		uint64 Generation = 0;
		EAssetUpgradeAuditServiceState State = EAssetUpgradeAuditServiceState::Idle;
		Asset::FAssetUpgradeSessionReport Session;
	};

	struct FAssetUpgradeAuditServiceDependencies
	{
		std::function<uint64()> GetRegistryRevision;
		std::function<std::vector<Asset::FAssetData>()> SnapshotAssets;
		std::function<Asset::FAssetResult(
			const Asset::FAssetData&,
			Asset::FAssetPackageAuditReport&)> AuditPackage;
		std::function<double()> GetSeconds;
	};

	// Owns the process-wide, object-free asset upgrade audit. Tick must run on the
	// game thread because reflection and migration contributor registries are global.
	class FAssetUpgradeAuditService
	{
	public:
		static constexpr uint32 DefaultMaxPackagesPerTick = 4;
		static constexpr double DefaultTimeBudgetMilliseconds = 2.0;

		DURINED_API explicit FAssetUpgradeAuditService(
			Asset::FAssetRegistry& Registry,
			uint32 InMaxPackagesPerTick = DefaultMaxPackagesPerTick,
			double InTimeBudgetMilliseconds = DefaultTimeBudgetMilliseconds);
		DURINED_API explicit FAssetUpgradeAuditService(
			FAssetUpgradeAuditServiceDependencies InDependencies,
			uint32 InMaxPackagesPerTick = DefaultMaxPackagesPerTick,
			double InTimeBudgetMilliseconds = DefaultTimeBudgetMilliseconds);
		DURINED_API ~FAssetUpgradeAuditService();

		FAssetUpgradeAuditService(const FAssetUpgradeAuditService&) = delete;
		auto operator=(const FAssetUpgradeAuditService&) -> FAssetUpgradeAuditService& = delete;

		DURINED_API auto Start() -> void;
		DURINED_API auto Tick() -> void;
		DURINED_API auto Pause() -> void;
		DURINED_API auto Resume() -> void;
		DURINED_API auto Cancel() -> void;
		DURINED_API auto Reaudit() -> void;
		DURINED_API auto Shutdown() -> void;
		DURINED_API auto GetSnapshot() const -> std::shared_ptr<const FAssetUpgradeAuditSnapshot>;

	private:
		auto BeginSession() -> void;
		auto PublishSnapshot() -> void;

		FAssetUpgradeAuditServiceDependencies Dependencies;
		uint32 MaxPackagesPerTick = DefaultMaxPackagesPerTick;
		double TimeBudgetMilliseconds = DefaultTimeBudgetMilliseconds;
		uint64 Generation = 0;
		uint64 RegistryRevision = 0;
		size_t NextPackageIndex = 0;
		EAssetUpgradeAuditServiceState State = EAssetUpgradeAuditServiceState::Idle;
		std::vector<Asset::FAssetData> Queue;
		Asset::FAssetUpgradeSessionReport Session;
		std::atomic<std::shared_ptr<const FAssetUpgradeAuditSnapshot>> PublishedSnapshot;
	};
}
