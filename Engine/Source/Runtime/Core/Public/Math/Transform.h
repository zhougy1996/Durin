#pragma once

#include "CoreAPI.h"
#include "Math/Vector.h"

namespace Durin
{
	struct FTransform
	{
		FQuat Rotation;
		FVector3 Translation;
		FVector3 Scale3D;

		CORE_API FTransform();
		CORE_API auto ToMatrix() const -> FMatrix;
	};
}
