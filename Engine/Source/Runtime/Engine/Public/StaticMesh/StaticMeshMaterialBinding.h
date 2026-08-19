#pragma once

#include "EngineAPI.h"
#include "DObject/ObjectPtr.h"

namespace Durin
{
	class DMaterialInterface;

	// Shared positional-override validation used by every StaticMesh geometry consumer.
	ENGINE_API auto ValidateStaticMeshMaterialOverrides(
		std::span<const TObjectPtr<DMaterialInterface>> Overrides,
		std::string_view ConsumerName,
		std::string& OutError) -> bool;
}
