#pragma once
#include "ShaderBuildAPI.h"
#include "DerivedDataBuildOutput.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin::ShaderSharedOutput
{
	SHADERBUILD_API auto Make(const FShaderCompileOptions& Options, const FShaderCompilerOutput& Product,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<DerivedData::FBuildOutput, FShaderError>;
	SHADERBUILD_API auto Validate(const FShaderCompileOptions& Options, const DerivedData::FBuildOutput& Output,
		const std::function<bool()>& ShouldCancel = {}) -> FShaderOperationResult;
	SHADERBUILD_API auto Assemble(const FShaderCompileOptions& Options, const DerivedData::FBuildOutput& Output,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<FShaderCompilerOutput, FShaderError>;
}
