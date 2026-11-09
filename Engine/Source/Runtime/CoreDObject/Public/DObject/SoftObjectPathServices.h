#pragma once

#include "CoreMinimal.h"

#include "CoreDObjectAPI.h"
#include "DObject/DObjectFwd.h"

namespace Durin
{
	class FSoftObjectPath;
}

namespace Durin::Private
{
	// Engine supplies asset routing and loading without an upward CoreDObject dependency.
	// Resolve must check access even on a cache hit and must never perform file I/O.
	// The provider owns its code until it clears these owner-thread operations.
	struct FSoftObjectPathOperations
	{
		DObject* (*Resolve)(const FSoftObjectPath&, const DClass*, DObject* Cached) = nullptr;
		DObject* (*Load)(const FSoftObjectPath&, const DClass*) = nullptr;
	};

	COREDOBJECT_API auto SetSoftObjectPathOperations(FSoftObjectPathOperations Operations) -> void;
	COREDOBJECT_API auto InvalidateSoftObjectCaches() -> void;
}
