#include "Math/Transform.h"

namespace Durin
{
	FTransform::FTransform()
		: Rotation(glm::identity<FQuat>())
		, Translation(FVectorConstants::Zero)
		, Scale3D(FVectorConstants::Unit)
	{
	}

	auto FTransform::ToMatrix() const -> FMatrix
	{
		const FMatrix TranslationMatrix = glm::translate(FMatrix(1.0), Translation);
		const FMatrix RotationMatrix = glm::mat4_cast(Rotation);
		const FMatrix ScaleMatrix = glm::scale(FMatrix(1.0), Scale3D);
		return TranslationMatrix * RotationMatrix * ScaleMatrix;
	}
}
