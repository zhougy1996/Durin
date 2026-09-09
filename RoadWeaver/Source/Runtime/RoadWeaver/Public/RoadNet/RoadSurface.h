#pragma once

#include "RoadNet/RoadNet.h"
#include "Math/Transform.h"
#include "Spline/SplineMeshDeformer.h"

#include "RoadSurface.gen.h"

namespace Durin::RoadNet
{
	// Selects an explicit editing operation; never used by preview rebuilding.
	DENUM()
	enum class ERoadSurfaceMode : uint8 { Unconstrained, Plane, Sphere };

	// Parameters for explicit fitting and legacy Actor deserialization only.
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
		uint32 SegmentIndex = 0;
		double StartT = 0.0;
		double EndT = 1.0;
	};

	// Built synchronously, then shared read-only; failed builds never replace an output.
	class FRoadAlignment final
	{
	public:
		ROADWEAVER_API static auto Build(const FRoad& Road, const FRoadPlanet& Planet,
			std::shared_ptr<const FRoadAlignment>& OutSnapshot,
			std::string& OutError) -> bool;
		// Rejects non-finite/out-of-range stations; does not clamp authored stationing.
		ROADWEAVER_API auto Sample(double DistanceMeters, FRoadSample& OutSample,
			std::string& OutError) const -> bool;
		ROADWEAVER_API auto SampleWorld(double DistanceMeters, const FTransform& Placement,
			FRoadSample& OutSample, std::string& OutError) const -> bool;
		auto GetLengthMeters() const -> double { return Evaluation->GetLocalLength(); }
		auto GetIntervals() const -> const std::vector<FRoadInterval>& { return Intervals; }

	private:
		FRoadPlanet Planet;
		std::vector<FLaneSection> Sections;
		std::vector<FRoadInterval> Intervals;
		std::shared_ptr<const FSplineEvaluationData> Evaluation;
	};

	// Fits a detached complete candidate, remaps section stations by normalized road
	// distance, and synchronizes nodes/connectors. Failure leaves the input untouched.
	// Cubic sphere fits are certified within 1 mm radial deviation, not exact arcs.
	ROADWEAVER_API auto FitRoadDefinition(FDefinition& Definition, const FRoadSurface& Operation,
		std::string& OutError) -> bool;

	ROADWEAVER_API auto ValidateRoadPlacement(const FTransform& Placement, std::string& OutError) -> bool;
}
