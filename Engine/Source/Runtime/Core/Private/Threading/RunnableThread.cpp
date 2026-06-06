#include "Threading/RunnableThread.h"

#include "HAL/PlatformLTS.h"
#include "Threading/StdRunnableThread.h"

namespace Durin
{
	thread_local FRunnableThread* CurrentThread = nullptr;

	FRunnableThread* GRenderingThread = nullptr;

	std::unordered_map<uint32, FRunnableThread*> GThreads;

	auto GetThreadRoleName(EThreadRole ThreadRole) -> const char*
	{
		switch (ThreadRole)
		{
		case EThreadRole::Unknown:
			return "Unknown";
		case EThreadRole::GameThread:
			return "GameThread";
		case EThreadRole::RenderingThread:
			return "RenderingThread";
		case EThreadRole::WorkerThread:
			return "WorkerThread";
		case EThreadRole::IOThread:
			return "IOThread";
		default:
			return "Invalid";
		}
	}

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

	auto GetCurrentThreadRole() -> EThreadRole
	{
		if (CurrentThread)
		{
			return CurrentThread->GetThreadRole();
		}
		if (GIsGameThreadIdInitialized && IsInGameThread())
		{
			return EThreadRole::GameThread;
		}
		return EThreadRole::Unknown;
	}

	auto IsInGameThread() -> bool
	{
		check(GIsGameThreadIdInitialized);
		return FPlatformLTS::GetCurrentThreadId() == GGameThreadId;
	}

	auto IsInRenderingThread() -> bool
	{
		return CurrentThread && (CurrentThread == GRenderingThread || CurrentThread->GetThreadRole() == EThreadRole::RenderingThread);
	}

	auto IsInWorkerThread() -> bool
	{
		return GetCurrentThreadRole() == EThreadRole::WorkerThread;
	}

	auto IsInTaskThread() -> bool
	{
		const EThreadRole CurrentRole = GetCurrentThreadRole();
		return CurrentRole == EThreadRole::WorkerThread || CurrentRole == EThreadRole::IOThread;
	}

	auto CheckGameThread() -> void
	{
		checkf(IsInGameThread(), "Expected game thread, current thread is {}.", GetCurrentThreadName());
	}

	auto CheckRenderingThread() -> void
	{
		checkf(IsInRenderingThread(), "Expected rendering thread, current thread is {}.", GetCurrentThreadName());
	}

	auto CheckThreadRole(EThreadRole ThreadRole) -> void
	{
		checkf(GetCurrentThreadRole() == ThreadRole, "Expected thread role {}, current thread role is {}.", GetThreadRoleName(ThreadRole), GetThreadRoleName(GetCurrentThreadRole()));
	}

	auto FRunnableThread::Create(FRunnable* InRunnable, const char* ThreadName, uint32 StackSize, EThreadPriority ThreadPri, EThreadRole ThreadRole) -> FRunnableThread*
	{
		FRunnableThread* Thread = new FRunnableThreadStd();
		if (Thread->CreateInternal(InRunnable, ThreadName, StackSize, ThreadPri, ThreadRole))
		{
			return Thread;
		}
		else
		{
			delete Thread;
			return nullptr;
		}
	}
} // namespace Durin
