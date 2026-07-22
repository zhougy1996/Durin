#pragma once

#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	struct FShaderCompileServiceStats
	{
		uint64 DependencyResolutions = 0;
		uint64 ManifestHits = 0;
		uint64 MemoryHits = 0;
		uint64 DiskHits = 0;
		uint64 Compilations = 0;
		uint64 ContentReads = 0;
	};

	auto InitShaderCompileService() -> void;
	auto ShutdownShaderCompileService() -> void;
	auto GetOrCompileShader(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput;
	auto GetShaderCompileServiceStats() -> FShaderCompileServiceStats;
} // namespace Durin
