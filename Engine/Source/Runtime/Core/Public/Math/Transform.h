#pragma once

#include "CoreAPI.h"
#include "Math/Vector.h"

namespace Durin
{
	// Represents a decomposed affine transform composed in scale, rotation, translation order.
	struct FTransform
	{
		FQuat Rotation;
		FVector3 Translation;
		FVector3 Scale3D;

		CORE_API FTransform();
		CORE_API auto ToMatrix() const -> FMatrix;

		CORE_API static auto Combine(const FTransform& Parent, const FTransform& Relative) -> FTransform;
		CORE_API static auto MakeRelative(const FTransform& World, const FTransform& Parent) -> FTransform;
	};
}
