#pragma once

#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialProgramTypes.h"

#include "MaterialGraphOperations.h"

namespace Durin::MaterialEditor
{
	inline auto ToGraphLink(const FMaterialExpressionInput& Input) -> FMaterialProgramLink
	{
		const FGuid Port = Input.GetOutputId();
		if (!Port.IsValid() && !std::in_range<uint8>(Input.OutputIndex)) return {};
		return {Input.GetExpressionId(), static_cast<uint8>(Port.IsValid() ? 0 : Input.OutputIndex), Port};
	}

	// Borrowed for a single expression read.
	struct FMaterialGraphPinSource
	{
		DMaterialExpression* Expression;
		const DMaterial* Material;
		const FMaterialGraphCatalogEntry* Shape;
	};

	using FMaterialGraphNodeVisitor = std::function<void(FMaterialGraphNodeView, const FMaterialGraphPinSource&)>;
	auto ReadGraphOutputs(const DMaterial& Material) -> FMaterialAttributeValues;
	auto ReadMaterialGraph(const DObject* Owner, std::span<const FMaterialGraphCatalogEntry> Catalog,
		std::optional<std::span<const FGuid>> Nodes, const FMaterialGraphNodeVisitor& Visit) -> FMaterialGraphView;
}
