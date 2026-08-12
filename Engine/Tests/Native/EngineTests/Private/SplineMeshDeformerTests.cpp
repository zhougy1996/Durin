#include "Math/Operations.h"
#include "Spline/SplineMeshDeformer.h"

#include <gtest/gtest.h>

#include <random>

namespace
{
	using namespace Durin;

	auto ExpectVectorNear(const FVector3& Actual, const FVector3& Expected, double Tolerance = 1.e-8) -> void
	{
		EXPECT_NEAR(Actual.x, Expected.x, Tolerance);
		EXPECT_NEAR(Actual.y, Expected.y, Tolerance);
		EXPECT_NEAR(Actual.z, Expected.z, Tolerance);
	}

	auto MakePoint(const FVector3& Position, const FVector3& Tangent) -> FSplinePoint
	{
		FSplinePoint Point(Position);
		Point.TangentMode = ESplineTangentMode::ManualBroken;
		Point.ArriveTangent = Tangent;
		Point.LeaveTangent = Tangent;
		return Point;
	}
}

TEST(FSplineMeshDeformerTests, StraightIdentityAndAllForwardAxesMatchFrozenMapping)
{
	for (const auto [Axis, Forward, Up] : std::array{
		std::tuple{ESplineMeshAxis::X, FVector3(100.0, 0.0, 0.0), FVector3(0.0, 0.0, 1.0)},
		std::tuple{ESplineMeshAxis::Y, FVector3(0.0, 100.0, 0.0), FVector3(1.0, 0.0, 0.0)},
		std::tuple{ESplineMeshAxis::Z, FVector3(0.0, 0.0, 100.0), FVector3(0.0, 1.0, 0.0)}})
	{
		FSplineMeshParams Params;
		Params.ForwardAxis = Axis;
		Params.EndPosition = Forward;
		Params.StartTangent = Forward;
		Params.EndTangent = Forward;
		Params.SplineUpDirection = Up;
		const FVector3 Source = Axis == ESplineMeshAxis::X ? FVector3(25.0, 2.0, 3.0)
			: Axis == ESplineMeshAxis::Y ? FVector3(3.0, 25.0, 2.0) : FVector3(2.0, 3.0, 25.0);
		ExpectVectorNear(FSplineMeshDeformer::DeformPosition(Params, Source), Source);
	}
}

TEST(FSplineMeshDeformerTests, GoldenCurvedTwistedScaledAndOffsetSamplesAreFiniteAndExactAtEndpoints)
{
	FSplineMeshParams Params;
	Params.StartPosition = {2.0, 3.0, 4.0};
	Params.StartTangent = {40.0, 20.0, 0.0};
	Params.EndPosition = {60.0, 30.0, 10.0};
	Params.EndTangent = {20.0, -10.0, 15.0};
	Params.StartScale = {2.0, 0.5};
	Params.EndScale = {-1.0, 3.0};
	Params.StartOffset = {1.0, -2.0};
	Params.EndOffset = {-4.0, 5.0};
	Params.EndRollRadians = Math::HalfPi<double>();
	Params.Interpolation = ESplineMeshInterpolation::SmoothStep;
	const FSplineMeshSample Start = FSplineMeshDeformer::Evaluate(Params, 0.0);
	const FSplineMeshSample End = FSplineMeshDeformer::Evaluate(Params, 1.0);
	ExpectVectorNear(Start.Position, Params.StartPosition);
	ExpectVectorNear(Start.Derivative, Params.StartTangent);
	ExpectVectorNear(End.Position, Params.EndPosition);
	ExpectVectorNear(End.Derivative, Params.EndTangent);
	EXPECT_EQ(Start.Scale, Params.StartScale);
	EXPECT_EQ(End.Scale, Params.EndScale);
	for (int Step = 0; Step <= 100; ++Step)
	{
		const FSplineMeshSample Sample = FSplineMeshDeformer::Evaluate(Params, Step / 100.0);
		EXPECT_TRUE(Math::IsFinite(Sample.Position));
		EXPECT_TRUE(Math::IsFinite(Sample.Frame.Forward));
		EXPECT_NEAR(Math::Length(Sample.Frame.Forward), 1.0, 1.e-8);
		EXPECT_NEAR(Math::Dot(Sample.Frame.Forward, Sample.Frame.Side), 0.0, 1.e-8);
		ExpectVectorNear(Math::Cross(Sample.Frame.Forward, Sample.Frame.Side), Sample.Frame.Up);
	}
}

