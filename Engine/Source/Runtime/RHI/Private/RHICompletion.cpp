#include "RHICompletion.h"

namespace Durin
{
	// Shared RHI metadata has no backend vtable, native handle or device pointer.
	struct FRHIGPUTimelineState final
	{
		uint64 DeviceGeneration;
		FRHIQueueId Queue;
		std::atomic<uint64> RetiredThrough = 0;
	};

	struct FRHIGPUTicketState final
	{
		std::shared_ptr<FRHIGPUTimelineState> Timeline;
		uint64 Value;
		std::atomic<ERHIGPUSubmissionState> Status = ERHIGPUSubmissionState::Pending;
		bool bObserved = false;
	};

	auto AllocateRHIDeviceGeneration() -> uint64
	{
		static std::atomic<uint64> Next = 1;
		uint64 Value = Next.load();
		for (;;)
		{
			requiref(Value != UINT64_MAX, "RHI device generation space exhausted.");
			if (Next.compare_exchange_weak(Value, Value + 1)) return Value;
		}
	}

	auto FRHIGPUSubmissionTicket::GetPoint() const -> FRHIGPUCompletionPoint
	{
		return State ? FRHIGPUCompletionPoint{
			State->Timeline->DeviceGeneration, State->Timeline->Queue, State->Value}
			: FRHIGPUCompletionPoint{};
	}

	auto FRHIGPUSubmissionTicket::GetState() const -> ERHIGPUSubmissionState
	{
		return State ? State->Status.load(std::memory_order_acquire)
			: ERHIGPUSubmissionState::Invalid;
	}

	auto FRHIGPUSubmissionTicket::IsRetirementEligible() const -> bool
	{
		const auto Status = GetState();
		return (Status == ERHIGPUSubmissionState::Complete
			|| Status == ERHIGPUSubmissionState::Canceled)
			&& State->Timeline->RetiredThrough.load(std::memory_order_acquire) >= State->Value;
	}

	FRHIGPUQueueTimeline::FRHIGPUQueueTimeline(uint64 DeviceGeneration, FRHIQueueId Queue)
		: State(std::make_shared<FRHIGPUTimelineState>())
	{
		require(DeviceGeneration != 0);
		State->DeviceGeneration = DeviceGeneration;
		State->Queue = Queue;
	}

	FRHIGPUQueueTimeline::~FRHIGPUQueueTimeline()
	{
		// Destruction is not completion evidence, including for abandoned recordings.
		Fail();
	}

	auto FRHIGPUQueueTimeline::Reserve() -> FRHIGPUSubmissionTicket
	{
		if (bClosed) return {};
		require(NextValue != UINT64_MAX);
		auto Ticket = std::make_shared<FRHIGPUTicketState>();
		Ticket->Timeline = State;
		Ticket->Value = NextValue;
		Pending.push_back(Ticket);
		++NextValue;
		return FRHIGPUSubmissionTicket(std::move(Ticket));
	}

	auto FRHIGPUQueueTimeline::Owns(const FRHIGPUSubmissionTicket& Ticket) const -> bool
	{
		return Ticket.State && Ticket.State->Timeline == State;
	}

	auto FRHIGPUQueueTimeline::CanSubmit(const FRHIGPUSubmissionTicket& Ticket) const -> bool
	{
		if (bClosed || !Owns(Ticket) || Ticket.GetState() != ERHIGPUSubmissionState::Pending)
			return false;
		for (const auto& Earlier : Pending)
		{
			if (Earlier == Ticket.State) return true;
			if (Earlier->Status.load() == ERHIGPUSubmissionState::Pending) return false;
		}
		return false;
	}

	auto FRHIGPUQueueTimeline::MarkSubmitted(const FRHIGPUSubmissionTicket& Ticket) -> bool
	{
		if (!CanSubmit(Ticket)) return false;
		Ticket.State->Status.store(ERHIGPUSubmissionState::Submitted, std::memory_order_release);
		return true;
	}

	auto FRHIGPUQueueTimeline::ObserveCompleted(const FRHIGPUSubmissionTicket& Ticket) -> bool
	{
		if (!Owns(Ticket)) return false;
		if (Ticket.GetState() == ERHIGPUSubmissionState::Complete) return true;
		if (bClosed || Ticket.GetState() != ERHIGPUSubmissionState::Submitted) return false;
		Ticket.State->bObserved = true;
		AdvanceRetirement();
		return true;
	}

	auto FRHIGPUQueueTimeline::Cancel(const FRHIGPUSubmissionTicket& Ticket) -> bool
	{
		if (bClosed || !Owns(Ticket) || Ticket.GetState() != ERHIGPUSubmissionState::Pending)
			return false;
		Ticket.State->Status.store(ERHIGPUSubmissionState::Canceled, std::memory_order_release);
		AdvanceRetirement();
		return true;
	}

	auto FRHIGPUQueueTimeline::AdvanceRetirement() -> void
	{
		while (!Pending.empty())
		{
			const auto& Ticket = Pending.front();
			if (Ticket->Status.load() != ERHIGPUSubmissionState::Canceled && !Ticket->bObserved)
				break;
			if (Ticket->bObserved)
				Ticket->Status.store(ERHIGPUSubmissionState::Complete, std::memory_order_release);
			State->RetiredThrough.store(Ticket->Value, std::memory_order_release);
			Pending.pop_front();
		}
	}

	auto FRHIGPUQueueTimeline::Fail(bool bDeviceLost) -> void
	{
		bClosed = true;
		for (auto& Ticket : Pending)
			if (Ticket->Status.load() != ERHIGPUSubmissionState::Canceled)
				Ticket->Status.store(bDeviceLost ? ERHIGPUSubmissionState::DeviceLost
					: ERHIGPUSubmissionState::Failed, std::memory_order_release);
		Pending.clear();
	}

	auto FRHIRetirementPrerequisites::Add(const FRHIGPUSubmissionTicket& Ticket) -> bool
	{
		if (Ticket.GetState() == ERHIGPUSubmissionState::Invalid) return false;
		const auto Point = Ticket.GetPoint();
		for (auto& Existing : Tickets)
		{
			const auto Other = Existing.GetPoint();
			if (Point.DeviceGeneration == Other.DeviceGeneration && Point.Queue == Other.Queue)
			{
				if (Ticket.State->Timeline != Existing.State->Timeline) return false;
				if (Point.Value > Other.Value) Existing = Ticket;
				return true;
			}
		}
		Tickets.push_back(Ticket);
		return true;
	}

	auto FRHIRetirementPrerequisites::IsRetirementEligible() const -> bool
	{
		return std::ranges::all_of(Tickets, &FRHIGPUSubmissionTicket::IsRetirementEligible);
	}
}
