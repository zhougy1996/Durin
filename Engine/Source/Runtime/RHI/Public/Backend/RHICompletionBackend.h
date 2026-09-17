#pragma once
#include "RHICompletion.h"

// Backend implementation seam. Do not include from RHI/renderer consumer APIs.
namespace Durin
{
	// A GPU timeline coordinate, never a CPU replay serial or graph pass index.
	struct FRHIGPUCompletionPoint final
	{
		uint64 DeviceGeneration = 0;
		FRHIQueueId Queue;
		uint64 Value = 0;
		auto operator==(const FRHIGPUCompletionPoint&) const -> bool = default;
	};

	struct FRHIGPUTimelineState;
	class FRHIGPUSyncPointBackend final
	{
	public:
		RHI_API static auto GetPoint(const FRHIGPUSyncPointRef& Signal) -> FRHIGPUCompletionPoint;
		RHI_API static auto Attach(const FRHIGPUSyncPointRef& Signal,
			const FRHIGPUSyncPointRef& Producer) -> bool;
		// Only executable recording leases may cancel an unassociated recording.
		RHI_API static auto CancelUnassociated(const FRHIGPUSyncPointRef& Signal) -> void;
		RHI_API static auto GetReservation(const FRHIGPUSyncPointRef& Signal)
			-> std::shared_ptr<FRHIGPUReservationState>;
	};

	// RHI owns generation allocation so backend replacement cannot reuse an epoch.
	RHI_API auto AllocateRHIDeviceGeneration() -> uint64;

	// One backend writer serializes reservations and native submission on a queue.
	// SyncPoint observations are thread safe. Mutations must follow native acceptance;
	// cancel only after detaching every executable owner of the pending work.
	class FRHIGPUQueueTimeline final
	{
	public:
		RHI_API FRHIGPUQueueTimeline(uint64 DeviceGeneration, FRHIQueueId Queue);
		RHI_API ~FRHIGPUQueueTimeline();
		FRHIGPUQueueTimeline(const FRHIGPUQueueTimeline&) = delete;
		auto operator=(const FRHIGPUQueueTimeline&) -> FRHIGPUQueueTimeline& = delete;
		RHI_API auto Reserve() -> FRHIGPUSyncPointRef;
		RHI_API auto CanSubmit(const FRHIGPUSyncPointRef& SyncPoint) const -> bool;
		// Read-only preflight of an ordered prefix of outstanding reservations.
		// Submitted/canceled holes are allowed; missing pending work is not.
		RHI_API auto CanSubmitBatch(std::span<const FRHIGPUSyncPointRef> SyncPoints) const -> bool;
		RHI_API auto MarkSubmitted(const FRHIGPUSyncPointRef& SyncPoint) -> bool;
		RHI_API auto ObserveCompleted(const FRHIGPUSyncPointRef& SyncPoint) -> bool;
		RHI_API auto Cancel(const FRHIGPUSyncPointRef& SyncPoint) -> bool;
		// Failure closes admission; outstanding metadata becomes terminal without
		// granting normal retirement. Native owners require backend teardown.
		RHI_API auto Fail(bool bDeviceLost = false) -> void;
		RHI_API auto Owns(const FRHIGPUSyncPointRef& SyncPoint) const -> bool;
		auto GetPendingCount() const -> size_t { return Pending.size(); }

	private:
		auto AdvanceRetirement() -> void;
		std::shared_ptr<FRHIGPUTimelineState> State;
		std::deque<std::shared_ptr<FRHIGPUReservationState>> Pending;
		uint64 NextValue = 1;
		bool bClosed = false;
	};

}
