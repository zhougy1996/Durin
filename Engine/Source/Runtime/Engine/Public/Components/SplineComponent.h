#pragma once

#include "Components/SceneComponent.h"
#include "Spline/SplineCurve.h"

#include "SplineComponent.gen.h"

namespace Durin
{
	enum class ESplineCoordinateSpace : uint8
	{
		Local,
		World
	};

	DCLASS()
	class DSplineComponent : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		auto GetSplineCurve() const -> const FSplineCurve& { return SplineCurve; }
		auto GetSplinePoints() const -> const std::vector<FSplinePoint>& { return SplineCurve.GetPoints(); }
		auto GetSplinePoint(uint32 PointIndex) const -> const FSplinePoint* { return SplineCurve.GetPoint(PointIndex); }
		auto GetNumSplinePoints() const -> uint32 { return SplineCurve.GetNumPoints(); }
		auto GetNumSplineSegments() const -> uint32 { return SplineCurve.GetNumSegments(); }

		ENGINE_API auto SetSplinePoints(std::vector<FSplinePoint> InPoints) -> void;
		ENGINE_API auto AddSplinePoint(const FSplinePoint& Point) -> uint32;
		ENGINE_API auto UpdateSplinePoint(uint32 PointIndex, const FSplinePoint& Point) -> bool;
		ENGINE_API auto RemoveSplinePoint(uint32 PointIndex) -> bool;
		ENGINE_API auto ClearSplinePoints() -> void;

		auto IsClosedLoop() const -> bool { return SplineCurve.IsClosedLoop(); }
		ENGINE_API auto SetClosedLoop(bool bClosedLoop) -> void;
		auto GetReparamStepsPerSegment() const -> int32 { return SplineCurve.GetReparamStepsPerSegment(); }
		ENGINE_API auto SetReparamStepsPerSegment(int32 Steps) -> void;

		ENGINE_API auto GetLocationAtParam(double Param, ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FVector3;
		ENGINE_API auto GetTangentAtParam(double Param, ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FVector3;
		ENGINE_API auto GetDirectionAtParam(double Param, ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FVector3;
		ENGINE_API auto GetRotationAtParam(double Param, ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FQuat;
		ENGINE_API auto GetScaleAtParam(double Param, ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FVector3;
		ENGINE_API auto GetTransformAtParam(double Param, ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FTransform;

		// Distance is measured along the local curve; coordinate space only controls the returned value.
		auto GetSplineLength() const -> double { return SplineCurve.GetSplineLength(); }
		auto GetDistanceAtParam(double Param) const -> double { return SplineCurve.GetDistanceAtParam(Param); }
		auto GetParamAtDistance(double Distance) const -> double { return SplineCurve.GetParamAtDistance(Distance); }
		ENGINE_API auto GetLocationAtDistance(double Distance, ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FVector3;
		ENGINE_API auto GetTangentAtDistance(double Distance, ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FVector3;
		ENGINE_API auto GetDirectionAtDistance(double Distance, ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FVector3;
		ENGINE_API auto GetTransformAtDistance(double Distance, ESplineCoordinateSpace Space = ESplineCoordinateSpace::Local) const -> FTransform;

		ENGINE_API auto UpdateSpline() -> void;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;

	private:
		auto TransformTangentToWorld(const FVector3& LocalTangent) const -> FVector3;

		DPROPERTY(Edit)
		FSplineCurve SplineCurve;
	};
} // namespace Durin
