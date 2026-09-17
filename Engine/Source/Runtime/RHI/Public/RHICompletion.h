#pragma once

#include "RHIAPI.h"
#include "Templates/RefCounting.h"

namespace Durin
{
	// Physical identity within one device; command class and queue family are separate.
	struct FRHIQueueId final
	{
		uint32 Index = 0;
		auto operator==(const FRHIQueueId&) const -> bool = default;
	};

	// Published physical topology; multiple command roles can name the same queue.
	struct FRHIQueueInfo final
	{
		FRHIQueueId Id;
		uint32 OwnershipDomain = 0;
		bool bGraphics = false;
		bool bCompute = false;
		bool bCopy = false;
		bool bTimestamps = false;
	};
	struct FRHIQueueCapabilities final
	{
		uint64 DeviceGeneration = 0;
		std::vector<FRHIQueueInfo> Queues;
		FRHIQueueId Graphics;
		FRHIQueueId Compute;
		bool bIndependentCompute = false;
		bool bSplitBarriers = false;
	};

	// Pending work is never implicitly submitted by an exact completion wait.
	enum class ERHIGPUWaitResult : uint8
	{
		Complete, Timeout, Pending, Canceled, Failed, DeviceLost, Invalid
	};

	// Cancellation and device failure do not claim that GPU work completed.
	enum class ERHIGPUSubmissionState : uint8
	{
		Invalid, Pending, Submitted, Complete, Canceled, Failed, DeviceLost
	};


	struct FRHIGPUReservationState;
	class FRHIGPUQueueTimeline;
	class FRHIGPUSyncPointBackend;
	class FRHIGPUSyncPointRef;

	// One-shot metadata only. Release never calls a backend or schedules deletion.
	class FRHIGPUSyncPoint final
	{
	public:
		RHI_API static auto Create() -> FRHIGPUSyncPointRef;
		FRHIGPUSyncPoint(const FRHIGPUSyncPoint&) = delete;
		auto operator=(const FRHIGPUSyncPoint&) -> FRHIGPUSyncPoint& = delete;
		auto AddRef() const -> uint32 { return References.fetch_add(1, std::memory_order_relaxed) + 1; }
		RHI_API auto Release() const -> uint32;
		auto GetRefCount() const -> uint32 { return References.load(std::memory_order_relaxed); }
		RHI_API auto GetState() const -> ERHIGPUSubmissionState;
		auto IsComplete() const -> bool { return GetState() == ERHIGPUSubmissionState::Complete; }
		RHI_API auto IsRetirementEligible() const -> bool;
	private:
		friend class FRHIGPUQueueTimeline;
		friend class FRHIGPUSyncPointBackend;
		FRHIGPUSyncPoint() = default;
		~FRHIGPUSyncPoint() = default;
		mutable std::atomic<uint32> References = 0;
		mutable std::mutex Mutex;
		std::shared_ptr<FRHIGPUReservationState> Reservation;
		bool bCanceled = false;
	};

	// Owning RHI reference with null-safe observation for optional signals.
	class FRHIGPUSyncPointRef final : public TRefCountPtr<FRHIGPUSyncPoint>
	{
	public:
		using TRefCountPtr<FRHIGPUSyncPoint>::TRefCountPtr;
		auto GetState() const -> ERHIGPUSubmissionState
		{ return *this ? GetReference()->GetState() : ERHIGPUSubmissionState::Invalid; }
		auto IsComplete() const -> bool { return *this && GetReference()->IsComplete(); }
		auto IsRetirementEligible() const -> bool
		{ return *this && GetReference()->IsRetirementEligible(); }
		auto operator==(const FRHIGPUSyncPointRef& Other) const -> bool
		{ return GetReference() == Other.GetReference(); }
	};

	struct FRHIGPUSubmissionDesc final
	{
		FRHIQueueId Queue;
		std::vector<FRHIGPUSyncPointRef> Waits;
	};

	// Exact logical uses, never compressed by caller-visible queue coordinates.
	class FRHIRetirementPrerequisites final
	{
	public:
		RHI_API auto Add(const FRHIGPUSyncPointRef& Signal) -> bool;
		RHI_API auto IsRetirementEligible() const -> bool;
		auto GetSyncPoints() const -> std::span<const FRHIGPUSyncPointRef> { return Signals; }
	private:
		std::vector<FRHIGPUSyncPointRef> Signals;
	};

	// Observes terminal metadata without a live backend; never submits pending work.
	RHI_API auto WaitForRHIGPUSyncPoint(const FRHIGPUSyncPointRef& Signal,
		uint64 TimeoutNanoseconds) -> ERHIGPUWaitResult;
}
