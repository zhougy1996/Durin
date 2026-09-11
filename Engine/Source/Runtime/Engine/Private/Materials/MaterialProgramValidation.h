#pragma once

#include "Materials/MaterialProgramTypes.h"
#include <functional>

namespace Durin::Private
{
	auto ValidateMaterialSurfacePayload(const FMaterialProgramNode& Node,
		const std::function<std::optional<EMaterialProgramValueType>(const FMaterialProgramLink&)>& ResolveType)
		-> FMaterialProgramValidationResult;
	auto ResolveMaterialBuiltinOutput(const FMaterialProgramNode& Node, const FMaterialProgramLink& Link)
		-> std::optional<EMaterialProgramValueType>;
	struct FMaterialProgramGraphView
	{
		uint32 SchemaVersion;
		std::span<const FMaterialProgramNode> Nodes;
		const FMaterialSurfaceOutputs& Outputs;
	};

	auto ValidateMaterialProgramGraph(FMaterialProgramGraphView Program,
		std::span<const FMaterialParameterDefinition> Definitions,
		std::span<const FMaterialFunctionCallSnapshot> Calls, bool bExpanded)
		-> FMaterialProgramValidationResult;

	// Shared built-in shape/payload validation; function ports supply resolved types.
	auto ValidateMaterialBuiltinNode(const FMaterialProgramNode& Node,
		std::span<const EMaterialProgramValueType> InputTypes)
		-> FMaterialProgramValidationResult;
}
