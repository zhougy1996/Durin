#include "Spline/SplineCurve.h"

#include <unordered_set>

namespace Durin
{
	namespace
	{
		constexpr double AbsoluteLengthError = 1.e-4;
		constexpr double RelativeLengthError = 1.e-5;
		constexpr double DegenerateChordEpsilon = 1.e-9;
		constexpr int32 MaximumSubdivisionDepth = 16;

		auto SafeNormalize(const FVector3& Value) -> FVector3
		{
			const double LengthSquared = glm::dot(Value, Value);
			return LengthSquared > DegenerateChordEpsilon * DegenerateChordEpsilon
				? Value / std::sqrt(LengthSquared) : FVectorConstants::Zero;
		}

		struct FAutomaticTangents
		{
			FVector3 Arrive{0.0};
			FVector3 Leave{0.0};
		};

		auto ComputeAutomaticTangents(const std::vector<FSplinePoint>& Points, uint32 PointIndex, bool bClosedLoop, bool bClamped)
			-> FAutomaticTangents
		{
			const uint32 PointCount = static_cast<uint32>(Points.size());
			if (PointCount < 2 || PointIndex >= PointCount) return {};
			if (!bClosedLoop && PointIndex == 0)
				return {.Leave = Points[1].Position - Points[0].Position};
			if (!bClosedLoop && PointIndex + 1 == PointCount)
				return {.Arrive = Points[PointIndex].Position - Points[PointIndex - 1].Position};

			const uint32 PreviousIndex = PointIndex == 0 ? PointCount - 1 : PointIndex - 1;
			const uint32 NextIndex = (PointIndex + 1) % PointCount;
			const FVector3 IncomingChord = Points[PointIndex].Position - Points[PreviousIndex].Position;
			const FVector3 OutgoingChord = Points[NextIndex].Position - Points[PointIndex].Position;
			const double IncomingLength = glm::length(IncomingChord);
			const double OutgoingLength = glm::length(OutgoingChord);
			if (IncomingLength <= DegenerateChordEpsilon && OutgoingLength <= DegenerateChordEpsilon) return {};
			if (IncomingLength <= DegenerateChordEpsilon) return {.Leave = OutgoingChord};
			if (OutgoingLength <= DegenerateChordEpsilon) return {.Arrive = IncomingChord};

			const FVector3 IncomingDirection = IncomingChord / IncomingLength;
			const FVector3 OutgoingDirection = OutgoingChord / OutgoingLength;
			FVector3 KnotDerivative = (IncomingDirection * OutgoingLength + OutgoingDirection * IncomingLength)
				/ (IncomingLength + OutgoingLength);
			if (bClamped && (glm::dot(KnotDerivative, IncomingDirection) <= 0.0
				|| glm::dot(KnotDerivative, OutgoingDirection) <= 0.0)) KnotDerivative = FVectorConstants::Zero;
			const double DerivativeMagnitude = glm::length(KnotDerivative);
			const FVector3 Direction = SafeNormalize(KnotDerivative);
			return {
				.Arrive = Direction * std::min(DerivativeMagnitude * IncomingLength, IncomingLength),
				.Leave = Direction * std::min(DerivativeMagnitude * OutgoingLength, OutgoingLength),
			};
		}

		auto GetManualTangents(const FSplinePoint& Point) -> FAutomaticTangents
		{
			if (Point.TangentMode != ESplineTangentMode::ManualAligned)
				return {.Arrive = Point.ArriveTangent, .Leave = Point.LeaveTangent};
			const double ArriveLength = glm::length(Point.ArriveTangent);
			const double LeaveLength = glm::length(Point.LeaveTangent);
			const FVector3 Direction = LeaveLength > DegenerateChordEpsilon
				? Point.LeaveTangent / LeaveLength : SafeNormalize(Point.ArriveTangent);
			return {.Arrive = Direction * ArriveLength, .Leave = Direction * LeaveLength};
		}

		auto EvaluateSegment(const FSplineEvaluationSegment& Segment, double T) -> FSplineSample
		{
			T = std::clamp(T, 0.0, 1.0);
			const double T2 = T * T;
			const FVector3 Position = Segment.Coefficient0 + Segment.Coefficient1 * T
				+ Segment.Coefficient2 * T2 + Segment.Coefficient3 * (T2 * T);
			const FVector3 FirstDerivative = Segment.Coefficient1 + Segment.Coefficient2 * (2.0 * T)
				+ Segment.Coefficient3 * (3.0 * T2);
			const FVector3 SecondDerivative = Segment.Coefficient2 * 2.0 + Segment.Coefficient3 * (6.0 * T);
			return {Position, FirstDerivative, SecondDerivative, SafeNormalize(FirstDerivative)};
		}

