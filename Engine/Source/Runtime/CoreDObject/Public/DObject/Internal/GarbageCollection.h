#pragma once
#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"

namespace Durin::Private
{
	// Request intent only; Engine chooses retention policy at its safe point.
	COREDOBJECT_API auto IsFullPurgeRequested() -> bool;
}
