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

	// One mapped slice. External references cover CPU locks, recorded commands,
	// current bindings and submissions; completion metadata covers every GPU use.
	class FRHIDynamicBufferWriteRange final
	{
		friend class FRHIDynamicBufferPagePool;
	public:
		FRHIDynamicBufferWriteRange(std::shared_ptr<void> InOwner, void* InData)
			: Owner(std::move(InOwner)), Data(InData) { require(Owner && Data); }
		auto GetData() const -> void* { return Data; }
		auto GetOwner() const -> const std::shared_ptr<void>& { return Owner; }
		auto GetOffset() const -> uint32 { return Offset; }
		auto GetSize() const -> uint32 { return Size; }
		// Backend replaces the page owner with a slice owner before publishing the lock.
		// That slice must retain the same page and mapped address.
		auto SetOwner(std::shared_ptr<void> InOwner) -> void { require(InOwner); Owner = std::move(InOwner); }
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
		uint32 Offset = 0;
		uint32 Size = 0;
	};

	// A disjoint occupancy snapshot, taken before selecting the next write range.
	struct FRHIDynamicBufferWritePoolSnapshot final
	{
		uint32 RangeCount = 0;
		uint32 RetainedRangeCount = 0;
		uint32 AwaitingRetirementRangeCount = 0;
		uint32 PageCount = 0;
		uint64 ReservedBytes = 0;
		uint64 FreeBytes = 0;
	};

}
