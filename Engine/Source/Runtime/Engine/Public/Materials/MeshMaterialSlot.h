#pragma once

#include "DObject/CoreDObject.h"
#include "EngineAPI.h"

#include "MeshMaterialSlot.gen.h"

namespace Durin
{
	class DMaterialInterface;

	inline constexpr uint32 MaximumMeshMaterialSlots = 4096;

	// Preserves a stable positional mesh material binding and its source-import provenance.
	DSTRUCT()
	struct FMeshMaterialSlotDefinition
	{
		GENERATED_BODY()

		DPROPERTY()
		FName Name;

		DPROPERTY(EditorOnly)
		std::string SourceName;

		// Original importer index used only for source reconciliation.
		DPROPERTY(EditorOnly)
		uint32 SourceMaterialIndex = 0;

		DPROPERTY()
		TObjectPtr<DMaterialInterface> DefaultMaterial;

		auto operator==(const FMeshMaterialSlotDefinition&) const -> bool = default;
	};
}
