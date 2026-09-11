#pragma once

#include "RHIAPI.h"

namespace Durin
{
	// Physical identity within one device; command class and queue family are separate.
	struct FRHIQueueId final
	{
		uint32 Index = 0;
		auto operator==(const FRHIQueueId&) const -> bool = default;
	};

	// A GPU timeline coordinate, never a CPU replay serial or graph pass index.
	struct FRHIGPUCompletionPoint final
	{
		uint64 DeviceGeneration = 0;
		FRHIQueueId Queue;
		uint64 Value = 0;
		auto operator==(const FRHIGPUCompletionPoint&) const -> bool = default;
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

	struct FRHIGPUTicketState;
	struct FRHIGPUTimelineState;
	class FRHIGPUQueueTimeline;
	struct FRHIGPUReceiptState;

	// Owns metadata only; safe to retain after backend shutdown and fence recycling.
	class FRHIGPUSubmissionTicket final
	{
	public:
		FRHIGPUSubmissionTicket() = default;
		RHI_API auto GetPoint() const -> FRHIGPUCompletionPoint;
		RHI_API auto GetState() const -> ERHIGPUSubmissionState;
		// Includes the ordered prefix even for a canceled reservation, permitting
		// same-queue maximum compression without hiding earlier live GPU uses.
		RHI_API auto IsRetirementEligible() const -> bool;

	private:
		friend class FRHIGPUQueueTimeline;
		friend class FRHIRetirementPrerequisites;
		explicit FRHIGPUSubmissionTicket(std::shared_ptr<FRHIGPUTicketState> InState)
			: State(std::move(InState)) {}
		std::shared_ptr<FRHIGPUTicketState> State;
	};

	// A recorded signal whose native queue point is assigned by backend replay.
	// Distinct from a ticket: cancellation of a recording never completes GPU work.
	class FRHIGPUSubmissionReceipt final
	{
	public:
		FRHIGPUSubmissionReceipt() = default;
		RHI_API static auto CreatePending() -> FRHIGPUSubmissionReceipt;
		RHI_API auto GetState() const -> ERHIGPUSubmissionState;
		RHI_API auto GetTicket() const -> FRHIGPUSubmissionTicket;
		// Backend publication is single assignment; a ticket may still be Pending.
		RHI_API auto Resolve(const FRHIGPUSubmissionTicket& Ticket) const -> bool;
		// Called only after all executable recording leases have been detached.
		RHI_API auto CancelUnresolved() const -> void;
		auto operator==(const FRHIGPUSubmissionReceipt&) const -> bool = default;
	private:
		std::shared_ptr<FRHIGPUReceiptState> State;
	};

	// Owns graph-execution dependencies; the command recorder copies this value.
	struct FRHIGPUSubmissionDesc final
	{
		FRHIQueueId Queue;
		std::vector<FRHIGPUSubmissionReceipt> Waits;
	};

	// RHI owns generation allocation so backend replacement cannot reuse an epoch.
	RHI_API auto AllocateRHIDeviceGeneration() -> uint64;

	// One backend writer serializes reservations and native submission on a queue.
	// Ticket observations are thread safe. Mutations must follow native acceptance;
	// cancel only after detaching every executable owner of the pending work.
	class FRHIGPUQueueTimeline final
	{
	public:
		RHI_API FRHIGPUQueueTimeline(uint64 DeviceGeneration, FRHIQueueId Queue);
		RHI_API ~FRHIGPUQueueTimeline();
		FRHIGPUQueueTimeline(const FRHIGPUQueueTimeline&) = delete;
		auto operator=(const FRHIGPUQueueTimeline&) -> FRHIGPUQueueTimeline& = delete;
		RHI_API auto Reserve() -> FRHIGPUSubmissionTicket;
		RHI_API auto CanSubmit(const FRHIGPUSubmissionTicket& Ticket) const -> bool;
		RHI_API auto MarkSubmitted(const FRHIGPUSubmissionTicket& Ticket) -> bool;
		RHI_API auto ObserveCompleted(const FRHIGPUSubmissionTicket& Ticket) -> bool;
		RHI_API auto Cancel(const FRHIGPUSubmissionTicket& Ticket) -> bool;
		// Failure closes admission; outstanding metadata becomes terminal without
		// granting normal retirement. Native owners require backend teardown.
		RHI_API auto Fail(bool bDeviceLost = false) -> void;
		RHI_API auto Owns(const FRHIGPUSubmissionTicket& Ticket) const -> bool;
		auto GetPendingCount() const -> size_t { return Pending.size(); }

	private:
		auto AdvanceRetirement() -> void;
		std::shared_ptr<FRHIGPUTimelineState> State;
		std::deque<std::shared_ptr<FRHIGPUTicketState>> Pending;
		uint64 NextValue = 1;
		bool bClosed = false;
	};

	// Conjunction of queue prefixes, with no numeric ordering across queues/epochs.
	class FRHIRetirementPrerequisites final
	{
	public:
		RHI_API auto Add(const FRHIGPUSubmissionTicket& Ticket) -> bool;
		RHI_API auto IsRetirementEligible() const -> bool;
		auto GetTickets() const -> std::span<const FRHIGPUSubmissionTicket> { return Tickets; }
	private:
		std::vector<FRHIGPUSubmissionTicket> Tickets;
	};
}
