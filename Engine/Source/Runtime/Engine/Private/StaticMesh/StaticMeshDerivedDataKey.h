#pragma once

#include "CoreMinimal.h"

#if DURIN_WITH_EDITOR

#include "Asset/PayloadTargetPlatform.h"
#include "Materials/MeshMaterialSlot.h"
#include "StaticMesh/StaticMeshBuildTypes.h"
#include "DerivedDataBuildDefinition.h"

namespace Durin
{
	inline constexpr uint32 StaticMeshRenderBuildFunctionVersion = 1;
	inline constexpr uint32 StaticMeshRenderOutputSchemaVersion = 2;

	ENGINE_API auto BuildStaticMeshReconciliationHash(
		std::span<const FMeshMaterialSlotDefinition> MaterialSlots,
		float NormalizedSize) -> FXxHash128;
	ENGINE_API auto BuildStaticMeshReconciliationHash(
		std::span<const FStaticMeshBuildMaterialSlot> MaterialSlots, float NormalizedSize) -> FXxHash128;
	ENGINE_API auto MakeStaticMeshSessionDefinition(uint32 MaterialSlotCount, uint64 BuilderVersion)
		-> std::expected<DerivedData::FBuildDefinition, DerivedData::FBuildDefinitionError>;
	ENGINE_API auto GetStaticMeshBuildDescriptor(uint32 FunctionVersion = StaticMeshRenderBuildFunctionVersion, uint32 OutputVersion = StaticMeshRenderOutputSchemaVersion)
		-> DerivedData::FBuildFunctionDescriptor;

}

#endif
