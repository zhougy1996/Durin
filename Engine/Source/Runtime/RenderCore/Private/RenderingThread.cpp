#include "RenderingThread.h"

#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"

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
			DURIN_DEBUG(
				"Inline RHI executor drained: {} command(s), {} payload byte(s), "
				"{} batch(es), {} submission group(s), {} ns replay, {} wait(s), "
				"{} rejection(s).",
				ExecutorStats.RecordedCommandCount,
				ExecutorStats.RecordedPayloadBytes,
				ExecutorStats.SubmittedBatchCount,
				ExecutorStats.SubmissionGroupCount,
				ExecutorStats.ReplayDurationNanoseconds,
				ExecutorStats.WaitCount,
				ExecutorStats.RejectedSubmissionCount);
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

	auto FRenderCommandFence::BeginFence() -> void
	{
		bIsComplete = false;
		const bool bAccepted =
			FRenderThreadCommandPipe::TryEnqueue<FFenceRenderCommand>(
				[this](FRHICommandListImmediate&) {
					this->bIsComplete.store(
						true, std::memory_order::release);
					std::unique_lock<std::mutex> Lock(Mutex);
					this->CV.notify_all();
				});
		checkf(bAccepted,
			"Render command fence was rejected because command admission is closed.");
	}

	auto FRenderCommandFence::Wait() -> void
	{
		std::unique_lock<std::mutex> Lock(Mutex);
		CV.wait(Lock, [this]() {
			return bIsComplete.load(std::memory_order::acquire);
		});
	}

	namespace FFrameSync
	{
		struct FRenderThreadFence
		{
			FRenderThreadFence()
			{
				Fence.BeginFence();
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

			RenderThreadFences[NextFenceIndex].emplace();
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
			RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources, ERHISubmitFlags::FlushRHIThread);
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
		const bool bAccepted = TryEnqueueImpl(Name, std::move(Function));
		checkf(bAccepted,
			"Render command '{}' was rejected because command admission "
			"is closed.", Name);
	}

	auto FRenderThreadCommandPipe::TryEnqueueImpl(
		const char* Name,
		std::function<void(FRHICommandListImmediate&)>&& Function) -> bool
	{
		ERenderCommandAdmissionState RejectionState;
		size_t PendingCount = 0;
		{
			std::lock_guard Lock(Mutex);
			if (AdmissionState != ERenderCommandAdmissionState::Running)
			{
				RejectionState = AdmissionState;
				PendingCount = CommandQueue[0].size()
					+ CommandQueue[1].size() + ActiveCommandCount;
				const char* StateName =
					RejectionState == ERenderCommandAdmissionState::Draining
						? "draining"
						: "stopped";
				DURIN_ERROR(
					"Rejected render command '{}' synchronously: command "
					"admission is {} ({} accepted commands pending).",
					Name, StateName, PendingCount);
				return false;
			}
			CommandQueue[ProduceIndex].emplace_back(Name, std::move(Function));
		}
		CommandAvailableCV.notify_one();
		return true;
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

		for (const FCommand& Command : CommandsToExecute)
		{
			Command.Function(FRHICommandListImmediate::Get());
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
		return CommandQueue[0].size() + CommandQueue[1].size()
			+ ActiveCommandCount;
	}

	FRenderThreadCommandPipe FRenderThreadCommandPipe::Instance;

} // namespace Durin
