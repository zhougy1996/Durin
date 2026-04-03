#include "RenderingThread.h"

#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"

#include "RHICommandList.h"

namespace Doge
{
	FRunnable* GRenderingThreadRunnable = nullptr;

	class FRenderingThread : public FRunnable
	{
	public:
		~FRenderingThread() override
		{
		}

		auto Run() -> uint32 override
		{
			check(IsInRenderingThread());
			DOGE_DEBUG("Rendering thread started. ({}, id: {})", GetCurrentThread()->GetThreadName(), GetCurrentThread()->GetThreadId());
			while (!bStopRequested)
			{
				FRenderThreadCommandPipe::Launch();
			}
			return 0;
		}

		auto Stop() -> void override
		{
			DOGE_DEBUG("Rendering thread stop requested.");
			bStopRequested = true;
		}

		auto Exit() -> void override
		{
			DOGE_DEBUG("Rendering thread shut down.");
		}

	private:
		bool bStopRequested = false;
	};

	static auto StartRenderingThread() -> void
	{
		GRenderingThreadRunnable = new FRenderingThread();
		GRenderingThread = FRunnableThread::Create(GRenderingThreadRunnable, "RenderingThread", 0, EThreadPriority::Normal);
	}

	static auto StopRenderingThread() -> void
	{
		GRenderingThreadRunnable->Stop();
		GRenderingThread->WaitForCompletion();

		delete GRenderingThread;
		GRenderingThread = nullptr;

		delete GRenderingThreadRunnable;
		GRenderingThreadRunnable = nullptr;
	}

	auto FRenderCommandFence::BeginFence() -> void
	{
		bIsComplete = false;
		ENQUEUE_RENDER_COMMAND(FenceCommand)([this](FRHICommandListImmediate& RHICmdList) {
			this->bIsComplete.store(true, std::memory_order::release);
			std::unique_lock<std::mutex> Lock(Mutex);
			this->CV.notify_all();
		});
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
		StartRenderingThread();
	}

	auto ShutdownRenderingThread() -> void
	{
		StopRenderingThread();
	}

	auto FlushRenderingCommands() -> void
	{
		ENQUEUE_RENDER_COMMAND(FlushPendingDeleteRHIResourcesCmd)([](FRHICommandListImmediate& RHICmdList) {
			RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources, ERHISubmitFlags::EndFrame);
		});
		FFrameSync::Sync(FFrameSync::EFlushMode::Threads);
	}

	auto GetImmediateCommandList_ForRenderCommand() -> FRHICommandListImmediate&
	{
		return FRHICommandListImmediate::Get();
	}

	auto FRenderThreadCommandPipe::EnqueueImpl(const char* Name, std::function<void(FRHICommandListImmediate&)>&& Function) -> void
	{
		std::lock_guard lock(Mutex);
		CommandQueue[ProduceIndex].emplace_back(Name, std::move(Function));
	}

	auto FRenderThreadCommandPipe::LaunchImpl() -> void
	{
		check(IsInRenderingThread());
		Mutex.lock();
		std::vector<FCommand>& CommandsToExecute = CommandQueue[ProduceIndex];
		ProduceIndex ^= 1;
		Mutex.unlock();

		for (const FCommand& Command : CommandsToExecute)
		{
			Command.Function(FRHICommandListImmediate::Get());
		}

		CommandsToExecute.clear();
	}

	FRenderThreadCommandPipe FRenderThreadCommandPipe::Instance;

} // namespace Doge