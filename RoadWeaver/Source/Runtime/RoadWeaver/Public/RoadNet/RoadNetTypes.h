#pragma once

#include "Misc/Guid.h"
#include "RoadWeaverAPI.h"
#include "Spline/SplineCurve.h"

#include "RoadNetTypes.gen.h"

namespace Durin::RoadNet
{
	// Identifies the direction in which traffic travels relative to a road reference line.
	DENUM(DisplayName = "Road Lane Direction")
	enum class ELaneDirection : uint8
	{
		AlongReferenceLine,
		AgainstReferenceLine
	};

	// Identifies the traffic role assigned to a lane.
	DENUM(DisplayName = "Road Lane Type")
	enum class ELaneType : uint8
	{
		Driving,
		Shoulder,
		Bicycle,
		Sidewalk
	};

	// Describes the intended movement through a junction lane connection.
	DENUM(DisplayName = "Road Turn Type")
	enum class ETurnType : uint8
	{
		Straight,
		Left,
		Right,
		UTurn
	};

	// Anchors road endpoints in network-local space and carries no render state.
	DSTRUCT()
	struct FNode
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		FGuid Id;

		DPROPERTY(Edit)
		std::string Name;

		DPROPERTY(Edit)
		FVector3 Position{0.0};
	};

	// Defines one directed lane over the containing longitudinal lane section.
	DSTRUCT()
	struct FLane
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		FGuid Id;

		// Signed lateral order within the section; uniqueness is scoped to that section.
		DPROPERTY(Edit)
		int32 Index = 0;

		DPROPERTY(Edit)
		ELaneDirection Direction = ELaneDirection::AlongReferenceLine;

		DPROPERTY(Edit)
		ELaneType Type = ELaneType::Driving;

		DPROPERTY(Edit)
		double WidthMeters = 3.5;

		// Zero inherits the containing road's speed limit.
		DPROPERTY(Edit)
		double SpeedLimitMetersPerSecond = 0.0;
	};

	// Groups the lane layout over one half-open distance interval on a road.
	DSTRUCT()
	struct FLaneSection
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		double StartDistanceMeters = 0.0;

		DPROPERTY(Edit)
		double EndDistanceMeters = 0.0;

		DPROPERTY(Edit)
		std::vector<FLane> Lanes;
	};

	// Owns a road reference line and the authored lane layout along it.
	DSTRUCT()
	struct FRoad
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		FGuid Id;

		DPROPERTY(Edit)
		std::string Name;

		DPROPERTY(Edit)
		FGuid StartNodeId;

		DPROPERTY(Edit)
		FGuid EndNodeId;

		DPROPERTY(Edit)
		FSplineCurve ReferenceLine;

		DPROPERTY(Edit)
		double SpeedLimitMetersPerSecond = 13.8888888889;

		DPROPERTY(Edit)
		std::vector<FLaneSection> LaneSections;
	};

	// Maps one incoming lane to one outgoing lane through a junction.
	DSTRUCT()
	struct FLaneConnection
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		FGuid Id;

		DPROPERTY(Edit)
		FGuid IncomingLaneId;

		DPROPERTY(Edit)
		FGuid OutgoingLaneId;

		DPROPERTY(Edit)
		ETurnType TurnType = ETurnType::Straight;

		// Optional authored connector; an empty curve is reserved for generated geometry.
		DPROPERTY(Edit)
		FSplineCurve ConnectorCurve;
	};

	// Owns the explicit lane-to-lane connectivity at one network node.
	DSTRUCT()
	struct FJunction
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		FGuid Id;

		DPROPERTY(Edit)
		std::string Name;

		DPROPERTY(Edit)
		FGuid NodeId;

		DPROPERTY(Edit)
		std::vector<FLaneConnection> LaneConnections;
	};

	// Owns one complete authored road-network value before it is installed in an asset.
	DSTRUCT()
	struct FDefinition
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		std::vector<FNode> Nodes;

		DPROPERTY(Edit)
		std::vector<FRoad> Roads;

		DPROPERTY(Edit)
		std::vector<FJunction> Junctions;
	};
} // namespace Durin::RoadNet
