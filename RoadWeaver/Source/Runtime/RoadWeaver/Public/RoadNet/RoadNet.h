#pragma once

#include "DObject/Object.h"
#include "RoadNet/RoadNetTypes.h"
#include "RoadWeaverAPI.h"

#include "RoadNet.gen.h"

namespace Durin::RoadNet
{
	inline constexpr uint32 RoadNetSchemaVersion = 2;
	inline constexpr double RoadEndpointTolerance = 1.e-4;
	inline constexpr double RoadCoordinateLimit = 1.e7;

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
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetDefinition() const -> const FDefinition& { return Definition; }
		auto GetNodes() const -> const std::vector<FNode>& { return Definition.Nodes; }
		auto GetRoads() const -> const std::vector<FRoad>& { return Definition.Roads; }
		auto GetJunctions() const -> const std::vector<FJunction>& { return Definition.Junctions; }

		// Validates the complete candidate before replacing authored state.
		ROADWEAVER_API auto SetDefinition(
			FDefinition InDefinition, std::string& OutError) -> bool;
		ROADWEAVER_API auto PostLoad(std::string& OutError) -> bool override;
		ROADWEAVER_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal,
			std::string& OutError) -> bool override;
		ROADWEAVER_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		// Owning-thread observers run after publication; reentrant mutations are rejected.
		ROADWEAVER_API auto AddMutationListener(std::function<void()> Listener) -> uint64;
		ROADWEAVER_API auto RemoveMutationListener(uint64 Id) -> void;

	private:
		auto PublishRevision() -> void;
		std::map<uint64, std::function<void()>> Listeners;
		uint64 NextListenerId = 1;
		bool bPublishing = false;

		DPROPERTY()
		uint32 SchemaVersion = RoadNetSchemaVersion;

		DPROPERTY()
		uint64 Revision = 0;

		DPROPERTY(Edit)
		FDefinition Definition;
	};

	// Validates stable identities, topology references, and finite authored dimensions.
	ROADWEAVER_API auto ValidateDefinition(
		const FDefinition& Definition, std::string& OutError) -> bool;
} // namespace Durin::RoadNet
