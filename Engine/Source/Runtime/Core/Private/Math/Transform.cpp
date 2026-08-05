#include "Math/Transform.h"
#include "Math/TransformDecomposition.h"
#include "Math/Operations.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace Durin
{
	namespace
	{
		auto SafeNormalize(const FQuat& Rotation) -> FQuat
		{
			return Math::LengthSquared(Rotation) > kSmallNumber
				? Math::Normalize(Rotation) : FQuatConstants::Identity;
		}

		auto SafeReciprocal(const FVector3& Value) -> FVector3
		{
			FVector3 Result(0.0);
			for (uint32 Axis = 0; Axis < 3; ++Axis)
			{
				if (std::abs(Value[Axis]) > kSmallNumber)
				{
					Result[Axis] = 1.0 / Value[Axis];
				}
			}
			return Result;
		}
	}

	FTransform::FTransform()
		: Rotation(FQuatConstants::Identity)
		, Translation(FVectorConstants::Zero)
		, Scale3D(FVectorConstants::Unit)
	{
	}

	auto FTransform::ToMatrix() const -> FMatrix
	{
		const FMatrix TranslationMatrix = Math::TranslationMatrix(Translation);
		const FMatrix RotationMatrix = Math::RotationMatrix(Rotation);
		const FMatrix ScaleMatrix = Math::ScaleMatrix(Scale3D);
		return TranslationMatrix * RotationMatrix * ScaleMatrix;
	}

	auto TryMakeTransformFromMatrix(const FMatrix& Matrix, FTransform& OutTransform) -> bool
	{
		for (uint32 Column = 0; Column < 4; ++Column)
		{
			for (uint32 Row = 0; Row < 4; ++Row)
			{
				if (!std::isfinite(Matrix[Column][Row])) return false;
			}
		}

		FVector3 Scale;
		FQuat Rotation;
		FVector3 Translation;
		FVector3 Skew;
		FVector4 Perspective;
		if (!glm::decompose(Matrix, Scale, Rotation, Translation, Skew, Perspective)) return false;
		if (!std::isfinite(Math::Length(Rotation)) || Math::LengthSquared(Rotation) <= kSmallNumber) return false;
		for (uint32 Axis = 0; Axis < 3; ++Axis)
		{
			if (!std::isfinite(Scale[Axis]) || !std::isfinite(Translation[Axis])) return false;
		}

		OutTransform.Translation = Translation;
		OutTransform.Rotation = Math::Normalize(Rotation);
		OutTransform.Scale3D = Scale;
		return true;
	}

	auto FTransform::Combine(const FTransform& Parent, const FTransform& Relative) -> FTransform
	{
		FTransform Result;
		const FQuat ParentRotation = SafeNormalize(Parent.Rotation);
		Result.Rotation = SafeNormalize(ParentRotation * SafeNormalize(Relative.Rotation));
		Result.Scale3D = Parent.Scale3D * Relative.Scale3D;
		Result.Translation = Parent.Translation + Math::RotateVector(
			ParentRotation, Parent.Scale3D * Relative.Translation);
		return Result;
	}

	auto FTransform::MakeRelative(const FTransform& World, const FTransform& Parent) -> FTransform
	{
		FTransform Result;
		const FQuat InverseParentRotation = Math::Inverse(SafeNormalize(Parent.Rotation));
		const FVector3 InverseParentScale = SafeReciprocal(Parent.Scale3D);
		Result.Rotation = SafeNormalize(InverseParentRotation * SafeNormalize(World.Rotation));
		Result.Scale3D = World.Scale3D * InverseParentScale;
		Result.Translation = Math::RotateVector(
			InverseParentRotation, World.Translation - Parent.Translation) * InverseParentScale;
		return Result;
	}
}
