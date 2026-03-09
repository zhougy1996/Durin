#include "RenderingThread.h"

#include "DynamicRHI.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"

#include "RHICommandList.h"

namespace Doge
{
	FRunnable* GRenderingThreadRunnable = nullptr;

	class FRenderingThread : public FRunnable
	{
	public:
		~FRenderingThread() override
		{
			DOGE_DEBUG("Rendering thread shut down.");
		}

		auto Run() -> uint32 override
		{
			check(IsInRenderingThread());
			DOGE_DEBUG("Rendering thread started. (Thread {}: {})", GetCurrentThread()->GetThreadId(), GetCurrentThread()->GetThreadName());
			while (!bStopRequested)
			{
				FRenderThreadCommandPipe::Launch();
			}
			return 0;
		}

		auto Stop() -> void override
		{
			bStopRequested = true;
		}

		auto Exit() -> void override
		{
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
		ENQUEUE_RENDER_COMMAND(FenceCommand)([this](FRHICommandListImmediate& RHICmdList)
		{
			this->bIsComplete.store(true, std::memory_order::release);
			std::unique_lock<std::mutex> Lock(Mutex);
			this->CV.notify_all();
		});
	}

	auto FRenderCommandFence::Wait() -> void
	{
		std::unique_lock<std::mutex> Lock(Mutex);
		CV.wait(Lock, [this]()
		{
			return bIsComplete.load(std::memory_order::acquire);
		});
	}

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
		auto FlushPromisePtr = std::make_shared<std::promise<void>>();
		std::future<void> FlushFuture = FlushPromisePtr->get_future();

		ENQUEUE_RENDER_COMMAND(FlushCommand)([FlushPromisePtr](FRHICommandListImmediate& RHICmdList)
		{
			DOGE_DEBUG("Rendering thread flush command executed.");
			FlushPromisePtr->set_value();
		});

		FlushFuture.wait();
	}

	auto GetImmediateCommandList_ForRenderCommand() -> FRHICommandListImmediate&
	{
		return FRHICommandListImmediate::Get();
	}

	auto FRenderThreadCommandPipe::EnqueueImpl(const CharT* Name, std::function<void(FRHICommandListImmediate&)>&& Function) -> void
	{
		DOGE_DEBUG("Enqueuing render command: {}", Name);
		std::lock_guard<std::mutex> lock(Mutex);
		bool bWasEmpty = CommandQueue[ProduceIndex].empty();
		CommandQueue[ProduceIndex].emplace_back(Name, std::move(Function));
	}

	auto FRenderThreadCommandPipe::LaunchImpl() -> void
	{
		check(IsInRenderingThread());
		Mutex.lock();
		std::vector<FRenderThreadCommandPipe::FCommand>& CommandsToExecute = CommandQueue[ProduceIndex];
		ProduceIndex ^= 1;
		Mutex.unlock();

		for (const FRenderThreadCommandPipe::FCommand& Command : CommandsToExecute)
		{
			Command.Function(FRHICommandListImmediate::Get());
		}

		CommandsToExecute.clear();
	}

	FRenderThreadCommandPipe FRenderThreadCommandPipe::Instance;

} // namespace Doge