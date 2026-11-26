#pragma once

#include "CoreMinimal.h"

#include "RHIAPI.h"

namespace Durin
{
	class FRunnableThread;

	// Backend execution affinity, independent of command replay scope.
	// With a dedicated RHI owner, only that exact thread may execute. Without
	// one, execution belongs to the rendering thread, or the registered game
	// thread when no rendering thread exists. This is an affinity check, not
	// synchronization: rendering-thread start/stop and command fences order work.
	// Thread-owner publication/removal must be synchronized with these calls.
	RHI_API auto IsInRHIExecutionThread() -> bool;
	RHI_API auto CheckRHIExecutionThread() -> void;

	enum class ERHIThreadAdmissionState : uint8
	{
		Stopped,
		Running,
		Draining,
	};

	struct FRHIThreadQueueLimits
	{
		uint32 ThreadStackSize = 0;
		uint32 MaxFrames = 3;
	};

	struct FRHIThreadWork
	{
		FRHIThreadWork() = default;
		FRHIThreadWork(FRHIThreadWork&&) noexcept = default;
		auto operator=(FRHIThreadWork&&) noexcept -> FRHIThreadWork& = default;
		FRHIThreadWork(const FRHIThreadWork&) = delete;
		auto operator=(const FRHIThreadWork&) -> FRHIThreadWork& = delete;

		// Exceptions escaping work are fatal; the queue does not translate them.
		std::function<void()> Execute;
		uint32 BatchCount = 0;
		uint64 PayloadBytes = 0;
		uint32 FrameCount = 0;
	};

	struct FRHIThreadStats
	{
		ERHIThreadAdmissionState AdmissionState = ERHIThreadAdmissionState::Stopped;
		uint64 LastSubmittedSerial = 0;
		uint64 CompletedSerial = 0;
		uint64 OutstandingEntryCount = 0;
		uint64 OutstandingBatchCount = 0;
		uint32 OutstandingFrameCount = 0;
		uint64 OutstandingPayloadBytes = 0;
		uint64 PeakOutstandingEntryCount = 0;
		uint64 PeakOutstandingBatchCount = 0;
		uint32 PeakOutstandingFrameCount = 0;
		uint64 PeakOutstandingPayloadBytes = 0;
		uint64 BackpressureWaitCount = 0;
		uint64 BackpressureWaitNanoseconds = 0;
		uint64 FramePressureWaitCount = 0;
		uint64 FramePressureWaitNanoseconds = 0;
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
		// Closes admission but drains every accepted entry through execution and capture retirement.
		RHI_API auto BeginDrain() -> void;
		RHI_API auto Stop() -> void;

		// Transfers work to the running queue and returns its nonzero submission serial.
		// Invalid work, self-enqueue, and serial overflow are programming errors.
		RHI_API auto Enqueue(FRHIThreadWork&& Work) -> uint64;
		// Returns after execution and capture retirement.
		RHI_API auto EnqueueSynchronous(FRHIThreadWork&& Work) -> void;
		// Serial must be zero or an accepted submission from this queue.
		RHI_API auto WaitForSerial(uint64 Serial) const -> void;
		RHI_API auto Flush() const -> void;
		RHI_API auto CaptureLastSubmittedSerial() const -> uint64;

		RHI_API auto GetStats() const -> FRHIThreadStats;

	private:
		class FState;
		class FRunnableOwner;

		std::unique_ptr<FState> State;
		std::unique_ptr<FRunnableOwner> RunnableOwner;
		std::unique_ptr<FRunnableThread> Thread;
	};
} // namespace Durin
