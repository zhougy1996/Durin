#include "Backend/RHICompletionBackend.h"
#include "DynamicRHI.h"

namespace Durin
{
	// Shared RHI metadata has no backend vtable, native handle or device pointer.
	struct FRHIGPUTimelineState final
	{
		uint64 DeviceGeneration;
		FRHIQueueId Queue;
		std::atomic<uint64> RetiredThrough = 0;
	};

	struct FRHIGPUReservationState final
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

	auto FRHIGPUQueueTimeline::Reserve() -> FRHIGPUSyncPointRef
	{
		if (bClosed) return {};
		require(NextValue != UINT64_MAX);
		auto Signal = FRHIGPUSyncPoint::Create();
		auto Reservation = std::make_shared<FRHIGPUReservationState>();
		Reservation->Timeline = State;
		Reservation->Value = NextValue;
		Signal->Reservation = Reservation;
		Pending.push_back(std::move(Reservation));
		++NextValue;
		return Signal;
	}

	auto FRHIGPUQueueTimeline::Owns(const FRHIGPUSyncPointRef& SyncPoint) const -> bool
	{
		const auto Reservation = FRHIGPUSyncPointBackend::GetReservation(SyncPoint);
		return Reservation && Reservation->Timeline == State;
	}

	auto FRHIGPUQueueTimeline::CanSubmit(const FRHIGPUSyncPointRef& SyncPoint) const -> bool
	{
		return CanSubmitBatch(std::span{&SyncPoint, 1});
	}

	auto FRHIGPUQueueTimeline::CanSubmitBatch(std::span<const FRHIGPUSyncPointRef> SyncPoints) const -> bool
	{
		if (bClosed) return false;
		if (SyncPoints.empty()) return true;
		size_t Index = 0;
		for (const auto& Reservation : Pending)
		{
			if (Reservation->Status.load() != ERHIGPUSubmissionState::Pending) continue;
			if (Reservation != FRHIGPUSyncPointBackend::GetReservation(SyncPoints[Index])) return false;
			if (++Index == SyncPoints.size()) return true;
		}
		return false;
	}

	auto FRHIGPUQueueTimeline::MarkSubmitted(const FRHIGPUSyncPointRef& SyncPoint) -> bool
	{
		if (!CanSubmit(SyncPoint)) return false;
		FRHIGPUSyncPointBackend::GetReservation(SyncPoint)->Status.store(ERHIGPUSubmissionState::Submitted, std::memory_order_release);
		return true;
	}

	auto FRHIGPUQueueTimeline::ObserveCompleted(const FRHIGPUSyncPointRef& SyncPoint) -> bool
	{
		if (!Owns(SyncPoint)) return false;
		if (SyncPoint.GetState() == ERHIGPUSubmissionState::Complete) return true;
		if (bClosed || SyncPoint.GetState() != ERHIGPUSubmissionState::Submitted) return false;
		FRHIGPUSyncPointBackend::GetReservation(SyncPoint)->bObserved = true;
		AdvanceRetirement();
		return true;
	}

	auto FRHIGPUQueueTimeline::Cancel(const FRHIGPUSyncPointRef& SyncPoint) -> bool
	{
		if (bClosed || !Owns(SyncPoint) || SyncPoint.GetState() != ERHIGPUSubmissionState::Pending)
			return false;
		FRHIGPUSyncPointBackend::GetReservation(SyncPoint)->Status.store(ERHIGPUSubmissionState::Canceled, std::memory_order_release);
		AdvanceRetirement();
		return true;
	}

	auto FRHIGPUQueueTimeline::AdvanceRetirement() -> void
	{
		while (!Pending.empty())
		{
			const auto& SyncPoint = Pending.front();
			if (SyncPoint->Status.load() != ERHIGPUSubmissionState::Canceled && !SyncPoint->bObserved)
				break;
			if (SyncPoint->bObserved)
				SyncPoint->Status.store(ERHIGPUSubmissionState::Complete, std::memory_order_release);
			State->RetiredThrough.store(SyncPoint->Value, std::memory_order_release);
			Pending.pop_front();
		}
	}

	auto FRHIGPUQueueTimeline::Fail(bool bDeviceLost) -> void
	{
		bClosed = true;
		for (auto& SyncPoint : Pending)
			if (SyncPoint->Status.load() != ERHIGPUSubmissionState::Canceled)
				SyncPoint->Status.store(bDeviceLost ? ERHIGPUSubmissionState::DeviceLost
					: ERHIGPUSubmissionState::Failed, std::memory_order_release);
		Pending.clear();
	}


	auto FRHIGPUSyncPoint::Create() -> FRHIGPUSyncPointRef
	{ return FRHIGPUSyncPointRef(new FRHIGPUSyncPoint); }

