#include "RenderingThread.h"

#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"

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
			auto* CurrentThread = GetCurrentThread();
			check(CurrentThread);
			DOGE_DEBUG("Rendering thread started. (Thread {}: {})", GetCurrentThread()->GetThreadId(), GetCurrentThread()->GetThreadName());
			while (!bStopRequested)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			return 0;
		}

		auto Stop() -> void override
		{
			bStopRequested = true;
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

} // namespace Doge