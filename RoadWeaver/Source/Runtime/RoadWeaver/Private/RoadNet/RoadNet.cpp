#include "RoadNet/RoadNet.h"

namespace Durin::RoadNet
{
	namespace
	{
		auto IsFinite(const FVector3& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y)
				&& std::isfinite(Value.z);
		}

		auto AddUniqueId(
			const FGuid& Id, std::string_view Kind, std::unordered_set<FGuid>& Ids,
			std::string& OutError) -> bool
		{
			if (!Id.IsValid())
			{
				OutError = std::format("Road Net {} has an invalid ID.", Kind);
				return false;
			}
			if (!Ids.insert(Id).second)
			{
				OutError = std::format(
					"Road Net {} ID '{}' is duplicated.", Kind, Id.ToString());
				return false;
			}
			return true;
		}
	}

	DRoadNet::DRoadNet(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto ValidateDefinition(const FDefinition& Definition, std::string& OutError) -> bool
	{
		OutError.clear();
		std::unordered_set<FGuid> NodeIds;
		std::unordered_set<FGuid> RoadIds;
		std::unordered_set<FGuid> LaneIds;
		std::unordered_set<FGuid> JunctionIds;
		std::unordered_set<FGuid> ConnectionIds;

		for (const FNode& Node : Definition.Nodes)
		{
			if (!AddUniqueId(Node.Id, "node", NodeIds, OutError)) return false;
			if (!IsFinite(Node.Position))
			{
				OutError = std::format(
					"Road Net node '{}' has a non-finite position.", Node.Id.ToString());
				return false;
			}
		}

		for (const FRoad& Road : Definition.Roads)
		{
			if (!AddUniqueId(Road.Id, "road", RoadIds, OutError)) return false;
			if (!NodeIds.contains(Road.StartNodeId) || !NodeIds.contains(Road.EndNodeId))
			{
				OutError = std::format(
					"Road Net road '{}' references a missing endpoint node.",
					Road.Id.ToString());
				return false;
			}
			if (Road.ReferenceLine.GetNumPoints() < 2)
			{
				OutError = std::format(
					"Road Net road '{}' requires at least two reference-line points.",
					Road.Id.ToString());
				return false;
			}
			if (!std::isfinite(Road.SpeedLimitMetersPerSecond)
				|| Road.SpeedLimitMetersPerSecond <= 0.0)
			{
				OutError = std::format(
					"Road Net road '{}' has an invalid speed limit.", Road.Id.ToString());
				return false;
			}

			double PreviousEnd = 0.0;
			for (size_t SectionIndex = 0;
				SectionIndex < Road.LaneSections.size(); ++SectionIndex)
			{
				const FLaneSection& Section = Road.LaneSections[SectionIndex];
				if (!std::isfinite(Section.StartDistanceMeters)
					|| !std::isfinite(Section.EndDistanceMeters)
					|| Section.StartDistanceMeters < 0.0
					|| Section.EndDistanceMeters <= Section.StartDistanceMeters
					|| (SectionIndex != 0 && Section.StartDistanceMeters < PreviousEnd))
				{
					OutError = std::format(
						"Road Net road '{}' has an invalid or overlapping lane section at index {}.",
						Road.Id.ToString(), SectionIndex);
					return false;
				}
				PreviousEnd = Section.EndDistanceMeters;
				std::unordered_set<int32> LaneIndices;
				for (const FLane& Lane : Section.Lanes)
				{
					if (!AddUniqueId(Lane.Id, "lane", LaneIds, OutError)) return false;
					if (!LaneIndices.insert(Lane.Index).second)
					{
						OutError = std::format(
							"Road Net road '{}' has duplicate lane index {} in section {}.",
							Road.Id.ToString(), Lane.Index, SectionIndex);
						return false;
					}
					if (!std::isfinite(Lane.WidthMeters) || Lane.WidthMeters <= 0.0
						|| !std::isfinite(Lane.SpeedLimitMetersPerSecond)
						|| Lane.SpeedLimitMetersPerSecond < 0.0)
					{
						OutError = std::format(
							"Road Net lane '{}' has an invalid width or speed limit.",
							Lane.Id.ToString());
						return false;
					}
				}
			}
		}

		for (const FJunction& Junction : Definition.Junctions)
		{
			if (!AddUniqueId(Junction.Id, "junction", JunctionIds, OutError)) return false;
			if (!NodeIds.contains(Junction.NodeId))
			{
				OutError = std::format(
					"Road Net junction '{}' references a missing node.",
					Junction.Id.ToString());
				return false;
			}
			for (const FLaneConnection& Connection : Junction.LaneConnections)
			{
				if (!AddUniqueId(
					Connection.Id, "lane connection", ConnectionIds, OutError)) return false;
				if (!LaneIds.contains(Connection.IncomingLaneId)
					|| !LaneIds.contains(Connection.OutgoingLaneId))
				{
					OutError = std::format(
						"Road Net lane connection '{}' references a missing lane.",
						Connection.Id.ToString());
					return false;
				}
				if (Connection.ConnectorCurve.GetNumPoints() == 1)
				{
					OutError = std::format(
						"Road Net lane connection '{}' has an incomplete connector curve.",
						Connection.Id.ToString());
					return false;
				}
			}
		}
		return true;
	}

	auto DRoadNet::SetDefinition(FDefinition InDefinition, std::string& OutError) -> bool
	{
		if (!ValidateDefinition(InDefinition, OutError)) return false;
		Definition = std::move(InDefinition);
		++Revision;
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DRoadNet::PostLoad(std::string& OutError) -> bool
	{
		if (SchemaVersion != RoadNetSchemaVersion)
		{
			OutError = std::format(
				"Road Net schema version {} is unsupported; expected {}.",
				SchemaVersion, RoadNetSchemaVersion);
			return false;
		}
		return ValidateDefinition(Definition, OutError);
	}
} // namespace Durin::RoadNet
