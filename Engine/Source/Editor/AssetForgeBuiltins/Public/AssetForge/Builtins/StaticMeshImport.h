#pragma once

#include "CoreMinimal.h"
#include "StaticMesh/StaticMeshImportSettings.h"

#include "AssetForgeBuiltinsAPI.h"
#include "Import/EditorReimportHandler.h"
#include "Materials/MeshMaterialSlot.h"
#include "MeshDescription/MeshDescription.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"

namespace Durin::AssetForge::Builtins
{
	// Import policy: preserve existing slot identities and bindings while matching the new source.
	ASSETFORGEBUILTINS_API auto ReconcileStaticMeshMaterialSlots(
		std::span<const FMeshMaterialSlotDefinition> PreviousMaterialSlots,
		std::span<const FMeshPolygonGroup> ImportedSlots) -> std::vector<FMeshMaterialSlotDefinition>;

	// Authored import completion is independent of derived-data compilation and saving.
	[[nodiscard]] ASSETFORGEBUILTINS_API auto ReimportStaticMesh(DStaticMesh& Mesh) -> FReimportResult;
	[[nodiscard]] ASSETFORGEBUILTINS_API auto ReimportStaticMeshFromFile(DStaticMesh& Mesh,
		std::string_view FilePath) -> FReimportResult;
	[[nodiscard]] ASSETFORGEBUILTINS_API auto CreateTransientStaticMeshFromFile(std::string_view FilePath,
		DObject* Outer, std::string_view ObjectName, const FStaticMeshImportSettings& Settings = {})
		-> std::expected<DStaticMesh*, std::string>;

}
