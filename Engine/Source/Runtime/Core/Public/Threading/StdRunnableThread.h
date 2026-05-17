#pragma once

#include "Threading/RunnableThread.h"

namespace Durin
{
	class FRunnableThreadStd final : public FRunnableThread
	{
	public:
		FRunnableThreadStd() = default;

		auto Kill(bool bShouldWait = false) -> void override;
		auto Suspend(bool bShouldPause = true) -> void override;
		auto Resume() -> void override;
		auto WaitForCompletion() -> void override;

	protected:
		auto CreateInternal(FRunnable* InRunnable, const char* InThreadName, uint32 InStackSize, EThreadPriority InThreadPriority) -> bool override;

		std::thread Thread;
	};
}