		auto AddAdaptiveSamples(FSplineEvaluationSegment& Segment, double StartT, const FVector3& Start,
			double EndT, const FVector3& End, double ErrorBudget, int32 Depth) -> void
		{
			const double MiddleT = (StartT + EndT) * 0.5;
			const FVector3 Middle = EvaluateSegment(Segment, MiddleT).Position;
			const double ChordLength = glm::length(End - Start);
			const double SplitLength = glm::length(Middle - Start) + glm::length(End - Middle);
			if (SplitLength - ChordLength <= ErrorBudget || Depth >= MaximumSubdivisionDepth)
			{
				auto AddSample = [&](double T, const FVector3& Position) {
					const FSplineDistanceSample& Previous = Segment.DistanceSamples.back();
					if (T <= Previous.T) return;
					const FVector3 PreviousPosition = EvaluateSegment(Segment, Previous.T).Position;
					Segment.LocalLength += glm::length(Position - PreviousPosition);
					Segment.DistanceSamples.push_back({T, Segment.LocalLength});
				};
				AddSample(MiddleT, Middle);
				AddSample(EndT, End);
				return;
			}
			AddAdaptiveSamples(Segment, StartT, Start, MiddleT, Middle, ErrorBudget * 0.5, Depth + 1);
			AddAdaptiveSamples(Segment, MiddleT, Middle, EndT, End, ErrorBudget * 0.5, Depth + 1);
		}

		auto DistanceSquared(const FVector3& Left, const FVector3& Right) -> double
		{
			const FVector3 Delta = Left - Right;
			return glm::dot(Delta, Delta);
		}

		auto DistanceSquaredToBox(const FVector3& Point, const FBox& Box) -> double
		{
			if (!Box.bIsValid) return 0.0;
			return DistanceSquared(Point, glm::clamp(Point, Box.Min, Box.Max));
		}
	} // namespace

	FSplinePoint::FSplinePoint()
		: Id(FGuid::NewGuid())
	{
	}

	FSplinePoint::FSplinePoint(const FVector3& InPosition)
		: Id(FGuid::NewGuid()), Position(InPosition)
	{
	}

	FSplineCurve::FSplineCurve()
	{
		Points.emplace_back(FVector3(0.0, 0.0, 0.0));
		Points.emplace_back(FVector3(100.0, 0.0, 0.0));
	}

	auto FSplineCurve::GetPoint(uint32 PointIndex) const -> const FSplinePoint*
	{
		return PointIndex < Points.size() ? &Points[PointIndex] : nullptr;
	}

	auto FSplineCurve::FindPointIndex(const FGuid& PointId) const -> std::optional<uint32>
	{
		for (uint32 Index = 0; Index < Points.size(); ++Index)
			if (Points[Index].Id == PointId) return Index;
		return std::nullopt;
	}

	auto FSplineCurve::GetNumSegments() const -> uint32
	{
		if (Points.size() < 2) return 0;
		return bClosedLoop ? static_cast<uint32>(Points.size()) : static_cast<uint32>(Points.size() - 1);
	}

	auto FSplineCurve::SetPoints(std::vector<FSplinePoint> InPoints) -> void
	{
		if (Points == InPoints) return;
		Points = std::move(InPoints);
		RepairPointIds();
	}

	auto FSplineCurve::AddPoint(FSplinePoint Point) -> uint32
	{
		Point.Id = FGuid::NewGuid();
		Points.push_back(std::move(Point));
		return static_cast<uint32>(Points.size() - 1);
	}

	auto FSplineCurve::InsertPoint(uint32 PointIndex, FSplinePoint Point) -> bool
	{
		if (PointIndex > Points.size()) return false;
		Point.Id = FGuid::NewGuid();
		Points.insert(Points.begin() + PointIndex, std::move(Point));
		return true;
	}

