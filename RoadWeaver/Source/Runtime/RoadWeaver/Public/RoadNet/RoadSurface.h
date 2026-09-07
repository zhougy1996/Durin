#pragma once

#include "RoadNet/RoadNet.h"
#include "Math/Transform.h"
#include "Spline/SplineMeshDeformer.h"

#include "RoadSurface.gen.h"

namespace Durin::RoadNet
{
	// Selects the analytic projection applied after Cartesian spline evaluation.
	DENUM()
	enum class ERoadSurfaceMode : uint8 { Unconstrained, Plane, Sphere };

	// Pure authored parameters in network-local meters; snapshots retain their own copy.
	DSTRUCT()
	struct FRoadSurface
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		ERoadSurfaceMode Mode = ERoadSurfaceMode::Unconstrained;

		DPROPERTY(Edit)
		FVector3 Origin{0.0};

		DPROPERTY(Edit)
		FVector3 Normal{0.0, 0.0, 1.0};

		DPROPERTY(Edit)
		double RadiusMeters = 1000.0;

		DPROPERTY(Edit)
		double ElevationMeters = 0.0;

		// Exact comparison of active inputs; no tolerance may hide a small authored edit.
		ROADWEAVER_API auto HasSameGenerationConfig(const FRoadSurface& Other) const -> bool;
	};

	// Signed lateral extents use positive side to the right of the reference curve.
	struct FRoadLaneExtent
	{
		FGuid LaneId;
		double MinimumMeters = 0.0;
		double MaximumMeters = 0.0;
	};

	// Immutable query result at a final evaluated station, in meters.
	struct FRoadSample
	{
		double DistanceMeters = 0.0;
		FVector3 Position{0.0};
		FSplineMeshFrame Frame;
		std::vector<FRoadLaneExtent> Lanes;
	};

	// One bounded Hermite interval shared by distance queries and preview deformation.
	struct FRoadInterval
	{
		FGuid SourcePointId;
		uint32 SubdivisionKey = 1;
		FSplineMeshParams Params;
	};

	// Built synchronously, then shared read-only; failed builds never replace an output.
	class FRoadAlignment final
	{
	public:
		ROADWEAVER_API static auto Build(const FRoad& Road, const FRoadSurface& Surface,
			std::shared_ptr<const FRoadAlignment>& OutSnapshot,
			std::string& OutError) -> bool;
		// Rejects non-finite/out-of-range stations; does not clamp authored stationing.
		ROADWEAVER_API auto Sample(double DistanceMeters, FRoadSample& OutSample,
			std::string& OutError) const -> bool;
		ROADWEAVER_API auto SampleWorld(double DistanceMeters, const FTransform& Placement,
			FRoadSample& OutSample, std::string& OutError) const -> bool;
		auto GetLengthMeters() const -> double { return Evaluation->GetLocalLength(); }
		auto GetIntervals() const -> const std::vector<FRoadInterval>& { return Intervals; }
		auto GetSurface() const -> const FRoadSurface& { return Surface; }

	private:
		FRoadSurface Surface;
		std::vector<FLaneSection> Sections;
		std::vector<FRoadInterval> Intervals;
		std::shared_ptr<const FSplineEvaluationData> Evaluation;
	};

	ROADWEAVER_API auto ValidateRoadPlacement(const FTransform& Placement, std::string& OutError) -> bool;
}
