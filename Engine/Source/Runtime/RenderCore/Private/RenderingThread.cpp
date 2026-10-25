#include "RenderingThread.h"

#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"
#include "Threading/TaskComposition.h"

#include "RHICommandList.h"
#include "RenderResource.h"

namespace Durin
{
	FRunnable* GRenderingThreadRunnable = nullptr;

	namespace
	{
		struct FFenceRenderCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "FenceCommand";
			}
		};

		auto CheckRenderingThreadShutdown() -> void
		{
			const bool bResourcesReleased =
				ValidateRenderResourceShutdown_RenderThread();
			FRHICommandListImmediate::Get().ImmediateFlush(
				EImmediateFlushType::FlushRHIThreadFlushResources,
				ERHISubmitFlags::FlushRHIThread);
			const FRHICommandListExecutorStats ExecutorStats =
				GCommandListExecutor.GetStats();
			const char* ExecutorMode = ExecutorStats.Mode
				== ERHICommandListExecutorMode::Threaded ? "threaded" : "inline";
			DURIN_DEBUG(
				"RHI executor drained (mode: {}): {} command(s), {} payload byte(s), "
				"{} batch(es), {} submission group(s), {} ns replay, {} wait(s), "
				"{} synchronous operation(s), "
				"{} ns wait duration, peak queue {} entry(s)/{} batch(es)/{} byte(s), "
				"{} backpressure event(s), {} rejection(s).",
				ExecutorMode,
				ExecutorStats.RecordedCommandCount,
				ExecutorStats.RecordedPayloadBytes,
				ExecutorStats.SubmittedBatchCount,
				ExecutorStats.SubmissionGroupCount,
				ExecutorStats.ReplayDurationNanoseconds,
				ExecutorStats.WaitCount,
				ExecutorStats.SynchronousOperationCount,
				ExecutorStats.WaitDurationNanoseconds,
				ExecutorStats.PeakQueueEntryCount,
				ExecutorStats.PeakQueueBatchCount,
				ExecutorStats.PeakQueuePayloadBytes,
				ExecutorStats.BackpressureWaitCount,
				ExecutorStats.RejectedSubmissionCount);
			const auto UploadStats = GetBufferUploadStats();
			DURIN_DEBUG("RHI upload/frame bounds: peak queued frames={}, frame-pressure waits={} ({} ns), "
				"CPU upload live/peak={}/{}, backing reserved live/peak/capacity={}/{}/{}.",
				ExecutorStats.PeakQueueFrameCount, ExecutorStats.FramePressureWaitCount,
				ExecutorStats.FramePressureWaitNanoseconds, UploadStats.LiveBytes, UploadStats.PeakBytes,
				UploadStats.BackingLiveBytes, UploadStats.BackingPeakBytes,
				UploadStats.BackingReservedCapacity);
			const size_t PendingRHIDeletes =
				FRHIResource::GetNumPendingDeletes();
			if (PendingRHIDeletes != 0)
			{
				DURIN_ERROR(
					"Rendering thread shutdown left {} RHI resource(s) "
					"pending deletion.",
					PendingRHIDeletes);
			}
			checkf(bResourcesReleased,
				"Rendering thread shutdown found live render resources.");
			checkf(PendingRHIDeletes == 0,
				"Rendering thread shutdown left pending RHI deletions.");
			checkf(ExecutorStats.PendingBatchCount == 0,
				"Rendering thread shutdown left unreplayed RHI command batches.");
			checkf(GCommandListExecutor.GetLastSubmittedSerial()
				== GCommandListExecutor.GetCompletedSerial(),
				"Rendering thread shutdown left incomplete RHI submissions.");
		}
	}

	class FRenderingThread : public FRunnable
	{
	public:
		~FRenderingThread() override
		{
		}

		auto Run() -> uint32 override
		{
			check(IsInRenderingThread());
			// RHIInit binds the command-list context; context-free tests may still use the render command pipe.
			DURIN_DEBUG("Rendering thread started. ({}, id: {})", GetCurrentThread()->GetThreadName(), GetCurrentThread()->GetThreadId());
			while (FRenderThreadCommandPipe::Launch())
			{
			}
			return 0;
		}

		auto Stop() -> void override
		{
			FRenderThreadCommandPipe::Shutdown();
		}

		auto Exit() -> void override
		{
			DURIN_DEBUG("Rendering thread shut down.");
		}
	};

	static auto StartRenderingThread() -> void
	{
		GRenderingThreadRunnable = new FRenderingThread();
		GRenderingThread = FRunnableThread::Create(GRenderingThreadRunnable, "RenderingThread", 0, EThreadPriority::Normal, EThreadRole::RenderingThread);
	}

	static auto StopRenderingThread() -> void
	{
		GRenderingThread->WaitForCompletion();

		delete GRenderingThread;
		GRenderingThread = nullptr;

		delete GRenderingThreadRunnable;
		GRenderingThreadRunnable = nullptr;
	}

	FRenderCommandFence::FRenderCommandFence() = default;
	FRenderCommandFence::~FRenderCommandFence() = default;

	auto FRenderCommandFence::BeginFence(ERenderCommandFenceMode Mode) -> void
	{
		check(IsInGameThread());
		// The render queue owns this scheduler-independent signal through execution.
		// Signaling never needs GameThread work.
		Tasks::FTaskEvent Event("RenderCommandFence", false);
		auto Receipt = Mode == ERenderCommandFenceMode::RHIThread
			? std::make_shared<FRHICommandListFence>() : nullptr;
		CompletionTask = Event;
		RHIFence = Receipt;
		FRenderThreadCommandPipe::Enqueue<FFenceRenderCommand>(
			[Event = std::move(Event), Receipt = std::move(Receipt)](FRHICommandListImmediate&) mutable {
				if (Receipt)
				{
					const uint64 Serial = GCommandListExecutor.Submit({}, ERHISubmitFlags::None);
					*Receipt = GCommandListExecutor.CreateFence(Serial);
				}
				Event.Trigger();
			});
	}

	auto FRenderCommandFence::Wait() -> void
	{
		check(IsInGameThread());
		if (!CompletionTask.IsValid()) return;
		const FTaskWaitResult Result = WaitTask(CompletionTask);
		requiref(Result.WaitStatus == ETaskWaitStatus::Completed && Result.TaskState == ETaskState::Succeeded,
			"Render fence completion failed: {}", CompletionTask.GetDiagnostic());
		if (RHIFence) RHIFence->Wait();
		CompletionTask = {};
		RHIFence.reset();
	}

	auto FRenderCommandFence::IsFenceComplete() const -> bool
	{
		check(IsInGameThread());
		if (!CompletionTask.IsValid()) return true;
		if (!CompletionTask.IsCompleted()) return false;
		requiref(CompletionTask.GetState() == ETaskState::Succeeded,
			"Render fence completion failed: {}", CompletionTask.GetDiagnostic());
		if (!RHIFence) return true;
		const ERHICommandBatchState State = RHIFence->GetState();
		requiref(State == ERHICommandBatchState::Pending || State == ERHICommandBatchState::Succeeded,
			"RHI render fence failed for serial {}.", RHIFence->GetTargetSerial());
		return State == ERHICommandBatchState::Succeeded;
	}

	namespace FFrameSync
	{
		struct FRenderThreadFence
		{
			explicit FRenderThreadFence(ERenderCommandFenceMode Mode)
			{
				Fence.BeginFence(Mode);
			}

			~FRenderThreadFence()
			{
				Fence.Wait();
			}

			FRenderCommandFence Fence;
		};

		std::array<std::optional<FRenderThreadFence>, 2> RenderThreadFences;

		static uint32 NextFenceIndex = 0;

		auto Sync(EFlushMode FlushMode) -> void
		{
			check(IsInGameThread());
			bool bFullSync = (FlushMode == EFlushMode::Threads);

			RenderThreadFences[NextFenceIndex].emplace(
				bFullSync ? ERenderCommandFenceMode::RHIThread
					: ERenderCommandFenceMode::RenderThread);
			NextFenceIndex ^= 1;

			if (bFullSync)
			{
				for (size_t i = 0; i < RenderThreadFences.size(); ++i)
				{
					if (RenderThreadFences[i].has_value())
					{
						RenderThreadFences[i].reset();
					}
				}
			}
			else
			{
				if (RenderThreadFences[NextFenceIndex].has_value())
				{
					RenderThreadFences[NextFenceIndex].reset();
				}
			}
		}
	} // namespace FFrameSync

	auto InitRenderingThread() -> void
	{
		FRenderThreadCommandPipe::Start();
		StartRenderingThread();
	}

	auto ShutdownRenderingThread() -> void
	{
		check(IsInGameThread());
		FRenderThreadCommandPipe::Shutdown();
		StopRenderingThread();
	}

	auto FlushRenderingCommands() -> void
	{
		ENQUEUE_RENDER_COMMAND(FlushPendingDeleteRHIResourcesCmd)([](FRHICommandListImmediate& RHICmdList) {
			RHICmdList.ImmediateFlush(
				EImmediateFlushType::DispatchToRHIThread,
				ERHISubmitFlags::DeleteResources);
		});
		FFrameSync::Sync(FFrameSync::EFlushMode::Threads);
	}

	auto GetImmediateCommandList_ForRenderCommand() -> FRHICommandListImmediate&
	{
		return FRHICommandListImmediate::Get();
	}

	auto GetRenderCommandAdmissionState() -> ERenderCommandAdmissionState
	{
		return FRenderThreadCommandPipe::GetAdmissionState();
	}

	auto GetNumPendingRenderCommands() -> size_t
	{
		return FRenderThreadCommandPipe::GetNumPendingCommands();
	}

	auto FRenderThreadCommandPipe::EnqueueImpl(
		const char* Name,
		std::function<void(FRHICommandListImmediate&)>&& Function) -> void
	{
		bool bWasEmpty;
		{
			std::lock_guard Lock(Mutex);
			checkf(AdmissionState == ERenderCommandAdmissionState::Running,
				"Render command '{}' was submitted after admission closed.", Name);
			bWasEmpty = CommandQueue[ProduceIndex].empty();
			CommandQueue[ProduceIndex].emplace_back(Name, std::move(Function));
		}
		if (bWasEmpty)
		{
			CommandAvailableCV.notify_one();
		}
	}

	auto FRenderThreadCommandPipe::LaunchImpl() -> bool
	{
		check(IsInRenderingThread());
		std::unique_lock Lock(Mutex);
		CommandAvailableCV.wait(Lock, [this]() {
			return !CommandQueue[ProduceIndex].empty()
				|| AdmissionState == ERenderCommandAdmissionState::Draining;
		});

		if (CommandQueue[ProduceIndex].empty())
		{
			check(AdmissionState == ERenderCommandAdmissionState::Draining);
			Lock.unlock();
			CheckRenderingThreadShutdown();
			Lock.lock();
			check(CommandQueue[0].empty());
			check(CommandQueue[1].empty());
			check(ActiveCommandCount == 0);
			AdmissionState = ERenderCommandAdmissionState::Stopped;
			return false;
		}

		std::vector<FCommand>& CommandsToExecute = CommandQueue[ProduceIndex];
		ProduceIndex ^= 1;
		ActiveCommandCount += CommandsToExecute.size();
		Lock.unlock();

		for (FCommand& Command : CommandsToExecute)
		{
			const auto Started = std::chrono::steady_clock::now();
			Command.Function(FRHICommandListImmediate::Get());
			// A completed command must release its captured payload before the
			// next command can complete a fence. Keeping every lambda alive until
			// the whole batch is cleared makes fence-visible lifetime depend on
			// whether adjacent commands happened to land in the same batch.
			Command.Function = {};
			const double Milliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - Started).count();
			if (Milliseconds >= 100.0)
				DURIN_DEBUG("Slow render command: name={}, elapsed_ms={:.3f}", Command.Name, Milliseconds);
		}

		CommandsToExecute.clear();
		Lock.lock();
		ActiveCommandCount = 0;
		return true;
	}

	auto FRenderThreadCommandPipe::StartImpl() -> void
	{
		std::lock_guard Lock(Mutex);
		checkf(AdmissionState == ERenderCommandAdmissionState::Stopped,
			"Rendering thread command admission can only start from stopped.");
		check(CommandQueue[0].empty());
		check(CommandQueue[1].empty());
		check(ActiveCommandCount == 0);
		ProduceIndex = 0;
		AdmissionState = ERenderCommandAdmissionState::Running;
	}

	auto FRenderThreadCommandPipe::ShutdownImpl() -> void
	{
		{
			std::lock_guard Lock(Mutex);
			checkf(AdmissionState == ERenderCommandAdmissionState::Running,
				"Rendering thread can only shut down while running.");
			AdmissionState = ERenderCommandAdmissionState::Draining;
		}
		CommandAvailableCV.notify_one();
	}

	auto FRenderThreadCommandPipe::GetAdmissionStateImpl() const
		-> ERenderCommandAdmissionState
	{
		std::lock_guard Lock(Mutex);
		return AdmissionState;
	}

	auto FRenderThreadCommandPipe::GetNumPendingCommandsImpl() const -> size_t
	{
		std::lock_guard Lock(Mutex);
		// The consumer clears its vector outside Mutex. Count that batch only
		// through ActiveCommandCount, which is published under Mutex.
		return CommandQueue[ProduceIndex].size() + ActiveCommandCount;
	}

	FRenderThreadCommandPipe FRenderThreadCommandPipe::Instance;

} // namespace Durin
