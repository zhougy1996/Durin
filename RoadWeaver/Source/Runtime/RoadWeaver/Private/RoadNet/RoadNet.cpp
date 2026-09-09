#include "RoadNet/RoadNet.h"
#include "RoadNet/RoadSurface.h"
#include "Logging/LogMacros.h"
#include "DObject/Property.h"
#include "Math/Operations.h"

namespace Durin::RoadNet
{
	namespace
	{
		auto IsFinite(const FVector3& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y)
				&& std::isfinite(Value.z) && std::abs(Value.x) <= RoadCoordinateLimit
				&& std::abs(Value.y) <= RoadCoordinateLimit && std::abs(Value.z) <= RoadCoordinateLimit;
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

		auto ValidateCurve(const FSplineCurve& Curve, std::string_view Kind,
			const FGuid& Id, std::string& OutError) -> bool
		{
			auto Fail = [&](std::string_view Message) {
				OutError = std::format("Road Net {} '{}' {}", Kind, Id.ToString(), Message);
				return false;
			};
			if (Curve.IsClosedLoop() || Curve.GetNumPoints() < 2)
				return Fail("requires an open curve with at least two points.");
			std::unordered_set<FGuid> PointIds;
			for (const auto& Point : Curve.GetPoints())
			{
				if (!AddUniqueId(Point.Id, "curve point", PointIds, OutError)) return false;
				if (!IsFinite(Point.Position) || !IsFinite(Point.ArriveTangent) || !IsFinite(Point.LeaveTangent))
					return Fail("has non-finite or unsupported point/tangent coordinates.");
				if (Point.OutgoingInterpolation > ESplineSegmentInterpolation::Cubic
					|| Point.TangentMode > ESplineTangentMode::ManualBroken)
					return Fail("has an invalid curve interpolation or tangent mode.");
			}
			const auto Evaluation = Curve.BuildEvaluationData();
			for (const auto& Segment : Evaluation->GetSegments())
				if (!std::isfinite(Segment.LocalLength) || Segment.LocalLength <= 1.e-6)
					return Fail("has degenerate reference geometry.");
			return true;
		}
	}

	auto FindLaneOwnership(const FDefinition& Definition, const FGuid& LaneId)
		-> std::optional<FLaneOwnership>
	{
		for (const auto& Road : Definition.Roads)
			for (size_t Index = 0; Index < Road.LaneSections.size(); ++Index)
				for (const auto& Lane : Road.LaneSections[Index].Lanes)
					if (Lane.Id == LaneId) return FLaneOwnership{&Road, &Road.LaneSections[Index], &Lane, Index};
		return std::nullopt;
	}

	auto IsTerminalLane(const FLaneOwnership& Owner, const FGuid& NodeId, bool bIncoming) -> bool
	{
		if (!Owner.Road || !Owner.Section || !Owner.Lane) return false;
		const bool bEnd = (Owner.Lane->Direction == ELaneDirection::AlongReferenceLine) == bIncoming;
		return (bEnd ? Owner.Road->EndNodeId : Owner.Road->StartNodeId) == NodeId
			&& Owner.SectionIndex == (bEnd ? Owner.Road->LaneSections.size() - 1 : 0);
	}

