#pragma once

#include "CoreMinimal.h"
#include "Threading/TaskRuntime.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataRequestOwner.h"
#endif

namespace Durin::AssetPrivate
{
#if DURIN_WITH_EDITOR
	// Called only after the producer task finishes, so no further submissions can race this join.
	inline auto WaitForAssetBuildOwner(const Tasks::FTask& Task, DerivedData::FRequestOwner& Owner) -> bool
	{
		if (!Task.IsValid() || !Task.IsCompleted() || Owner.Poll()) return false;
		Owner.Wait();
		return true;
	}
#endif

}
