#pragma once

#include "Shader/ShaderBuildProvider.h"

namespace Durin
{
	auto GetOrCompileShader(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options) -> FShaderCompilerOutput;
	auto GetOrCompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request) -> FShaderCompilerOutput;
	auto GetShaderCompilerEnvironmentIdentityFromProvider() -> std::string;
	auto BuildShaderSourceDependencyManifestFromProvider(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies,
		std::string& OutError) -> bool;
	auto BuildShaderSourceTreeFingerprintFromProvider(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint,
		std::string& OutError) -> bool;
}
