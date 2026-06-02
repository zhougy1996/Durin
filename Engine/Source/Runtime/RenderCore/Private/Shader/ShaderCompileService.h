#pragma once

#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	auto InitShaderCompileService() -> void;
	auto ShutdownShaderCompileService() -> void;
	auto GetOrCompileShader(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput;
} // namespace Durin
