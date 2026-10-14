#pragma once

#include "CoreMinimal.h"

namespace Durin::AssetPrivate
{
	class FMeshBuildReservation;
	struct FMeshBuildAccounting
	{
		uint64 ReservedBytes = 0;
		uint32 RecordCount = 0;
	};
	// Owner-thread memory/record quotas only. Execution belongs to the asset scheduler;
	// products and terminal delivery belong to their typed managers.
	struct FMeshBuildBudget
	{
		static constexpr uint64 MaximumRequestBytes = 512ull * 1024 * 1024;
		static constexpr uint64 MaximumTotalBytes = 1024ull * 1024 * 1024;
		static constexpr uint32 MaximumRecords = 32;
		uint64 ReservedBytes = 0;
		uint32 RecordCount = 0;
		auto CanReserve(uint64 Bytes) const -> bool
		{
			return RecordCount < MaximumRecords && Bytes <= MaximumTotalBytes - ReservedBytes;
		}
	private:
		friend class FMeshBuildReservation;
		auto Reserve(uint64 Bytes) -> void
		{
			++RecordCount;
			ReservedBytes += Bytes;
		}
		auto Release(uint64 Bytes) -> void
		{
			--RecordCount;
			ReservedBytes -= Bytes;
		}
	};
	// Owner-thread reservation, including output and notification lifetime. Records
	// transfer it to the finalization scope so snapshots cannot delay its release.
	class FMeshBuildReservation
	{
	public:
		static auto TryCreate(std::shared_ptr<FMeshBuildBudget> Budget,
			std::shared_ptr<FMeshBuildAccounting> Accounting, uint64 Bytes) -> std::unique_ptr<FMeshBuildReservation>
		{
			if (!Budget->CanReserve(Bytes)) return {};
			return std::unique_ptr<FMeshBuildReservation>(new FMeshBuildReservation(std::move(Budget), std::move(Accounting), Bytes));
		}
		~FMeshBuildReservation()
		{
			Budget->Release(Bytes);
			Accounting->ReservedBytes -= Bytes;
			--Accounting->RecordCount;
		}
		FMeshBuildReservation(const FMeshBuildReservation&) = delete;
		auto operator=(const FMeshBuildReservation&) -> FMeshBuildReservation& = delete;
	private:
		FMeshBuildReservation(std::shared_ptr<FMeshBuildBudget> InBudget,
			std::shared_ptr<FMeshBuildAccounting> InAccounting, uint64 InBytes)
			: Budget(std::move(InBudget)), Accounting(std::move(InAccounting)), Bytes(InBytes)
		{
			Budget->Reserve(Bytes);
			Accounting->ReservedBytes += Bytes;
			++Accounting->RecordCount;
		}
		std::shared_ptr<FMeshBuildBudget> Budget;
		std::shared_ptr<FMeshBuildAccounting> Accounting;
		uint64 Bytes;
	};
	inline auto GetMeshBuildBudget() -> std::shared_ptr<FMeshBuildBudget>
	{
		static auto Budget = std::make_shared<FMeshBuildBudget>();
		return Budget;
	}
} // namespace Durin::AssetPrivate
