#include "Math/Transform.h"


FTransform::FTransform()
	: Rotation(FQuatConstants::Identity)
	, Translation(FVectorConstants::Zero)
	, Scale3D(FVectorConstants::Unit)
{
}

