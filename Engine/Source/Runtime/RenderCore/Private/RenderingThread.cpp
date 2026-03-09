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
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

	auto InitRenderingThread() -> void
	{
		StartRenderingThread();
	}

	auto ShutdownRenderingThread() -> void
	{
		StopRenderingThread();
	}

	auto GetImmediateCommandList_ForRenderCommand() -> FRHICommandListImmediate&
	{
		return FRHICommandListImmediate::Get();
	}

	auto FRenderThreadCommandPipe::EnqueueImpl(const CharT* Name, std::function<void(FRHICommandListImmediate&)>&& Function) -> void
	{
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