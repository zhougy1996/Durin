#pragma once

#include "Spline/SplineTypes.h"

#include "SplineCurve.gen.h"

namespace Durin
{
	DSTRUCT()
	struct ENGINE_API FSplineCurve
	{
		GENERATED_BODY()

		FSplineCurve();

		auto GetPoints() const -> const std::vector<FSplinePoint>& { return Points; }
		auto GetPoint(uint32 PointIndex) const -> const FSplinePoint*;
		auto GetNumPoints() const -> uint32 { return static_cast<uint32>(Points.size()); }
		auto GetNumSegments() const -> uint32;

		auto SetPoints(std::vector<FSplinePoint> InPoints) -> void;
		auto AddPoint(const FSplinePoint& Point) -> uint32;
		auto UpdatePoint(uint32 PointIndex, const FSplinePoint& Point) -> bool;
		auto RemovePoint(uint32 PointIndex) -> bool;
		auto ClearPoints() -> void;

		auto IsClosedLoop() const -> bool { return bClosedLoop; }
		auto SetClosedLoop(bool bInClosedLoop) -> void;

		auto GetReparamStepsPerSegment() const -> int32 { return ReparamStepsPerSegment; }
		auto SetReparamStepsPerSegment(int32 InSteps) -> void;

		auto GetLocationAtParam(double Param) const -> FVector3;
		auto GetTangentAtParam(double Param) const -> FVector3;
		auto GetDirectionAtParam(double Param) const -> FVector3;
		auto GetRotationAtParam(double Param) const -> FQuat;
		auto GetScaleAtParam(double Param) const -> FVector3;
		auto GetTransformAtParam(double Param) const -> FTransform;

		auto GetSplineLength() const -> double;
		auto GetDistanceAtParam(double Param) const -> double;
		auto GetParamAtDistance(double Distance) const -> double;
		auto GetLocationAtDistance(double Distance) const -> FVector3;
		auto GetTangentAtDistance(double Distance) const -> FVector3;
		auto GetDirectionAtDistance(double Distance) const -> FVector3;
		auto GetTransformAtDistance(double Distance) const -> FTransform;

		// Rebuild explicitly after archive code writes reflected fields directly.
		auto UpdateSpline() -> void;

	private:
		struct FReparamSample
		{
			double Distance = 0.0;
			double Param = 0.0;
		};

		struct FSegmentParam
		{
			uint32 SegmentIndex = 0;
			double T = 0.0;
		};

		auto MarkCacheDirty() -> void;
		auto EnsureCache() const -> void;
		auto ResolveParam(double Param) const -> FSegmentParam;
		auto GetAutoTangent(uint32 PointIndex) const -> FVector3;
		auto GetLeaveTangent(uint32 PointIndex) const -> FVector3;
		auto GetArriveTangent(uint32 PointIndex) const -> FVector3;

		DPROPERTY(Edit)
		std::vector<FSplinePoint> Points;

		DPROPERTY(Edit)
		bool bClosedLoop = false;

		DPROPERTY(Edit)
		int32 ReparamStepsPerSegment = 10;

		mutable bool bCacheDirty = true;
		mutable std::vector<FReparamSample> ReparamTable;
		mutable double SplineLength = 0.0;
	};
} // namespace Durin
