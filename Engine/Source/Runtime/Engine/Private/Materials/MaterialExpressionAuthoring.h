#pragma once

#include "Materials/MaterialExpressions.h"

namespace Durin::Private
{
	// Temporary native candidate construction for callers awaiting Stage 3 migration.
	// Never called by an archive or an asset upgrade path.
	auto ConstructFunctionExpressions(DObject* Owner, const FMaterialFunctionGraph& Candidate,
		FMaterialExpressionCollection& OutCollection) -> bool;
	auto ConstructMaterialExpressions(DObject* Owner, const FMaterialProgram& Program,
		std::span<const FMaterialFunctionCall> Calls, FMaterialExpressionCollection& OutCollection) -> bool;
	auto ConstructMaterialOutputs(const FMaterialSurfaceOutputs& Source) -> FMaterialExpressionSurfaceOutputs;
	auto ProjectMaterialOutputs(const FMaterialExpressionSurfaceOutputs& Source) -> FMaterialSurfaceOutputs;
}
