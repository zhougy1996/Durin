#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace
{
	struct FVectorFixture
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;

		auto operator+(const FVectorFixture& Other) const -> FVectorFixture
		{
			return {X + Other.X, Y + Other.Y, Z + Other.Z};
		}

		auto operator-(const FVectorFixture& Other) const -> FVectorFixture
		{
			return {X - Other.X, Y - Other.Y, Z - Other.Z};
		}

		auto operator*(double Scalar) const -> FVectorFixture
		{
			return {X * Scalar, Y * Scalar, Z * Scalar};
		}
	};

	struct FHermiteFixture
	{
		FVectorFixture Start;
		FVectorFixture StartTangent;
		FVectorFixture End;
		FVectorFixture EndTangent;
	};

	struct FSampleFixture
	{
		double T = 0.0;
		FVectorFixture Position;
		FVectorFixture FirstDerivative;
		FVectorFixture SecondDerivative;
	};

	struct FAutomaticTangents
	{
		FVectorFixture Arrive;
		FVectorFixture Leave;
	};

	constexpr double AbsoluteLengthError = 1.e-4;
	constexpr double RelativeLengthError = 1.e-5;
	constexpr int MaximumSubdivisionDepth = 16;
	constexpr double DegenerateChordEpsilon = 1.e-9;

	auto Dot(const FVectorFixture& Left, const FVectorFixture& Right) -> double
	{
		return Left.X * Right.X + Left.Y * Right.Y + Left.Z * Right.Z;
	}

	auto Length(const FVectorFixture& Value) -> double
	{
		return std::sqrt(Dot(Value, Value));
	}

	auto Normalize(const FVectorFixture& Value) -> FVectorFixture
	{
		const double ValueLength = Length(Value);
		return ValueLength > DegenerateChordEpsilon ? Value * (1.0 / ValueLength) : FVectorFixture{};
	}

	auto ExpectVectorNear(const FVectorFixture& Actual, const FVectorFixture& Expected, double Tolerance = 1.e-10) -> void
	{
		EXPECT_NEAR(Actual.X, Expected.X, Tolerance);
		EXPECT_NEAR(Actual.Y, Expected.Y, Tolerance);
		EXPECT_NEAR(Actual.Z, Expected.Z, Tolerance);
	}

	auto EvaluateHermite(const FHermiteFixture& Curve, double T) -> FSampleFixture
	{
		const double T2 = T * T;
		const double T3 = T2 * T;
		return {
			.T = T,
			.Position = Curve.Start * (2.0 * T3 - 3.0 * T2 + 1.0)
				+ Curve.StartTangent * (T3 - 2.0 * T2 + T)
				+ Curve.End * (-2.0 * T3 + 3.0 * T2)
				+ Curve.EndTangent * (T3 - T2),
			.FirstDerivative = Curve.Start * (6.0 * T2 - 6.0 * T)
				+ Curve.StartTangent * (3.0 * T2 - 4.0 * T + 1.0)
				+ Curve.End * (-6.0 * T2 + 6.0 * T)
				+ Curve.EndTangent * (3.0 * T2 - 2.0 * T),
			.SecondDerivative = Curve.Start * (12.0 * T - 6.0)
				+ Curve.StartTangent * (6.0 * T - 4.0)
				+ Curve.End * (-12.0 * T + 6.0)
				+ Curve.EndTangent * (6.0 * T - 2.0),
		};
	}

	auto ComputeAutomaticTangents(
		const FVectorFixture& Previous,
		const FVectorFixture& Point,
		const FVectorFixture& Next,
		bool bClamped) -> FAutomaticTangents
	{
		const FVectorFixture IncomingChord = Point - Previous;
		const FVectorFixture OutgoingChord = Next - Point;
		const double IncomingLength = Length(IncomingChord);
		const double OutgoingLength = Length(OutgoingChord);

		if (IncomingLength <= DegenerateChordEpsilon && OutgoingLength <= DegenerateChordEpsilon)
			return {};
		if (IncomingLength <= DegenerateChordEpsilon)
			return {.Arrive = {}, .Leave = OutgoingChord};
		if (OutgoingLength <= DegenerateChordEpsilon)
			return {.Arrive = IncomingChord, .Leave = {}};

		const FVectorFixture IncomingDirection = IncomingChord * (1.0 / IncomingLength);
		const FVectorFixture OutgoingDirection = OutgoingChord * (1.0 / OutgoingLength);
		FVectorFixture KnotDerivative =
			(IncomingDirection * OutgoingLength + OutgoingDirection * IncomingLength)
			* (1.0 / (IncomingLength + OutgoingLength));

		if (bClamped
			&& (Dot(KnotDerivative, IncomingDirection) <= 0.0
				|| Dot(KnotDerivative, OutgoingDirection) <= 0.0))
		{
			KnotDerivative = {};
		}

		const FVectorFixture Direction = Normalize(KnotDerivative);
		const double DerivativeMagnitude = Length(KnotDerivative);
		return {
			.Arrive = Direction * std::min(DerivativeMagnitude * IncomingLength, IncomingLength),
			.Leave = Direction * std::min(DerivativeMagnitude * OutgoingLength, OutgoingLength),
		};
	}

	auto ControlPolygonLength(const FHermiteFixture& Curve) -> double
	{
		const FVectorFixture Control1 = Curve.Start + Curve.StartTangent * (1.0 / 3.0);
		const FVectorFixture Control2 = Curve.End - Curve.EndTangent * (1.0 / 3.0);
		return Length(Control1 - Curve.Start) + Length(Control2 - Control1) + Length(Curve.End - Control2);
	}

	struct FAdaptiveResult
	{
		double Length = 0.0;
		int LeafCount = 0;
		bool bReachedMaximumDepth = false;
	};

	auto AccumulateAdaptiveLength(
		const FHermiteFixture& Curve,
		double StartT,
		const FVectorFixture& Start,
		double EndT,
		const FVectorFixture& End,
		double ErrorBudget,
		int Depth) -> FAdaptiveResult
	{
		const double MiddleT = (StartT + EndT) * 0.5;
		const FVectorFixture Middle = EvaluateHermite(Curve, MiddleT).Position;
		const double ChordLength = Length(End - Start);
		const double SplitLength = Length(Middle - Start) + Length(End - Middle);
		if (SplitLength - ChordLength <= ErrorBudget || Depth >= MaximumSubdivisionDepth)
		{
			return {
				.Length = SplitLength,
				.LeafCount = 1,
				.bReachedMaximumDepth = Depth >= MaximumSubdivisionDepth,
			};
		}

		const FAdaptiveResult Left = AccumulateAdaptiveLength(
			Curve, StartT, Start, MiddleT, Middle, ErrorBudget * 0.5, Depth + 1);
		const FAdaptiveResult Right = AccumulateAdaptiveLength(
			Curve, MiddleT, Middle, EndT, End, ErrorBudget * 0.5, Depth + 1);
		return {
			.Length = Left.Length + Right.Length,
			.LeafCount = Left.LeafCount + Right.LeafCount,
			.bReachedMaximumDepth = Left.bReachedMaximumDepth || Right.bReachedMaximumDepth,
		};
	}

	auto ComputeAdaptiveLength(const FHermiteFixture& Curve) -> FAdaptiveResult
	{
		const double ErrorBudget = std::max(AbsoluteLengthError, RelativeLengthError * ControlPolygonLength(Curve));
		return AccumulateAdaptiveLength(Curve, 0.0, Curve.Start, 1.0, Curve.End, ErrorBudget, 0);
	}

	auto ComputeDenseReferenceLength(const FHermiteFixture& Curve) -> double
	{
		constexpr std::size_t StepCount = 1u << 18u;
		double Result = 0.0;
		FVectorFixture Previous = Curve.Start;
		for (std::size_t Step = 1; Step <= StepCount; ++Step)
		{
			const FVectorFixture Current = EvaluateHermite(Curve, static_cast<double>(Step) / StepCount).Position;
			Result += Length(Current - Previous);
			Previous = Current;
		}
		return Result;
	}
} // namespace

