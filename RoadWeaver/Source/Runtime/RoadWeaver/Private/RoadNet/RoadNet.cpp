#include "RoadNet/RoadPropertyEditValidation.h"
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
			const FGuid& Id, ERoadValidationEntity Kind, std::unordered_set<FGuid>& Ids,
			FRoadDefinitionError& OutError) -> bool
		{
			if (!Id.IsValid())
			{
				OutError = {.Code = ERoadDefinitionError::InvalidId, .Entity = Kind, .Id = Id};
				return false;
			}
			if (!Ids.insert(Id).second)
			{
				OutError = {.Code = ERoadDefinitionError::DuplicateId, .Entity = Kind, .Id = Id};
				return false;
			}
			return true;
		}

		auto ValidateCurve(const FSplineCurve& Curve, ERoadValidationEntity Kind,
			const FGuid& Id, FRoadDefinitionError& OutError) -> bool
		{
			auto Fail = [&](ERoadDefinitionError Code, const FSplinePoint* Point = nullptr) {
				OutError = {.Code = Code, .Entity = Kind, .Id = Id};
				if (Point) OutError.Point = *Point;
				return false;
			};
			if (Curve.IsClosedLoop() || Curve.GetNumPoints() < 2)
				return Fail(ERoadDefinitionError::InvalidCurveShape);
			std::unordered_set<FGuid> PointIds;
			for (const auto& Point : Curve.GetPoints())
			{
				if (!AddUniqueId(Point.Id, ERoadValidationEntity::CurvePoint, PointIds, OutError)) return false;
				if (!IsFinite(Point.Position) || !IsFinite(Point.ArriveTangent) || !IsFinite(Point.LeaveTangent))
					return Fail(ERoadDefinitionError::InvalidCurveCoordinates, &Point);
				if (Point.OutgoingInterpolation > ESplineSegmentInterpolation::Cubic
					|| Point.TangentMode > ESplineTangentMode::ManualBroken)
					return Fail(ERoadDefinitionError::InvalidCurveMode, &Point);
			}
			const auto Evaluation = Curve.BuildEvaluationData();
			for (const auto& Segment : Evaluation->GetSegments())
				if (!std::isfinite(Segment.LocalLength) || Segment.LocalLength <= 1.e-6)
					return Fail(ERoadDefinitionError::DegenerateCurve);
			return true;
		}
	}

	auto FormatRoadDefinitionError(const FRoadDefinitionError& Error) -> std::string
	{
		std::string_view Kind;
		switch (Error.Entity)
		{
		case ERoadValidationEntity::CurvePoint: Kind = "curve point"; break;
		case ERoadValidationEntity::Connection: Kind = "lane connection"; break;
		case ERoadValidationEntity::Junction: Kind = "junction"; break;
		case ERoadValidationEntity::Node: Kind = "node"; break;
		case ERoadValidationEntity::Road: Kind = "road"; break;
		case ERoadValidationEntity::Lane: Kind = "lane"; break;
		default: break;
		}
		switch (Error.Code)
		{
		case ERoadDefinitionError::None: return {};
		case ERoadDefinitionError::InvalidId: return std::format("Road Net {} has an invalid ID.", Kind);
		case ERoadDefinitionError::DuplicateId: return std::format("Road Net {} ID '{}' is duplicated.", Kind, Error.Id.ToString());
		case ERoadDefinitionError::InvalidCurveShape: return std::format("Road Net {} '{}' requires an open curve with at least two points.", Kind, Error.Id.ToString());
		case ERoadDefinitionError::InvalidCurveCoordinates: return std::format("Road Net {} '{}' has non-finite or unsupported point/tangent coordinates.", Kind, Error.Id.ToString());
		case ERoadDefinitionError::InvalidCurveMode: return std::format("Road Net {} '{}' has an invalid curve interpolation or tangent mode.", Kind, Error.Id.ToString());
		case ERoadDefinitionError::DegenerateCurve: return std::format("Road Net {} '{}' has degenerate reference geometry.", Kind, Error.Id.ToString());
		case ERoadDefinitionError::InvalidPlanet: return "Road network requires a valid fixed planet binding and reference up.";
		case ERoadDefinitionError::InvalidNodePosition: return std::format(
					"Road Net node '{}' has a non-finite position.", Error.Id.ToString());
		case ERoadDefinitionError::MissingEndpoint: return std::format(
					"Road Net road '{}' references a missing endpoint node.",
					Error.Id.ToString());
		case ERoadDefinitionError::EndpointMismatch: return std::format("Road Net road '{}' curve endpoints disagree with nodes.", Error.Id.ToString());
		case ERoadDefinitionError::InvalidRoadSpeed: return std::format(
					"Road Net road '{}' has an invalid speed limit.", Error.Id.ToString());
		case ERoadDefinitionError::MissingSections: return std::format("Road Net road '{}' has no lane sections.", Error.Id.ToString());
		case ERoadDefinitionError::StationCoverage: return std::format("Road '{}' lane stations must cover final curve length {}.", Error.Id.ToString(), Error.Length);
		case ERoadDefinitionError::InvalidSection: return std::format(
						"Road Net road '{}' has an empty, gapped or invalid lane section at index {}.",
						Error.Id.ToString(), Error.SectionIndex);
		case ERoadDefinitionError::DuplicateLaneIndex: return std::format(
							"Road Net road '{}' has duplicate lane index {} in section {}.",
							Error.Id.ToString(), Error.LaneIndex, Error.SectionIndex);
		case ERoadDefinitionError::InvalidLane: return std::format(
							"Road Net lane '{}' has an invalid width or speed limit.",
							Error.Id.ToString());
		case ERoadDefinitionError::InvalidTransition: return std::format("Road Net road '{}' has invalid section transition '{}' -> '{}'.",
						Error.Id.ToString(), Error.FromId.ToString(), Error.ToId.ToString());
		case ERoadDefinitionError::MissingContinuity: return std::format("Road Net lane '{}' lacks explicit section continuity.", Error.Id.ToString());
		case ERoadDefinitionError::MissingJunctionNode: return std::format(
					"Road Net junction '{}' references a missing node.",
					Error.Id.ToString());
		case ERoadDefinitionError::MissingConnectionLane: return std::format(
						"Road Net lane connection '{}' references a missing lane.",
						Error.Id.ToString());
		case ERoadDefinitionError::InvalidTerminalFlow: return std::format(
						"Road Net lane connection '{}' has disconnected or reversed terminal flow, or invalid turn type.",
						Error.Id.ToString());
		case ERoadDefinitionError::ConnectorEndpointMismatch: return std::format("Road Net lane connection '{}' connector endpoints disagree with its junction node.", Error.Id.ToString());
		case ERoadDefinitionError::PlanetChanged: return "Planet identity, center and radius are fixed at network creation.";
		}
		return {};
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

	static auto ValidateDefinitionImpl(const FDefinition& Definition, FRoadDefinitionError& OutError, bool RequireLength) -> bool
	{
		OutError = {};
		const auto& Planet = Definition.Planet;
		FVector3 Up;
		if (!Planet.Id.IsValid() || !IsFinite(Planet.Center) || !std::isfinite(Planet.RadiusMeters)
			|| Planet.RadiusMeters < 1 || Planet.RadiusMeters > RoadCoordinateLimit
			|| (!Planet.bRadialUp && !Math::TryNormalize(Planet.ReferenceUp, Up, 1.e-18)))
		{ OutError = {.Code = ERoadDefinitionError::InvalidPlanet, .Planet = Planet}; return false; }
		std::unordered_set<FGuid> NodeIds;
		std::unordered_set<FGuid> RoadIds;
		std::unordered_set<FGuid> LaneIds;
		std::unordered_set<FGuid> JunctionIds;
		std::unordered_set<FGuid> ConnectionIds;

		for (const FNode& Node : Definition.Nodes)
		{
			if (!AddUniqueId(Node.Id, ERoadValidationEntity::Node, NodeIds, OutError)) return false;
			if (!IsFinite(Node.Position))
			{
				OutError = {.Code = ERoadDefinitionError::InvalidNodePosition, .Entity = ERoadValidationEntity::Node, .Id = Node.Id};
				return false;
			}
		}

		for (const FRoad& Road : Definition.Roads)
		{
			if (!AddUniqueId(Road.Id, ERoadValidationEntity::Road, RoadIds, OutError)) return false;
			if (!NodeIds.contains(Road.StartNodeId) || !NodeIds.contains(Road.EndNodeId))
			{
				OutError = {.Code = ERoadDefinitionError::MissingEndpoint, .Entity = ERoadValidationEntity::Road, .Id = Road.Id};
				return false;
			}
			if (!ValidateCurve(Road.ReferenceLine, ERoadValidationEntity::Road, Road.Id, OutError)) return false;
			const auto FindNode = [&](const FGuid& Id) -> const FNode& {
				return *std::ranges::find(Definition.Nodes, Id, &FNode::Id);
			};
			if (Math::Length(Road.ReferenceLine.GetPoints().front().Position - FindNode(Road.StartNodeId).Position) > RoadEndpointTolerance
				|| Math::Length(Road.ReferenceLine.GetPoints().back().Position - FindNode(Road.EndNodeId).Position) > RoadEndpointTolerance)
			{
				OutError = {.Code = ERoadDefinitionError::EndpointMismatch, .Entity = ERoadValidationEntity::Road, .Id = Road.Id};
				return false;
			}
			if (!std::isfinite(Road.SpeedLimitMetersPerSecond)
				|| Road.SpeedLimitMetersPerSecond <= 0.0)
			{
				OutError = {.Code = ERoadDefinitionError::InvalidRoadSpeed, .Entity = ERoadValidationEntity::Road, .Id = Road.Id};
				return false;
			}

			if (Road.LaneSections.empty())
			{
				OutError = {.Code = ERoadDefinitionError::MissingSections, .Entity = ERoadValidationEntity::Road, .Id = Road.Id};
				return false;
			}
			const double Length = Road.ReferenceLine.BuildEvaluationData()->GetLocalLength();
			if (RequireLength && std::abs(Road.LaneSections.back().EndDistanceMeters - Length) > std::max(1.e-4, 1.e-6 * Length))
			{
				OutError = {.Code = ERoadDefinitionError::StationCoverage, .Entity = ERoadValidationEntity::Road, .Id = Road.Id, .Length = Length};
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
					OutError = {.Code = ERoadDefinitionError::InvalidSection, .Entity = ERoadValidationEntity::Road, .Id = Road.Id, .SectionIndex = SectionIndex};
					return false;
				}
				PreviousEnd = Section.EndDistanceMeters;
				std::unordered_set<int32> LaneIndices;
				for (const FLane& Lane : Section.Lanes)
				{
					if (!AddUniqueId(Lane.Id, ERoadValidationEntity::Lane, LaneIds, OutError)) return false;
					if (!LaneIndices.insert(Lane.Index).second)
					{
						OutError = {.Code = ERoadDefinitionError::DuplicateLaneIndex, .Entity = ERoadValidationEntity::Road, .Id = Road.Id, .SectionIndex = SectionIndex, .LaneIndex = Lane.Index};
						return false;
					}
					if (Lane.Index == 0 || Lane.Direction > ELaneDirection::AgainstReferenceLine
						|| Lane.Type > ELaneType::Sidewalk
						|| !std::isfinite(Lane.WidthMeters) || Lane.WidthMeters <= 0.0
						|| !std::isfinite(Lane.SpeedLimitMetersPerSecond)
						|| Lane.SpeedLimitMetersPerSecond < 0.0)
					{
						OutError = {.Code = ERoadDefinitionError::InvalidLane, .Entity = ERoadValidationEntity::Lane, .Id = Lane.Id};
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
					OutError = {.Code = ERoadDefinitionError::InvalidTransition, .Entity = ERoadValidationEntity::Road, .Id = Road.Id, .FromId = Transition.IncomingLaneId, .ToId = Transition.OutgoingLaneId};
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
						OutError = {.Code = ERoadDefinitionError::MissingContinuity, .Entity = ERoadValidationEntity::Lane, .Id = Lane.Id};
						return false;
					}
				}
		}

		for (const FJunction& Junction : Definition.Junctions)
		{
			if (!AddUniqueId(Junction.Id, ERoadValidationEntity::Junction, JunctionIds, OutError)) return false;
			if (!NodeIds.contains(Junction.NodeId))
			{
				OutError = {.Code = ERoadDefinitionError::MissingJunctionNode, .Entity = ERoadValidationEntity::Junction, .Id = Junction.Id};
				return false;
			}
			for (const FLaneConnection& Connection : Junction.LaneConnections)
			{
				if (!AddUniqueId(
					Connection.Id, ERoadValidationEntity::Connection, ConnectionIds, OutError)) return false;
				if (!LaneIds.contains(Connection.IncomingLaneId)
					|| !LaneIds.contains(Connection.OutgoingLaneId))
				{
					OutError = {.Code = ERoadDefinitionError::MissingConnectionLane, .Entity = ERoadValidationEntity::Connection, .Id = Connection.Id};
					return false;
				}
				const auto From = FindLaneOwnership(Definition, Connection.IncomingLaneId);
				const auto To = FindLaneOwnership(Definition, Connection.OutgoingLaneId);
				if (!IsTerminalLane(*From, Junction.NodeId, true) || !IsTerminalLane(*To, Junction.NodeId, false)
					|| Connection.TurnType > ETurnType::UTurn)
				{
					OutError = {.Code = ERoadDefinitionError::InvalidTerminalFlow, .Entity = ERoadValidationEntity::Connection, .Id = Connection.Id};
					return false;
				}
				if (Connection.ConnectorCurve.GetNumPoints() != 0)
				{
					if (!ValidateCurve(Connection.ConnectorCurve, ERoadValidationEntity::Connection, Connection.Id, OutError)) return false;
					const auto& Node = *std::ranges::find(Definition.Nodes, Junction.NodeId, &FNode::Id);
					if (Math::Length(Connection.ConnectorCurve.GetPoints().front().Position - Node.Position) > RoadEndpointTolerance
						|| Math::Length(Connection.ConnectorCurve.GetPoints().back().Position - Node.Position) > RoadEndpointTolerance)
					{
						OutError = {.Code = ERoadDefinitionError::ConnectorEndpointMismatch, .Entity = ERoadValidationEntity::Connection, .Id = Connection.Id};
						return false;
					}
				}
			}
		}
		return true;
	}

	auto ValidateDefinition(const FDefinition& Definition) -> FRoadDefinitionResult
	{
		FRoadDefinitionResult Result;
		ValidateDefinitionImpl(Definition, Result.Error, true);
		return Result;
	}

	auto DRoadNet::ValidateCandidate(const FDefinition& Candidate) const -> FRoadDefinitionResult
	{
		if (!Definition.Roads.empty() && (Candidate.Planet.Id != Definition.Planet.Id
			|| Candidate.Planet.Center != Definition.Planet.Center
			|| Candidate.Planet.RadiusMeters != Definition.Planet.RadiusMeters))
			return {{.Code = ERoadDefinitionError::PlanetChanged, .Planet = Candidate.Planet, .ExpectedPlanet = Definition.Planet}};
		return ValidateDefinition(Candidate);
	}

	auto DRoadNet::FitToSurface(const FRoadSurface& Operation, std::string& OutError) -> bool
	{
		auto Candidate = Definition;
		return FitRoadDefinition(Candidate, Operation, OutError) && SetDefinition(std::move(Candidate), OutError);
	}

	auto DRoadNet::SetDefinition(FDefinition InDefinition, std::string& OutError) -> bool
	{
		if (bPublishing) { OutError = "Reentrant road mutation is unsupported."; return false; }
		const auto Validation = ValidateCandidate(InDefinition);
		if (!Validation) { OutError = FormatRoadDefinitionError(Validation.Error); return false; }
		Definition = std::move(InDefinition);
		SchemaVersion = RoadNetSchemaVersion;
		MarkPackageDirty();
		NotifyMutation();
		OutError.clear();
		return true;
	}

	auto DRoadNet::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> FObjectValidationResult
	{
		if (auto Result = Super::ValidateLoadedObjectGraph(Context); !Result) return Result;
		if (SchemaVersion != RoadNetSchemaVersion)
			return RejectLoadedObjectGraph(GetObjectPath(), std::format("Road Net schema version {} is unsupported; expected {}.", SchemaVersion, RoadNetSchemaVersion));
		if (const auto Validation = ValidateDefinition(Definition); !Validation)
			return RejectLoadedObjectGraph(GetObjectPath(), FormatRoadDefinitionError(Validation.Error));
		return {};
	}

	auto DRoadNet::PostLoad() -> void
	{
		std::string Error;
		if (SchemaVersion != RoadNetSchemaVersion)
		{
			Error = std::format(
				"Road Net schema version {} is unsupported; expected {}.",
				SchemaVersion, RoadNetSchemaVersion);
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		auto Candidate = Definition;
		if (const auto Validation = ValidateDefinition(Candidate); !Validation)
		{
			Error = FormatRoadDefinitionError(Validation.Error) + " Repair the complete graph candidate (endpoints, stations and lane mappings) and resave as schema 3.";
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		Definition = std::move(Candidate);
		SchemaVersion = RoadNetSchemaVersion;
		NotifyMutation();
	}

	auto DRoadNet::PreEditChangeProperty(FPropertyEditProposal& Proposal) -> FObjectValidationResult
	{
		if (auto Result = Super::PreEditChangeProperty(Proposal); !Result) return Result;
		if (bPublishing) { return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::ReentrantEdit); }
		if (Proposal.MemberProperty && Proposal.MemberProperty->NamePrivate == FName("Definition"))
		{
			if (Proposal.DraftRootProperty != Proposal.MemberProperty || !Proposal.DraftRootContainer)
			{
				return RejectPropertyEdit(*this, Proposal, EPropertyEditRejection::IncompleteDraft);
			}
			const auto Validation = ValidateCandidate(*Proposal.DraftRootProperty->ContainerPtrToValuePtr<FDefinition>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex));
			if (!Validation) { return RejectRoadPropertyEdit(*this, Proposal, Validation.Error); }
		}
		return {};
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