	auto FRHIGPUSyncPoint::Release() const -> uint32
	{
		const auto Remaining = References.fetch_sub(1, std::memory_order_acq_rel) - 1;
		if (!Remaining) delete this;
		return Remaining;
	}

	auto FRHIGPUSyncPoint::GetState() const -> ERHIGPUSubmissionState
	{
		std::lock_guard Lock(Mutex);
		if (bCanceled) return ERHIGPUSubmissionState::Canceled;
		return Reservation ? Reservation->Status.load(std::memory_order_acquire)
			: ERHIGPUSubmissionState::Pending;
	}

	auto FRHIGPUSyncPoint::IsRetirementEligible() const -> bool
	{
		std::lock_guard Lock(Mutex);
		if (!Reservation) return bCanceled;
		const auto Status = Reservation->Status.load(std::memory_order_acquire);
		return (Status == ERHIGPUSubmissionState::Complete || Status == ERHIGPUSubmissionState::Canceled)
			&& Reservation->Timeline->RetiredThrough.load(std::memory_order_acquire) >= Reservation->Value;
	}

	auto FRHIGPUSyncPointBackend::GetReservation(const FRHIGPUSyncPointRef& Signal)
		-> std::shared_ptr<FRHIGPUReservationState>
	{
		if (!Signal) return {};
		std::lock_guard Lock(Signal->Mutex);
		return Signal->Reservation;
	}

	auto FRHIGPUSyncPointBackend::GetPoint(const FRHIGPUSyncPointRef& Signal) -> FRHIGPUCompletionPoint
	{
		const auto State = GetReservation(Signal);
		return State ? FRHIGPUCompletionPoint{State->Timeline->DeviceGeneration,
			State->Timeline->Queue, State->Value} : FRHIGPUCompletionPoint{};
	}

	auto FRHIGPUSyncPointBackend::Attach(const FRHIGPUSyncPointRef& Signal,
		const FRHIGPUSyncPointRef& Producer) -> bool
	{
		const auto State = GetReservation(Producer);
		if (!Signal || !State || State->Status.load() != ERHIGPUSubmissionState::Pending) return false;
		std::lock_guard Lock(Signal->Mutex);
		if (Signal->Reservation || Signal->bCanceled) return false;
		Signal->Reservation = State;
		return true;
	}

	auto FRHIGPUSyncPointBackend::CancelUnassociated(const FRHIGPUSyncPointRef& Signal) -> void
	{
		if (!Signal) return;
		std::lock_guard Lock(Signal->Mutex);
		if (!Signal->Reservation) Signal->bCanceled = true;
	}

	auto FRHIRetirementPrerequisites::Add(const FRHIGPUSyncPointRef& Signal) -> bool
	{
		if (!Signal) return false;
		const auto State = FRHIGPUSyncPointBackend::GetReservation(Signal);
		for (const auto& Existing : Signals)
		{
			if (Existing == Signal) return true;
			const auto Other = FRHIGPUSyncPointBackend::GetReservation(Existing);
			if (State && Other && State->Timeline->DeviceGeneration == Other->Timeline->DeviceGeneration
				&& State->Timeline->Queue == Other->Timeline->Queue && State->Timeline != Other->Timeline) return false;
		}
		std::erase_if(Signals, [](const auto& Existing) { return Existing.IsRetirementEligible(); });
		Signals.push_back(Signal);
		return true;
	}

	auto FRHIRetirementPrerequisites::IsRetirementEligible() const -> bool
	{ return std::ranges::all_of(Signals, &FRHIGPUSyncPointRef::IsRetirementEligible); }

	auto WaitForRHIGPUSyncPoint(const FRHIGPUSyncPointRef& Signal, uint64 TimeoutNanoseconds) -> ERHIGPUWaitResult
	{
		switch (Signal.GetState())
		{
		case ERHIGPUSubmissionState::Complete: return ERHIGPUWaitResult::Complete;
		case ERHIGPUSubmissionState::Pending: return ERHIGPUWaitResult::Pending;
		case ERHIGPUSubmissionState::Canceled: return ERHIGPUWaitResult::Canceled;
		case ERHIGPUSubmissionState::Failed: return ERHIGPUWaitResult::Failed;
		case ERHIGPUSubmissionState::DeviceLost: return ERHIGPUWaitResult::DeviceLost;
		case ERHIGPUSubmissionState::Invalid: return ERHIGPUWaitResult::Invalid;
		case ERHIGPUSubmissionState::Submitted: break;
		}
		return GDynamicRHI ? GDynamicRHI->RHIWaitForCompletion(Signal, TimeoutNanoseconds) : ERHIGPUWaitResult::Invalid;
	}
}
