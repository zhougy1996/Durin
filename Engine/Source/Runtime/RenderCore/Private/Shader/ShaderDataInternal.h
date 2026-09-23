#pragma once

#include "Shader/IShaderBuildModule.h"

namespace Durin
{
	auto GetOrCompileShader(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options) -> FShaderCompilerOutput;
	auto GetOrCompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request) -> FShaderCompilerOutput;
	auto GetShaderCompilerEnvironmentIdentityFromModule() -> std::string;
	auto BuildShaderSourceDependencyManifestFromModule(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult;
	auto BuildShaderSourceTreeFingerprintFromModule(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult;
}
