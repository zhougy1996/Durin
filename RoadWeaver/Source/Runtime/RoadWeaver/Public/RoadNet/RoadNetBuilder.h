#pragma once

#include "RoadNet/RoadNetTypes.h"
#include "RoadWeaverAPI.h"

namespace Durin::RoadNet
{
	// Builds a complete road-network value while assigning missing stable identities.
	class FRoadNetBuilder
	{
	public:
		ROADWEAVER_API auto AddNode(std::string Name, const FVector3& Position) -> FGuid;
		ROADWEAVER_API auto AddRoad(FRoad Road) -> FGuid;
		ROADWEAVER_API auto AddJunction(FJunction Junction) -> FGuid;

		auto GetDefinition() const -> const FDefinition& { return Definition; }
		ROADWEAVER_API auto TakeDefinition() -> FDefinition;
		ROADWEAVER_API auto Reset() -> void;

	private:
		FDefinition Definition;
	};
} // namespace Durin::RoadNet
