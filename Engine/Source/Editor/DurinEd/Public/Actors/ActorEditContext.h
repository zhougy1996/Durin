#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	// Document owners and native adapters must state editability explicitly.
	struct FActorEditContext
	{
		bool bReadOnly;
		FActorEditContext(bool bInReadOnly) : bReadOnly(bInReadOnly) {}
	};
}
