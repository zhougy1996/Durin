#include "Math/Transform.h"

namespace Doge
{
	FTransform::FTransform()
		: Rotation(FQuatConstants::Identity)
		, Translation(FVectorConstants::Zero)
		, Scale3D(FVectorConstants::Unit)
	{
	}
}

