#pragma once

#include "DObject/Object.h"
#include "RoadNet/RoadNetTypes.h"
#include "RoadWeaverAPI.h"

#include "RoadNet.gen.h"

namespace Durin::RoadNet
{
	inline constexpr uint32 RoadNetSchemaVersion = 1;

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

	private:
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
