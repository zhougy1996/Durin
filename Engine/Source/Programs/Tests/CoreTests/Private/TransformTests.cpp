#include "Math/Transform.h"
#include "Math/TransformDecomposition.h"

#include <gtest/gtest.h>

namespace
{
	auto ExpectVectorNear(const Durin::FVector3& Actual, const Durin::FVector3& Expected, double Tolerance = 1.e-8) -> void
	{
		EXPECT_NEAR(Actual.x, Expected.x, Tolerance);
		EXPECT_NEAR(Actual.y, Expected.y, Tolerance);
		EXPECT_NEAR(Actual.z, Expected.z, Tolerance);
	}

	auto ExpectRotationNear(const Durin::FQuat& Actual, const Durin::FQuat& Expected, double Tolerance = 1.e-8) -> void
	{
		EXPECT_NEAR(std::abs(glm::dot(Actual, Expected)), 1.0, Tolerance);
	}
}

TEST(FTransformTests, CombinesParentAndRelativeTransforms)
{
	Durin::FTransform Parent;
	Parent.Translation = Durin::FVector3(10.0, -2.0, 3.0);
	Parent.Rotation = glm::angleAxis(glm::radians(90.0), Durin::FVector3(0.0, 0.0, 1.0));
	Parent.Scale3D = Durin::FVector3(2.0, 3.0, 4.0);

	Durin::FTransform Relative;
	Relative.Translation = Durin::FVector3(1.0, 2.0, -1.0);
	Relative.Rotation = glm::angleAxis(glm::radians(30.0), Durin::FVector3(0.0, 1.0, 0.0));
	Relative.Scale3D = Durin::FVector3(0.5, 2.0, 0.25);

	const Durin::FTransform World = Durin::FTransform::Combine(Parent, Relative);

	ExpectVectorNear(World.Translation, Durin::FVector3(4.0, 0.0, -1.0));
	ExpectVectorNear(World.Scale3D, Durin::FVector3(1.0, 6.0, 1.0));
	ExpectRotationNear(World.Rotation, glm::normalize(Parent.Rotation * Relative.Rotation));
}

TEST(FTransformTests, SafelyDecomposesFiniteTransformMatrix)
{
	Durin::FTransform Expected;
	Expected.Translation = {3.0, -4.0, 5.0};
	Expected.Rotation = glm::angleAxis(glm::radians(37.0), glm::normalize(Durin::FVector3(1.0, 2.0, 3.0)));
	Expected.Scale3D = {2.0, 3.0, 4.0};
	Durin::FTransform Actual;
	ASSERT_TRUE(Durin::TryMakeTransformFromMatrix(Expected.ToMatrix(), Actual));
	ExpectVectorNear(Actual.Translation, Expected.Translation);
	ExpectVectorNear(Actual.Scale3D, Expected.Scale3D);
	ExpectRotationNear(Actual.Rotation, Expected.Rotation);

	Durin::FMatrix Invalid(1.0);
	Invalid[0][0] = std::numeric_limits<double>::quiet_NaN();
	EXPECT_FALSE(Durin::TryMakeTransformFromMatrix(Invalid, Actual));
}

TEST(FTransformTests, RelativeTransformRoundTripsThroughParent)
{
	Durin::FTransform Parent;
	Parent.Translation = Durin::FVector3(-4.0, 8.0, 2.0);
	Parent.Rotation = glm::angleAxis(glm::radians(-35.0), glm::normalize(Durin::FVector3(1.0, 2.0, 3.0)));
	Parent.Scale3D = Durin::FVector3(2.0, 2.0, 2.0);

	Durin::FTransform World;
	World.Translation = Durin::FVector3(3.0, -7.0, 11.0);
	World.Rotation = glm::angleAxis(glm::radians(70.0), glm::normalize(Durin::FVector3(-2.0, 1.0, 0.5)));
	World.Scale3D = Durin::FVector3(4.0, 6.0, 8.0);

	const Durin::FTransform Relative = Durin::FTransform::MakeRelative(World, Parent);
	const Durin::FTransform Reconstructed = Durin::FTransform::Combine(Parent, Relative);

	ExpectVectorNear(Reconstructed.Translation, World.Translation);
	ExpectVectorNear(Reconstructed.Scale3D, World.Scale3D);
	ExpectRotationNear(Reconstructed.Rotation, World.Rotation);
}

TEST(FTransformTests, ZeroParentScaleProducesFiniteRelativeTransform)
{
	Durin::FTransform Parent;
	Parent.Scale3D = Durin::FVector3(0.0, 2.0, 0.0);

	Durin::FTransform World;
	World.Translation = Durin::FVector3(5.0, 6.0, 7.0);
	World.Scale3D = Durin::FVector3(3.0, 4.0, 5.0);

	const Durin::FTransform Relative = Durin::FTransform::MakeRelative(World, Parent);

	for (Durin::uint32 Axis = 0; Axis < 3; ++Axis)
	{
		EXPECT_TRUE(std::isfinite(Relative.Translation[Axis]));
		EXPECT_TRUE(std::isfinite(Relative.Scale3D[Axis]));
	}
}
