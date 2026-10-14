#pragma once

#include "CoreMinimal.h"
#include "RHICompletion.h"

namespace Durin
{
	class FRHIBuffer;
	class IRHICommandContext;

	// A recorded discard write owns its mapping until replay or abandonment.
	class FRHIBufferMappedWrite
	{
	public:
		virtual ~FRHIBufferMappedWrite() = default;
		virtual auto GetData() const -> void* = 0;
		virtual auto Commit(IRHICommandContext& Context, FRHIBuffer* Buffer,
			uint32 Offset, uint32 Size) -> void = 0;
	};

	// One native allocation. External references cover CPU locks, recorded commands,
	// current bindings and submissions; completion metadata covers every GPU use.
	class FRHIDynamicBufferWriteRange final
	{
		friend class FRHIDynamicBufferWritePool;
	public:
		FRHIDynamicBufferWriteRange(std::shared_ptr<void> InOwner, void* InData)
			: Owner(std::move(InOwner)), Data(InData) { require(Owner && Data); }
		auto GetData() const -> void* { return Data; }
		auto GetOwner() const -> const std::shared_ptr<void>& { return Owner; }
		auto AddGPUUse(const FRHIGPUSyncPointRef& Signal) -> void
		{
			std::lock_guard Lock(Mutex);
			require(Uses.Add(Signal));
		}
	private:
		std::shared_ptr<void> Owner;
		void* Data;
		std::mutex Mutex;
		FRHIRetirementPrerequisites Uses;
	};

	// A disjoint occupancy snapshot, taken before selecting the next write range.
	struct FRHIDynamicBufferWritePoolSnapshot final
	{
		uint32 RangeCount = 0;
		uint32 RetainedRangeCount = 0;
		uint32 AwaitingRetirementRangeCount = 0;
		uint32 ReusableRangeCount = 0;
	};

	// Initialized before publication. Acquisition never allocates native memory,
	// polls a native fence, waits for replay/GPU completion, or grows the pool.
	class FRHIDynamicBufferWritePool final
	{
	public:
		auto Add(std::shared_ptr<void> Owner, void* Data) -> void
		{
			std::lock_guard Lock(Mutex);
			Ranges.push_back(std::make_shared<FRHIDynamicBufferWriteRange>(std::move(Owner), Data));
		}
		// Optional diagnostics describe occupancy before this acquisition. Retained
		// ranges include CPU, command, binding and submission references.
		auto TryAcquire(FRHIDynamicBufferWritePoolSnapshot* Snapshot = nullptr)
			-> std::shared_ptr<FRHIDynamicBufferWriteRange>
		{
			std::lock_guard Lock(Mutex);
			if (Snapshot) *Snapshot = {static_cast<uint32>(Ranges.size())};
			std::shared_ptr<FRHIDynamicBufferWriteRange> Result;
			for (const auto& Range : Ranges)
			{
				if (Range.use_count() != 1)
				{
					if (Snapshot) ++Snapshot->RetainedRangeCount;
					continue;
				}
				std::lock_guard RangeLock(Range->Mutex);
				if (Range->Uses.IsRetirementEligible())
				{
					if (!Snapshot) return Range;
					++Snapshot->ReusableRangeCount;
					if (!Result) Result = Range;
				}
				else if (Snapshot) ++Snapshot->AwaitingRetirementRangeCount;
			}
			return Result;
		}
	private:
		std::mutex Mutex;
		std::vector<std::shared_ptr<FRHIDynamicBufferWriteRange>> Ranges;
	};
}
