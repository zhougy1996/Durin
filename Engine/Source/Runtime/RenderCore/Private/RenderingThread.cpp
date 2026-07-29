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
			while (!bStopRequested.load(std::memory_order::relaxed))
			{
				FRenderThreadCommandPipe::Launch();
			}
			return 0;
		}

		auto Stop() -> void override
		{
			DURIN_TRACE("Rendering thread stop requested.");
			bStopRequested.store(true, std::memory_order::relaxed);
			FRenderThreadCommandPipe::Wake();
		}

		auto Exit() -> void override
		{
			DURIN_DEBUG("Rendering thread shut down.");
		}

	private:
		std::atomic<bool> bStopRequested = false;
	};

	static auto StartRenderingThread() -> void
	{
		GRenderingThreadRunnable = new FRenderingThread();
		GRenderingThread = FRunnableThread::Create(GRenderingThreadRunnable, "RenderingThread", 0, EThreadPriority::Normal, EThreadRole::RenderingThread);
	}

	static auto StopRenderingThread() -> void
	{
		GRenderingThread->Kill(true);

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
		FRenderThreadCommandPipe::StartAdmission();
		StartRenderingThread();
	}

	auto ShutdownRenderingThreadBeforeRHIExit() -> void
	{
		check(IsInGameThread());
		FRenderThreadCommandPipe::CloseWithFinalCommand(
			"ShutdownRenderingThreadBeforeRHIExit",
			[](FRHICommandListImmediate& RHICmdList) {
				const bool bResourcesReleased =
					ValidateRenderResourceShutdown_RenderThread(
						"pre-RHI-exit");
				RHICmdList.ImmediateFlush(
					EImmediateFlushType::FlushRHIThreadFlushResources,
					ERHISubmitFlags::FlushRHIThread);
				checkf(bResourcesReleased,
					"Render-resource shutdown audit failed before RHIExit.");
				checkf(FRHIResource::GetNumPendingDeletes() == 0,
					"RHI deferred-delete queue still contains {} resources "
					"after the pre-RHI-exit drain.",
					FRHIResource::GetNumPendingDeletes());
			});
		FRenderThreadCommandPipe::DrainAcceptedCommands();
		StopRenderingThread();
		FRenderThreadCommandPipe::MarkStopped();
	}

	auto ShutdownRenderingThread() -> void
	{
		FRenderThreadCommandPipe::CloseAdmission();
		FRenderThreadCommandPipe::DrainAcceptedCommands();
		StopRenderingThread();
		FRenderThreadCommandPipe::MarkStopped();
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

	auto FRenderThreadCommandPipe::LaunchImpl() -> void
	{
		check(IsInRenderingThread());
		std::unique_lock Lock(Mutex);
		CommandAvailableCV.wait(Lock, [this]() {
			return bWakeRequested || !CommandQueue[ProduceIndex].empty();
		});

		bWakeRequested = false;
		if (CommandQueue[ProduceIndex].empty())
		{
			return;
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
		if (CommandQueue[0].empty() && CommandQueue[1].empty())
		{
			DrainCompleteCV.notify_all();
		}
	}

	auto FRenderThreadCommandPipe::WakeImpl() -> void
	{
		{
			std::lock_guard Lock(Mutex);
			bWakeRequested = true;
		}
		CommandAvailableCV.notify_one();
	}

	auto FRenderThreadCommandPipe::StartAdmissionImpl() -> void
	{
		std::lock_guard Lock(Mutex);
		checkf(AdmissionState == ERenderCommandAdmissionState::Stopped,
			"Rendering thread command admission can only start from stopped.");
		check(CommandQueue[0].empty());
		check(CommandQueue[1].empty());
		check(ActiveCommandCount == 0);
		ProduceIndex = 0;
		bWakeRequested = false;
		AdmissionState = ERenderCommandAdmissionState::Running;
	}

	auto FRenderThreadCommandPipe::CloseAdmissionImpl() -> void
	{
		std::lock_guard Lock(Mutex);
		if (AdmissionState == ERenderCommandAdmissionState::Stopped) return;
		AdmissionState = ERenderCommandAdmissionState::Draining;
		CommandAvailableCV.notify_one();
	}

	auto FRenderThreadCommandPipe::CloseWithFinalCommandImpl(
		const char* Name,
		std::function<void(FRHICommandListImmediate&)>&& Function) -> void
	{
		{
			std::lock_guard Lock(Mutex);
			checkf(AdmissionState == ERenderCommandAdmissionState::Running,
				"Final rendering-thread shutdown command requires running "
				"command admission.");
			CommandQueue[ProduceIndex].emplace_back(
				Name, std::move(Function));
			AdmissionState = ERenderCommandAdmissionState::Draining;
		}
		CommandAvailableCV.notify_one();
	}

	auto FRenderThreadCommandPipe::DrainAcceptedCommandsImpl() -> void
	{
		check(IsInGameThread());
		std::unique_lock Lock(Mutex);
		checkf(AdmissionState == ERenderCommandAdmissionState::Draining,
			"Accepted render commands can only drain after admission closes.");
		CommandAvailableCV.notify_one();
		DrainCompleteCV.wait(Lock, [this]() {
			return CommandQueue[0].empty() && CommandQueue[1].empty()
				&& ActiveCommandCount == 0;
		});
	}

	auto FRenderThreadCommandPipe::MarkStoppedImpl() -> void
	{
		std::lock_guard Lock(Mutex);
		check(CommandQueue[0].empty());
		check(CommandQueue[1].empty());
		check(ActiveCommandCount == 0);
		AdmissionState = ERenderCommandAdmissionState::Stopped;
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
