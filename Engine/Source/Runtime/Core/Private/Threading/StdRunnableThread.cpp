#include "Threading/StdRunnableThread.h"

#include "HAL/PlatformLTS.h"
#include "Profiling/Profiling.h"
#include "Threading/Runnable.h"

namespace Durin
{
	FRunnableThreadStd::~FRunnableThreadStd()
	{
		if (!Thread.joinable())
		{
			return;
		}

		if (bCompleted.load(std::memory_order::acquire))
		{
			WaitForCompletion();
		}
		else
		{
			Kill(true);
		}
	}

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
		if (InStackSize != 0)
		{
			DURIN_WARN("Thread creation rejected because custom stack sizes are unsupported. (name: {}, stack size: {})", InThreadName ? InThreadName : "", InStackSize);
			return false;
		}

		if (InThreadPriority != EThreadPriority::Normal)
		{
			DURIN_WARN("Thread creation rejected because custom priorities are unsupported. (name: {}, priority: {})", InThreadName ? InThreadName : "", static_cast<uint32>(InThreadPriority));
			return false;
		}

		ThreadName = InThreadName ? InThreadName : "Thread";
		Runnable = InRunnable;
		ThreadPriority = InThreadPriority;
		ThreadRole = InThreadRole;
		bStopRequested.store(false, std::memory_order::release);
		bCompleted.store(false, std::memory_order::release);

		try
		{
			Thread = std::thread([this]() {
				ThreadId.store(FPlatformLTS::GetCurrentThreadId(), std::memory_order::release);
				this->AsCurrentThread();
				DURIN_PROFILE_THREAD(GetThreadName());
				DURIN_DEBUG("Thread started. (name: {}, id: {}, role: {})", GetThreadName(), GetThreadId(), GetThreadRoleName(GetThreadRole()));

				const bool bInitialized = Runnable->Init();
				if (bInitialized)
				{
					Runnable->Run();
				}
				Runnable->Exit();
				bCompleted.store(true, std::memory_order::release);

				DURIN_DEBUG("Thread exited. (name: {}, id: {}, role: {})", GetThreadName(), GetThreadId(), GetThreadRoleName(GetThreadRole()));
			});
		}
		catch (const std::exception& Exception)
		{
			DURIN_ERROR("Thread creation failed. (name: {}, error: {})", GetThreadName(), Exception.what());
			Runnable = nullptr;
			return false;
		}

		ThreadId.store(PlatformGetThreadIdFromStdThread(Thread), std::memory_order::release);
		return true;
	}

} // namespace Durin
