#pragma once

#include "Engine/World.h"
#include "DObject/ObjectLifecycle.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	// Owns a complete non-reentrant World operation, including its final stop requests.
	class DWorld::FOperationScope
	{
	public:
		FOperationScope(DWorld& InWorld, EOperation InOperation) : World(InWorld)
		{
			require(!GIsGameThreadIdInitialized || IsInGameThread());
			require(World.Operation == EOperation::Idle);
			World.Operation = InOperation;
		}
		~FOperationScope()
		{
			World.Operation = EOperation::Idle;
			World.FlushLifecycleRequests();
		}
		FOperationScope(const FOperationScope&) = delete;
		auto operator=(const FOperationScope&) -> FOperationScope& = delete;
	private:
		FGarbageCollectionDeferralScope CollectionDeferral;
		DWorld& World;
	};
}
