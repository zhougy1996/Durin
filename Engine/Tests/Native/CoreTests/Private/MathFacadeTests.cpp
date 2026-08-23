#include "Math/DurinMath.h"

#include <gtest/gtest.h>

#include <concepts>
#include <limits>

namespace
{
	template<typename TVector>
	auto ExpectVectorNear(const TVector& Actual, const TVector& Expected, double Tolerance) -> void
	{
		for (uint32 Index = 0; Index < Actual.length(); ++Index)
		{
			EXPECT_NEAR(Actual[Index], Expected[Index], Tolerance);
		}
	}

	auto ExpectIdentity(const Durin::FMatrix& Matrix, double Tolerance = 1.e-10) -> void
	{
		for (uint32 Column = 0; Column < 4; ++Column)
		{
			for (uint32 Row = 0; Row < 4; ++Row)
			{
				EXPECT_NEAR(Matrix[Column][Row], Column == Row ? 1.0 : 0.0, Tolerance);
			}
		}
	}

	static_assert(std::same_as<decltype(Durin::Math::Dot(Durin::FVector3f{}, Durin::FVector3f{})), float>);
	static_assert(std::same_as<decltype(Durin::Math::Dot(Durin::FVector3d{}, Durin::FVector3d{})), double>);
	static_assert(std::same_as<decltype(Durin::Math::Length(Durin::FVector2f{})), float>);
	static_assert(std::same_as<decltype(Durin::Math::Length(Durin::FVector2d{})), double>);
	static_assert(std::same_as<decltype(Durin::Math::Normalize(Durin::FVector4f{})), Durin::FVector4f>);
	static_assert(std::same_as<decltype(Durin::Math::Normalize(Durin::FVector4d{})), Durin::FVector4d>);
	static_assert(std::same_as<decltype(Durin::Math::DegreesToRadians(1.0f)), float>);
	static_assert(std::same_as<decltype(Durin::Math::DegreesToRadians(1.0)), double>);
	static_assert(std::same_as<decltype(Durin::Math::Inverse(Durin::FMatrix{})), Durin::FMatrix>);
	static_assert(std::same_as<decltype(Durin::Math::TransposeToFloat(Durin::FMatrix{})), Durin::FMatrix4f>);
	static_assert(std::same_as<decltype(Durin::Math::Scale(
		Durin::FMatrix4f{}, Durin::FVector3f{})), Durin::FMatrix4f>);
}

TEST(FMathFacadeTests, PreservesVectorPrecisionAndOrientation)
{
	const Durin::FVector3d Left(1.0, 2.0, 3.0);
	const Durin::FVector3d Right(4.0, -5.0, 6.0);
	EXPECT_DOUBLE_EQ(Durin::Math::Dot(Left, Right), 12.0);
	ExpectVectorNear(Durin::Math::Cross(Durin::FVector3d(1.0, 0.0, 0.0), Durin::FVector3d(0.0, 1.0, 0.0)),
		Durin::FVector3d(0.0, 0.0, 1.0), 0.0);
	EXPECT_DOUBLE_EQ(Durin::Math::Length(Durin::FVector2d(3.0, 4.0)), 5.0);
	EXPECT_FLOAT_EQ(Durin::Math::LengthSquared(Durin::FVector3f(1.0f, 2.0f, 2.0f)), 9.0f);
	ExpectVectorNear(Durin::Math::Lerp(Durin::FVector3d(0.0), Durin::FVector3d(4.0, 8.0, 12.0), 0.25),
		Durin::FVector3d(1.0, 2.0, 3.0), 0.0);
	ExpectVectorNear(Durin::Math::Clamp(Durin::FVector3d(-2.0, 0.5, 4.0), Durin::FVector3d(0.0), Durin::FVector3d(1.0)),
		Durin::FVector3d(0.0, 0.5, 1.0), 0.0);
	ExpectVectorNear(Durin::Math::Floor(Durin::FVector3d(-1.25, 2.75, 3.0)),
		Durin::FVector3d(-2.0, 2.0, 3.0), 0.0);
	EXPECT_TRUE(Durin::Math::AnyGreaterThan(
		Durin::FVector3d(1.0, 4.0, 2.0), Durin::FVector3d(1.0, 3.0, 5.0)));
	EXPECT_FALSE(Durin::Math::AnyGreaterThan(Durin::FVector3d(1.0), Durin::FVector3d(1.0)));
}

