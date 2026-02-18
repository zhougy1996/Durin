#pragma once

struct FTransform
{
	FQuat Rotation;
	FVector3 Translation;
	FVector3 Scale3D;

	CORE_API FTransform();
};