	auto FSplineCurve::DuplicatePoint(uint32 PointIndex) -> std::optional<uint32>
	{
		if (PointIndex >= Points.size()) return std::nullopt;
		FSplinePoint Duplicate = Points[PointIndex];
		Duplicate.Id = FGuid::NewGuid();
		const uint32 InsertIndex = PointIndex + 1;
		Points.insert(Points.begin() + InsertIndex, std::move(Duplicate));
		return InsertIndex;
	}

	auto FSplineCurve::UpdatePoint(uint32 PointIndex, FSplinePoint Point) -> bool
	{
		if (PointIndex >= Points.size()) return false;
		Point.Id = Points[PointIndex].Id;
		if (Points[PointIndex] == Point) return false;
		Points[PointIndex] = std::move(Point);
		return true;
	}

	auto FSplineCurve::RemovePoint(uint32 PointIndex) -> bool
	{
		if (PointIndex >= Points.size()) return false;
		Points.erase(Points.begin() + PointIndex);
		return true;
	}

	auto FSplineCurve::MovePoint(uint32 FromIndex, uint32 ToIndex) -> bool
	{
		if (FromIndex >= Points.size() || ToIndex >= Points.size()) return false;
		if (FromIndex == ToIndex) return false;
		FSplinePoint Point = std::move(Points[FromIndex]);
		Points.erase(Points.begin() + FromIndex);
		Points.insert(Points.begin() + ToIndex, std::move(Point));
		return true;
	}

	auto FSplineCurve::ClearPoints() -> void
	{
		Points.clear();
	}

	auto FSplineCurve::RepairPointIds() -> bool
	{
		bool bRepaired = false;
		std::unordered_set<FGuid> Seen;
		for (FSplinePoint& Point : Points)
		{
			if (!Point.Id.IsValid() || Seen.contains(Point.Id))
			{
				do Point.Id = FGuid::NewGuid(); while (Seen.contains(Point.Id));
				bRepaired = true;
			}
			Seen.insert(Point.Id);
		}
		return bRepaired;
	}

	auto FSplineCurve::SetClosedLoop(bool bInClosedLoop) -> void
	{
		bClosedLoop = bInClosedLoop;
	}

