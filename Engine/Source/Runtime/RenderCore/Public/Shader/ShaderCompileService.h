#pragma once

#include "RenderCoreAPI.h"
#include "Shader/ShaderCompiler.h"

namespace Durin
{
	// Public shader entry point for runtime modules. Resolves virtual paths, handles cache, and invokes the active backend compiler on misses.
	RENDERCORE_API auto GetOrCompileShader(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput;
}
