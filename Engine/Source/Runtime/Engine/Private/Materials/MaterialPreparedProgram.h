#pragma once

#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
	// Captured together, then shared as const by submission and compilation.
	struct FMaterialPreparedProgram
	{
		FMaterialNormalizationResult Normalized;
		FMaterialStaticProperties StaticProperties;
		FMaterialCompilerEnvironment Environment;
		uint64 NormalizationMicroseconds = 0;
	};

	auto PrepareMaterialProgram(const FMaterialIRCompilerInput& Input) -> FMaterialPreparedProgram;
	auto CompilePreparedMaterialProgram(const FMaterialPreparedProgram& Input,
		bool bForceRecompile) -> FMaterialCompilerResult;
}
