#pragma once

#include "Materials/MaterialProgramCompiler.h"
#include "MaterialProgramValidation.h"

#include <unordered_map>

namespace Durin::Private
{
	// Independent bounded intermediate; never admits an oversized authored document.
	struct FMaterialExpandedProgram
	{
		uint32 SchemaVersion = CurrentMaterialProgramSchemaVersion;
		std::vector<FMaterialProgramNode> Nodes;
		FMaterialSurfaceOutputs Outputs;
		std::unordered_map<FGuid, FMaterialExpressionSource> Sources;
	};

	auto ExpandMaterialFunctionCalls(const FMaterialCompilerInput& Input,
		std::span<const FMaterialParameterDefinition> Definitions,
		FMaterialExpandedProgram& OutProgram) -> FMaterialProgramValidationResult;
}
