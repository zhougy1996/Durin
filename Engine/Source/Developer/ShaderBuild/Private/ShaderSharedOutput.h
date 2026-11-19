#pragma once

#include "CoreMinimal.h"
#include "ShaderBuildAPI.h"
#include "DerivedDataBuildFunction.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin::ShaderSharedOutput
{
	SHADERBUILD_API auto SaveErrors(DerivedData::FBuildContext& Context, std::span<const FShaderCompilerError> Errors) -> void;
	SHADERBUILD_API auto ReadErrors(const DerivedData::FBuildOutput& Output) -> std::vector<FShaderCompilerError>;
	SHADERBUILD_API auto Write(DerivedData::FBuildContext& Context, const FShaderCompileOptions& Options,
		const FShaderCompilerOutput& Product) -> void;
	SHADERBUILD_API auto Validate(const FShaderCompileOptions& Options, const DerivedData::FBuildOutput& Output,
		const std::function<bool()>& ShouldCancel = {}) -> FShaderCompletion;
	SHADERBUILD_API auto Assemble(const FShaderCompileOptions& Options, const DerivedData::FBuildOutput& Output,
		const std::function<bool()>& ShouldCancel = {})
		-> FShaderCompilerOutput;
}
