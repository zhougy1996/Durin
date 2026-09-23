#pragma once

#include "Misc/Name.h"

namespace Durin::Detail
{
	// Immutable load-generation attribution; resources are owned by their modules.
	struct FModuleOwnerState
	{
		FName Name;
		uint64 Generation = 0;
	};
}
