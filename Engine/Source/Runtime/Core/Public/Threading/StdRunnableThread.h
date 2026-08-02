#pragma once

#include "Threading/RunnableThread.h"

namespace Durin
{
	class FRunnableThreadStd final : public FRunnableThread
	{
	public:
		FRunnableThreadStd() = default;
		~FRunnableThreadStd() override;

		auto Kill(bool bShouldWait = false) -> void override;
		auto Suspend(bool bShouldPause = true) -> void override;
		auto Resume() -> void override;
		auto WaitForCompletion() -> void override;

	protected:
		auto CreateInternal(FRunnable* InRunnable, const char* InThreadName, uint32 InStackSize, EThreadPriority InThreadPriority, EThreadRole InThreadRole) -> bool override;

		std::thread Thread;
		std::atomic<bool> bStopRequested = false;
		std::atomic<bool> bCompleted = false;
	};
}