TEST(FMathFacadeTests, RejectsInvalidAndNearZeroNormalizationWithoutMutatingOutput)
{
	Durin::FVector3d Output(7.0, 8.0, 9.0);
	EXPECT_FALSE(Durin::Math::TryNormalize(Durin::FVector3d(-0.0, 0.0, 0.0), Output));
	EXPECT_EQ(Output, Durin::FVector3d(7.0, 8.0, 9.0));
	EXPECT_FALSE(Durin::Math::TryNormalize(Durin::FVector3d(1.e-5, 0.0, 0.0), Output, 1.e-8));
	EXPECT_EQ(Output, Durin::FVector3d(7.0, 8.0, 9.0));

	Durin::FVector3d Invalid(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0);
	EXPECT_FALSE(Durin::Math::TryNormalize(Invalid, Output));
	Invalid = Durin::FVector3d(std::numeric_limits<double>::infinity(), 0.0, 0.0);
	EXPECT_FALSE(Durin::Math::TryNormalize(Invalid, Output));
	EXPECT_EQ(Output, Durin::FVector3d(7.0, 8.0, 9.0));

	ASSERT_TRUE(Durin::Math::TryNormalize(Durin::FVector3d(3.0, 4.0, 0.0), Output));
	ExpectVectorNear(Output, Durin::FVector3d(0.6, 0.8, 0.0), 1.e-12);
	Durin::FVector3f FloatOutput(0.0f);
	ASSERT_TRUE(Durin::Math::TryNormalize(Durin::FVector3f(0.0f, 3.0f, 4.0f), FloatOutput));
	ExpectVectorNear(FloatOutput, Durin::FVector3f(0.0f, 0.6f, 0.8f), 1.e-6);
	EXPECT_EQ(Durin::Math::NormalizeOr(Durin::FVector3d(0.0), Durin::FVectorConstants::Up),
		Durin::FVectorConstants::Up);
}

TEST(FMathFacadeTests, UsesExplicitAngleUnitsAndQuaternionSignEquivalence)
{
	EXPECT_NEAR(Durin::Math::DegreesToRadians(180.0), Durin::Math::Pi<double>(), 1.e-15);
	EXPECT_NEAR(Durin::Math::RadiansToDegrees(Durin::Math::HalfPi<double>()), 90.0, 1.e-13);
	EXPECT_FLOAT_EQ(Durin::Math::TwoPi<float>(), Durin::Math::Pi<float>() * 2.0f);

	const Durin::FQuat QuarterTurn = Durin::Math::MakeQuaternionFromAxisAngleDegrees(
		90.0, Durin::FVectorConstants::Up);
	ExpectVectorNear(Durin::Math::RotateVector(QuarterTurn, Durin::FVectorConstants::Forward),
		Durin::FVectorConstants::Right, 1.e-12);
	EXPECT_TRUE(Durin::Math::AreRotationsEquivalent(QuarterTurn, -QuarterTurn));
	EXPECT_FALSE(Durin::Math::AreRotationsEquivalent(Durin::FQuat(0.0, 0.0, 0.0, 0.0), QuarterTurn));
	Durin::FQuat QuaternionOutput = QuarterTurn;
	EXPECT_FALSE(Durin::Math::TryNormalize(
		Durin::FQuat(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0), QuaternionOutput));
	EXPECT_EQ(QuaternionOutput, QuarterTurn);
	EXPECT_FALSE(Durin::Math::TryNormalize(
		Durin::FQuat(std::numeric_limits<double>::infinity(), 0.0, 0.0, 0.0), QuaternionOutput));
	EXPECT_EQ(QuaternionOutput, QuarterTurn);

	const Durin::FQuat EulerRotation = Durin::Math::MakeQuaternionFromEulerDegrees(Durin::FVector3(10.0, 20.0, 30.0));
	const Durin::FQuat RoundTrip = Durin::Math::MakeQuaternionFromEulerDegrees(
		Durin::Math::QuaternionToEulerDegrees(EulerRotation));
	EXPECT_TRUE(Durin::Math::AreRotationsEquivalent(EulerRotation, RoundTrip, 1.e-10));
	EXPECT_TRUE(Durin::Math::AreRotationsEquivalent(
		QuarterTurn * Durin::Math::Inverse(QuarterTurn), Durin::FQuatConstants::Identity));
}

