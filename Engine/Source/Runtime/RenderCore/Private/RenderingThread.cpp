#include "RenderingThread.h"

#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"

#include "RHICommandList.h"
#include "RenderResource.h"
#include "Profiling/Profiling.h"

namespace Durin
{
	namespace
	{
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
			DURIN_TRACE(
				"RHI executor drained (mode: {}): {} command(s), {} payload byte(s), "
				"{} batch(es), {} submission group(s), {} ns replay, {} wait(s), "
				"{} synchronous operation(s), "
				"{} ns wait duration, peak queue {} entry(s)/{} batch(es)/{} byte(s), "
				"{} backpressure event(s).",
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
				ExecutorStats.BackpressureWaitCount);
			const auto UploadStats = GetBufferUploadStats();
			DURIN_TRACE("RHI upload/frame bounds: peak queued frames={}, frame-pressure waits={} ({} ns), "
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

		class FRenderingThread final : public FRunnable
		{
		public:
			auto Start() -> void
			{
				std::lock_guard Lock(Mutex);
				require(!GRenderingThread);
				bStopRequested = false;
				GRenderingThread = FRunnableThread::Create(this, "RenderingThread", 0,
					EThreadPriority::Normal, EThreadRole::RenderingThread);
				requiref(GRenderingThread, "Rendering thread creation failed.");
				bRunning = true;
			}

			auto Stop() -> void override
			{
				{
					std::lock_guard Lock(Mutex);
					require(bRunning && !bStopRequested);
					bStopRequested = true;
				}
				CommandAvailable.notify_one();
			}

			auto IsRunning() -> bool
			{
				std::lock_guard Lock(Mutex);
				return bRunning;
			}

			auto Enqueue(const char* Name,
				TMoveOnlyFunction<void(FRHICommandListImmediate&)>&& Function) -> void
			{
				bool bWasEmpty;
				{
					std::lock_guard Lock(Mutex);
					// Accepted commands can enqueue ordered cleanup until the consumer runs out of work.
					requiref(bRunning && (!bStopRequested || GetCurrentThread() == GRenderingThread),
						"Render command '{}' was submitted outside the rendering thread lifetime.", Name);
					bWasEmpty = PendingCommands.empty();
					PendingCommands.emplace_back(Name, std::move(Function));
				}
				if (bWasEmpty)
				{
					CommandAvailable.notify_one();
				}
			}

			auto Run() -> uint32 override
			{
				std::vector<FCommand> Commands;
				std::unique_lock Lock(Mutex);
				check(IsInRenderingThread());
				for (;;)
				{
					CommandAvailable.wait(Lock, [this]() {
						return bStopRequested || !PendingCommands.empty();
					});
					if (PendingCommands.empty())
					{
						bRunning = false;
						break;
					}

					Commands.swap(PendingCommands);
					Lock.unlock();
					for (FCommand& Command : Commands)
					{
						DURIN_PROFILE_CPU_ZONE_NAMED("RenderCommand.Execute");
						DURIN_PROFILE_CPU_ZONE_TEXT(std::string_view(Command.Name ? Command.Name : "").substr(0, 128));
						Command.Function(FRHICommandListImmediate::Get());
						// Release captures before the next command can complete a fence.
						Command.Function = nullptr;
					}
					Commands.clear();
					Lock.lock();
				}
				Lock.unlock();
				CheckRenderingThreadShutdown();
				return 0;
			}

		private:
			struct FCommand
			{
				const char* Name;
				TMoveOnlyFunction<void(FRHICommandListImmediate&)> Function;
			};

			std::vector<FCommand> PendingCommands;
			std::mutex Mutex;
			std::condition_variable CommandAvailable;
			bool bRunning = false;
			bool bStopRequested = false;
		};

		FRenderingThread RenderingThread;
		std::array<FRenderCommandFence, 2> FrameFences;
		uint32 NextFrameFence = 0;
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
		EnqueueRenderCommand("FenceCommand",
			[Event = std::move(Event), Receipt = std::move(Receipt)](FRHICommandListImmediate&) mutable {
				if (Receipt)
				{
					const uint64 Serial = GCommandListExecutor.Submit({}, ERHISubmitFlags::None);
					*Receipt = GCommandListExecutor.CreateFence(Serial);
				}
				Event.Trigger();
			});
	}

