#pragma once

#include <expected>

#include "EngineAPI.h"
#include "DObject/ObjectPtr.h"

namespace Durin
{
	class DMaterialInterface;

	enum class EStaticMeshMaterialOverrideError : uint8 { None, TooManySlots, IncompatibleObject };
	struct FStaticMeshMaterialOverrideError
	{
		EStaticMeshMaterialOverrideError Code = EStaticMeshMaterialOverrideError::None;
		uint64 ActualCount = 0;
		uint64 MaximumCount = 0;
		uint64 Index = 0;
		std::string ObjectPath;
		std::string ActualType;
	};

	ENGINE_API auto FormatStaticMeshMaterialOverrideError(const FStaticMeshMaterialOverrideError& Error,
		std::string_view ConsumerName) -> std::string;

	// Shared positional-override validation used by every StaticMesh geometry consumer.
	ENGINE_API auto ValidateStaticMeshMaterialOverrides(
		std::span<const TObjectPtr<DMaterialInterface>> Overrides) -> std::expected<void, FStaticMeshMaterialOverrideError>;
}
