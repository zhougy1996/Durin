#include "HAL/RunnableThread.h"

#include "HAL/StdRunnableThread.h"

namespace Doge
{
	thread_local FRunnableThread* CurrentThread = nullptr;

	FRunnableThread* GRenderingThread = nullptr;

	std::unordered_map<uint32, FRunnableThread*> GThreads;

	auto FRunnableThread::AsCurrentThread() -> void
	{
		CurrentThread = this;
	}

	auto GetCurrentThread() -> FRunnableThread*
	{
		return CurrentThread;
	}

	auto GetCurrentThreadName() -> const char*
	{
		if (CurrentThread)
		{
			return CurrentThread->GetThreadName();
		}
		if (IsInGameThread())
		{
			return "GameThread";
		}
		return "Unknown";
	}

	auto IsInGameThread() -> bool
	{
		check(GIsGameThreadIdInitialized);
		return std::this_thread::get_id() == GGameThreadId;
	}

	auto IsInRenderingThread() -> bool
	{
		return CurrentThread == GRenderingThread;
	}

	auto FRunnableThread::Create(FRunnable* InRunnable, const char* ThreadName, uint32 StackSize, EThreadPriority ThreadPri) -> FRunnableThread*
	{
		FRunnableThread* Thread = new FRunnableThreadStd();
		if (Thread->CreateInternal(InRunnable, ThreadName, StackSize, ThreadPri))
		{
			return Thread;
		}
		else
		{
			delete Thread;
			return nullptr;
		}
	}
} // namespace Doge