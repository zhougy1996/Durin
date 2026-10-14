#pragma once

#include "CoreMinimal.h"
#include "DObject/ObjectKey.h"

namespace Durin::AssetPrivate
{
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
		std::unordered_set<FObjectKey> MutatingOwners;
		// Advance retired DDC dispatches without applying unrelated render results.
		std::function<void()> ProgressRender;
		struct FQueuedLaunch
		{
			bool Interactive;
			std::function<bool()> Launch;
		};
		std::deque<FQueuedLaunch> Queue;
		uint32 InteractiveBurst = 0;
		uint64 PumpIdentity = 0;
		uint32 PumpCount = 0;
		std::chrono::steady_clock::time_point PumpDeadline;
		auto BeginPump(uint64 Identity) -> void
		{
			if (!Identity || Identity != PumpIdentity)
			{
				PumpIdentity = Identity;
				PumpCount = 0;
				PumpDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
			}
		}
		auto CanReserve(uint64 Bytes) const -> bool
		{
			return RecordCount < MaximumRecords && Bytes <= MaximumTotalBytes - ReservedBytes;
		}
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
	inline auto GetMeshBuildBudget() -> std::shared_ptr<FMeshBuildBudget>
	{
		static auto Budget = std::make_shared<FMeshBuildBudget>();
		return Budget;
	}
} // namespace Durin::AssetPrivate
