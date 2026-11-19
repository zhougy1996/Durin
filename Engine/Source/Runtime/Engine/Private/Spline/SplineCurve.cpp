#include "Spline/SplineCurve.h"

#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		constexpr double AbsoluteLengthError = 1.e-4;
		constexpr double RelativeLengthError = 1.e-5;
		constexpr double DegenerateChordEpsilon = 1.e-9;
		constexpr double MinimumTangentKeySpan = 1.e-4;
		constexpr int32 MaximumSubdivisionDepth = 16;

		auto SafeNormalize(const FVector3& Value) -> FVector3
		{
			const double LengthSquared = Math::LengthSquared(Value);
			return LengthSquared > DegenerateChordEpsilon * DegenerateChordEpsilon
				? Value / std::sqrt(LengthSquared) : FVectorConstants::Zero;
		}

		struct FAutomaticTangents
		{
			FVector3 Arrive{0.0};
			FVector3 Leave{0.0};
		};

		auto ClampAutomaticTangent(double Incoming, double Outgoing, double PreviousSpan, double NextSpan) -> double
		{
			if ((Incoming <= 0.0 && Outgoing >= 0.0) || (Incoming >= 0.0 && Outgoing <= 0.0)) return 0.0;
			constexpr double Threshold = 0.333;
			const double Alpha = Incoming / (Incoming + Outgoing);
			const double Unclamped = (Incoming + Outgoing) / std::max(PreviousSpan + NextSpan, MinimumTangentKeySpan);
			double Limit = Unclamped;
			if (Alpha < Threshold)
				Limit = std::lerp(Unclamped, Incoming / std::max(PreviousSpan, 1.e-4), 1.0 - Alpha / Threshold);
			else if (Alpha > 1.0 - Threshold)
				Limit = std::lerp(Unclamped, Outgoing / std::max(NextSpan, 1.e-4), (Alpha - (1.0 - Threshold)) / Threshold);
			return Incoming > 0.0 ? std::min(Unclamped, Limit) : std::max(Unclamped, Limit);
		}

		auto GetManualTangents(const FSplinePoint& Point) -> FAutomaticTangents
		{
			if (Point.TangentMode != ESplineTangentMode::ManualAligned)
				return {.Arrive = Point.ArriveTangent, .Leave = Point.LeaveTangent};
			const double ArriveLength = Math::Length(Point.ArriveTangent);
			const double LeaveLength = Math::Length(Point.LeaveTangent);
			const FVector3 Direction = LeaveLength > DegenerateChordEpsilon
				? Point.LeaveTangent / LeaveLength : SafeNormalize(Point.ArriveTangent);
			return {.Arrive = Direction * ArriveLength, .Leave = Direction * LeaveLength};
		}

		auto BuildPointTangents(const std::vector<FSplinePoint>& Points, bool bClosedLoop,
			bool bStationaryEndpoints, double LoopKey) -> std::vector<FAutomaticTangents>
		{
			std::vector<FAutomaticTangents> Tangents;
			Tangents.reserve(Points.size());
			for (const auto& Point : Points) Tangents.push_back(GetManualTangents(Point));
			for (size_t Index = 0; Index < Points.size(); ++Index)
			{
				const bool bFirst = Index == 0;
				const bool bLast = Index + 1 == Points.size();
				const size_t PreviousIndex = bFirst ? (bClosedLoop ? Points.size() - 1 : Index) : Index - 1;
				const auto& Previous = Points[PreviousIndex];
				const auto& Point = Points[Index];
				const auto& Next = Points[bLast ? (bClosedLoop ? 0 : Index) : Index + 1];
				const FVector3 Incoming = Point.Position - Previous.Position;
				const FVector3 Outgoing = Next.Position - Point.Position;
				const double SeamSpan = LoopKey - Points.back().InputKey;
				const double PreviousSpan = bFirst && bClosedLoop ? SeamSpan : Point.InputKey - Previous.InputKey;
				const double NextSpan = bLast && bClosedLoop ? SeamSpan : Next.InputKey - Point.InputKey;
				if (Point.OutgoingInterpolation == ESplineSegmentInterpolation::Linear)
				{
					Tangents[Index] = {Previous.OutgoingInterpolation == ESplineSegmentInterpolation::Cubic ? Outgoing : Incoming, Outgoing};
				}
				else if (Point.OutgoingInterpolation == ESplineSegmentInterpolation::Constant)
				{
					Tangents[Index] = {};
				}
				else if (Point.TangentMode == ESplineTangentMode::Automatic || Point.TangentMode == ESplineTangentMode::AutomaticClamped)
				{
					if (bStationaryEndpoints && (bFirst || (bLast && !bClosedLoop)))
					{
						Tangents[Index] = {};
						continue;
					}
					if (Previous.OutgoingInterpolation != ESplineSegmentInterpolation::Cubic)
					{
						Tangents[Index] = Previous.OutgoingInterpolation == ESplineSegmentInterpolation::Constant
							? FAutomaticTangents{} : Tangents[PreviousIndex];
						continue;
					}
					FVector3 Tangent = (Incoming + Outgoing) / std::max(PreviousSpan + NextSpan, MinimumTangentKeySpan);
					if (Point.TangentMode == ESplineTangentMode::AutomaticClamped)
						for (int32 Axis = 0; Axis < 3; ++Axis)
							Tangent[Axis] = ClampAutomaticTangent(Incoming[Axis], Outgoing[Axis], PreviousSpan, NextSpan);
					Tangents[Index] = {Tangent, Tangent};
				}
			}
			return Tangents;
		}

		auto UnitQuaternionLog(const FQuat& Value) -> FVector3
		{
			const FVector3 Axis(Value.x, Value.y, Value.z);
			const double Angle = std::acos(std::clamp(Value.w, -1.0, 1.0));
			const double SinAngle = std::sin(Angle);
			return std::abs(SinAngle) >= 1.e-8 ? Axis * (Angle / SinAngle) : Axis;
		}

		auto PureQuaternionExp(const FVector3& Value) -> FQuat
		{
			const double Angle = Math::Length(Value);
			const double SinAngle = std::sin(Angle);
			const FVector3 Axis = std::abs(SinAngle) >= 1.e-8 ? Value * (SinAngle / Angle) : Value;
			return FQuat(std::cos(Angle), Axis.x, Axis.y, Axis.z);
		}

		struct FSplineAttributeKnot
		{
			FQuat Rotation;
			FQuat RotationTangent = FQuatConstants::Identity;
			FVector3 ScaleTangent{0.0};
		};

		auto BuildAttributeKnots(const std::vector<FSplinePoint>& Points, bool bClosedLoop,
			bool bStationaryEndpoints, double LoopKey) -> std::vector<FSplineAttributeKnot>
		{
			std::vector<FSplineAttributeKnot> Knots;
			Knots.reserve(Points.size());
			for (const auto& Point : Points)
				Knots.push_back({Math::NormalizeOr(Point.Rotation, FQuatConstants::Identity)});

			for (size_t Index = 0; Index < Points.size(); ++Index)
			{
				const bool bFirst = Index == 0;
				const bool bLast = Index + 1 == Points.size();
				if (bStationaryEndpoints && (bFirst || (bLast && !bClosedLoop))) continue;

				const size_t Previous = bFirst ? (bClosedLoop ? Points.size() - 1 : Index) : Index - 1;
				const size_t Next = bLast ? (bClosedLoop ? 0 : Index) : Index + 1;
				const double SeamSpan = LoopKey - Points.back().InputKey;
				const double PreviousKey = bFirst && bClosedLoop ? Points[Index].InputKey - SeamSpan : Points[Previous].InputKey;
				const double NextKey = bLast && bClosedLoop ? LoopKey : Points[Next].InputKey;
				const double KeySpan = std::max(NextKey - PreviousKey, MinimumTangentKeySpan);
				auto& Knot = Knots[Index];
				const FQuat Inverse = glm::conjugate(Knot.Rotation);
				const FVector3 LogSum = UnitQuaternionLog(Inverse * Knots[Previous].Rotation)
					+ UnitQuaternionLog(Inverse * Knots[Next].Rotation);
				Knot.RotationTangent = (Knot.Rotation * PureQuaternionExp(LogSum * -0.5)) / KeySpan;
				Knot.ScaleTangent = (Points[Next].Scale - Points[Previous].Scale) / KeySpan;
			}
			return Knots;
		}

		auto SlerpRotation(const FQuat& Start, const FQuat& End, double T, bool bShortestPath) -> FQuat
		{
			double CosAngle = glm::dot(Start, End);
			FQuat Target = End;
			if (bShortestPath)
			{
				if (CosAngle < 0.0)
				{
					Target = -Target;
					CosAngle = -CosAngle;
				}
				if (CosAngle >= 0.9999) return Start * (1.0 - T) + Target * T;
			}
			const double Angle = std::acos(std::clamp(CosAngle, -1.0, 1.0));
			if (Angle < 1.e-4) return Start;
			return (Start * std::sin((1.0 - T) * Angle) + Target * std::sin(T * Angle)) / std::sin(Angle);
		}

		auto EvaluateRotation(const FQuat& Start, const FQuat& End,
			const FQuat& StartControl, const FQuat& EndControl, double T) -> FQuat
		{
			const FQuat Endpoints = SlerpRotation(Start, End, T, true);
			const FQuat Controls = SlerpRotation(StartControl, EndControl, T, false);
			return Math::NormalizeOr(SlerpRotation(Endpoints, Controls, 2.0 * T * (1.0 - T), false),
				FQuatConstants::Identity, 1.e-8);
		}

		auto EvaluateScale(const FVector3& Start, const FVector3& End,
			const FVector3& StartTangent, const FVector3& EndTangent, double T) -> FVector3
		{
			const double T2 = T * T;
			const double T3 = T2 * T;
			return Start * (2.0 * T3 - 3.0 * T2 + 1.0) + StartTangent * (T3 - 2.0 * T2 + T)
				+ End * (-2.0 * T3 + 3.0 * T2) + EndTangent * (T3 - T2);
		}

		auto EvaluatePositionSegment(const FSplineEvaluationSegment& Segment, double T) -> FSplineSample
		{
			T = std::clamp(T, 0.0, 1.0);
			if (Segment.Interpolation == ESplineSegmentInterpolation::Constant)
				return {.Position = T < 1.0 ? Segment.Coefficient0 : Segment.EndPosition};
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
			const FVector3 Middle = EvaluatePositionSegment(Segment, MiddleT).Position;
						const double SplitLength = Math::Length(Middle - Start) + Math::Length(End - Middle);
			// Flat or S-shaped cubics can have zero midpoint chord error while
			// speed varies strongly. Bound both curvature and station interpolation
			// using the Bezier control polygon of this exact restricted cubic.
			const auto D0 = EvaluatePositionSegment(Segment, StartT).FirstDerivative * (EndT - StartT);
			const auto D1 = EvaluatePositionSegment(Segment, EndT).FirstDerivative * (EndT - StartT);
			const auto C1 = Start + D0 / 3.0;
			const auto C2 = End - D1 / 3.0;
			const double PolygonLength = Math::Length(C1 - Start) + Math::Length(C2 - C1) + Math::Length(End - C2);
			const double StationError = std::max(Math::Length(C1 - Math::Lerp(Start, End, 1.0 / 3.0)),
				Math::Length(C2 - Math::Lerp(Start, End, 2.0 / 3.0)));
			if ((PolygonLength - SplitLength <= ErrorBudget && StationError <= AbsoluteLengthError * 0.25)
				|| Depth >= MaximumSubdivisionDepth)
			{
				auto AddSample = [&](double T, const FVector3& Position) {
					const FSplineDistanceSample& Previous = Segment.DistanceSamples.back();
					if (T <= Previous.T) return;
					const FVector3 PreviousPosition = EvaluatePositionSegment(Segment, Previous.T).Position;
					Segment.LocalLength += Math::Length(Position - PreviousPosition);
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
			return Math::LengthSquared(Delta);
		}

		auto DistanceSquaredToBox(const FVector3& Point, const FBox& Box) -> double
		{
			if (!Box.bIsValid) return 0.0;
			return DistanceSquared(Point, Math::Clamp(Point, Box.Min, Box.Max));
		}
	} // namespace

	FSplinePoint::FSplinePoint(const FVector3& InPosition)
		: Position(InPosition)
	{
	}

	FSplineCurve::FSplineCurve()
	{
		Points.emplace_back(FVector3(0.0, 0.0, 0.0));
		Points.emplace_back(FVector3(5.0, 0.0, 0.0));
		Points[0].InputKey = 0.0;
		Points[1].InputKey = 1.0;
	}

	auto FSplineCurve::GetPoint(uint32 PointIndex) const -> const FSplinePoint*
	{
		return PointIndex < Points.size() ? &Points[PointIndex] : nullptr;
	}

	auto FSplineCurve::GetNumSegments() const -> uint32
	{
		if (Points.size() < 2) return 0;
		return bClosedLoop ? static_cast<uint32>(Points.size()) : static_cast<uint32>(Points.size() - 1);
	}

	auto FSplineCurve::SetPoints(std::vector<FSplinePoint> InPoints, std::optional<double> LoopKey) -> bool
	{
		for (size_t Index = 0; Index < InPoints.size();)
		{
			if (InPoints[Index].InputKey != FSplinePoint::AutoInputKey)
			{
				++Index;
				continue;
			}
			const size_t First = Index;
			while (Index < InPoints.size() && InPoints[Index].InputKey == FSplinePoint::AutoInputKey) ++Index;
			for (size_t Slot = First; Slot < Index; ++Slot)
			{
				if (First > 0 && Index < InPoints.size())
					InPoints[Slot].InputKey = std::lerp(InPoints[First - 1].InputKey, InPoints[Index].InputKey,
						static_cast<double>(Slot - First + 1) / static_cast<double>(Index - First + 1));
				else if (First > 0)
					InPoints[Slot].InputKey = InPoints[First - 1].InputKey + static_cast<double>(Slot - First + 1);
				else if (Index < InPoints.size())
					InPoints[Slot].InputKey = InPoints[Index].InputKey - static_cast<double>(Index - Slot);
				else
					InPoints[Slot].InputKey = static_cast<double>(Slot);
			}
		}

		double Previous = FSplinePoint::AutoInputKey;
		for (const auto& Point : InPoints)
		{
			if (!std::isfinite(Point.InputKey) || Point.InputKey <= Previous
				|| (Previous != FSplinePoint::AutoInputKey && !std::isfinite(Point.InputKey - Previous))) return false;
			Previous = Point.InputKey;
		}

		const bool bNewClosedLoop = bClosedLoop || LoopKey.has_value();
		const double Override = LoopKey.value_or(LoopKeyOverride);
		if (InPoints.empty())
		{
			if (LoopKey) return false;
		}
		else
		{
			const double Last = InPoints.back().InputKey;
			const double End = !bNewClosedLoop ? Last : Override != FSplinePoint::AutoInputKey ? Override
				: std::max(Last + 1.0, std::nextafter(Last, std::numeric_limits<double>::infinity()));
			if ((LoopKey && *LoopKey == FSplinePoint::AutoInputKey)
				|| (bNewClosedLoop && (!(End > Last) || !std::isfinite(End - Last)))
				|| !std::isfinite(End - InPoints.front().InputKey)) return false;
		}

		Points = std::move(InPoints);
		bClosedLoop = bNewClosedLoop;
		LoopKeyOverride = Override;
		return true;
	}

	auto FSplineCurve::AddPoint(FSplinePoint Point) -> uint32
	{
		const uint32 Index = GetNumPoints();
		if (!InsertPoint(Index, Point)) throw std::invalid_argument("Invalid spline input key");
		return Index;
	}

	auto FSplineCurve::InsertPoint(uint32 PointIndex, FSplinePoint Point) -> bool
	{
		const auto InputKey = ResolveInsertionKey(PointIndex, Point.InputKey);
		if (!InputKey) return false;
		Point.InputKey = *InputKey;
		Points.insert(Points.begin() + PointIndex, std::move(Point));
		return true;
	}

	auto FSplineCurve::DuplicatePoint(uint32 PointIndex) -> std::optional<uint32>
	{
		if (PointIndex >= Points.size()) return std::nullopt;
		FSplinePoint Duplicate = Points[PointIndex];
		Duplicate.InputKey = FSplinePoint::AutoInputKey;
		const uint32 InsertIndex = PointIndex + 1;
		if (!InsertPoint(InsertIndex, Duplicate)) return std::nullopt;
		return InsertIndex;
	}

	auto FSplineCurve::UpdatePoint(uint32 PointIndex, FSplinePoint Point) -> bool
	{
		if (PointIndex >= Points.size()) return false;
		if (Point.InputKey == FSplinePoint::AutoInputKey) Point.InputKey = Points[PointIndex].InputKey;
		if (!IsValidPointKey(PointIndex, Point.InputKey)) return false;
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
		const uint32 First = std::min(FromIndex, ToIndex);
		const uint32 Last = std::max(FromIndex, ToIndex);
		std::vector<double> Keys;
		for (uint32 Index = First; Index <= Last; ++Index) Keys.push_back(Points[Index].InputKey);
		if (FromIndex < ToIndex) std::rotate(Points.begin() + First, Points.begin() + First + 1, Points.begin() + Last + 1);
		else std::rotate(Points.begin() + First, Points.begin() + Last, Points.begin() + Last + 1);
		for (uint32 Index = First; Index <= Last; ++Index) Points[Index].InputKey = Keys[Index - First];
		return true;
	}

	auto FSplineCurve::ClearPoints() -> void
	{
		Points.clear();
	}

	auto FSplineCurve::HasValidInputKeys() const -> bool
	{
		double Previous = FSplinePoint::AutoInputKey;
		for (const FSplinePoint& Point : Points)
		{
			if (!std::isfinite(Point.InputKey) || Point.InputKey <= Previous
				|| (Previous != FSplinePoint::AutoInputKey && !std::isfinite(Point.InputKey - Previous)))
				return false;
			Previous = Point.InputKey;
		}
		return true;
	}

	auto FSplineCurve::RepairInputKeys() -> bool
	{
		if (HasValidInputKeys()) return false;
		for (size_t Index = 0; Index < Points.size(); ++Index)
			Points[Index].InputKey = static_cast<double>(Index);
		return true;
	}

	auto FSplineCurve::ResolveInsertionKey(uint32 Index, double RequestedKey) const -> std::optional<double>
	{
		if (Index > Points.size()) return std::nullopt;
		const bool bHasPrevious = Index > 0;
		const bool bHasNext = Index < Points.size();
		const double Previous = bHasPrevious ? Points[Index - 1].InputKey : FSplinePoint::AutoInputKey;
		const double Next = bHasNext ? Points[Index].InputKey : std::numeric_limits<double>::infinity();
		if (RequestedKey == FSplinePoint::AutoInputKey)
		{
			if (bHasPrevious && bHasNext) RequestedKey = std::midpoint(Previous, Next);
			else if (bHasPrevious && bClosedLoop && LoopKeyOverride > Previous)
				RequestedKey = std::midpoint(Previous, LoopKeyOverride);
			else if (bHasPrevious) RequestedKey = Previous + 1.0;
			else RequestedKey = bHasNext ? Next - 1.0 : 0.0;
		}
		if (!std::isfinite(RequestedKey) || RequestedKey <= Previous || RequestedKey >= Next
			|| (bHasPrevious && !std::isfinite(RequestedKey - Previous))
			|| (bHasNext && !std::isfinite(Next - RequestedKey))
			|| (!bHasNext && bClosedLoop && LoopKeyOverride > Previous && RequestedKey >= LoopKeyOverride))
			return std::nullopt;
		return RequestedKey;
	}

	auto FSplineCurve::IsValidPointKey(uint32 Index, double InputKey) const -> bool
	{
		if (Index >= Points.size() || !std::isfinite(InputKey) || InputKey == FSplinePoint::AutoInputKey) return false;
		if (Index > 0 && (!(InputKey > Points[Index - 1].InputKey) || !std::isfinite(InputKey - Points[Index - 1].InputKey))) return false;
		if (Index + 1 < Points.size()) return InputKey < Points[Index + 1].InputKey && std::isfinite(Points[Index + 1].InputKey - InputKey);
		return !bClosedLoop || LoopKeyOverride <= Points.back().InputKey || InputKey < LoopKeyOverride;
	}

	auto FSplineCurve::GetLoopKey() const -> double
	{
		if (Points.empty()) return 0.0;
		const double LastKey = Points.back().InputKey;
		if (LoopKeyOverride > LastKey && std::isfinite(LoopKeyOverride - LastKey)) return LoopKeyOverride;
		return std::max(LastKey + 1.0, std::nextafter(LastKey, std::numeric_limits<double>::infinity()));
	}

	auto FSplineCurve::SetClosedLoopAtPosition(double InputKey) -> bool
	{
		if (Points.empty() || !std::isfinite(InputKey - Points.back().InputKey) || InputKey <= Points.back().InputKey) return false;
		bClosedLoop = true;
		LoopKeyOverride = InputKey;
		return true;
	}

	auto FSplineCurve::SetClosedLoop(bool bInClosedLoop) -> void
	{
		bClosedLoop = bInClosedLoop;
		LoopKeyOverride = FSplinePoint::AutoInputKey;
	}

	auto FSplineCurve::BuildEvaluationData() const -> std::shared_ptr<const FSplineEvaluationData>
	{
		if (!HasValidInputKeys())
		{
			// Embedded reflected curves can be queried before their owner publishes repaired data.
			FSplineCurve Repaired = *this;
			Repaired.RepairInputKeys();
			return Repaired.BuildEvaluationData();
		}
		auto Result = std::make_shared<FSplineEvaluationData>();
		Result->bClosedLoop = bClosedLoop;
		if (!Points.empty())
		{
			const FSplinePoint& First = Points.front();
			Result->FirstKey = First.InputKey;
			Result->SinglePoint = {.Position = First.Position, .Rotation = Math::NormalizeOr(First.Rotation, FQuatConstants::Identity), .Scale = First.Scale};
			for (const FSplinePoint& Point : Points) Result->LocalBounds.AddPoint(Point.Position);
		}
		const uint32 SegmentCount = GetNumSegments();
		Result->Segments.reserve(SegmentCount);
		Result->AttributeSegments.reserve(SegmentCount);
		const auto AttributeKnots = BuildAttributeKnots(Points, bClosedLoop, bStationaryEndpoints, GetLoopKey());
		const auto Tangents = BuildPointTangents(Points, bClosedLoop, bStationaryEndpoints, GetLoopKey());
		for (uint32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
		{
			const uint32 EndIndex = (SegmentIndex + 1) % static_cast<uint32>(Points.size());
			const FSplinePoint& Start = Points[SegmentIndex];
			const FSplinePoint& End = Points[EndIndex];
			FSplineEvaluationSegment Segment;
			Segment.Interpolation = Start.OutgoingInterpolation;
			Segment.Coefficient0 = Start.Position;
			Segment.EndPosition = End.Position;
			Segment.StartKey = Start.InputKey;
			Segment.EndKey = EndIndex == 0 ? GetLoopKey() : End.InputKey;
			const double KeySpan = Segment.EndKey - Segment.StartKey;
			const auto& StartAttributes = AttributeKnots[SegmentIndex];
			const auto& EndAttributes = AttributeKnots[EndIndex];
			// Like UE's keyed cubic evaluator, scale the quaternion controls before Squad.
			Result->AttributeSegments.push_back({
				StartAttributes.Rotation, EndAttributes.Rotation,
				StartAttributes.RotationTangent * KeySpan, EndAttributes.RotationTangent * KeySpan,
				Start.Scale, End.Scale, StartAttributes.ScaleTangent * KeySpan, EndAttributes.ScaleTangent * KeySpan});
			if (Segment.Interpolation == ESplineSegmentInterpolation::Linear)
			{
				Segment.Coefficient1 = End.Position - Start.Position;
			}
			else if (Segment.Interpolation == ESplineSegmentInterpolation::Cubic)
			{
				const FVector3 StartTangent = Tangents[SegmentIndex].Leave * KeySpan;
				const FVector3 EndTangent = Tangents[EndIndex].Arrive * KeySpan;
				Segment.Coefficient1 = StartTangent;
				Segment.Coefficient2 = -3.0 * Start.Position - 2.0 * StartTangent + 3.0 * End.Position - EndTangent;
				Segment.Coefficient3 = 2.0 * Start.Position + StartTangent - 2.0 * End.Position + EndTangent;

				Segment.LocalBounds.AddPoint(Start.Position);
				Segment.LocalBounds.AddPoint(Start.Position + StartTangent / 3.0);
				Segment.LocalBounds.AddPoint(End.Position - EndTangent / 3.0);
				Segment.LocalBounds.AddPoint(End.Position);
			}
			if (Segment.Interpolation != ESplineSegmentInterpolation::Cubic)
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
			const double ControlPolygonLength = Math::Length(Control1 - Control0)
				+ Math::Length(Control2 - Control1) + Math::Length(Control3 - Control2);
			const double ErrorBudget = std::max(AbsoluteLengthError, RelativeLengthError * ControlPolygonLength);
			if (Segment.Interpolation != ESplineSegmentInterpolation::Cubic)
			{
				Segment.LocalLength = Math::Length(End.Position - Start.Position);
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
		return Parameter;
	}

	auto FSplineEvaluationData::Evaluate(FSplineParameter Parameter) const -> FSplineSample
	{
		if (Segments.empty()) return SinglePoint;
		Parameter = ResolveParameter(Parameter);
		FSplineSample Sample = EvaluatePositionSegment(Segments[Parameter.SegmentIndex], Parameter.T);
		const auto& Attributes = AttributeSegments[Parameter.SegmentIndex];
		const double T = Parameter.T;
		if (T <= 0.0)
		{
			Sample.Rotation = Attributes.StartRotation;
			Sample.Scale = Attributes.StartScale;
		}
		else if (T >= 1.0)
		{
			Sample.Rotation = Attributes.EndRotation;
			Sample.Scale = Attributes.EndScale;
		}
		else
		{
			Sample.Rotation = EvaluateRotation(Attributes.StartRotation, Attributes.EndRotation,
				Attributes.StartRotationControl, Attributes.EndRotationControl, T);
			Sample.Scale = EvaluateScale(Attributes.StartScale, Attributes.EndScale,
				Attributes.StartScaleTangent, Attributes.EndScaleTangent, T);
		}
		return Sample;
	}

	auto FSplineEvaluationData::BuildScaledDistanceData(const FVector3& Scale) const -> std::shared_ptr<const FSplineEvaluationData>
	{
		auto Result = std::make_shared<FSplineEvaluationData>(*this);
		Result->LocalLength = 0.0;
		for (auto& Segment : Result->Segments)
		{
			Segment.StartLocalDistance = Result->LocalLength;
			if (Segment.Interpolation == ESplineSegmentInterpolation::Cubic)
			{
				FSplineEvaluationSegment Metric;
				Metric.Interpolation = Segment.Interpolation;
				Metric.Coefficient1 = Segment.Coefficient1 * Scale;
				Metric.Coefficient2 = Segment.Coefficient2 * Scale;
				Metric.Coefficient3 = Segment.Coefficient3 * Scale;
				Metric.DistanceSamples.push_back({0.0, 0.0});
				const FVector3 End = Metric.Coefficient1 + Metric.Coefficient2 + Metric.Coefficient3;
				const FVector3 C1 = Metric.Coefficient1 / 3.0;
				const FVector3 C2 = 2.0 * C1 + Metric.Coefficient2 / 3.0;
				const double PolygonLength = Math::Length(C1) + Math::Length(C2 - C1) + Math::Length(End - C2);
				AddAdaptiveSamples(Metric, 0.0, FVectorConstants::Zero, 1.0, End,
					std::max(AbsoluteLengthError, RelativeLengthError * PolygonLength), 0);
				Segment.LocalLength = Metric.LocalLength;
				Segment.DistanceSamples = std::move(Metric.DistanceSamples);
			}
			else
			{
				Segment.LocalLength = Math::Length((Segment.EndPosition - Segment.Coefficient0) * Scale);
				Segment.DistanceSamples.back().LocalDistance = Segment.LocalLength;
			}
			Result->LocalLength += Segment.LocalLength;
		}
		return Result;
	}

	auto FSplineEvaluationData::GetParameterAtInputKey(double InputKey) const -> FSplineParameter
	{
		if (Segments.empty() || !(InputKey > FirstKey)) return {};
		if (InputKey >= Segments.back().EndKey)
			return {static_cast<uint32>(Segments.size() - 1), 1.0};
		const auto Segment = std::upper_bound(Segments.begin(), Segments.end(), InputKey,
			[](double Key, const FSplineEvaluationSegment& Entry) { return Key < Entry.EndKey; });
		return {static_cast<uint32>(Segment - Segments.begin()),
			(InputKey - Segment->StartKey) / (Segment->EndKey - Segment->StartKey)};
	}

	auto FSplineEvaluationData::GetInputKeyAtParameter(FSplineParameter Parameter) const -> double
	{
		if (Segments.empty()) return FirstKey;
		Parameter = ResolveParameter(Parameter);
		const FSplineEvaluationSegment& Segment = Segments[Parameter.SegmentIndex];
		return std::lerp(Segment.StartKey, Segment.EndKey, Parameter.T);
	}

	auto FSplineEvaluationData::EvaluateAtInputKey(double InputKey) const -> FSplineSample
	{
		const FSplineParameter Parameter = GetParameterAtInputKey(InputKey);
		FSplineSample Sample = Evaluate(Parameter);
		if (!Segments.empty())
		{
			const auto& Segment = Segments[Parameter.SegmentIndex];
			const double KeySpan = Segment.EndKey - Segment.StartKey;
			Sample.FirstDerivative /= KeySpan;
			Sample.SecondDerivative = Sample.SecondDerivative / KeySpan / KeySpan;
		}
		return Sample;
	}

	auto FSplineEvaluationData::GetLocalDistanceAtParameter(FSplineParameter Parameter) const -> double
	{
		if (Segments.empty()) return 0.0;
		if (bClosedLoop && Parameter.SegmentIndex + 1 == Segments.size() && Parameter.T >= 1.0) return LocalLength;
		Parameter = ResolveParameter(Parameter);
		const FSplineEvaluationSegment& Segment = Segments[Parameter.SegmentIndex];
		if (Segment.Interpolation == ESplineSegmentInterpolation::Constant)
			return Segment.StartLocalDistance + (Parameter.T >= 1.0 ? Segment.LocalLength : 0.0);
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
		if (Segment.Interpolation == ESplineSegmentInterpolation::Constant)
			return {SegmentIndex, SegmentDistance < Segment.LocalLength ? 0.0 : 1.0};
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
				const double CandidateDistance = DistanceSquared(EvaluatePositionSegment(Segment, CandidateT).Position, LocalPosition);
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
					const double FirstDistance = DistanceSquared(EvaluatePositionSegment(Segment, First).Position, LocalPosition);
					const double SecondDistance = DistanceSquared(EvaluatePositionSegment(Segment, Second).Position, LocalPosition);
					if (FirstDistance <= SecondDistance) Right = Second;
					else Left = First;
				}
				Consider((Left + Right) * 0.5);
			}
		}
		return Best;
	}
} // namespace Durin
