#pragma once

#include "Spline/SplineTypes.h"

#include "SplineCurve.gen.h"

namespace Durin
{
	// Owns spline control points and a lazily rebuilt distance-to-parameter table.
	DSTRUCT()
	struct FSplineCurve
	{
		GENERATED_BODY()

		ENGINE_API FSplineCurve();

		auto GetPoints() const -> const std::vector<FSplinePoint>& { return Points; }
		ENGINE_API auto GetPoint(uint32 PointIndex) const -> const FSplinePoint*;
		auto GetNumPoints() const -> uint32 { return static_cast<uint32>(Points.size()); }
		ENGINE_API auto GetNumSegments() const -> uint32;

		ENGINE_API auto SetPoints(std::vector<FSplinePoint> InPoints) -> void;
		ENGINE_API auto AddPoint(const FSplinePoint& Point) -> uint32;
		ENGINE_API auto UpdatePoint(uint32 PointIndex, const FSplinePoint& Point) -> bool;
		ENGINE_API auto RemovePoint(uint32 PointIndex) -> bool;
		ENGINE_API auto ClearPoints() -> void;

		auto IsClosedLoop() const -> bool { return bClosedLoop; }
		ENGINE_API auto SetClosedLoop(bool bInClosedLoop) -> void;

		auto GetReparamStepsPerSegment() const -> int32 { return ReparamStepsPerSegment; }
		ENGINE_API auto SetReparamStepsPerSegment(int32 InSteps) -> void;

		ENGINE_API auto GetLocationAtParam(double Param) const -> FVector3;
		ENGINE_API auto GetTangentAtParam(double Param) const -> FVector3;
		ENGINE_API auto GetDirectionAtParam(double Param) const -> FVector3;
		ENGINE_API auto GetRotationAtParam(double Param) const -> FQuat;
		ENGINE_API auto GetScaleAtParam(double Param) const -> FVector3;
		ENGINE_API auto GetTransformAtParam(double Param) const -> FTransform;

		ENGINE_API auto GetSplineLength() const -> double;
		ENGINE_API auto GetDistanceAtParam(double Param) const -> double;
		ENGINE_API auto GetParamAtDistance(double Distance) const -> double;
		ENGINE_API auto GetLocationAtDistance(double Distance) const -> FVector3;
		ENGINE_API auto GetTangentAtDistance(double Distance) const -> FVector3;
		ENGINE_API auto GetDirectionAtDistance(double Distance) const -> FVector3;
		ENGINE_API auto GetTransformAtDistance(double Distance) const -> FTransform;

		// Rebuild explicitly after archive code writes reflected fields directly.
		ENGINE_API auto UpdateSpline() -> void;

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

		// Controls the sampling density of the distance reparameterization table.
		DPROPERTY(Edit)
		int32 ReparamStepsPerSegment = 10;

		// Derived distance data is invalidated by every control-point or loop edit.
		mutable bool bCacheDirty = true;
		mutable std::vector<FReparamSample> ReparamTable;
		mutable double SplineLength = 0.0;
	};
} // namespace Durin
