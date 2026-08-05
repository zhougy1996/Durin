#pragma once

#include "Math/Box.h"
#include "Spline/SplineTypes.h"

#include "SplineCurve.gen.h"

namespace Durin
{
	struct FSplineDistanceSample
	{
		double T = 0.0;
		double LocalDistance = 0.0;
	};

	struct FSplineEvaluationSegment
	{
		ESplineSegmentInterpolation Interpolation = ESplineSegmentInterpolation::Linear;
		FVector3 Coefficient0{0.0};
		FVector3 Coefficient1{0.0};
		FVector3 Coefficient2{0.0};
		FVector3 Coefficient3{0.0};
		std::vector<FSplineDistanceSample> DistanceSamples;
		double StartLocalDistance = 0.0;
		double LocalLength = 0.0;
		FBox LocalBounds;
	};

	class FSplineEvaluationData final
	{
	public:
		ENGINE_API auto Evaluate(FSplineParameter Parameter) const -> FSplineSample;
		ENGINE_API auto EvaluateAtLocalDistance(double LocalDistance) const -> FSplineSample;
		ENGINE_API auto GetLocalDistanceAtParameter(FSplineParameter Parameter) const -> double;
		ENGINE_API auto GetParameterAtLocalDistance(double LocalDistance) const -> FSplineParameter;
		ENGINE_API auto FindNearestParameter(const FVector3& LocalPosition) const -> FSplineParameter;

		auto GetNumSegments() const -> uint32 { return static_cast<uint32>(Segments.size()); }
		auto GetLocalLength() const -> double { return LocalLength; }
		auto GetLocalBounds() const -> const FBox& { return LocalBounds; }
		auto IsClosedLoop() const -> bool { return bClosedLoop; }
		auto GetSegments() const -> const std::vector<FSplineEvaluationSegment>& { return Segments; }

	private:
		friend struct FSplineCurve;
		auto ResolveParameter(FSplineParameter Parameter) const -> FSplineParameter;

		std::vector<FSplineEvaluationSegment> Segments;
		FVector3 SinglePoint{0.0};
		FBox LocalBounds;
		double LocalLength = 0.0;
		bool bHasPoint = false;
		bool bClosedLoop = false;
	};

	// Owns only reflected spline authoring data. Query state is built into an immutable snapshot.
	DSTRUCT()
	struct FSplineCurve
	{
		GENERATED_BODY()

		ENGINE_API FSplineCurve();

		auto GetPoints() const -> const std::vector<FSplinePoint>& { return Points; }
		ENGINE_API auto GetPoint(uint32 PointIndex) const -> const FSplinePoint*;
		ENGINE_API auto FindPointIndex(const FGuid& PointId) const -> std::optional<uint32>;
		auto GetNumPoints() const -> uint32 { return static_cast<uint32>(Points.size()); }
		ENGINE_API auto GetNumSegments() const -> uint32;

		ENGINE_API auto SetPoints(std::vector<FSplinePoint> InPoints) -> void;
		ENGINE_API auto AddPoint(FSplinePoint Point) -> uint32;
		ENGINE_API auto InsertPoint(uint32 PointIndex, FSplinePoint Point) -> bool;
		ENGINE_API auto DuplicatePoint(uint32 PointIndex) -> std::optional<uint32>;
		ENGINE_API auto UpdatePoint(uint32 PointIndex, FSplinePoint Point) -> bool;
		ENGINE_API auto RemovePoint(uint32 PointIndex) -> bool;
		ENGINE_API auto MovePoint(uint32 FromIndex, uint32 ToIndex) -> bool;
		ENGINE_API auto ClearPoints() -> void;
		ENGINE_API auto RepairPointIds() -> bool;

		auto IsClosedLoop() const -> bool { return bClosedLoop; }
		ENGINE_API auto SetClosedLoop(bool bInClosedLoop) -> void;

		ENGINE_API auto BuildEvaluationData() const -> std::shared_ptr<const FSplineEvaluationData>;

	private:
		DPROPERTY(Edit)
		std::vector<FSplinePoint> Points;

		DPROPERTY(Edit)
		bool bClosedLoop = false;
	};
} // namespace Durin