TEST(FSplineMeshDeformerTests, DegenerateUpAndDerivativeUseDeterministicFiniteFallbacks)
{
	FSplineMeshParams Params;
	Params.StartPosition = Params.EndPosition = {4.0, 5.0, 6.0};
	Params.StartTangent = Params.EndTangent = FVectorConstants::Zero;
	Params.SplineUpDirection = FVectorConstants::Zero;
	FSplineMeshParams Normalized;
	std::string Error;
	ASSERT_TRUE(FSplineMeshDeformer::Normalize(Params, Normalized, &Error)) << Error;
	const FSplineMeshSample Sample = FSplineMeshDeformer::Evaluate(Normalized, 0.5);
	EXPECT_TRUE(Math::IsFinite(Sample.Frame.Forward));
	EXPECT_NEAR(Math::Length(Sample.Frame.Forward), 1.0, 1.e-8);
	EXPECT_NEAR(Math::Length(Sample.Frame.Up), 1.0, 1.e-8);
	Params.SplineUpDirection = Params.StartTangent = Params.EndTangent = {100.0, 0.0, 0.0};
	EXPECT_TRUE(FSplineMeshDeformer::Normalize(Params, Normalized, &Error));
	EXPECT_TRUE(Math::IsFinite(FSplineMeshDeformer::Evaluate(Normalized, 0.5).Frame.Up));
}

TEST(FSplineMeshDeformerTests, TangentBasisPreservesIdentityAndReportsMirroredHandedness)
{
	FSplineMeshParams Params;
	const FSplineMeshTangentBasis Identity = FSplineMeshDeformer::DeformTangentBasis(
		Params, {50.0, 0.0, 0.0}, FVectorConstants::Up, FVectorConstants::Forward, 1.0);
	ExpectVectorNear(Identity.Normal, FVectorConstants::Up);
	ExpectVectorNear(Identity.Tangent, FVectorConstants::Forward);
	EXPECT_DOUBLE_EQ(Identity.Handedness, 1.0);
	Params.StartScale = Params.EndScale = {-1.0, 1.0};
	const FSplineMeshTangentBasis Mirrored = FSplineMeshDeformer::DeformTangentBasis(
		Params, {50.0, 0.0, 0.0}, FVectorConstants::Up, FVectorConstants::Forward, 1.0);
	EXPECT_DOUBLE_EQ(Mirrored.Handedness, -1.0);
}

TEST(FSplineMeshDeformerTests, NormalizationRejectsNonFiniteAndDegenerateForwardExtentAtomically)
{
	FSplineMeshParams Params;
	FSplineMeshParams Output;
	Output.EndPosition = {7.0, 8.0, 9.0};
	const FSplineMeshParams Original = Output;
	Params.StartRollRadians = std::numeric_limits<double>::infinity();
	EXPECT_FALSE(FSplineMeshDeformer::Normalize(Params, Output));
	EXPECT_EQ(Output, Original);
	Params.StartRollRadians = 0.0;
	Params.SourceForwardMax = Params.SourceForwardMin;
	EXPECT_FALSE(FSplineMeshDeformer::Normalize(Params, Output));
	EXPECT_EQ(Output, Original);
}

TEST(FSplineMeshDeformerTests, ConservativeBoundsContainDeterministicDeformedCorpus)
{
	FSplineMeshParams Params;
	Params.StartTangent = {50.0, 100.0, 20.0};
	Params.EndPosition = {100.0, 20.0, -10.0};
	Params.EndTangent = {30.0, -80.0, 40.0};
	Params.StartScale = {2.0, -1.0};
	Params.EndScale = {-3.0, 4.0};
	Params.StartOffset = {-5.0, 7.0};
	Params.EndOffset = {11.0, -13.0};
	Params.EndRollRadians = 2.1;
	const FBox SourceBounds({0.0, -4.0, -6.0}, {100.0, 5.0, 8.0});
	const FBox Bounds = FSplineMeshDeformer::ComputeConservativeBounds(Params, SourceBounds);
	ASSERT_TRUE(Bounds.bIsValid);
	for (int Forward = 0; Forward <= 100; ++Forward)
		for (double Side : {-4.0, 5.0}) for (double Up : {-6.0, 8.0})
		{
			const FVector3 Point = FSplineMeshDeformer::DeformPosition(Params,
				{static_cast<double>(Forward), Side, Up});
			EXPECT_GE(Point.x + 1.e-8, Bounds.Min.x); EXPECT_LE(Point.x - 1.e-8, Bounds.Max.x);
			EXPECT_GE(Point.y + 1.e-8, Bounds.Min.y); EXPECT_LE(Point.y - 1.e-8, Bounds.Max.y);
			EXPECT_GE(Point.z + 1.e-8, Bounds.Min.z); EXPECT_LE(Point.z - 1.e-8, Bounds.Max.z);
		}
}

