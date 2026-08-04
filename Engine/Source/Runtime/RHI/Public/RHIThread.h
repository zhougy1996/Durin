#pragma once

#include "RHIAPI.h"

namespace Durin
{
	class FRunnableThread;

	enum class ERHIThreadAdmissionState : uint8
	{
		Stopped,
		Running,
		Draining,
	};

	enum class ERHIThreadEnqueueResult : uint8
	{
		Accepted,
		Stopped,
		Draining,
		Failed,
		Oversized,
		SerialExhausted,
		InvalidWork,
		SelfEnqueue,
	};

	enum class ERHIThreadWaitResult : uint8
	{
		Completed,
		Failed,
		Stopped,
		SelfWait,
	};

	struct FRHIThreadQueueLimits
	{
		uint32 MaxEntries = 8;
		uint32 MaxBatches = 16;
		uint64 MaxPayloadBytes = 32ull * 1024ull * 1024ull;
		uint32 ThreadStackSize = 0;
	};

	struct FRHIThreadWorkResult
	{
		bool bSucceeded = true;
		std::string Diagnostic;

		RHI_API static auto Success() -> FRHIThreadWorkResult;
		RHI_API static auto Failure(std::string InDiagnostic) -> FRHIThreadWorkResult;
	};

	struct FRHIThreadWork
	{
		FRHIThreadWork() = default;
		FRHIThreadWork(FRHIThreadWork&&) noexcept = default;
		auto operator=(FRHIThreadWork&&) noexcept -> FRHIThreadWork& = default;
		FRHIThreadWork(const FRHIThreadWork&) = delete;
		auto operator=(const FRHIThreadWork&) -> FRHIThreadWork& = delete;

		std::function<FRHIThreadWorkResult()> Execute;
		uint32 BatchCount = 0;
		uint64 PayloadBytes = 0;
	};

	struct FRHIThreadSubmission
	{
		ERHIThreadEnqueueResult Result = ERHIThreadEnqueueResult::Stopped;
		uint64 Serial = 0;

		auto IsAccepted() const -> bool
		{
			return Result == ERHIThreadEnqueueResult::Accepted;
		}
	};

	struct FRHIThreadSynchronousResult
	{
		FRHIThreadSubmission Submission;
		ERHIThreadWaitResult WaitResult = ERHIThreadWaitResult::Stopped;

		auto IsCompleted() const -> bool
		{
			return Submission.IsAccepted()
				&& WaitResult == ERHIThreadWaitResult::Completed;
		}
	};

	struct FRHIThreadStats
	{
		ERHIThreadAdmissionState AdmissionState = ERHIThreadAdmissionState::Stopped;
		uint64 LastSubmittedSerial = 0;
		uint64 CompletedSerial = 0;
		uint64 FailedSerial = 0;
		uint32 OutstandingEntryCount = 0;
		uint32 OutstandingBatchCount = 0;
		uint64 OutstandingPayloadBytes = 0;
		uint64 BackpressureWaitCount = 0;
		uint64 BackpressureWaitNanoseconds = 0;
		uint64 RejectedWorkCount = 0;
		std::string FailureDiagnostic;
	};

	// Owns the single FIFO consumer used by threaded RHI execution.
	class FRHIThread
	{
	public:
		RHI_API FRHIThread();
		RHI_API ~FRHIThread();

		FRHIThread(const FRHIThread&) = delete;
		auto operator=(const FRHIThread&) -> FRHIThread& = delete;

		RHI_API auto Start(const FRHIThreadQueueLimits& InLimits = {}) -> bool;
		RHI_API auto BeginDrain() -> void;
		RHI_API auto Stop() -> void;

		// On success this moves Work into the queue. Rejection leaves Work intact.
		RHI_API auto Enqueue(FRHIThreadWork& Work) -> FRHIThreadSubmission;
		RHI_API auto EnqueueSynchronous(FRHIThreadWork& Work)
			-> FRHIThreadSynchronousResult;
		RHI_API auto WaitForSerial(uint64 Serial) const -> ERHIThreadWaitResult;
		RHI_API auto Flush() const -> ERHIThreadWaitResult;

		RHI_API auto GetStats() const -> FRHIThreadStats;

	private:
		class FState;
		class FRunnableOwner;

		std::unique_ptr<FState> State;
		std::unique_ptr<FRunnableOwner> RunnableOwner;
		std::unique_ptr<FRunnableThread> Thread;
	};
} // namespace Durin
