#pragma once

#include "EngineAPI.h"

namespace Durin
{
	extern ENGINE_API float GAverageFPS;
	extern ENGINE_API float GAverageMS;

	ENGINE_API auto CalculateFPSTimings() -> void;
}