TEST(FMathFacadeTests, PreservesColumnMajorTransformOrder)
{
	Durin::FMatrix Matrix(1.0);
	Matrix = Durin::Math::Translate(Matrix, Durin::FVector3(10.0, 20.0, 30.0));
	Matrix = Durin::Math::RotateDegrees(Matrix, 90.0, Durin::FVectorConstants::Up);
	Matrix = Durin::Math::Scale(Matrix, Durin::FVector3(2.0, 3.0, 4.0));
	const Durin::FVector4 Transformed = Matrix * Durin::FVector4(1.0, 0.0, 0.0, 1.0);
	ExpectVectorNear(Transformed, Durin::FVector4(10.0, 22.0, 30.0, 1.0), 1.e-12);
	EXPECT_DOUBLE_EQ(Matrix[3][0], 10.0);
	EXPECT_DOUBLE_EQ(Matrix[3][1], 20.0);
	EXPECT_DOUBLE_EQ(Matrix[3][2], 30.0);
	EXPECT_DOUBLE_EQ(Durin::Math::LinearDeterminant(Matrix), 24.0);

	Durin::FMatrix Inverse(7.0);
	ASSERT_TRUE(Durin::Math::TryInverse(Matrix, Inverse));
	ExpectIdentity(Matrix * Inverse);
	ExpectIdentity(Durin::Math::Transpose(Durin::Math::Transpose(Matrix)) * Inverse);
}

TEST(FMathFacadeTests, TransposesWhileNarrowingMatrixPrecision)
{
	Durin::FMatrix Matrix(0.0);
	for (uint32 Column = 0; Column < 4; ++Column)
	{
		for (uint32 Row = 0; Row < 4; ++Row)
		{
			Matrix[Column][Row] = static_cast<double>(Column * 4 + Row) + 0.25;
		}
	}

	const Durin::FMatrix4f Result = Durin::Math::TransposeToFloat(Matrix);
	for (uint32 Column = 0; Column < 4; ++Column)
	{
		for (uint32 Row = 0; Row < 4; ++Row)
		{
			EXPECT_FLOAT_EQ(Result[Column][Row],
				static_cast<float>(Matrix[Row][Column]));
		}
	}
}

TEST(FMathFacadeTests, RejectsInvalidMatrixInverseWithoutMutatingOutput)
{
	const Durin::FMatrix Sentinel(7.0);
	Durin::FMatrix Output = Sentinel;
	EXPECT_FALSE(Durin::Math::TryInverse(Durin::FMatrix(0.0), Output));
	EXPECT_EQ(Output, Sentinel);

	Durin::FMatrix Invalid(1.0);
	Invalid[2][1] = std::numeric_limits<double>::infinity();
	EXPECT_FALSE(Durin::Math::TryInverse(Invalid, Output));
	EXPECT_EQ(Output, Sentinel);
	Invalid[2][1] = std::numeric_limits<double>::quiet_NaN();
	EXPECT_FALSE(Durin::Math::TryInverse(Invalid, Output));
	EXPECT_EQ(Output, Sentinel);
}
