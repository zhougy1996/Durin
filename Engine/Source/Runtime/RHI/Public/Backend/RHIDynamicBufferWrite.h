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
		auto TryAcquire() -> std::shared_ptr<FRHIDynamicBufferWriteRange>
		{
			std::lock_guard Lock(Mutex);
			for (const auto& Range : Ranges)
			{
				if (Range.use_count() != 1) continue;
				std::lock_guard RangeLock(Range->Mutex);
				if (Range->Uses.IsRetirementEligible()) return Range;
			}
			return {};
		}
	private:
		std::mutex Mutex;
		std::vector<std::shared_ptr<FRHIDynamicBufferWriteRange>> Ranges;
	};
}
