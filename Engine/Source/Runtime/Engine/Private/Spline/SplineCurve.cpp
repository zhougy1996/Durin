#include "Spline/SplineCurve.h"

#include "Math/Transform.h"

namespace Durin
{
	namespace
	{
		constexpr int32 MinimumReparamSteps = 1;
		constexpr int32 MaximumReparamSteps = 1024;

		auto SafeNormalize(const FVector3& Value) -> FVector3
		{
			const double LengthSquared = glm::dot(Value, Value);
			return LengthSquared > kSmallNumber ? Value / std::sqrt(LengthSquared) : FVectorConstants::Zero;
		}

		auto SafeNormalize(const FQuat& Value) -> FQuat
		{
			return glm::dot(Value, Value) > kSmallNumber ? glm::normalize(Value) : glm::identity<FQuat>();
		}

		auto InterpolateRotation(const FQuat& Start, const FQuat& End, double T) -> FQuat
		{
			const FQuat NormalizedStart = SafeNormalize(Start);
			FQuat NormalizedEnd = SafeNormalize(End);
			if (glm::dot(NormalizedStart, NormalizedEnd) < 0.0) NormalizedEnd = -NormalizedEnd;
			return SafeNormalize(glm::slerp(NormalizedStart, NormalizedEnd, T));
		}
	} // namespace

