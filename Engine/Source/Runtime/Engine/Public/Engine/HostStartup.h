#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	// Host readiness is independent of process-service initialization and profiling.
	enum class EHostStartupState : uint8 { Pending, Ready, Failed };
}
