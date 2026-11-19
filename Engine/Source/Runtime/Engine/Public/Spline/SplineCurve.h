#pragma once

#include "CoreMinimal.h"

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
		double StartKey = 0.0;
		double EndKey = 1.0;
		FVector3 EndPosition{0.0};
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
		ENGINE_API auto BuildScaledDistanceData(const FVector3& Scale) const -> std::shared_ptr<const FSplineEvaluationData>;
		ENGINE_API auto EvaluateAtInputKey(double InputKey) const -> FSplineSample;
		ENGINE_API auto GetParameterAtInputKey(double InputKey) const -> FSplineParameter;
		ENGINE_API auto GetInputKeyAtParameter(FSplineParameter Parameter) const -> double;
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

		struct FSplineAttributeSegment
		{
			FQuat StartRotation;
			FQuat EndRotation;
			FQuat StartRotationControl;
			FQuat EndRotationControl;
			FVector3 StartScale;
			FVector3 EndScale;
			FVector3 StartScaleTangent;
			FVector3 EndScaleTangent;
		};

		std::vector<FSplineEvaluationSegment> Segments;
		std::vector<FSplineAttributeSegment> AttributeSegments;
		FSplineSample SinglePoint;
		double FirstKey = 0.0;
		FBox LocalBounds;
		double LocalLength = 0.0;
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
		ENGINE_API auto HasValidInputKeys() const -> bool;
		ENGINE_API auto RepairInputKeys() -> bool;
		ENGINE_API auto ResolveInsertionKey(uint32 Index, double RequestedKey) const -> std::optional<double>;
		ENGINE_API auto IsValidPointKey(uint32 Index, double InputKey) const -> bool;

		auto IsClosedLoop() const -> bool { return bClosedLoop; }
		ENGINE_API auto SetClosedLoop(bool bInClosedLoop) -> void;
		ENGINE_API auto SetClosedLoopAtPosition(double InputKey) -> bool;
		ENGINE_API auto GetLoopKey() const -> double;

		ENGINE_API auto BuildEvaluationData() const -> std::shared_ptr<const FSplineEvaluationData>;

	private:
		friend class DSplineComponent;
		DPROPERTY(Edit)
		std::vector<FSplinePoint> Points;

		DPROPERTY(Edit)
		bool bClosedLoop = false;

		DPROPERTY()
		double LoopKeyOverride = FSplinePoint::AutoInputKey;

		DPROPERTY(Edit)
		bool bStationaryEndpoints = false;
	};
} // namespace Durin