	auto FSplineCurve::BuildEvaluationData() const -> std::shared_ptr<const FSplineEvaluationData>
	{
		auto Result = std::make_shared<FSplineEvaluationData>();
		Result->bClosedLoop = bClosedLoop;
		Result->bHasPoint = !Points.empty();
		if (!Points.empty())
		{
			Result->SinglePoint = Points.front().Position;
			for (const FSplinePoint& Point : Points) Result->LocalBounds.AddPoint(Point.Position);
		}
		const uint32 SegmentCount = GetNumSegments();
		Result->Segments.reserve(SegmentCount);
		for (uint32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
		{
			const uint32 EndIndex = (SegmentIndex + 1) % static_cast<uint32>(Points.size());
			const FSplinePoint& Start = Points[SegmentIndex];
			const FSplinePoint& End = Points[EndIndex];
			FSplineEvaluationSegment Segment;
			Segment.Interpolation = Start.OutgoingInterpolation;
			Segment.Coefficient0 = Start.Position;
			if (Segment.Interpolation == ESplineSegmentInterpolation::Linear)
			{
				Segment.Coefficient1 = End.Position - Start.Position;
			}
			else
			{
				const bool bStartAutomatic = Start.TangentMode == ESplineTangentMode::Automatic
					|| Start.TangentMode == ESplineTangentMode::AutomaticClamped;
				const bool bEndAutomatic = End.TangentMode == ESplineTangentMode::Automatic
					|| End.TangentMode == ESplineTangentMode::AutomaticClamped;
				const FAutomaticTangents StartManual = GetManualTangents(Start);
				const FAutomaticTangents EndManual = GetManualTangents(End);
				const FVector3 StartTangent = bStartAutomatic
					? ComputeAutomaticTangents(Points, SegmentIndex, bClosedLoop,
						Start.TangentMode == ESplineTangentMode::AutomaticClamped).Leave
					: StartManual.Leave;
				const FVector3 EndTangent = bEndAutomatic
					? ComputeAutomaticTangents(Points, EndIndex, bClosedLoop,
						End.TangentMode == ESplineTangentMode::AutomaticClamped).Arrive
					: EndManual.Arrive;
				Segment.Coefficient1 = StartTangent;
				Segment.Coefficient2 = -3.0 * Start.Position - 2.0 * StartTangent + 3.0 * End.Position - EndTangent;
				Segment.Coefficient3 = 2.0 * Start.Position + StartTangent - 2.0 * End.Position + EndTangent;

				Segment.LocalBounds.AddPoint(Start.Position);
				Segment.LocalBounds.AddPoint(Start.Position + StartTangent / 3.0);
				Segment.LocalBounds.AddPoint(End.Position - EndTangent / 3.0);
				Segment.LocalBounds.AddPoint(End.Position);
			}
			if (Segment.Interpolation == ESplineSegmentInterpolation::Linear)
			{
				Segment.LocalBounds.AddPoint(Start.Position);
				Segment.LocalBounds.AddPoint(End.Position);
			}
			Segment.StartLocalDistance = Result->LocalLength;
			Segment.DistanceSamples.push_back({0.0, 0.0});
			const FVector3 Control0 = Segment.Coefficient0;
			const FVector3 Control1 = Segment.Coefficient0 + Segment.Coefficient1 / 3.0;
			const FVector3 Control2 = Segment.Coefficient0 + Segment.Coefficient1 * (2.0 / 3.0)
				+ Segment.Coefficient2 / 3.0;
			const FVector3 Control3 = Segment.Coefficient0 + Segment.Coefficient1
				+ Segment.Coefficient2 + Segment.Coefficient3;
			const double ControlPolygonLength = glm::length(Control1 - Control0)
				+ glm::length(Control2 - Control1) + glm::length(Control3 - Control2);
			const double ErrorBudget = std::max(AbsoluteLengthError, RelativeLengthError * ControlPolygonLength);
			if (Segment.Interpolation == ESplineSegmentInterpolation::Linear)
			{
				Segment.LocalLength = glm::length(End.Position - Start.Position);
				Segment.DistanceSamples.push_back({1.0, Segment.LocalLength});
			}
			else
			{
				AddAdaptiveSamples(Segment, 0.0, Start.Position, 1.0, End.Position, ErrorBudget, 0);
			}
			Result->LocalLength += Segment.LocalLength;
			Result->LocalBounds.AddPoint(Segment.LocalBounds.Min);
			Result->LocalBounds.AddPoint(Segment.LocalBounds.Max);
			Result->Segments.push_back(std::move(Segment));
		}
		return Result;
	}

	auto FSplineEvaluationData::ResolveParameter(FSplineParameter Parameter) const -> FSplineParameter
	{
		if (Segments.empty()) return {};
		if (!bClosedLoop)
		{
			Parameter.SegmentIndex = std::min(Parameter.SegmentIndex, static_cast<uint32>(Segments.size() - 1));
			Parameter.T = std::clamp(Parameter.T, 0.0, 1.0);
			return Parameter;
		}
		Parameter.SegmentIndex %= static_cast<uint32>(Segments.size());
		Parameter.T = std::clamp(Parameter.T, 0.0, 1.0);
		if (Parameter.T >= 1.0)
		{
			Parameter.SegmentIndex = (Parameter.SegmentIndex + 1) % static_cast<uint32>(Segments.size());
			Parameter.T = 0.0;
		}
		return Parameter;
	}

	auto FSplineEvaluationData::Evaluate(FSplineParameter Parameter) const -> FSplineSample
	{
		if (Segments.empty()) return {.Position = bHasPoint ? SinglePoint : FVectorConstants::Zero};
		Parameter = ResolveParameter(Parameter);
		return EvaluateSegment(Segments[Parameter.SegmentIndex], Parameter.T);
	}

	auto FSplineEvaluationData::GetLocalDistanceAtParameter(FSplineParameter Parameter) const -> double
	{
		if (Segments.empty()) return 0.0;
		if (bClosedLoop && Parameter.SegmentIndex + 1 == Segments.size() && Parameter.T >= 1.0) return LocalLength;
		Parameter = ResolveParameter(Parameter);
		const FSplineEvaluationSegment& Segment = Segments[Parameter.SegmentIndex];
		const auto Upper = std::lower_bound(Segment.DistanceSamples.begin(), Segment.DistanceSamples.end(), Parameter.T,
			[](const FSplineDistanceSample& Sample, double T) { return Sample.T < T; });
		if (Upper == Segment.DistanceSamples.begin()) return Segment.StartLocalDistance;
		if (Upper == Segment.DistanceSamples.end()) return Segment.StartLocalDistance + Segment.LocalLength;
		const FSplineDistanceSample& Previous = *std::prev(Upper);
		const double Alpha = (Parameter.T - Previous.T) / (Upper->T - Previous.T);
		return Segment.StartLocalDistance + std::lerp(Previous.LocalDistance, Upper->LocalDistance, Alpha);
	}

	auto FSplineEvaluationData::GetParameterAtLocalDistance(double Distance) const -> FSplineParameter
	{
		if (Segments.empty() || LocalLength <= DegenerateChordEpsilon) return {};
		if (bClosedLoop)
		{
			Distance = std::fmod(Distance, LocalLength);
			if (Distance < 0.0) Distance += LocalLength;
		}
		else Distance = std::clamp(Distance, 0.0, LocalLength);
		const auto SegmentIt = std::upper_bound(Segments.begin(), Segments.end(), Distance,
			[](double Value, const FSplineEvaluationSegment& Segment) {
				return Value < Segment.StartLocalDistance + Segment.LocalLength;
			});
		const uint32 SegmentIndex = SegmentIt == Segments.end()
			? static_cast<uint32>(Segments.size() - 1) : static_cast<uint32>(SegmentIt - Segments.begin());
		const FSplineEvaluationSegment& Segment = Segments[SegmentIndex];
		const double SegmentDistance = std::clamp(Distance - Segment.StartLocalDistance, 0.0, Segment.LocalLength);
		const auto Upper = std::lower_bound(Segment.DistanceSamples.begin(), Segment.DistanceSamples.end(), SegmentDistance,
			[](const FSplineDistanceSample& Sample, double Value) { return Sample.LocalDistance < Value; });
		if (Upper == Segment.DistanceSamples.begin()) return {SegmentIndex, 0.0};
		if (Upper == Segment.DistanceSamples.end()) return {SegmentIndex, 1.0};
		const FSplineDistanceSample& Previous = *std::prev(Upper);
		const double Range = Upper->LocalDistance - Previous.LocalDistance;
		const double Alpha = Range > DegenerateChordEpsilon ? (SegmentDistance - Previous.LocalDistance) / Range : 0.0;
		return {SegmentIndex, std::lerp(Previous.T, Upper->T, Alpha)};
	}

	auto FSplineEvaluationData::EvaluateAtLocalDistance(double LocalDistanceValue) const -> FSplineSample
	{
		return Evaluate(GetParameterAtLocalDistance(LocalDistanceValue));
	}

	auto FSplineEvaluationData::FindNearestParameter(const FVector3& LocalPosition) const -> FSplineParameter
	{
		if (Segments.empty()) return {};
		FSplineParameter Best{};
		double BestDistanceSquared = std::numeric_limits<double>::max();
		for (uint32 SegmentIndex = 0; SegmentIndex < Segments.size(); ++SegmentIndex)
		{
			const FSplineEvaluationSegment& Segment = Segments[SegmentIndex];
			if (DistanceSquaredToBox(LocalPosition, Segment.LocalBounds) > BestDistanceSquared) continue;
			auto Consider = [&](double CandidateT) {
				const double CandidateDistance = DistanceSquared(EvaluateSegment(Segment, CandidateT).Position, LocalPosition);
				if (CandidateDistance < BestDistanceSquared)
				{
					BestDistanceSquared = CandidateDistance;
					Best = {SegmentIndex, CandidateT};
				}
			};
			for (const FSplineDistanceSample& Sample : Segment.DistanceSamples) Consider(Sample.T);
			for (size_t SampleIndex = 1; SampleIndex < Segment.DistanceSamples.size(); ++SampleIndex)
			{
				double Left = Segment.DistanceSamples[SampleIndex - 1].T;
				double Right = Segment.DistanceSamples[SampleIndex].T;
				for (int32 Iteration = 0; Iteration < 48; ++Iteration)
				{
					const double First = std::lerp(Left, Right, 1.0 / 3.0);
					const double Second = std::lerp(Left, Right, 2.0 / 3.0);
					const double FirstDistance = DistanceSquared(EvaluateSegment(Segment, First).Position, LocalPosition);
					const double SecondDistance = DistanceSquared(EvaluateSegment(Segment, Second).Position, LocalPosition);
					if (FirstDistance <= SecondDistance) Right = Second;
					else Left = First;
				}
				Consider((Left + Right) * 0.5);
			}
		}
		return Best;
	}
} // namespace Durin
