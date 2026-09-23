#pragma once

#include "Shader/IShaderBuildModule.h"

namespace Durin
{
	using FShaderCompileServiceStats = FShaderBuildStats;

	// Optional private test seam; configure only while the service is stopped.
	auto InitShaderCompileService(
		std::function<void(std::string_view)> BeforeGeneratedCompile = {}) -> void;
	auto ShutdownShaderCompileService() -> void;
	auto GetOrCompileShader(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput;
	auto GetOrCompileGeneratedShader(
		const FGeneratedShaderCompileRequest& Request)
		-> FShaderCompilerOutput;
	auto GetShaderCompilerEnvironmentIdentityFromService() -> std::string;
	auto BuildShaderSourceDependencyManifestFromService(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		std::vector<FShaderSourceDependencyFingerprint>& OutDependencies) -> FShaderOperationResult;
	auto BuildShaderSourceTreeFingerprintFromService(
		std::string_view VirtualShaderPath,
		const FShaderCompileOptions& Options,
		FShaderSourceDependencyFingerprint& OutFingerprint) -> FShaderOperationResult;
	auto GetShaderCompileServiceStats() -> FShaderCompileServiceStats;
} // namespace Durin
