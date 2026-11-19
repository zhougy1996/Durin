#pragma once

#include "CoreMinimal.h"
#include "MaterialGraphOperations.h"

namespace Durin::MaterialEditor
{
	MATERIALEDITOR_API auto EnumerateCatalog()
		-> std::vector<FMaterialGraphCatalogEntry>;
	auto DefaultNodePresentation(const FMaterialGraphNodeDescriptor& Node, size_t Ordinal) -> FMaterialGraphNodePresentation;
	// Projects authored expressions, optionally restricting the returned node views.
	MATERIALEDITOR_API auto ProjectMaterialGraph(const DObject* Owner,
		std::span<const FMaterialGraphCatalogEntry> Catalog = EnumerateCatalog(),
		std::optional<std::span<const FGuid>> Nodes = {}) -> FMaterialGraphView;
	MATERIALEDITOR_API auto SearchCatalog(
		std::string_view Query,
		std::optional<EMaterialProgramValueType> SourceType = std::nullopt)
		-> std::vector<FMaterialGraphCatalogEntry>;
	MATERIALEDITOR_API auto SearchCatalog(
		std::span<const FMaterialGraphCatalogEntry> Catalog,
		std::string_view Query,
		std::optional<EMaterialProgramValueType> SourceType = std::nullopt)
		-> std::vector<FMaterialGraphCatalogEntry>;
	MATERIALEDITOR_API auto SearchCatalogIndices(
		std::span<const FMaterialGraphCatalogEntry> Catalog,
		std::string_view Query,
		std::optional<EMaterialProgramValueType> SourceType = std::nullopt)
		-> std::vector<size_t>;
}
