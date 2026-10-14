#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Threading/Task.h"

namespace Durin::AssetPrivate
{
	struct FAssetBuildExecutionState;
	class ENGINE_API FAssetBuildExecutionSlot
	{
	public:
		// The last callable capture releases capacity, including cancellation before entry.
		~FAssetBuildExecutionSlot();
		FAssetBuildExecutionSlot(const FAssetBuildExecutionSlot&) = delete;
		auto operator=(const FAssetBuildExecutionSlot&) -> FAssetBuildExecutionSlot& = delete;
	private:
		friend class FAssetBuildScheduler;
		FAssetBuildExecutionSlot(std::shared_ptr<FAssetBuildExecutionState> InState, uint64 InRequiredMemory);
		std::shared_ptr<FAssetBuildExecutionState> State;
		uint64 RequiredMemory;
	};
	// Owner-thread dispatch with worker-side slot release. No objects, products,
	// application callbacks are owned by this scheduler. Memory estimates control
	// execution concurrency, not retained input/product lifetimes or allocator limits.
	class ENGINE_API FAssetBuildScheduler
	{
	public:
		explicit FAssetBuildScheduler(uint32 MaximumConcurrency = 2, uint64 MemoryBudget = 1024ull * 1024 * 1024);
		// Optional readiness only observes external retirement; it must not mutate this queue.
		auto Enqueue(bool Interactive, uint64 RequiredMemory, std::function<bool(std::shared_ptr<FAssetBuildExecutionSlot>)> Launch,
			FTaskHandle Prerequisite = {}, std::function<bool()> IsReady = {}) -> void;
		auto Pump() -> void;
		auto WaitForProgress() -> void;
		auto GetScheduledMemory() const -> uint64;
	private:
		struct FQueuedLaunch
		{
			bool Interactive;
			uint64 RequiredMemory;
			std::function<bool(std::shared_ptr<FAssetBuildExecutionSlot>)> Launch;
			FTaskHandle Prerequisite;
			std::function<bool()> IsReady;
		};
		std::shared_ptr<FAssetBuildExecutionState> State;
		std::deque<FQueuedLaunch> Queue;
		uint32 InteractiveBurst = 0;
		bool bPumping = false;
		uint32 MaximumConcurrency;
		uint64 MemoryBudget;
	};
	ENGINE_API auto GetAssetBuildScheduler() -> std::shared_ptr<FAssetBuildScheduler>;
}
