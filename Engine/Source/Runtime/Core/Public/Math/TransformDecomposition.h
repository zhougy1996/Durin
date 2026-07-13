#pragma once

#include "CoreAPI.h"
#include "Math/Transform.h"

namespace Durin
{
	CORE_API auto TryMakeTransformFromMatrix(const FMatrix& Matrix, FTransform& OutTransform) -> bool;
}
