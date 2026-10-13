#pragma once

#include "CoreMinimal.h"

namespace Durin::DerivedData
{
	// Common completion status for cache, build and asset compilation requests.
	enum class EStatus : uint8 { Ok, Error, Canceled };
}
