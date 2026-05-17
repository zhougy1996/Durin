#include "Math/Transform.h"

namespace Durin
{
	FTransform::FTransform()
		: Rotation(FQuatConstants::Identity)
		, Translation(FVectorConstants::Zero)
		, Scale3D(FVectorConstants::Unit)
	{
	}
}

