#pragma once

#include "Shader/ShaderBuildProvider.h"

namespace Durin
{
	using FShaderCompileServiceStats = FShaderBuildStats;

	auto InitShaderCompileService() -> void;
	auto ShutdownShaderCompileService() -> void;
	auto GetOrCompileShader(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput;
	auto GetOrCompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request)
		-> FShaderCompilerOutput;
	auto GetShaderCompilerEnvironmentIdentityFromService() -> std::string;
	auto BuildShaderSourceDependencyManifestFromService(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies,
		std::string& OutError) -> bool;
	auto BuildShaderSourceTreeFingerprintFromService(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint,
		std::string& OutError) -> bool;
	auto GetShaderCompileServiceStats() -> FShaderCompileServiceStats;
} // namespace Durin
