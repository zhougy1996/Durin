#include "Spline/SplineMeshDeformer.h"

#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		constexpr double FrameEpsilon = 1.e-12;

		auto AxisVector(ESplineMeshAxis Axis) -> FVector3
		{
			switch (Axis)
			{
			case ESplineMeshAxis::Y: return {0.0, 1.0, 0.0};
			case ESplineMeshAxis::Z: return {0.0, 0.0, 1.0};
			default: return {1.0, 0.0, 0.0};
			}
		}

		auto SourceCoordinates(const FVector3& Value, ESplineMeshAxis Axis) -> FVector3
		{
			switch (Axis)
			{
			case ESplineMeshAxis::Y: return {Value.y, Value.z, Value.x};
			case ESplineMeshAxis::Z: return {Value.z, Value.x, Value.y};
			default: return Value;
			}
		}

		auto SafeDirection(const FVector3& Value, const FVector3& Fallback) -> FVector3
		{
			return Math::NormalizeOr(Value, Fallback, FrameEpsilon);
		}

		auto LeastAlignedCardinal(const FVector3& Direction) -> FVector3
		{
			const FVector3 Absolute = Math::Abs(Direction);
			if (Absolute.x <= Absolute.y && Absolute.x <= Absolute.z) return {1.0, 0.0, 0.0};
			if (Absolute.y <= Absolute.z) return {0.0, 1.0, 0.0};
			return {0.0, 0.0, 1.0};
		}

		auto MakeFrame(const FVector3& Derivative, const FVector3& Chord,
			const FVector3& RequestedUp, ESplineMeshAxis Axis, double Roll) -> FSplineMeshFrame
		{
			const FVector3 Forward = SafeDirection(Derivative,
				SafeDirection(Chord, AxisVector(Axis)));
			FVector3 UpCandidate = RequestedUp - Forward * Math::Dot(RequestedUp, Forward);
			if (!Math::TryNormalize(UpCandidate, UpCandidate, FrameEpsilon))
			{
				const FVector3 Cardinal = LeastAlignedCardinal(Forward);
				UpCandidate = SafeDirection(Cardinal - Forward * Math::Dot(Cardinal, Forward), FVectorConstants::Up);
			}
			const FVector3 Side = SafeDirection(Math::Cross(UpCandidate, Forward), FVectorConstants::Right);
			const FVector3 Up = SafeDirection(Math::Cross(Forward, Side), UpCandidate);
			const double CosRoll = std::cos(Roll);
			const double SinRoll = std::sin(Roll);
			return {Forward, Side * CosRoll + Up * SinRoll, Up * CosRoll - Side * SinRoll};
		}

		auto InterpolationAlpha(ESplineMeshInterpolation Interpolation, double T) -> double
		{
			T = std::clamp(T, 0.0, 1.0);
			return Interpolation == ESplineMeshInterpolation::SmoothStep ? T * T * (3.0 - 2.0 * T) : T;
		}

		auto RotateAroundAxis(const FVector3& Value, const FVector3& Axis, double Angle) -> FVector3
		{
			return Value * std::cos(Angle) + Math::Cross(Axis, Value) * std::sin(Angle)
				+ Axis * Math::Dot(Axis, Value) * (1.0 - std::cos(Angle));
		}

		auto TransportFrame(const FSplineMeshFrame& Previous, const FVector3& NewForward) -> FSplineMeshFrame
		{
			const double Dot = std::clamp(Math::Dot(Previous.Forward, NewForward), -1.0, 1.0);
			FVector3 Axis = Math::Cross(Previous.Forward, NewForward);
			const double AxisLength = Math::Length(Axis);
			if (AxisLength <= FrameEpsilon)
			{
				if (Dot >= 0.0) return {NewForward, Previous.Side, Previous.Up};
				Axis = SafeDirection(Math::Cross(Previous.Forward, LeastAlignedCardinal(Previous.Forward)), Previous.Up);
			}
			else Axis /= AxisLength;
			const double Angle = std::atan2(AxisLength, Dot);
			const FVector3 Side = SafeDirection(RotateAroundAxis(Previous.Side, Axis, Angle), Previous.Side);
			return {NewForward, Side, SafeDirection(Math::Cross(NewForward, Side), Previous.Up)};
		}
	}

	auto FSplineMeshDeformer::Normalize(const FSplineMeshParams& Params,
		FSplineMeshParams& OutParams, std::string* OutError) -> bool
	{
		if (Params.ForwardAxis != ESplineMeshAxis::X && Params.ForwardAxis != ESplineMeshAxis::Y
			&& Params.ForwardAxis != ESplineMeshAxis::Z)
			return Fail("SplineMesh forward axis is invalid.", OutError);
		if (Params.Interpolation != ESplineMeshInterpolation::Linear
			&& Params.Interpolation != ESplineMeshInterpolation::SmoothStep)
			return Fail("SplineMesh interpolation policy is invalid.", OutError);
		if (!Math::IsFinite(Params.StartPosition) || !Math::IsFinite(Params.StartTangent)
			|| !Math::IsFinite(Params.EndPosition) || !Math::IsFinite(Params.EndTangent)
			|| !Math::IsFinite(Params.StartScale) || !Math::IsFinite(Params.EndScale)
			|| !Math::IsFinite(Params.StartOffset) || !Math::IsFinite(Params.EndOffset)
			|| !Math::IsFinite(Params.SplineUpDirection) || !std::isfinite(Params.StartRollRadians)
			|| !std::isfinite(Params.EndRollRadians) || !std::isfinite(Params.SourceForwardMin)
			|| !std::isfinite(Params.SourceForwardMax))
			return Fail("SplineMesh parameters must be finite.", OutError);
		if (Params.SourceForwardMax - Params.SourceForwardMin <= FrameEpsilon)
			return Fail("SplineMesh canonical forward extent must be positive and non-degenerate.", OutError);
		FSplineMeshParams Candidate = Params;
		Candidate.SplineUpDirection = SafeDirection(Params.SplineUpDirection, FVectorConstants::Up);
		OutParams = Candidate;
		if (OutError) OutError->clear();
		return true;
	}

	auto FSplineMeshDeformer::Evaluate(const FSplineMeshParams& Params, double T) -> FSplineMeshSample
	{
		T = std::clamp(T, 0.0, 1.0);
		const double T2 = T * T;
		const double T3 = T2 * T;
		const double H00 = 2.0 * T3 - 3.0 * T2 + 1.0;
		const double H10 = T3 - 2.0 * T2 + T;
		const double H01 = -2.0 * T3 + 3.0 * T2;
		const double H11 = T3 - T2;
		const FVector3 Position = Params.StartPosition * H00 + Params.StartTangent * H10
			+ Params.EndPosition * H01 + Params.EndTangent * H11;
		const FVector3 Derivative = Params.StartPosition * (6.0 * T2 - 6.0 * T)
			+ Params.StartTangent * (3.0 * T2 - 4.0 * T + 1.0)
			+ Params.EndPosition * (-6.0 * T2 + 6.0 * T)
			+ Params.EndTangent * (3.0 * T2 - 2.0 * T);
		const double Alpha = InterpolationAlpha(Params.Interpolation, T);
		const FVector2 Scale = Math::Lerp(Params.StartScale, Params.EndScale, Alpha);
		const FVector2 Offset = Math::Lerp(Params.StartOffset, Params.EndOffset, Alpha);
		const double Roll = std::lerp(Params.StartRollRadians, Params.EndRollRadians, Alpha);
		return {Position, Derivative, Scale, Offset, Roll,
			MakeFrame(Derivative, Params.EndPosition - Params.StartPosition,
				Params.SplineUpDirection, Params.ForwardAxis, Roll)};
	}

	auto FSplineMeshDeformer::DeformPosition(const FSplineMeshParams& Params,
		const FVector3& SourcePosition) -> FVector3
	{
		const FVector3 Source = SourceCoordinates(SourcePosition, Params.ForwardAxis);
		const double T = std::clamp((Source.x - Params.SourceForwardMin)
			/ (Params.SourceForwardMax - Params.SourceForwardMin), 0.0, 1.0);
		const FSplineMeshSample Sample = Evaluate(Params, T);
		return Sample.Position + Sample.Frame.Side * (Source.y * Sample.Scale.x + Sample.Offset.x)
			+ Sample.Frame.Up * (Source.z * Sample.Scale.y + Sample.Offset.y);
	}

	auto FSplineMeshDeformer::DeformDirection(const FSplineMeshParams& Params,
		const FVector3& SourcePosition, const FVector3& SourceDirection) -> FVector3
	{
		const FVector3 Source = SourceCoordinates(SourcePosition, Params.ForwardAxis);
		const FVector3 Direction = SourceCoordinates(SourceDirection, Params.ForwardAxis);
		const double T = std::clamp((Source.x - Params.SourceForwardMin)
			/ (Params.SourceForwardMax - Params.SourceForwardMin), 0.0, 1.0);
		const FSplineMeshSample Sample = Evaluate(Params, T);
		return SafeDirection(Sample.Frame.Forward * Direction.x
			+ Sample.Frame.Side * (Direction.y * Sample.Scale.x)
			+ Sample.Frame.Up * (Direction.z * Sample.Scale.y), Sample.Frame.Forward);
	}

	auto FSplineMeshDeformer::DeformNormal(const FSplineMeshParams& Params,
		const FVector3& SourcePosition, const FVector3& SourceNormal) -> FVector3
	{
		const FVector3 Source = SourceCoordinates(SourcePosition, Params.ForwardAxis);
		const FVector3 Normal = SourceCoordinates(SourceNormal, Params.ForwardAxis);
		const double T = std::clamp((Source.x - Params.SourceForwardMin)
			/ (Params.SourceForwardMax - Params.SourceForwardMin), 0.0, 1.0);
		const FSplineMeshSample Sample = Evaluate(Params, T);
		const double Side = std::abs(Sample.Scale.x) > FrameEpsilon ? Normal.y / Sample.Scale.x : 0.0;
		const double Up = std::abs(Sample.Scale.y) > FrameEpsilon ? Normal.z / Sample.Scale.y : 0.0;
		return SafeDirection(Sample.Frame.Forward * Normal.x + Sample.Frame.Side * Side
			+ Sample.Frame.Up * Up, Sample.Frame.Up);
	}

	auto FSplineMeshDeformer::DeformTangentBasis(const FSplineMeshParams& Params,
		const FVector3& SourcePosition, const FVector3& SourceNormal,
		const FVector3& SourceTangent, double SourceHandedness) -> FSplineMeshTangentBasis
	{
		const FVector3 Normal = DeformNormal(Params, SourcePosition, SourceNormal);
		FVector3 Tangent = DeformDirection(Params, SourcePosition, SourceTangent);
		Tangent = SafeDirection(Tangent - Normal * Math::Dot(Normal, Tangent),
			SafeDirection(Math::Cross(LeastAlignedCardinal(Normal), Normal), FVectorConstants::Forward));
		const double Handedness = SourceHandedness < 0.0 ? -1.0 : 1.0;
		const FVector3 SourceBitangent = Math::Cross(SourceNormal, SourceTangent) * Handedness;
		const FVector3 Bitangent = DeformDirection(Params, SourcePosition, SourceBitangent);
		return {Normal, Tangent, Math::Dot(Math::Cross(Normal, Tangent), Bitangent) < 0.0 ? -1.0 : 1.0};
	}

	auto FSplineMeshDeformer::ComputeConservativeBounds(const FSplineMeshParams& Params,
		const FBox& CanonicalSourceBounds) -> FBox
	{
		if (!CanonicalSourceBounds.bIsValid) return {};
		FBox Result;
		Result.AddPoint(Params.StartPosition);
		Result.AddPoint(Params.StartPosition + Params.StartTangent / 3.0);
		Result.AddPoint(Params.EndPosition - Params.EndTangent / 3.0);
		Result.AddPoint(Params.EndPosition);
		const FVector3 SourceMin = SourceCoordinates(CanonicalSourceBounds.Min, Params.ForwardAxis);
		const FVector3 SourceMax = SourceCoordinates(CanonicalSourceBounds.Max, Params.ForwardAxis);
		const double Cross0 = std::max(std::abs(SourceMin.y), std::abs(SourceMax.y));
		const double Cross1 = std::max(std::abs(SourceMin.z), std::abs(SourceMax.z));
		const double Scale0 = std::max(std::abs(Params.StartScale.x), std::abs(Params.EndScale.x));
		const double Scale1 = std::max(std::abs(Params.StartScale.y), std::abs(Params.EndScale.y));
		const double Offset0 = std::max(std::abs(Params.StartOffset.x), std::abs(Params.EndOffset.x));
		const double Offset1 = std::max(std::abs(Params.StartOffset.y), std::abs(Params.EndOffset.y));
		const double Radius = std::hypot(Cross0 * Scale0 + Offset0, Cross1 * Scale1 + Offset1);
		Result.Min -= FVector3(Radius);
		Result.Max += FVector3(Radius);
		return Result;
	}

	auto FSplinePathFrameData::Build(const FSplineEvaluationData& Evaluation,
		const FVector3& SeedUp) -> FSplinePathFrameData
	{
		FSplinePathFrameData Result;
		const auto& Segments = Evaluation.GetSegments();
		if (Segments.empty()) return Result;
		for (uint32 SegmentIndex = 0; SegmentIndex < Segments.size(); ++SegmentIndex)
		{
			const auto& Segment = Segments[SegmentIndex];
			for (size_t SampleIndex = 0; SampleIndex < Segment.DistanceSamples.size(); ++SampleIndex)
			{
				if (SegmentIndex > 0 && SampleIndex == 0) continue;
				const auto& DistanceSample = Segment.DistanceSamples[SampleIndex];
				const FSplineParameter Parameter{SegmentIndex, DistanceSample.T};
				const FSplineSample SplineSample = Evaluation.Evaluate(Parameter);
				const FVector3 PreviousForward = Result.Samples.empty()
					? FVectorConstants::Forward : Result.Samples.back().Frame.Forward;
				const FVector3 Forward = SafeDirection(SplineSample.FirstDerivative, PreviousForward);
				const FSplineMeshFrame Frame = Result.Samples.empty()
					? MakeFrame(Forward, Forward, SafeDirection(SeedUp, FVectorConstants::Up), ESplineMeshAxis::X, 0.0)
					: TransportFrame(Result.Samples.back().Frame, Forward);
				Result.Samples.push_back({Parameter,
					Segment.StartLocalDistance + DistanceSample.LocalDistance, Frame});
			}
		}
		if (Evaluation.IsClosedLoop() && Result.Samples.size() > 1 && Evaluation.GetLocalLength() > FrameEpsilon)
		{
			const FSplineMeshFrame& First = Result.Samples.front().Frame;
			const FSplineMeshFrame& Last = Result.Samples.back().Frame;
			const double Correction = std::atan2(Math::Dot(First.Forward, Math::Cross(Last.Up, First.Up)),
				Math::Dot(Last.Up, First.Up));
			for (FSplinePathFrameSample& Sample : Result.Samples)
			{
				const double Angle = Correction * (Sample.LocalDistance / Evaluation.GetLocalLength());
				Sample.Frame.Side = SafeDirection(RotateAroundAxis(Sample.Frame.Side, Sample.Frame.Forward, Angle), Sample.Frame.Side);
				Sample.Frame.Up = SafeDirection(Math::Cross(Sample.Frame.Forward, Sample.Frame.Side), Sample.Frame.Up);
			}
		}
		return Result;
	}
} // namespace Durin
