#pragma once

#include "CoreMinimal.h"

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

	// Detached translation can succeed without producing any compiled Shader.
	// Candidate remains unpublished until typed final assembly succeeds.
	struct FMaterialTranslatedProgram
	{
		bool bSucceeded = false;
		FMaterialCompilerResult Candidate;
		FMaterialStaticProperties StaticProperties;
		FMaterialCompilerResourceLimits ResourceLimits;

		explicit operator bool() const { return bSucceeded; }
	};

	ENGINE_API auto TranslatePreparedMaterialProgram(const FMaterialPreparedProgram& Input)
		-> FMaterialTranslatedProgram;
	ENGINE_API auto MakeMaterialShaderCompileRequest(const FMaterialTranslatedProgram& Translation,
		bool bForceRecompile) -> FGeneratedShaderCompileRequest;
	ENGINE_API auto AssembleMaterialProgram(FMaterialTranslatedProgram Translation,
		FShaderCompilerOutput Output) -> FMaterialCompilerResult;

	auto PrepareMaterialProgram(const MIR::FCompilerInput& Input) -> FMaterialPreparedProgram;
	auto CompilePreparedMaterialProgram(const FMaterialPreparedProgram& Input,
		bool bForceRecompile) -> FMaterialCompilerResult;
}
