#pragma once
#include "CoreMinimal.h"
#include "Materials/MaterialProgramCompiler.h"
namespace Durin::MaterialBuildPrivate
{
	auto FindProgram(FMaterialProgramIdentity Identity) -> std::shared_ptr<const FMaterialCompilerResult>;
	auto BindProgramCollections(std::shared_ptr<const FMaterialCompilerResult> Program,
		std::span<const FMaterialParameterCollectionLayout> Collections, const FMaterialCompilerEnvironment* Environment = nullptr) -> std::shared_ptr<const FMaterialCompilerResult>;
	auto ConfigureProgram(std::shared_ptr<const FMaterialCompilerResult> Program, const MIR::FCompilerInput& Input) -> std::shared_ptr<const FMaterialCompilerResult>;
	auto RetainProgram(std::shared_ptr<const FMaterialCompilerResult> Program, bool Replace = false) -> std::shared_ptr<const FMaterialCompilerResult>;
	ENGINE_API auto ClearPrograms() -> void;
	auto ProgramCacheStats() -> std::pair<uint32, uint64>;
	auto EstimateProgramBytes(const FMaterialCompilerResult& Result) -> uint64;
}
