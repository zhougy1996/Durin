#include "RHIThread.h"

#include "CoreGlobals.h"
#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	struct FRHIThreadQueueEntry
	{
		uint64 Serial = 0;
		FRHIThreadWork Work;
	};

	class FRHIThread::FState
	{
	public:
		FRHIThreadQueueLimits Limits;
		ERHIThreadAdmissionState AdmissionState = ERHIThreadAdmissionState::Stopped;
		std::deque<FRHIThreadQueueEntry> Queue;
		uint64 LastSubmittedSerial = 0;
		uint64 CompletedSerial = 0;
		uint64 FailedSerial = 0;
		uint32 OutstandingEntryCount = 0;
		uint32 OutstandingBatchCount = 0;
		uint64 OutstandingPayloadBytes = 0;
		uint32 PeakOutstandingEntryCount = 0;
		uint32 PeakOutstandingBatchCount = 0;
		uint64 PeakOutstandingPayloadBytes = 0;
		uint64 BackpressureWaitCount = 0;
		uint64 BackpressureWaitNanoseconds = 0;
		uint64 RejectedWorkCount = 0;
		std::string FailureDiagnostic;
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
					State.WorkCV.wait(Lock, [this]() {
						return !State.Queue.empty()
							|| State.AdmissionState != ERHIThreadAdmissionState::Running;
					});
					if (State.Queue.empty())
					{
						break;
					}
					Entry = std::move(State.Queue.front());
					State.Queue.pop_front();
				}

				FRHIThreadWorkResult Result;
				try
				{
					Result = Entry.Work.Execute();
				}
				catch (const std::exception& Exception)
				{
					Result = FRHIThreadWorkResult::Failure(Exception.what());
				}
				catch (...)
				{
					Result = FRHIThreadWorkResult::Failure(
						"RHI thread work failed with an unknown exception.");
				}

				const uint32 CompletedBatchCount = Entry.Work.BatchCount;
				const uint64 CompletedPayloadBytes = Entry.Work.PayloadBytes;
				Entry.Work = {};
				std::deque<FRHIThreadQueueEntry> RejectedEntries;
				{
					std::lock_guard Lock(State.Mutex);
					ReleaseCapacity(CompletedBatchCount, CompletedPayloadBytes);
					if (Result.bSucceeded)
					{
						State.CompletedSerial = Entry.Serial;
					}
					else
					{
						State.FailedSerial = Entry.Serial;
						State.FailureDiagnostic = std::move(Result.Diagnostic);
						State.AdmissionState = ERHIThreadAdmissionState::Draining;
						RejectedEntries = std::move(State.Queue);
						for (const FRHIThreadQueueEntry& RejectedEntry : RejectedEntries)
						{
							ReleaseCapacity(
								RejectedEntry.Work.BatchCount,
								RejectedEntry.Work.PayloadBytes);
						}
						State.RejectedWorkCount += RejectedEntries.size();
					}
				}
				RejectedEntries.clear();
				State.CompletionCV.notify_all();
				State.WorkCV.notify_all();
				if (!Result.bSucceeded)
				{
					break;
				}
			}

			{
				std::lock_guard Lock(State.Mutex);
				State.AdmissionState = ERHIThreadAdmissionState::Stopped;
				State.bConsumerReady = false;
				State.CompletionCV.notify_all();
				State.WorkCV.notify_all();
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
		auto ReleaseCapacity(uint32 BatchCount, uint64 PayloadBytes) -> void
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

	auto FRHIThreadWorkResult::Success() -> FRHIThreadWorkResult
	{
		return {};
	}

	auto FRHIThreadWorkResult::Failure(std::string InDiagnostic)
		-> FRHIThreadWorkResult
	{
		return {
			.bSucceeded = false,
			.Diagnostic = std::move(InDiagnostic)
		};
	}

	FRHIThread::FRHIThread()
		: State(std::make_unique<FState>())
	{
	}

	FRHIThread::~FRHIThread()
	{
		Stop();
	}

	auto FRHIThread::Start(const FRHIThreadQueueLimits& InLimits) -> bool
	{
		check(InLimits.MaxEntries > 0);
		check(InLimits.MaxBatches > 0);
		check(InLimits.MaxPayloadBytes > 0);
		{
			std::lock_guard Lock(State->Mutex);
			if (State->AdmissionState != ERHIThreadAdmissionState::Stopped
				|| Thread || GRHIThread)
			{
				return false;
			}
			State->Limits = InLimits;
			State->AdmissionState = ERHIThreadAdmissionState::Running;
			State->LastSubmittedSerial = 0;
			State->CompletedSerial = 0;
			State->FailedSerial = 0;
			State->OutstandingEntryCount = 0;
			State->OutstandingBatchCount = 0;
			State->OutstandingPayloadBytes = 0;
			State->PeakOutstandingEntryCount = 0;
			State->PeakOutstandingBatchCount = 0;
			State->PeakOutstandingPayloadBytes = 0;
			State->BackpressureWaitCount = 0;
			State->BackpressureWaitNanoseconds = 0;
			State->RejectedWorkCount = 0;
			State->FailureDiagnostic.clear();
			State->bConsumerReady = false;
		}

		RunnableOwner = std::make_unique<FRunnableOwner>(*State);
		Thread.reset(FRunnableThread::Create(
			RunnableOwner.get(), "RHIThread", InLimits.ThreadStackSize,
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

	auto FRHIThread::Enqueue(FRHIThreadWork& Work) -> FRHIThreadSubmission
	{
		if (IsInRHIThread())
		{
			std::lock_guard Lock(State->Mutex);
			++State->RejectedWorkCount;
			return {.Result = ERHIThreadEnqueueResult::SelfEnqueue};
		}
		if (!Work.Execute)
		{
			std::lock_guard Lock(State->Mutex);
			++State->RejectedWorkCount;
			return {.Result = ERHIThreadEnqueueResult::InvalidWork};
		}

		std::unique_lock Lock(State->Mutex);
		if (Work.BatchCount > State->Limits.MaxBatches
			|| Work.PayloadBytes > State->Limits.MaxPayloadBytes)
		{
			++State->RejectedWorkCount;
			return {.Result = ERHIThreadEnqueueResult::Oversized};
		}

		auto GetAdmissionFailure = [this]() -> ERHIThreadEnqueueResult {
			if (State->FailedSerial != 0)
			{
				return ERHIThreadEnqueueResult::Failed;
			}
			return State->AdmissionState == ERHIThreadAdmissionState::Draining
				? ERHIThreadEnqueueResult::Draining
				: ERHIThreadEnqueueResult::Stopped;
		};
		if (State->AdmissionState != ERHIThreadAdmissionState::Running)
		{
			++State->RejectedWorkCount;
			return {.Result = GetAdmissionFailure()};
		}

		auto HasCapacity = [this, &Work]() {
			return State->OutstandingEntryCount < State->Limits.MaxEntries
				&& State->OutstandingBatchCount + Work.BatchCount
					<= State->Limits.MaxBatches
				&& State->OutstandingPayloadBytes + Work.PayloadBytes
					<= State->Limits.MaxPayloadBytes;
		};
		if (!HasCapacity())
		{
			const auto WaitStart = std::chrono::steady_clock::now();
			++State->BackpressureWaitCount;
			State->CompletionCV.wait(Lock, [this, &HasCapacity]() {
				return HasCapacity()
					|| State->AdmissionState != ERHIThreadAdmissionState::Running;
			});
			State->BackpressureWaitNanoseconds += static_cast<uint64>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - WaitStart).count());
		}
		if (State->AdmissionState != ERHIThreadAdmissionState::Running)
		{
			++State->RejectedWorkCount;
			return {.Result = GetAdmissionFailure()};
		}
		if (State->LastSubmittedSerial == std::numeric_limits<uint64>::max())
		{
			++State->RejectedWorkCount;
			return {.Result = ERHIThreadEnqueueResult::SerialExhausted};
		}

		const uint64 Serial = ++State->LastSubmittedSerial;
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
		State->WorkCV.notify_one();
		return {
			.Result = ERHIThreadEnqueueResult::Accepted,
			.Serial = Serial
		};
	}

	auto FRHIThread::EnqueueTerminal(FRHIThreadWork& Work)
		-> FRHIThreadSubmission
	{
		if (IsInRHIThread())
		{
			std::lock_guard Lock(State->Mutex);
			++State->RejectedWorkCount;
			return {.Result = ERHIThreadEnqueueResult::SelfEnqueue};
		}
		if (!Work.Execute || Work.BatchCount != 0 || Work.PayloadBytes != 0)
		{
			std::lock_guard Lock(State->Mutex);
			++State->RejectedWorkCount;
			return {.Result = ERHIThreadEnqueueResult::InvalidWork};
		}

		std::lock_guard Lock(State->Mutex);
		if (State->FailedSerial != 0)
		{
			++State->RejectedWorkCount;
			return {.Result = ERHIThreadEnqueueResult::Failed};
		}
		if (State->AdmissionState != ERHIThreadAdmissionState::Running)
		{
			++State->RejectedWorkCount;
			return {
				.Result = State->AdmissionState == ERHIThreadAdmissionState::Draining
					? ERHIThreadEnqueueResult::Draining
					: ERHIThreadEnqueueResult::Stopped
			};
		}
		if (State->LastSubmittedSerial == std::numeric_limits<uint64>::max())
		{
			++State->RejectedWorkCount;
			return {.Result = ERHIThreadEnqueueResult::SerialExhausted};
		}

		State->AdmissionState = ERHIThreadAdmissionState::Draining;
		const uint64 Serial = ++State->LastSubmittedSerial;
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
		State->WorkCV.notify_all();
		State->CompletionCV.notify_all();
		return {
			.Result = ERHIThreadEnqueueResult::Accepted,
			.Serial = Serial
		};
	}

	auto FRHIThread::EnqueueSynchronous(FRHIThreadWork& Work)
		-> FRHIThreadSynchronousResult
	{
		FRHIThreadSynchronousResult Result;
		Result.Submission = Enqueue(Work);
		if (Result.Submission.IsAccepted())
		{
			Result.WaitResult = WaitForSerial(Result.Submission.Serial);
		}
		return Result;
	}

	auto FRHIThread::WaitForSerial(uint64 Serial) const -> ERHIThreadWaitResult
	{
		if (IsInRHIThread())
		{
			return ERHIThreadWaitResult::SelfWait;
		}
		if (Serial == 0)
		{
			return ERHIThreadWaitResult::Completed;
		}
		std::unique_lock Lock(State->Mutex);
		State->CompletionCV.wait(Lock, [this, Serial]() {
			return State->CompletedSerial >= Serial
				|| State->FailedSerial != 0
				|| State->AdmissionState == ERHIThreadAdmissionState::Stopped;
		});
		if (State->CompletedSerial >= Serial)
		{
			return ERHIThreadWaitResult::Completed;
		}
		if (State->FailedSerial != 0)
		{
			return ERHIThreadWaitResult::Failed;
		}
		return ERHIThreadWaitResult::Stopped;
	}

	auto FRHIThread::Flush() const -> ERHIThreadWaitResult
	{
		return WaitForSerial(CaptureLastSubmittedSerial());
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
			.FailedSerial = State->FailedSerial,
			.OutstandingEntryCount = State->OutstandingEntryCount,
			.OutstandingBatchCount = State->OutstandingBatchCount,
			.OutstandingPayloadBytes = State->OutstandingPayloadBytes,
			.PeakOutstandingEntryCount = State->PeakOutstandingEntryCount,
			.PeakOutstandingBatchCount = State->PeakOutstandingBatchCount,
			.PeakOutstandingPayloadBytes = State->PeakOutstandingPayloadBytes,
			.BackpressureWaitCount = State->BackpressureWaitCount,
			.BackpressureWaitNanoseconds = State->BackpressureWaitNanoseconds,
			.RejectedWorkCount = State->RejectedWorkCount,
			.FailureDiagnostic = State->FailureDiagnostic,
		};
	}
} // namespace Durin