TEST(FSplineV2ContractTests, LinearAndManualCubicGoldenSamplesAreExact)
{
	const FHermiteFixture ManualCurve{
		.Start = {0.0, 0.0, 0.0},
		.StartTangent = {12.0, 3.0, 0.0},
		.End = {10.0, 10.0, 0.0},
		.EndTangent = {-2.0, 9.0, 0.0},
	};
	const std::array<FSampleFixture, 5> GoldenSamples{{
		{.T = 0.0, .Position = {0.0, 0.0, 0.0}, .FirstDerivative = {12.0, 3.0, 0.0}, .SecondDerivative = {16.0, 30.0, 0.0}},
		{.T = 0.25, .Position = {3.34375, 1.5625, 0.0}, .FirstDerivative = {14.125, 9.0, 0.0}, .SecondDerivative = {1.0, 18.0, 0.0}},
		{.T = 0.5, .Position = {6.75, 4.25, 0.0}, .FirstDerivative = {12.5, 12.0, 0.0}, .SecondDerivative = {-14.0, 6.0, 0.0}},
		{.T = 0.75, .Position = {9.28125, 7.3125, 0.0}, .FirstDerivative = {7.125, 12.0, 0.0}, .SecondDerivative = {-29.0, -6.0, 0.0}},
		{.T = 1.0, .Position = {10.0, 10.0, 0.0}, .FirstDerivative = {-2.0, 9.0, 0.0}, .SecondDerivative = {-44.0, -18.0, 0.0}},
	}};

	for (const FSampleFixture& Golden : GoldenSamples)
	{
		const FSampleFixture Actual = EvaluateHermite(ManualCurve, Golden.T);
		ExpectVectorNear(Actual.Position, Golden.Position);
		ExpectVectorNear(Actual.FirstDerivative, Golden.FirstDerivative);
		ExpectVectorNear(Actual.SecondDerivative, Golden.SecondDerivative);
	}

	const FVectorFixture LinearStart{-4.0, 2.0, 8.0};
	const FVectorFixture LinearEnd{12.0, 10.0, -4.0};
	const double T = 0.375;
	ExpectVectorNear(LinearStart * (1.0 - T) + LinearEnd * T, {2.0, 5.0, 3.5});
	ExpectVectorNear(LinearEnd - LinearStart, {16.0, 8.0, -12.0});

	const FHermiteFixture ZeroTangentCurve{
		.Start = {0.0, 0.0, 0.0},
		.StartTangent = {},
		.End = {8.0, 0.0, 0.0},
		.EndTangent = {},
	};
	const FSampleFixture ZeroTangentMiddle = EvaluateHermite(ZeroTangentCurve, 0.5);
	ExpectVectorNear(ZeroTangentMiddle.Position, {4.0, 0.0, 0.0});
	ExpectVectorNear(EvaluateHermite(ZeroTangentCurve, 0.0).FirstDerivative, {});
	ExpectVectorNear(EvaluateHermite(ZeroTangentCurve, 1.0).FirstDerivative, {});
}