	auto FRenderCommandFence::Wait(std::source_location Caller) -> void
	{
		check(IsInGameThread());
		if (!CompletionTask.IsValid()) return;
		DURIN_PROFILE_CPU_ZONE_NAMED("RenderCommandFence.Wait");
		const auto Started = std::chrono::steady_clock::now();
		const FTaskWaitResult Result = WaitTask(CompletionTask);
		const auto RenderCompleted = std::chrono::steady_clock::now();
		requiref(Result.WaitStatus == ETaskWaitStatus::Completed && Result.TaskState == ETaskState::Succeeded,
			"Render fence completion failed: {}", CompletionTask.GetDiagnostic());
		if (RHIFence)
		{
			const auto Outcome = RHIFence->Wait();
			requiref(Outcome == ERHICommandBatchState::Succeeded, "RHI render fence did not succeed.");
		}
		const double RenderMS = std::chrono::duration<double, std::milli>(RenderCompleted - Started).count();
		const double RHIMS = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - RenderCompleted).count();
		if (Profiling::AreStartupDiagnosticsEnabled() && RenderMS + RHIMS >= 20.0)
			DURIN_DEBUG("Render fence wait: task={} | caller={}:{} ({}) | render={:.3f} ms | rhi={:.3f} ms",
				CompletionTask.GetTaskId(), Caller.file_name(), Caller.line(), Caller.function_name(), RenderMS, RHIMS);
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
		auto Sync(EFlushMode FlushMode) -> void
		{
			check(IsInGameThread());
			const auto SyncStarted = std::chrono::steady_clock::now();
			const bool bFullSync = FlushMode == EFlushMode::Threads;
			if (bFullSync)
			{
				FRenderCommandFence Fence;
				Fence.BeginFence(ERenderCommandFenceMode::RHIThread);
				FrameFences[NextFrameFence ^ 1].Wait();
				Fence.Wait();
			}
			else
			{
				FrameFences[NextFrameFence].BeginFence();
				NextFrameFence ^= 1;
				FrameFences[NextFrameFence].Wait();
			}
			const double SyncMS = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - SyncStarted).count();
			if (Profiling::AreStartupDiagnosticsEnabled() && SyncMS >= 20.0)
				DURIN_DEBUG("Render frame sync: mode={} | {:.3f} ms", bFullSync ? "Threads" : "EndFrame", SyncMS);
		}
	} // namespace FFrameSync

	auto InitRenderingThread() -> void
	{
		RenderingThread.Start();
	}

	auto ShutdownRenderingThread() -> void
	{
		check(IsInGameThread());
		require(GRenderingThread);
		GRenderingThread->Kill(true);
		delete GRenderingThread;
		GRenderingThread = nullptr;
		FrameFences[NextFrameFence ^ 1].Wait();
		NextFrameFence = 0;
	}

	auto IsRenderingThreadRunning() -> bool
	{
		return RenderingThread.IsRunning();
	}

	auto FlushRenderingCommands() -> void
	{
		check(IsInGameThread());
		// Stopped admission owns no accepted work, including before RHI startup.
		if (!IsRenderingThreadRunning()) return;
		ENQUEUE_RENDER_COMMAND(FlushPendingDeleteRHIResourcesCmd)([](FRHICommandListImmediate& RHICmdList) {
			RHICmdList.ImmediateFlush(
				EImmediateFlushType::DispatchToRHIThread,
				ERHISubmitFlags::DeleteResources);
		});
		FFrameSync::Sync(FFrameSync::EFlushMode::Threads);
	}

	auto EnqueueRenderCommand(const char* Name,
		TMoveOnlyFunction<void(FRHICommandListImmediate&)> Function) -> void
	{
		if (IsInRenderingThread())
		{
			Function(FRHICommandListImmediate::Get());
			return;
		}
		EnqueueRenderCommandDeferred(Name, std::move(Function));
	}

	auto EnqueueRenderCommandDeferred(const char* Name,
		TMoveOnlyFunction<void(FRHICommandListImmediate&)> Function) -> void
	{
		RenderingThread.Enqueue(Name, std::move(Function));
	}

} // namespace Durin
