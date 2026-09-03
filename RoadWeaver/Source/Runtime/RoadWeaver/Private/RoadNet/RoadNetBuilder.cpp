#include "RoadNet/RoadNetBuilder.h"

namespace Durin::RoadNet
{
	auto FRoadNetBuilder::AddNode(std::string Name, const FVector3& Position) -> FGuid
	{
		const FGuid Id = FGuid::NewGuid();
		Definition.Nodes.push_back({
			.Id = Id,
			.Name = std::move(Name),
			.Position = Position});
		return Id;
	}

	auto FRoadNetBuilder::AddRoad(FRoad Road) -> FGuid
	{
		if (!Road.Id.IsValid()) Road.Id = FGuid::NewGuid();
		for (FLaneSection& Section : Road.LaneSections)
			for (FLane& Lane : Section.Lanes)
				if (!Lane.Id.IsValid()) Lane.Id = FGuid::NewGuid();
		const FGuid Id = Road.Id;
		Definition.Roads.push_back(std::move(Road));
		return Id;
	}

	auto FRoadNetBuilder::AddJunction(FJunction Junction) -> FGuid
	{
		if (!Junction.Id.IsValid()) Junction.Id = FGuid::NewGuid();
		for (FLaneConnection& Connection : Junction.LaneConnections)
			if (!Connection.Id.IsValid()) Connection.Id = FGuid::NewGuid();
		const FGuid Id = Junction.Id;
		Definition.Junctions.push_back(std::move(Junction));
		return Id;
	}

	auto FRoadNetBuilder::TakeDefinition() -> FDefinition
	{
		return std::exchange(Definition, {});
	}

	auto FRoadNetBuilder::Reset() -> void
	{
		Definition = {};
	}
} // namespace Durin::RoadNet