TEST(FSplineV2ContractTests, AutomaticTangentsUseChordLengthKnotsAndDeterministicDegenerates)
{
	const FAutomaticTangents Interior = ComputeAutomaticTangents(
		{0.0, 0.0, 0.0}, {3.0, 4.0, 0.0}, {9.0, 4.0, 0.0}, false);
	ExpectVectorNear(Interior.Arrive, {3.909090909090909, 2.181818181818182, 0.0});
	ExpectVectorNear(Interior.Leave, {4.690909090909091, 2.618181818181818, 0.0});

	const FAutomaticTangents DuplicatePrevious = ComputeAutomaticTangents(
		{3.0, 4.0, 0.0}, {3.0, 4.0, 0.0}, {9.0, 4.0, 0.0}, false);
	ExpectVectorNear(DuplicatePrevious.Arrive, {});
	ExpectVectorNear(DuplicatePrevious.Leave, {6.0, 0.0, 0.0});

	const FAutomaticTangents AllDuplicate = ComputeAutomaticTangents(
		{3.0, 4.0, 0.0}, {3.0, 4.0, 0.0}, {3.0, 4.0, 0.0}, false);
	ExpectVectorNear(AllDuplicate.Arrive, {});
	ExpectVectorNear(AllDuplicate.Leave, {});

	const FVectorFixture OpenStartTangent = FVectorFixture{3.0, 4.0, 0.0} - FVectorFixture{0.0, 0.0, 0.0};
	const FVectorFixture OpenEndTangent = FVectorFixture{9.0, 4.0, 0.0} - FVectorFixture{3.0, 4.0, 0.0};
	ExpectVectorNear(OpenStartTangent, {3.0, 4.0, 0.0});
	ExpectVectorNear(OpenEndTangent, {6.0, 0.0, 0.0});
}

