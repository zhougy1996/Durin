#include "HAL/RunnableThread.h"

#include "HAL/StdRunnableThread.h"

namespace Doge
{
	thread_local FRunnableThread* GCurrentThreadTL = nullptr;

	FRunnableThread* GRenderingThread = nullptr;

	std::unordered_map<uint32, FRunnableThread*> GThreads;

	auto GetCurrentThread() -> FRunnableThread*
	{
		return GCurrentThreadTL;
	}

	auto GetCurrentThreadName() -> const char*
	{
		if (GCurrentThreadTL)
		{
			return GCurrentThreadTL->GetThreadName();
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
		return GCurrentThreadTL == GRenderingThread;
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