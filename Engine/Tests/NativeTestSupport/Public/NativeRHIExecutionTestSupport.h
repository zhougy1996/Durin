#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"
#include "Threading/ThreadEvent.h"

namespace Durin::Testing
{
	// Keep admission/window work and RHIInit/RHIExit on the test's game thread.
	// Inline runtime bodies execute on a real rendering thread and
	// join before returning, including when the body throws or reports a failure.
	inline auto RunOnRHIRenderingThreadForTests(std::function<void()> Body) -> void
	{
		if (GRHIThread)
		{
			Body();
			return;
		}
		require(!GRenderingThread);
		class FTestRenderingThread final : public FRunnable
		{
		public:
			std::function<void()> Body;
			FThreadEvent Published;
			std::exception_ptr Failure;
			auto Run() -> uint32 override
			{
				Published.Wait();
				try { Body(); }
				catch (...) { Failure = std::current_exception(); }
				return 0;
			}
		} Runnable;
		Runnable.Body = std::move(Body);
		std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(
			&Runnable, "InlineTestRenderingThread", 0, EThreadPriority::Normal,
			EThreadRole::RenderingThread));
		require(Thread);
		GRenderingThread = Thread.get();
		Runnable.Published.Trigger();
		Thread->WaitForCompletion();
		GRenderingThread = nullptr;
		if (Runnable.Failure) std::rethrow_exception(Runnable.Failure);
	}

}
