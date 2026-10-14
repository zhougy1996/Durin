#pragma once

#include "CoreMinimal.h"
#include "Threading/Task.h"

namespace Durin::AssetPrivate
{
	struct FAssetBuildExecutionState;
	class FAssetBuildExecutionSlot
	{
	public:
		~FAssetBuildExecutionSlot();
		auto Release() -> void;
		// Releases CPU capacity on function exit, independently of nested DDC retirement.
		struct FWorkScope
		{
			FAssetBuildExecutionSlot& Slot;
			~FWorkScope() { Slot.Release(); }
		};
	private:
		friend class FAssetBuildScheduler;
		explicit FAssetBuildExecutionSlot(std::shared_ptr<FAssetBuildExecutionState> InState);
		std::shared_ptr<FAssetBuildExecutionState> State;
		std::atomic<bool> bReleased = false;
	};
	// Owner-thread dispatch with worker-side slot release. No objects, products,
	// memory reservations or application callbacks are owned by this scheduler.
	class FAssetBuildScheduler
	{
	public:
		FAssetBuildScheduler();
		auto Enqueue(bool Interactive, std::function<FTaskHandle(std::shared_ptr<FAssetBuildExecutionSlot>)> Launch,
			FTaskHandle Prerequisite = {}) -> void;
		auto Pump() -> void;
		auto WaitForProgress() -> void;
	private:
		struct FQueuedLaunch
		{
			bool Interactive;
			std::function<FTaskHandle(std::shared_ptr<FAssetBuildExecutionSlot>)> Launch;
			FTaskHandle Prerequisite;
		};
		struct FRunningLaunch
		{
			FTaskHandle Task;
			std::shared_ptr<FAssetBuildExecutionSlot> Slot;
		};
		std::shared_ptr<FAssetBuildExecutionState> State;
		std::deque<FQueuedLaunch> Queue;
		std::vector<FRunningLaunch> Running;
		uint32 InteractiveBurst = 0;
	};
	auto GetAssetBuildScheduler() -> std::shared_ptr<FAssetBuildScheduler>;
}
