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
	// Shared quotas and launch ordering only. Products and terminal delivery belong
	// to their typed managers. Owner-thread fields coexist with worker slot retirement.
	struct FMeshBuildBudget
	{
		static constexpr uint64 MaximumRequestBytes = 512ull * 1024 * 1024;
		static constexpr uint64 MaximumTotalBytes = 1024ull * 1024 * 1024;
		static constexpr uint32 MaximumRecords = 32;
		std::atomic<uint32> AdmittedBuildCount = 0;
		std::mutex Mutex;
		std::condition_variable Changed;
		uint64 ReservedBytes = 0;
		uint32 RecordCount = 0;
		// Advance retired DDC dispatches without applying unrelated render results.
		std::function<void()> ProgressRender;
		struct FQueuedLaunch
		{
			bool Interactive;
			std::function<bool()> Launch;
		};
		std::deque<FQueuedLaunch> Queue;
		uint32 InteractiveBurst = 0;
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
	public:
		auto ReleaseSlot() -> void
		{
			AdmittedBuildCount.fetch_sub(1);
			Changed.notify_all();
		}
		auto Enqueue(bool Interactive, std::function<bool()> Launch) -> void
		{
			Queue.push_back({Interactive, std::move(Launch)});
			Admit();
		}
		auto Admit() -> void
		{
			while (AdmittedBuildCount.load() < 2 && !Queue.empty())
			{
				auto Background = std::ranges::find(Queue, false, &FQueuedLaunch::Interactive);
				auto Interactive = std::ranges::find(Queue, true, &FQueuedLaunch::Interactive);
				auto Selected = Interactive != Queue.end() && (Background == Queue.end() || InteractiveBurst < 4) ? Interactive : Background;
				const bool bInteractive = Selected->Interactive;
				auto Launch = std::move(Selected->Launch);
				Queue.erase(Selected); // Hooks can reenter admission; reserve before calling them.
				AdmittedBuildCount.fetch_add(1);
				if (Launch())
				{
					if (bInteractive)
						++InteractiveBurst;
					else
						InteractiveBurst = 0;
				}
				else
					ReleaseSlot();
			}
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
