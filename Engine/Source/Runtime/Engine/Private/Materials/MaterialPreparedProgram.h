#pragma once

#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
	// Captured together, then shared as const by submission and compilation.
	struct FMaterialPreparedProgram
	{
		MIR::FNormalizationResult Normalized;
		FMaterialStaticProperties StaticProperties;
		FMaterialCompilerEnvironment Environment;
		uint64 NormalizationMicroseconds = 0;
	};

	auto PrepareMaterialProgram(const MIR::FCompilerInput& Input) -> FMaterialPreparedProgram;
	auto CompilePreparedMaterialProgram(const FMaterialPreparedProgram& Input,
		bool bForceRecompile) -> FMaterialCompilerResult;
}
