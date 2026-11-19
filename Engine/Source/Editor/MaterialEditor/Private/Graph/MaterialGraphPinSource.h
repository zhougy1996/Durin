#pragma once

#include "MaterialGraphOperations.h"

namespace Durin::MaterialEditor
{
	// Borrowed for a single expression read. The source type lookup is shared by the batch.
	struct FMaterialGraphPinSource
	{
		DMaterialExpression* Expression;
		const DMaterial* Material;
		const FMaterialGraphCatalogEntry* Shape;
		const std::function<EMaterialProgramValueType(const FMaterialProgramLink&)>& SourceType;
	};

	using FMaterialGraphNodeVisitor = std::function<void(FMaterialGraphNodeView, const FMaterialGraphPinSource&)>;
	auto ReadMaterialGraph(const DObject* Owner, std::span<const FMaterialGraphCatalogEntry> Catalog,
		std::optional<std::span<const FGuid>> Nodes, const FMaterialGraphNodeVisitor& Visit) -> FMaterialGraphView;
}
