#pragma once

#include "Shader/ShaderCompilerCore.h"
#include "ShaderBuildAPI.h"

namespace Durin
{
	// Converts one reflected Slang SPIR-V stage to MSL with an explicit native slot map.
	SHADERBUILD_API auto TranslateMetalShader(FCompiledShader& Shader) -> FShaderOperationResult;
}
