#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
	auto MakeMaterialShaderCompileOptions(FShaderTargetIdentity Target, const FMaterialStaticProperties& Properties) -> FShaderCompileOptions;

	struct FMaterialTranslationInput
	{
		FSharedByteBuffer Bytes;
		FXxHash128 Identity;
	};

	// Captured together, then shared as const by submission and compilation.
	struct FMaterialPreparedProgram
	{
		MIR::FNormalizationResult Normalized;
		FMaterialStaticProperties StaticProperties;
		FMaterialCompilerEnvironment Environment;
		uint64 NormalizationMicroseconds = 0;
	};

	struct FMaterialTranslationSource
	{
		MIR::FSource Source;
		uint32 Ordinal = 0;
	};

	// Detached CPU product. Shader binaries and publication success exist only
	// in FMaterialCompilerResult, constructed by final assembly.
	struct FMaterialTranslatedProgram
	{
		bool bSucceeded = false;
		FMaterialProgramIdentity Identity;
		std::string CompilerIdentity;
		std::string Target;
		uint32 PassContractVersion = CurrentMaterialPassContractVersion;
		EMaterialQualityLevel Quality = EMaterialQualityLevel::High;
		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
		std::vector<FMaterialCompilerEnvironment::FStaticBoolValue> StaticBools;
		MIR::FModule IR;
		std::vector<FMaterialCompilerParameterDeclaration> ActiveParameters;
		std::vector<FMaterialParameterCollectionLayout> ActiveCollections;
		FMaterialRenderLayout Layout;
		FMaterialProgramRequirements Requirements;
		std::string GeneratedSource;
		std::vector<FMaterialCompilerDependency> Dependencies;
		FMaterialCompileTimings Timings;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		FMaterialStaticProperties StaticProperties;
		FMaterialCompilerResourceLimits ResourceLimits;
		std::vector<FMaterialTranslationSource> Sources;

		explicit operator bool() const { return bSucceeded; }
	};

	ENGINE_API auto TranslatePreparedMaterialProgram(const FMaterialPreparedProgram& Input)
		-> FMaterialTranslatedProgram;
	auto BuildMaterialProgramRequirements(const MIR::FModule& IR) -> FMaterialProgramRequirements;
	ENGINE_API auto MakeMaterialShaderCompileRequest(const FMaterialTranslatedProgram& Translation,
		bool bForceRecompile) -> FGeneratedShaderCompileRequest;
	ENGINE_API auto AssembleMaterialProgram(FMaterialTranslatedProgram Translation,
		FShaderBuildResult Output) -> FMaterialCompilerResult;

	auto PrepareMaterialProgram(const MIR::FCompilerInput& Input) -> FMaterialPreparedProgram;
	auto ValidateMaterialNormalizationEnvironment(const MIR::FCompilerInput& Input) -> MIR::FNormalizationResult;
}
