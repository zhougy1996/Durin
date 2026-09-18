#pragma once

#include "DObject/Object.h"
#include "RoadNet/RoadNetTypes.h"
#include "RoadWeaverAPI.h"

#include "RoadNet.gen.h"

namespace Durin::RoadNet
{
	struct FRoadSurface;

	inline constexpr uint32 RoadNetSchemaVersion = 3;
	inline constexpr double RoadEndpointTolerance = 1.e-4;
	inline constexpr double RoadCoordinateLimit = 1.e7;

	enum class ERoadValidationEntity : uint8 { None, Node, Road, Lane, Junction, Connection, CurvePoint };
	enum class ERoadDefinitionError : uint8
	{
		None, InvalidId, DuplicateId, InvalidCurveShape, InvalidCurveCoordinates,
		InvalidCurveMode, DegenerateCurve, InvalidPlanet, InvalidNodePosition,
		MissingEndpoint, EndpointMismatch, InvalidRoadSpeed, MissingSections,
		StationCoverage, InvalidSection, DuplicateLaneIndex, InvalidLane,
		InvalidTransition, MissingContinuity, MissingJunctionNode, MissingConnectionLane,
		InvalidTerminalFlow, ConnectorEndpointMismatch, PlanetChanged,
	};
	struct FRoadDefinitionError
	{
		ERoadDefinitionError Code = ERoadDefinitionError::None;
		ERoadValidationEntity Entity = ERoadValidationEntity::None;
		FGuid Id;
		FGuid FromId;
		FGuid ToId;
		size_t SectionIndex = 0;
		int32 LaneIndex = 0;
		double Length = 0;
		std::optional<FSplinePoint> Point;
		std::optional<FRoadPlanet> Planet;
		std::optional<FRoadPlanet> ExpectedPlanet;
	};
	struct FRoadDefinitionResult
	{
		FRoadDefinitionError Error;
		auto Succeeded() const -> bool { return Error.Code == ERoadDefinitionError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	ROADWEAVER_API auto FormatRoadDefinitionError(const FRoadDefinitionError& Error) -> std::string;

	// Borrows lane ownership from a definition; invalidated when that value changes.
	struct FLaneOwnership
	{
		const FRoad* Road = nullptr;
		const FLaneSection* Section = nullptr;
		const FLane* Lane = nullptr;
		size_t SectionIndex = 0;
	};

	ROADWEAVER_API auto FindLaneOwnership(const FDefinition& Definition,
		const FGuid& LaneId) -> std::optional<FLaneOwnership>;
	// True only at the traffic-selected terminal section incident to NodeId.
	ROADWEAVER_API auto IsTerminalLane(const FLaneOwnership& Owner,
		const FGuid& NodeId, bool bIncoming) -> bool;

	// Stores the authored semantic road graph without render or simulation-derived state.
	DCLASS(DisplayName = "Road Net")
	class DRoadNet : public DObject
	{
		GENERATED_BODY()

	public:
		ROADWEAVER_API explicit DRoadNet(const FObjectInitializer& ObjectInitializer);

		auto GetSchemaVersion() const -> uint32 { return SchemaVersion; }
		auto GetDefinition() const -> const FDefinition& { return Definition; }
		auto GetNodes() const -> const std::vector<FNode>& { return Definition.Nodes; }
		auto GetRoads() const -> const std::vector<FRoad>& { return Definition.Roads; }
		auto GetJunctions() const -> const std::vector<FJunction>& { return Definition.Junctions; }

		// Validates the complete candidate before replacing authored state.
		ROADWEAVER_API auto SetDefinition(
			FDefinition InDefinition, std::string& OutError) -> bool;
		// Explicit edit: publish final fitted geometry and reconciled stationing together.
		ROADWEAVER_API auto FitToSurface(const FRoadSurface& Operation, std::string& OutError) -> bool;
		ROADWEAVER_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> FObjectValidationResult override;
		ROADWEAVER_API auto PostLoad() -> void override;
		ROADWEAVER_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> FObjectValidationResult override;
		ROADWEAVER_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		// Owning-thread observers run after publication; reentrant mutations are rejected.
		ROADWEAVER_API auto AddMutationListener(std::function<void()> Listener) -> uint64;
		ROADWEAVER_API auto RemoveMutationListener(uint64 Id) -> void;

	private:
		auto ValidateCandidate(const FDefinition& Candidate) const -> FRoadDefinitionResult;
		auto NotifyMutation() -> void;
		std::map<uint64, std::function<void()>> Listeners;
		uint64 NextListenerId = 1;
		bool bPublishing = false;

		DPROPERTY()
		uint32 SchemaVersion = RoadNetSchemaVersion;

		DPROPERTY(Edit)
		FDefinition Definition;
	};

	// Validates stable identities, topology references, and finite authored dimensions.
	ROADWEAVER_API auto ValidateDefinition(
		const FDefinition& Definition) -> FRoadDefinitionResult;
} // namespace Durin::RoadNet