	FSplineCurve::FSplineCurve()
	{
		Points.emplace_back(FVector3(0.0, 0.0, 0.0));
		Points.emplace_back(FVector3(100.0, 0.0, 0.0));
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

	auto FSplineCurve::SetPoints(std::vector<FSplinePoint> InPoints) -> void
	{
		Points = std::move(InPoints);
		MarkCacheDirty();
	}

	auto FSplineCurve::AddPoint(const FSplinePoint& Point) -> uint32
	{
		Points.push_back(Point);
		MarkCacheDirty();
		return static_cast<uint32>(Points.size() - 1);
	}

	auto FSplineCurve::UpdatePoint(uint32 PointIndex, const FSplinePoint& Point) -> bool
	{
		if (PointIndex >= Points.size()) return false;
		Points[PointIndex] = Point;
		MarkCacheDirty();
		return true;
	}

	auto FSplineCurve::RemovePoint(uint32 PointIndex) -> bool
	{
		if (PointIndex >= Points.size()) return false;
		Points.erase(Points.begin() + PointIndex);
		MarkCacheDirty();
		return true;
	}

	auto FSplineCurve::ClearPoints() -> void
	{
		Points.clear();
		MarkCacheDirty();
	}

	auto FSplineCurve::SetClosedLoop(bool bInClosedLoop) -> void
	{
		if (bClosedLoop == bInClosedLoop) return;
		bClosedLoop = bInClosedLoop;
		MarkCacheDirty();
	}

	auto FSplineCurve::SetReparamStepsPerSegment(int32 InSteps) -> void
	{
		const int32 ClampedSteps = std::clamp(InSteps, MinimumReparamSteps, MaximumReparamSteps);
		if (ReparamStepsPerSegment == ClampedSteps) return;
		ReparamStepsPerSegment = ClampedSteps;
		MarkCacheDirty();
	}

	auto FSplineCurve::GetLocationAtParam(double Param) const -> FVector3
	{
		if (Points.empty()) return FVectorConstants::Zero;
		if (GetNumSegments() == 0) return Points.front().Position;

		const FSegmentParam Resolved = ResolveParam(Param);
		const uint32 EndIndex = (Resolved.SegmentIndex + 1) % static_cast<uint32>(Points.size());
		const FSplinePoint& Start = Points[Resolved.SegmentIndex];
		const FSplinePoint& End = Points[EndIndex];
		const double T = Resolved.T;
		if (Start.Type == ESplinePointType::Constant) return T < 1.0 ? Start.Position : End.Position;
		if (Start.Type == ESplinePointType::Linear) return glm::mix(Start.Position, End.Position, T);

		const double T2 = T * T;
		const double T3 = T2 * T;
		const double H00 = 2.0 * T3 - 3.0 * T2 + 1.0;
		const double H10 = T3 - 2.0 * T2 + T;
		const double H01 = -2.0 * T3 + 3.0 * T2;
		const double H11 = T3 - T2;
		return H00 * Start.Position + H10 * GetLeaveTangent(Resolved.SegmentIndex)
			+ H01 * End.Position + H11 * GetArriveTangent(EndIndex);
	}

	auto FSplineCurve::GetTangentAtParam(double Param) const -> FVector3
	{
		if (GetNumSegments() == 0) return FVectorConstants::Zero;
		const FSegmentParam Resolved = ResolveParam(Param);
		const uint32 EndIndex = (Resolved.SegmentIndex + 1) % static_cast<uint32>(Points.size());
		const FSplinePoint& Start = Points[Resolved.SegmentIndex];
		const FSplinePoint& End = Points[EndIndex];
		if (Start.Type == ESplinePointType::Constant) return FVectorConstants::Zero;
		if (Start.Type == ESplinePointType::Linear) return End.Position - Start.Position;

		const double T = Resolved.T;
		const double T2 = T * T;
		const double H00 = 6.0 * T2 - 6.0 * T;
		const double H10 = 3.0 * T2 - 4.0 * T + 1.0;
		const double H01 = -6.0 * T2 + 6.0 * T;
		const double H11 = 3.0 * T2 - 2.0 * T;
		return H00 * Start.Position + H10 * GetLeaveTangent(Resolved.SegmentIndex)
			+ H01 * End.Position + H11 * GetArriveTangent(EndIndex);
	}

	auto FSplineCurve::GetDirectionAtParam(double Param) const -> FVector3
	{
		return SafeNormalize(GetTangentAtParam(Param));
	}

	auto FSplineCurve::GetRotationAtParam(double Param) const -> FQuat
	{
		if (Points.empty()) return glm::identity<FQuat>();
		if (GetNumSegments() == 0) return SafeNormalize(Points.front().Rotation);
		const FSegmentParam Resolved = ResolveParam(Param);
		const uint32 EndIndex = (Resolved.SegmentIndex + 1) % static_cast<uint32>(Points.size());
		if (Points[Resolved.SegmentIndex].Type == ESplinePointType::Constant) return SafeNormalize(Points[Resolved.SegmentIndex].Rotation);
		return InterpolateRotation(Points[Resolved.SegmentIndex].Rotation, Points[EndIndex].Rotation, Resolved.T);
	}

	auto FSplineCurve::GetScaleAtParam(double Param) const -> FVector3
	{
		if (Points.empty()) return FVectorConstants::Unit;
		if (GetNumSegments() == 0) return Points.front().Scale;
		const FSegmentParam Resolved = ResolveParam(Param);
		const uint32 EndIndex = (Resolved.SegmentIndex + 1) % static_cast<uint32>(Points.size());
		if (Points[Resolved.SegmentIndex].Type == ESplinePointType::Constant) return Points[Resolved.SegmentIndex].Scale;
		return glm::mix(Points[Resolved.SegmentIndex].Scale, Points[EndIndex].Scale, Resolved.T);
	}

	auto FSplineCurve::GetTransformAtParam(double Param) const -> FTransform
	{
		FTransform Result;
		Result.Translation = GetLocationAtParam(Param);
		Result.Rotation = GetRotationAtParam(Param);
		Result.Scale3D = GetScaleAtParam(Param);
		return Result;
	}

	auto FSplineCurve::GetSplineLength() const -> double
	{
		EnsureCache();
		return SplineLength;
	}

	auto FSplineCurve::GetDistanceAtParam(double Param) const -> double
	{
		EnsureCache();
		if (ReparamTable.empty() || GetNumSegments() == 0) return 0.0;
		const double SegmentCount = static_cast<double>(GetNumSegments());
		double NormalizedParam = Param;
		if (bClosedLoop)
		{
			if (std::abs(Param - SegmentCount) <= kSmallNumber) return SplineLength;
			NormalizedParam = std::fmod(Param, SegmentCount);
			if (NormalizedParam < 0.0) NormalizedParam += SegmentCount;
		}
		else
		{
			NormalizedParam = std::clamp(Param, 0.0, SegmentCount);
		}

		const auto Upper = std::lower_bound(ReparamTable.begin(), ReparamTable.end(), NormalizedParam,
			[](const FReparamSample& Sample, double Value) { return Sample.Param < Value; });
		if (Upper == ReparamTable.begin()) return Upper->Distance;
		if (Upper == ReparamTable.end()) return SplineLength;
		const FReparamSample& Previous = *std::prev(Upper);
		const double ParamRange = Upper->Param - Previous.Param;
		if (ParamRange <= kSmallNumber) return Upper->Distance;
		const double Alpha = (NormalizedParam - Previous.Param) / ParamRange;
		return std::lerp(Previous.Distance, Upper->Distance, Alpha);
	}

	auto FSplineCurve::GetParamAtDistance(double Distance) const -> double
	{
		EnsureCache();
		if (ReparamTable.empty() || SplineLength <= kSmallNumber) return 0.0;
		double NormalizedDistance = Distance;
		if (bClosedLoop)
		{
			NormalizedDistance = std::fmod(Distance, SplineLength);
			if (NormalizedDistance < 0.0) NormalizedDistance += SplineLength;
		}
		else
		{
			NormalizedDistance = std::clamp(Distance, 0.0, SplineLength);
		}

		const auto Upper = std::lower_bound(ReparamTable.begin(), ReparamTable.end(), NormalizedDistance,
			[](const FReparamSample& Sample, double Value) { return Sample.Distance < Value; });
		if (Upper == ReparamTable.begin()) return Upper->Param;
		if (Upper == ReparamTable.end()) return ReparamTable.back().Param;
		const FReparamSample& Previous = *std::prev(Upper);
		const double DistanceRange = Upper->Distance - Previous.Distance;
		if (DistanceRange <= kSmallNumber) return Upper->Param;
		const double Alpha = (NormalizedDistance - Previous.Distance) / DistanceRange;
		return std::lerp(Previous.Param, Upper->Param, Alpha);
	}

	auto FSplineCurve::GetLocationAtDistance(double Distance) const -> FVector3
	{
		return GetLocationAtParam(GetParamAtDistance(Distance));
	}

	auto FSplineCurve::GetTangentAtDistance(double Distance) const -> FVector3
	{
		return GetTangentAtParam(GetParamAtDistance(Distance));
	}

	auto FSplineCurve::GetDirectionAtDistance(double Distance) const -> FVector3
	{
		return GetDirectionAtParam(GetParamAtDistance(Distance));
	}

	auto FSplineCurve::GetTransformAtDistance(double Distance) const -> FTransform
	{
		return GetTransformAtParam(GetParamAtDistance(Distance));
	}

	auto FSplineCurve::UpdateSpline() -> void
	{
		ReparamStepsPerSegment = std::clamp(ReparamStepsPerSegment, MinimumReparamSteps, MaximumReparamSteps);
		ReparamTable.clear();
		SplineLength = 0.0;
		ReparamTable.push_back({0.0, 0.0});
		const uint32 SegmentCount = GetNumSegments();
		if (SegmentCount == 0)
		{
			bCacheDirty = false;
			return;
		}

		FVector3 PreviousLocation = GetLocationAtParam(0.0);
		for (uint32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
		{
			for (int32 Step = 1; Step <= ReparamStepsPerSegment; ++Step)
			{
				const double Param = static_cast<double>(SegmentIndex) + static_cast<double>(Step) / ReparamStepsPerSegment;
				const FVector3 Location = GetLocationAtParam(Param);
				SplineLength += glm::length(Location - PreviousLocation);
				ReparamTable.push_back({SplineLength, Param});
				PreviousLocation = Location;
			}
		}
		bCacheDirty = false;
	}

	auto FSplineCurve::MarkCacheDirty() -> void
	{
		bCacheDirty = true;
	}

	auto FSplineCurve::EnsureCache() const -> void
	{
		if (bCacheDirty) const_cast<FSplineCurve*>(this)->UpdateSpline();
	}

	auto FSplineCurve::ResolveParam(double Param) const -> FSegmentParam
	{
		const uint32 SegmentCount = GetNumSegments();
		if (SegmentCount == 0) return {};
		const double MaxParam = static_cast<double>(SegmentCount);
		if (!bClosedLoop)
		{
			const double Clamped = std::clamp(Param, 0.0, MaxParam);
			if (Clamped >= MaxParam) return {SegmentCount - 1, 1.0};
			const uint32 SegmentIndex = static_cast<uint32>(std::floor(Clamped));
			return {SegmentIndex, Clamped - SegmentIndex};
		}

		double Wrapped = std::fmod(Param, MaxParam);
		if (Wrapped < 0.0) Wrapped += MaxParam;
		const uint32 SegmentIndex = std::min(static_cast<uint32>(std::floor(Wrapped)), SegmentCount - 1);
		return {SegmentIndex, Wrapped - SegmentIndex};
	}

	auto FSplineCurve::GetAutoTangent(uint32 PointIndex) const -> FVector3
	{
		const uint32 PointCount = static_cast<uint32>(Points.size());
		if (PointCount < 2 || PointIndex >= PointCount) return FVectorConstants::Zero;
		if (!bClosedLoop && PointIndex == 0) return Points[1].Position - Points[0].Position;
		if (!bClosedLoop && PointIndex + 1 == PointCount) return Points[PointIndex].Position - Points[PointIndex - 1].Position;
		const uint32 PreviousIndex = PointIndex == 0 ? PointCount - 1 : PointIndex - 1;
		const uint32 NextIndex = (PointIndex + 1) % PointCount;
		return 0.5 * (Points[NextIndex].Position - Points[PreviousIndex].Position);
	}

	auto FSplineCurve::GetLeaveTangent(uint32 PointIndex) const -> FVector3
	{
		return Points[PointIndex].Type == ESplinePointType::CurveAuto ? GetAutoTangent(PointIndex) : Points[PointIndex].LeaveTangent;
	}

	auto FSplineCurve::GetArriveTangent(uint32 PointIndex) const -> FVector3
	{
		return Points[PointIndex].Type == ESplinePointType::CurveAuto ? GetAutoTangent(PointIndex) : Points[PointIndex].ArriveTangent;
	}
} // namespace Durin
