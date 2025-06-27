#pragma once

struct FTransform
{
	FQuat Rotation;
	FVector Translation;
	FVector Scale3D;

	CORE_API FTransform();
};