TEST(FSplineMeshDeformerTests, SeededFiniteCorpusIsRepeatableAndContained)
{
	std::mt19937_64 Random(0x445552494E53504Cull);
	std::uniform_real_distribution<double> Value(-100.0, 100.0);
	std::uniform_real_distribution<double> Unit(0.0, 1.0);
	for (int Case = 0; Case < 128; ++Case)
	{
		FSplineMeshParams Params;
		Params.StartPosition = {Value(Random), Value(Random), Value(Random)};
		Params.EndPosition = {Value(Random), Value(Random), Value(Random)};
		Params.StartTangent = {Value(Random), Value(Random), Value(Random)};
		Params.EndTangent = {Value(Random), Value(Random), Value(Random)};
		Params.StartScale = {Value(Random) / 20.0, Value(Random) / 20.0};
		Params.EndScale = {Value(Random) / 20.0, Value(Random) / 20.0};
		Params.StartOffset = {Value(Random) / 10.0, Value(Random) / 10.0};
		Params.EndOffset = {Value(Random) / 10.0, Value(Random) / 10.0};
		Params.StartRollRadians = Value(Random);
		Params.EndRollRadians = Value(Random);
		Params.SplineUpDirection = {Value(Random), Value(Random), Value(Random)};
		Params.ForwardAxis = static_cast<ESplineMeshAxis>(Case % 3);
		Params.Interpolation = Case % 2 == 0
			? ESplineMeshInterpolation::Linear : ESplineMeshInterpolation::SmoothStep;
		FSplineMeshParams Normalized;
		ASSERT_TRUE(FSplineMeshDeformer::Normalize(Params, Normalized));
		const FBox SourceBounds({0.0, -7.0, -9.0}, {100.0, 11.0, 13.0});
		const FBox Bounds = FSplineMeshDeformer::ComputeConservativeBounds(Normalized, SourceBounds);
		for (int SampleIndex = 0; SampleIndex < 16; ++SampleIndex)
		{
			FVector3 Source{Unit(Random) * 100.0, std::lerp(-7.0, 11.0, Unit(Random)),
				std::lerp(-9.0, 13.0, Unit(Random))};
			if (Normalized.ForwardAxis == ESplineMeshAxis::Y) Source = {Source.z, Source.x, Source.y};
			else if (Normalized.ForwardAxis == ESplineMeshAxis::Z) Source = {Source.y, Source.z, Source.x};
			const FVector3 Point = FSplineMeshDeformer::DeformPosition(Normalized, Source);
			EXPECT_TRUE(Math::IsFinite(Point));
			EXPECT_GE(Point.x + 1.e-8, Bounds.Min.x); EXPECT_LE(Point.x - 1.e-8, Bounds.Max.x);
			EXPECT_GE(Point.y + 1.e-8, Bounds.Min.y); EXPECT_LE(Point.y - 1.e-8, Bounds.Max.y);
			EXPECT_GE(Point.z + 1.e-8, Bounds.Min.z); EXPECT_LE(Point.z - 1.e-8, Bounds.Max.z);
		}
	}
}

TEST(FSplinePathFrameTests, OpenAndClosedFramesAreDeterministicOrthonormalAndSeamContinuous)
{
	FSplineCurve Curve;
	Curve.SetPoints({
		MakePoint({0.0, 0.0, 0.0}, {50.0, 0.0, 20.0}),
		MakePoint({100.0, 20.0, 30.0}, {30.0, 80.0, 0.0}),
		MakePoint({20.0, 120.0, 10.0}, {-70.0, 20.0, -20.0})});
	Curve.SetClosedLoop(true);
	const auto Evaluation = Curve.BuildEvaluationData();
	const FSplinePathFrameData First = FSplinePathFrameData::Build(*Evaluation);
	const FSplinePathFrameData Second = FSplinePathFrameData::Build(*Evaluation);
	ASSERT_EQ(First.GetSamples().size(), Second.GetSamples().size());
	ASSERT_GT(First.GetSamples().size(), 3u);
	for (size_t Index = 0; Index < First.GetSamples().size(); ++Index)
	{
		const auto& A = First.GetSamples()[Index].Frame;
		const auto& B = Second.GetSamples()[Index].Frame;
		ExpectVectorNear(A.Forward, B.Forward);
		ExpectVectorNear(A.Up, B.Up);
		EXPECT_NEAR(Math::Length(A.Up), 1.0, 1.e-8);
		ExpectVectorNear(Math::Cross(A.Forward, A.Side), A.Up);
	}
	ExpectVectorNear(First.GetSamples().front().Frame.Up, First.GetSamples().back().Frame.Up, 1.e-6);
}