TEST(FSplineV2ContractTests, AutomaticClampingStopsReversalAndBoundsEachHandle)
{
	const FVectorFixture Previous{0.0, 0.0, 0.0};
	const FVectorFixture Point{10.0, 0.0, 0.0};
	const FVectorFixture Next{9.0, 0.1, 0.0};
	const FAutomaticTangents Clamped = ComputeAutomaticTangents(Previous, Point, Next, true);
	ExpectVectorNear(Clamped.Arrive, {});
	ExpectVectorNear(Clamped.Leave, {});

	const FAutomaticTangents Smooth = ComputeAutomaticTangents(
		{0.0, 0.0, 0.0}, {3.0, 4.0, 0.0}, {9.0, 4.0, 0.0}, true);
	EXPECT_GT(Dot(Smooth.Arrive, {3.0, 4.0, 0.0}), 0.0);
	EXPECT_GT(Dot(Smooth.Leave, {6.0, 0.0, 0.0}), 0.0);
	EXPECT_LE(Length(Smooth.Arrive), 5.0);
	EXPECT_LE(Length(Smooth.Leave), 6.0);
}

TEST(FSplineV2ContractTests, ManualAlignedAndBrokenModesHaveExplicitCoupling)
{
	const FVectorFixture PreviousArrive{6.0, 0.0, 0.0};
	const FVectorFixture EditedLeave{0.0, 2.0, 0.0};
	const FVectorFixture AlignedArrive = Normalize(EditedLeave) * Length(PreviousArrive);
	ExpectVectorNear(AlignedArrive, {0.0, 6.0, 0.0});
	ExpectVectorNear(EditedLeave, {0.0, 2.0, 0.0});

	const FVectorFixture BrokenArrive = PreviousArrive;
	ExpectVectorNear(BrokenArrive, {6.0, 0.0, 0.0});
	ExpectVectorNear(EditedLeave, {0.0, 2.0, 0.0});
}

TEST(FSplineV2ContractTests, AdaptiveDefaultsMeetDenseReferenceErrorBudget)
{
	const std::array<FHermiteFixture, 4> Curves{{
		{{0.0, 0.0, 0.0}, {12.0, 3.0, 0.0}, {10.0, 10.0, 0.0}, {-2.0, 9.0, 0.0}},
		{{0.0, 0.0, 0.0}, {0.0, 1000.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, -1000.0, 0.0}},
		{{-25.0, 4.0, 3.0}, {140.0, -80.0, 35.0}, {60.0, 45.0, -10.0}, {-120.0, 95.0, 20.0}},
		{{0.0, 0.0, 0.0}, {}, {0.0, 0.0, 0.0}, {}},
	}};

	for (const FHermiteFixture& Curve : Curves)
	{
		const FAdaptiveResult Adaptive = ComputeAdaptiveLength(Curve);
		const double Reference = ComputeDenseReferenceLength(Curve);
		const double ErrorBudget = std::max(AbsoluteLengthError, RelativeLengthError * ControlPolygonLength(Curve));
		EXPECT_LE(std::abs(Adaptive.Length - Reference), ErrorBudget);
		EXPECT_FALSE(Adaptive.bReachedMaximumDepth);
		EXPECT_GE(Adaptive.LeafCount, 1);
	}
}

TEST(FSplineV2ContractTests, EmptySinglePointAndClosedSeamPoliciesAreDeterministic)
{
	const FVectorFixture Neutral{};
	const FVectorFixture SinglePoint{4.0, 5.0, 6.0};
	ExpectVectorNear(Neutral, {});
	ExpectVectorNear(SinglePoint, {4.0, 5.0, 6.0});
	EXPECT_DOUBLE_EQ(0.0, 0.0);

	const auto WrapClosedParameter = [](double Parameter, double SegmentCount) {
		double Wrapped = std::fmod(Parameter, SegmentCount);
		if (Wrapped < 0.0)
			Wrapped += SegmentCount;
		return Wrapped;
	};
	EXPECT_DOUBLE_EQ(WrapClosedParameter(4.0, 4.0), 0.0);
	EXPECT_DOUBLE_EQ(WrapClosedParameter(-0.5, 4.0), 3.5);
	EXPECT_DOUBLE_EQ(WrapClosedParameter(4.5, 4.0), 0.5);
}
