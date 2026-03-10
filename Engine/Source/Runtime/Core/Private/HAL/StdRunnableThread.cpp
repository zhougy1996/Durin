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

	uint32 PlatformGetThreadIdFromStdThread(std::thread& Thread)
	{
		check(Thread.joinable());
#ifdef _WIN32
		return static_cast<uint32>(GetThreadId(Thread.native_handle()));
#elif defined(__linux__) || defined(__APPLE__)
		pthread_t pthread_id = Thread.native_handle();
		return static_cast<uint32>(pthread_id);
#endif
	}

	auto FRunnableThreadStd::CreateInternal(FRunnable* InRunnable, const char* InThreadName, uint32 InStackSize, EThreadPriority InThreadPriority) -> bool
	{
		ThreadName = InThreadName;
		Runnable = InRunnable;
		Thread = std::thread([this]() {
			this->AsCurrentThread();
			Runnable->Init();
			Runnable->Run();
			Runnable->Exit();
		});

		ThreadId = PlatformGetThreadIdFromStdThread(Thread);
		return true;
	}

} // namespace Doge