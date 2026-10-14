#pragma once

#include "Misc/CoreStd.h"
#include "Threading/QueuedThreadPool.h"

namespace Durin::Private
{
	// Internal execution ownership shared by scheduler tasks and callback adapters.
	// Links and membership are protected by the owning pool's mutex.
	struct FQueuedWorkNode
	{
		virtual ~FQueuedWorkNode() = default;
		virtual auto Execute() -> void = 0;
		virtual auto Discard() -> void = 0;
		virtual auto GetName() const -> const char* = 0;

		std::atomic<const void*> Pool = nullptr;
		FQueuedWorkNode* Previous = nullptr;
		std::shared_ptr<FQueuedWorkNode> Next;
		EQueuedWorkPriority QueuePriority = EQueuedWorkPriority::Normal;
		uint64 Sequence = 0;
		bool bQueued = false;
	};
}
