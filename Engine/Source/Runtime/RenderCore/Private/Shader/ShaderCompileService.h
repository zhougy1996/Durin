#pragma once

#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	// Reports compile-service cache hits, misses, and actual compiler invocations.
	struct FShaderCompileServiceStats
	{
		uint64 DependencyResolutions = 0;
		uint64 ManifestHits = 0;
		uint64 MemoryHits = 0;
		uint64 DiskHits = 0;
		uint64 Compilations = 0;
		uint64 ContentReads = 0;
		uint64 OutputEntries = 0;
	};

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