	DRoadNet::DRoadNet(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	static auto ValidateDefinitionImpl(const FDefinition& Definition, std::string& OutError, bool RequireLength) -> bool
	{
		OutError.clear();
		const auto& Planet = Definition.Planet;
		FVector3 Up;
		if (!Planet.Id.IsValid() || !IsFinite(Planet.Center) || !std::isfinite(Planet.RadiusMeters)
			|| Planet.RadiusMeters < 1 || Planet.RadiusMeters > RoadCoordinateLimit
			|| (!Planet.bRadialUp && !Math::TryNormalize(Planet.ReferenceUp, Up, 1.e-18)))
		{ OutError = "Road network requires a valid fixed planet binding and reference up."; return false; }
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
			if (!ValidateCurve(Road.ReferenceLine, "road", Road.Id, OutError)) return false;
			const auto FindNode = [&](const FGuid& Id) -> const FNode& {
				return *std::ranges::find(Definition.Nodes, Id, &FNode::Id);
			};
			if (Math::Length(Road.ReferenceLine.GetPoints().front().Position - FindNode(Road.StartNodeId).Position) > RoadEndpointTolerance
				|| Math::Length(Road.ReferenceLine.GetPoints().back().Position - FindNode(Road.EndNodeId).Position) > RoadEndpointTolerance)
			{
				OutError = std::format("Road Net road '{}' curve endpoints disagree with nodes.", Road.Id.ToString());
				return false;
			}
			if (!std::isfinite(Road.SpeedLimitMetersPerSecond)
				|| Road.SpeedLimitMetersPerSecond <= 0.0)
			{
				OutError = std::format(
					"Road Net road '{}' has an invalid speed limit.", Road.Id.ToString());
				return false;
			}

			if (Road.LaneSections.empty())
			{
				OutError = std::format("Road Net road '{}' has no lane sections.", Road.Id.ToString());
				return false;
			}
			const double Length = Road.ReferenceLine.BuildEvaluationData()->GetLocalLength();
			if (RequireLength && std::abs(Road.LaneSections.back().EndDistanceMeters - Length) > std::max(1.e-4, 1.e-6 * Length))
			{
				OutError = std::format("Road '{}' lane stations must cover final curve length {}.", Road.Id.ToString(), Length);
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
					|| std::abs(Section.StartDistanceMeters - PreviousEnd) > RoadEndpointTolerance
					|| Section.Lanes.empty())
				{
					OutError = std::format(
						"Road Net road '{}' has an empty, gapped or invalid lane section at index {}.",
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
					if (Lane.Index == 0 || Lane.Direction > ELaneDirection::AgainstReferenceLine
						|| Lane.Type > ELaneType::Sidewalk
						|| !std::isfinite(Lane.WidthMeters) || Lane.WidthMeters <= 0.0
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

		for (const auto& Road : Definition.Roads)
		{
			std::set<std::pair<FGuid, FGuid>> Pairs;
			std::unordered_set<FGuid> Incoming, Outgoing;
			for (const auto& Transition : Road.SectionTransitions)
			{
				const auto From = FindLaneOwnership(Definition, Transition.IncomingLaneId);
				const auto To = FindLaneOwnership(Definition, Transition.OutgoingLaneId);
				if (!From || !To || From->Road != &Road || To->Road != &Road
					|| From->Lane->Direction != To->Lane->Direction
					|| (From->Lane->Direction == ELaneDirection::AlongReferenceLine
						? From->SectionIndex + 1 != To->SectionIndex : To->SectionIndex + 1 != From->SectionIndex)
					|| !Pairs.emplace(Transition.IncomingLaneId, Transition.OutgoingLaneId).second)
				{
					OutError = std::format("Road Net road '{}' has invalid section transition '{}' -> '{}'.",
						Road.Id.ToString(), Transition.IncomingLaneId.ToString(), Transition.OutgoingLaneId.ToString());
					return false;
				}
				Incoming.insert(Transition.IncomingLaneId);
				Outgoing.insert(Transition.OutgoingLaneId);
			}
			for (size_t Index = 0; Index < Road.LaneSections.size(); ++Index)
				for (const auto& Lane : Road.LaneSections[Index].Lanes)
				{
					const bool Along = Lane.Direction == ELaneDirection::AlongReferenceLine;
					if (((Along ? Index + 1 < Road.LaneSections.size() : Index > 0) && !Incoming.contains(Lane.Id))
						|| ((Along ? Index > 0 : Index + 1 < Road.LaneSections.size()) && !Outgoing.contains(Lane.Id)))
					{
						OutError = std::format("Road Net lane '{}' lacks explicit section continuity.", Lane.Id.ToString());
						return false;
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
				const auto From = FindLaneOwnership(Definition, Connection.IncomingLaneId);
				const auto To = FindLaneOwnership(Definition, Connection.OutgoingLaneId);
				if (!IsTerminalLane(*From, Junction.NodeId, true) || !IsTerminalLane(*To, Junction.NodeId, false)
					|| Connection.TurnType > ETurnType::UTurn)
				{
					OutError = std::format(
						"Road Net lane connection '{}' has disconnected or reversed terminal flow, or invalid turn type.",
						Connection.Id.ToString());
					return false;
				}
				if (Connection.ConnectorCurve.GetNumPoints() != 0)
				{
					if (!ValidateCurve(Connection.ConnectorCurve, "lane connection", Connection.Id, OutError)) return false;
					const auto& Node = *std::ranges::find(Definition.Nodes, Junction.NodeId, &FNode::Id);
					if (Math::Length(Connection.ConnectorCurve.GetPoints().front().Position - Node.Position) > RoadEndpointTolerance
						|| Math::Length(Connection.ConnectorCurve.GetPoints().back().Position - Node.Position) > RoadEndpointTolerance)
					{
						OutError = std::format("Road Net lane connection '{}' connector endpoints disagree with its junction node.", Connection.Id.ToString());
						return false;
					}
				}
			}
		}
		return true;
	}

	auto ValidateDefinition(const FDefinition& Definition, std::string& OutError) -> bool
	{
		return ValidateDefinitionImpl(Definition, OutError, true);
	}

	auto MigrateRoadDefinition(FDefinition& Definition, std::string& OutError) -> bool
	{
		if (!ValidateDefinitionImpl(Definition, OutError, false)) return false;
		auto Candidate = Definition;
		for (auto& Road : Candidate.Roads)
		{
			const double Length = Road.ReferenceLine.BuildEvaluationData()->GetLocalLength();
			const double Scale = Length / Road.LaneSections.back().EndDistanceMeters;
			for (auto& Section : Road.LaneSections)
			{
				Section.StartDistanceMeters *= Scale;
				Section.EndDistanceMeters *= Scale;
			}
			Road.LaneSections.back().EndDistanceMeters = Length;
		}
		if (!ValidateDefinition(Candidate, OutError)) return false;
		Definition = std::move(Candidate);
		return true;
	}

	auto DRoadNet::InitializeMigratedDefinition(FDefinition InDefinition, std::string& OutError) -> bool
	{
		if (!ValidateDefinition(InDefinition, OutError)) return false;
		Definition = std::move(InDefinition);
		SchemaVersion = RoadNetSchemaVersion;
		return true;
	}

	auto DRoadNet::ValidateCandidate(const FDefinition& Candidate, std::string& OutError) const -> bool
	{
		if (!Definition.Roads.empty() && (Candidate.Planet.Id != Definition.Planet.Id
			|| Candidate.Planet.Center != Definition.Planet.Center
			|| Candidate.Planet.RadiusMeters != Definition.Planet.RadiusMeters))
		{
			OutError = "Planet identity, center and radius are fixed at network creation.";
			return false;
		}
		return ValidateDefinition(Candidate, OutError);
	}

	auto DRoadNet::FitToSurface(const FRoadSurface& Operation, std::string& OutError) -> bool
	{
		auto Candidate = Definition;
		return FitRoadDefinition(Candidate, Operation, OutError) && SetDefinition(std::move(Candidate), OutError);
	}

	auto DRoadNet::SetDefinition(FDefinition InDefinition, std::string& OutError) -> bool
	{
		if (bPublishing) { OutError = "Reentrant road mutation is unsupported."; return false; }
		if (!ValidateCandidate(InDefinition, OutError)) return false;
		Definition = std::move(InDefinition);
		SchemaVersion = RoadNetSchemaVersion;
		MarkPackageDirty();
		NotifyMutation();
		OutError.clear();
		return true;
	}

	auto DRoadNet::PostLoad() -> void
	{
		std::string Error;
		if (SchemaVersion != 0 && SchemaVersion != 1 && SchemaVersion != 2 && SchemaVersion != RoadNetSchemaVersion)
		{
			Error = std::format(
				"Road Net schema version {} is unsupported; expected {}.",
				SchemaVersion, RoadNetSchemaVersion);
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		auto Candidate = Definition;
		if (!(SchemaVersion < RoadNetSchemaVersion ? MigrateRoadDefinition(Candidate, Error) : ValidateDefinition(Candidate, Error)))
		{
			Error += " Repair the complete graph candidate (endpoints, stations and lane mappings) and resave as schema 3.";
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		Definition = std::move(Candidate);
		SchemaVersion = RoadNetSchemaVersion;
		NotifyMutation();
	}

	auto DRoadNet::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (bPublishing) { OutError = "Reentrant road mutation is unsupported."; return false; }
		if (Proposal.MemberProperty && Proposal.MemberProperty->NamePrivate == FName("Definition"))
		{
			if (Proposal.DraftRootProperty != Proposal.MemberProperty || !Proposal.DraftRootContainer)
			{
				OutError = "Road Net edits require a complete detached definition.";
				return false;
			}
			return ValidateCandidate(*Proposal.DraftRootProperty->ContainerPtrToValuePtr<FDefinition>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex), OutError);
		}
		return true;
	}

	auto DRoadNet::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("Definition")
			&& !(Event.Phase == EPropertyChangePhase::Committed && Event.Origin == EPropertyChangeOrigin::Edit))
			NotifyMutation();
	}

	auto DRoadNet::AddMutationListener(std::function<void()> Listener) -> uint64
	{
		const uint64 Id = NextListenerId++;
		Listeners.emplace(Id, std::move(Listener));
		return Id;
	}

	auto DRoadNet::RemoveMutationListener(uint64 Id) -> void { Listeners.erase(Id); }

	auto DRoadNet::NotifyMutation() -> void
	{
		bPublishing = true;
		std::vector<uint64> Ids;
		for (const auto& [Id, Listener] : Listeners) Ids.push_back(Id);
		for (uint64 Id : Ids)
			if (const auto It = Listeners.find(Id); It != Listeners.end())
			{
				const auto Callback = It->second;
				if (Callback) Callback();
			}
		bPublishing = false;
	}
} // namespace Durin::RoadNet
