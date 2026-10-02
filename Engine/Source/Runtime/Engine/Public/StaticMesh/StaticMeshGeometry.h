#pragma once

#include "EngineAPI.h"
#include "MeshDescription/MeshDescription.h"
#include "Math/Box.h"

#include <optional>

namespace Durin
{
	struct FStaticMeshPositionNormalization
	{
		FVector3f Center;
		float Scale;
	};
	// Shared by render and collision projections; preserves identical float arithmetic.
	ENGINE_API auto GetStaticMeshPositionNormalization(const FBox& Bounds, float NormalizedSize)
		-> std::optional<FStaticMeshPositionNormalization>;

}
