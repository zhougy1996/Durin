#pragma once

#include "Threading/Private/QueuedWorkNode.h"

namespace Durin
{
	class FTaskWaitSignal;
}

namespace Durin::Private
{
	// One queue per render-thread lifetime. Main commands only share its wake signal.
	class CORE_API FRenderThreadLocalQueue
	{
	public:
		static auto Open() -> std::shared_ptr<FRenderThreadLocalQueue>;
		auto AttachToCurrentThread() -> void;
		auto CloseIfEmpty() -> bool;
		auto ProcessOne() -> bool;
		auto PrepareWait() const -> uint64;
		auto Wait(uint64 Observed) const -> void;
		auto Notify() const -> void;
		auto Enqueue(const std::shared_ptr<FQueuedWorkNode>& Task) -> bool;
		const std::shared_ptr<FTaskWaitSignal> Signal;

	private:
		FRenderThreadLocalQueue();
		std::mutex Mutex;
		std::deque<std::shared_ptr<FQueuedWorkNode>> Tasks;
		bool bClosed = false;
	};
}
