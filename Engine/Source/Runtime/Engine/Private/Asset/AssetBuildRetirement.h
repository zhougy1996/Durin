#pragma once

#include "CoreMinimal.h"
#include "Threading/Task.h"
#include "Threading/CancellationToken.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataRequestOwner.h"
#endif

namespace Durin::AssetPrivate
{
	inline auto ObserveAssetBuildTaskCancellation(const Tasks::FTask& Task, FCancellationToken& Cancellation) -> void
	{
		if (Task.IsCancellationRequested()) Cancellation.Cancel();
	}

#if DURIN_WITH_EDITOR
	// Called only after the producer task finishes, so no further submissions can race this join.
	inline auto WaitForAssetBuildOwner(const Tasks::FTask& Task, DerivedData::FRequestOwner& Owner) -> bool
	{
		if (!Task.IsValid() || !Task.IsCompleted() || Owner.Poll()) return false;
		require(Owner.Wait() == DerivedData::ERequestWaitResult::Completed);
		return true;
	}
#endif

}
