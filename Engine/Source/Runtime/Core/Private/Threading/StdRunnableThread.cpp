#include "Threading/StdRunnableThread.h"

#include "HAL/PlatformLTS.h"
#include "Threading/Runnable.h"

namespace Durin
{
	auto FRunnableThreadStd::Kill(bool bShouldWait) -> void
	{
		if (bStopRequested.exchange(true, std::memory_order::acq_rel))
		{
			if (bShouldWait)
			{
				WaitForCompletion();
			}
			return;
		}

		DURIN_TRACE("Thread stop requested. (name: {}, id: {}, role: {})", GetThreadName(), GetThreadId(), GetThreadRoleName(GetThreadRole()));
		if (Runnable)
		{
			Runnable->Stop();
		}

		if (bShouldWait)
		{
			WaitForCompletion();
		}
	}

	auto FRunnableThreadStd::Suspend(bool bShouldPause) -> void
	{
		DURIN_WARN("Thread suspension is unsupported. (name: {}, id: {}, role: {})", GetThreadName(), GetThreadId(), GetThreadRoleName(GetThreadRole()));
	}

	auto FRunnableThreadStd::Resume() -> void
	{
		DURIN_WARN("Thread resume is unsupported. (name: {}, id: {}, role: {})", GetThreadName(), GetThreadId(), GetThreadRoleName(GetThreadRole()));
	}

	auto FRunnableThreadStd::WaitForCompletion() -> void
	{
		if (Thread.joinable())
		{
			Thread.join();
		}
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

	auto FRunnableThreadStd::CreateInternal(FRunnable* InRunnable, const char* InThreadName, uint32 InStackSize, EThreadPriority InThreadPriority, EThreadRole InThreadRole) -> bool
	{
		ThreadName = InThreadName;
		Runnable = InRunnable;
		ThreadPriority = InThreadPriority;
		ThreadRole = InThreadRole;
		bStopRequested.store(false, std::memory_order::release);

		Thread = std::thread([this]() {
			ThreadId.store(FPlatformLTS::GetCurrentThreadId(), std::memory_order::release);
			this->AsCurrentThread();
			DURIN_DEBUG("Thread started. (name: {}, id: {}, role: {})", GetThreadName(), GetThreadId(), GetThreadRoleName(GetThreadRole()));

			const bool bInitialized = Runnable->Init();
			if (bInitialized)
			{
				Runnable->Run();
			}
			Runnable->Exit();

			DURIN_DEBUG("Thread exited. (name: {}, id: {}, role: {})", GetThreadName(), GetThreadId(), GetThreadRoleName(GetThreadRole()));
		});

		ThreadId.store(PlatformGetThreadIdFromStdThread(Thread), std::memory_order::release);
		return true;
	}

} // namespace Durin
