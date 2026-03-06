#include "HAL/StdRunnableThread.h"

#include "HAL/Runnable.h"

namespace Doge
{
	auto FRunnableThreadStd::Kill(bool bShouldWait) -> void
	{

	}

	auto FRunnableThreadStd::Suspend(bool bShouldPause) -> void
	{
	}

	auto FRunnableThreadStd::Resume() -> void
	{
	}

	auto FRunnableThreadStd::WaitForCompletion() -> void
	{
		check(Thread.joinable());
		Thread.join();
	}

	auto FRunnableThreadStd::CreateInternal(FRunnable* InRunnable, const char* InThreadName, uint32 InStackSize, EThreadPriority InThreadPriority) -> bool
	{
		static std::atomic<uint32> NextThreadId{1};

		ThreadName = InThreadName;
		Runnable = InRunnable;
		Thread = std::thread([this]()
		{
			GCurrentThreadTL = this;
			Runnable->Init();
			Runnable->Run();
			Runnable->Exit();
		});

		ThreadId = NextThreadId.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

}