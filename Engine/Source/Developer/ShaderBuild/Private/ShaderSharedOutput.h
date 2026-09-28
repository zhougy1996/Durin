#pragma once
#include "ShaderBuildAPI.h"
#include "DerivedDataBuildFunction.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin::ShaderSharedOutput
{
	SHADERBUILD_API auto Make(const FShaderCompileOptions& Options, const FShaderCompilerOutput& Product,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<DerivedData::FBuildOutput, FShaderError>;
	SHADERBUILD_API auto Validate(const FShaderCompileOptions& Options, const DerivedData::FBuildOutput& Output,
		const std::function<bool()>& ShouldCancel = {}) -> FShaderOperationResult;
	SHADERBUILD_API auto ValidateWithReceipt(const FShaderCompileOptions& Options,
		const DerivedData::FBuildOutput& Output, const std::function<bool()>& ShouldCancel = {})
		-> std::expected<std::shared_ptr<const DerivedData::FBuildValidationReceipt>, FShaderError>;
	SHADERBUILD_API auto Assemble(const FShaderCompileOptions& Options, const DerivedData::FBuildOutput& Output,
		const std::function<bool()>& ShouldCancel = {},
		const DerivedData::FBuildValidationReceipt* Receipt = nullptr)
		-> std::expected<FShaderCompilerOutput, FShaderError>;
}
