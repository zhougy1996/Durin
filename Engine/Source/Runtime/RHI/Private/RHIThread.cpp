#include "Backend/RHICreation.h"
#include "RHIThread.h"

#include "CoreGlobals.h"
#include "Profiling/Profiling.h"
#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	auto IsInRHIExecutionThread() -> bool
	{
		if (GRHIThread) return GetCurrentThread() == GRHIThread;
		return IsInRenderingThread()
			|| (!GRenderingThread && GIsGameThreadIdInitialized && IsInGameThread());
	}

	auto CheckRHIExecutionThread() -> void
	{
		checkf(IsInRHIExecutionThread(),
			"Expected RHI execution thread (dedicated owner, rendering thread, or game thread without a rendering thread), current thread is {}.",
			GetCurrentThreadName());
	}

	struct FRHIThreadQueueEntry
	{
		uint64 Serial = 0;
		FRHIThreadWork Work;
	};

	class FRHIThread::FState
	{
	public:
		bool bStoppedPermanently = false;
		ERHIThreadAdmissionState AdmissionState = ERHIThreadAdmissionState::Stopped;
		std::deque<FRHIThreadQueueEntry> Queue;
		uint64 LastSubmittedSerial = 0;
		uint64 CompletedSerial = 0;
		uint64 OutstandingEntryCount = 0;
		uint64 OutstandingBatchCount = 0;
		uint64 OutstandingPayloadBytes = 0;
		uint64 PeakOutstandingEntryCount = 0;
		uint64 PeakOutstandingBatchCount = 0;
		uint64 PeakOutstandingPayloadBytes = 0;
		bool bConsumerReady = false;
		mutable std::mutex Mutex;
		mutable std::condition_variable WorkCV;
		mutable std::condition_variable CompletionCV;
	};

	class FRHIThread::FRunnableOwner final : public FRunnable
	{
	public:
		explicit FRunnableOwner(FState& InState)
			: State(InState)
		{
		}

		auto Init() -> bool override
		{
			std::lock_guard Lock(State.Mutex);
			State.bConsumerReady = true;
			State.CompletionCV.notify_all();
			return true;
		}

		auto Run() -> uint32 override
		{
			while (true)
			{
				FRHIThreadQueueEntry Entry;
				{
					std::unique_lock Lock(State.Mutex);
					while (State.Queue.empty() && State.AdmissionState == ERHIThreadAdmissionState::Running)
					{
						DURIN_PROFILE_CPU_ZONE_NAMED("RHI.Consumer.IdleWait");
						State.WorkCV.wait(Lock);
					}
					if (State.Queue.empty())
					{
						break;
					}
					Entry = std::move(State.Queue.front());
					State.Queue.pop_front();
				}

				{
					DURIN_PROFILE_CPU_ZONE_NAMED("RHI.Consumer.Execute");
					DURIN_PROFILE_CPU_ZONE_TEXT(std::format("serial={} batches={} payloadBytes={}", Entry.Serial, Entry.Work.BatchCount, Entry.Work.PayloadBytes));
					Entry.Work.Execute();
				}
				const uint32 CompletedBatchCount = Entry.Work.BatchCount;
				const uint64 CompletedPayloadBytes = Entry.Work.PayloadBytes;
				{
					DURIN_PROFILE_CPU_ZONE_NAMED("RHI.Consumer.ReleaseWork");
					Entry.Work = {};
				}
				{
					std::lock_guard Lock(State.Mutex);
					RetireWork(CompletedBatchCount, CompletedPayloadBytes);
					State.CompletedSerial = Entry.Serial;
				}
				State.CompletionCV.notify_all();
			}

			{
				std::lock_guard Lock(State.Mutex);
				State.AdmissionState = ERHIThreadAdmissionState::Stopped;
				State.bConsumerReady = false;
				State.CompletionCV.notify_all();
			}
			return 0;
		}

		auto Stop() -> void override
		{
			std::lock_guard Lock(State.Mutex);
			if (State.AdmissionState == ERHIThreadAdmissionState::Running)
			{
				State.AdmissionState = ERHIThreadAdmissionState::Draining;
			}
			State.WorkCV.notify_all();
			State.CompletionCV.notify_all();
		}

	private:
		auto RetireWork(uint32 BatchCount, uint64 PayloadBytes) -> void
		{
			check(State.OutstandingEntryCount > 0);
			check(State.OutstandingBatchCount >= BatchCount);
			check(State.OutstandingPayloadBytes >= PayloadBytes);
			--State.OutstandingEntryCount;
			State.OutstandingBatchCount -= BatchCount;
			State.OutstandingPayloadBytes -= PayloadBytes;
		}

		FState& State;
	};

	FRHIThread::FRHIThread()
		: State(std::make_unique<FState>())
	{
	}

	FRHIThread::~FRHIThread()
	{
		Stop();
	}

	auto FRHIThread::Start(uint32 ThreadStackSize) -> bool
	{
		{
			std::lock_guard Lock(State->Mutex);
			if (State->AdmissionState != ERHIThreadAdmissionState::Stopped
				|| State->bStoppedPermanently || Thread || GRHIThread)
			{
				return false;
			}
			State->AdmissionState = ERHIThreadAdmissionState::Running;
			State->LastSubmittedSerial = 0;
			State->CompletedSerial = 0;
			State->OutstandingEntryCount = 0;
			State->OutstandingBatchCount = 0;
			State->OutstandingPayloadBytes = 0;
			State->PeakOutstandingEntryCount = 0;
			State->PeakOutstandingBatchCount = 0;
			State->PeakOutstandingPayloadBytes = 0;
			State->bConsumerReady = false;
		}

		RunnableOwner = std::make_unique<FRunnableOwner>(*State);
		Thread.reset(FRunnableThread::Create(
			RunnableOwner.get(), "RHIThread", ThreadStackSize,
			EThreadPriority::Normal, EThreadRole::RHIThread));
		if (!Thread)
		{
			std::lock_guard Lock(State->Mutex);
			State->AdmissionState = ERHIThreadAdmissionState::Stopped;
			RunnableOwner.reset();
			State->CompletionCV.notify_all();
			return false;
		}
		GRHIThread = Thread.get();

		std::unique_lock Lock(State->Mutex);
		State->CompletionCV.wait(Lock, [this]() {
			return State->bConsumerReady
				|| State->AdmissionState == ERHIThreadAdmissionState::Stopped;
		});
		return State->bConsumerReady;
	}

	auto FRHIThread::BeginDrain() -> void
	{
		std::lock_guard Lock(State->Mutex);
		if (State->AdmissionState == ERHIThreadAdmissionState::Running)
		{
			State->AdmissionState = ERHIThreadAdmissionState::Draining;
		}
		State->WorkCV.notify_all();
		State->CompletionCV.notify_all();
	}

	auto FRHIThread::Stop() -> void
	{
		{
			std::lock_guard Lock(State->Mutex);
			State->bStoppedPermanently = true;
		}
		BeginDrain();
		if (Thread)
		{
			Thread->WaitForCompletion();
			if (GRHIThread == Thread.get())
			{
				GRHIThread = nullptr;
			}
			Thread.reset();
		}
		RunnableOwner.reset();
	}

	auto FRHIThread::Enqueue(FRHIThreadWork&& Work) -> uint64
	{
		if (IsInRHIThread())
		{
			RHITerminateOnFailure("RHI queue", "SelfEnqueue");
		}
		if (!Work.Execute)
		{
			RHITerminateOnFailure("RHI queue", "InvalidWork");
		}

		std::lock_guard Lock(State->Mutex);
		if (State->AdmissionState != ERHIThreadAdmissionState::Running)
		{
			RHITerminateOnFailure("RHI queue", "RHI thread is closed");
		}
		if (State->LastSubmittedSerial == std::numeric_limits<uint64>::max())
		{
			RHITerminateOnFailure("RHI queue", "SerialExhausted");
		}

		DURIN_PROFILE_CPU_ZONE_NAMED("RHI.Enqueue.Admit");
		require(Work.PayloadBytes <= std::numeric_limits<uint64>::max() - State->OutstandingPayloadBytes);
		require(Work.BatchCount <= std::numeric_limits<uint64>::max() - State->OutstandingBatchCount);
		const uint64 Serial = ++State->LastSubmittedSerial;
		DURIN_PROFILE_CPU_ZONE_TEXT(std::format("serial={} queuedEntries={} batches={} payloadBytes={}", Serial, State->Queue.size(), Work.BatchCount, Work.PayloadBytes));
		++State->OutstandingEntryCount;
		State->OutstandingBatchCount += Work.BatchCount;
		State->OutstandingPayloadBytes += Work.PayloadBytes;
		State->PeakOutstandingEntryCount = std::max(
			State->PeakOutstandingEntryCount, State->OutstandingEntryCount);
		State->PeakOutstandingBatchCount = std::max(
			State->PeakOutstandingBatchCount, State->OutstandingBatchCount);
		State->PeakOutstandingPayloadBytes = std::max(
			State->PeakOutstandingPayloadBytes, State->OutstandingPayloadBytes);
		State->Queue.push_back({Serial, std::move(Work)});
		// std::function's moved-from state is valid but otherwise unspecified.
		// Make accepted submission ownership explicit on every standard library.
		Work = {};
		State->WorkCV.notify_one();
		return Serial;
	}

	auto FRHIThread::EnqueueSynchronous(FRHIThreadWork&& Work) -> void
	{
		WaitForSerial(Enqueue(std::move(Work)));
	}

	auto FRHIThread::WaitForSerial(uint64 Serial) const -> void
	{
		if (IsInRHIThread())
		{
			RHITerminateOnFailure("RHI queue", "SelfWait");
		}
		if (Serial == 0) return;
		std::unique_lock Lock(State->Mutex);
		if (Serial > State->LastSubmittedSerial)
			RHITerminateOnFailure("RHI queue", "Wait for unsubmitted serial");
		State->CompletionCV.wait(Lock, [this, Serial]() {
			return State->CompletedSerial >= Serial;
		});
	}

	auto FRHIThread::Flush() const -> void
	{
		WaitForSerial(CaptureLastSubmittedSerial());
	}

	auto FRHIThread::CaptureLastSubmittedSerial() const -> uint64
	{
		std::lock_guard Lock(State->Mutex);
		return State->LastSubmittedSerial;
	}

	auto FRHIThread::GetStats() const -> FRHIThreadStats
	{
		std::lock_guard Lock(State->Mutex);
		return {
			.AdmissionState = State->AdmissionState,
			.LastSubmittedSerial = State->LastSubmittedSerial,
			.CompletedSerial = State->CompletedSerial,
			.OutstandingEntryCount = State->OutstandingEntryCount,
			.OutstandingBatchCount = State->OutstandingBatchCount,
			.OutstandingPayloadBytes = State->OutstandingPayloadBytes,
			.PeakOutstandingEntryCount = State->PeakOutstandingEntryCount,
			.PeakOutstandingBatchCount = State->PeakOutstandingBatchCount,
			.PeakOutstandingPayloadBytes = State->PeakOutstandingPayloadBytes,
		};
	}
} // namespace Durin
