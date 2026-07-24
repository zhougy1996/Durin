#pragma once

#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	// Defines the compiler backend contract used by the shader compile service.
	class FShaderCompiler
	{
	public:
		FShaderCompiler() = default;
		virtual ~FShaderCompiler() = default;

		virtual auto Compile(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput = 0;

		DURIN_NONCOPYABLE(FShaderCompiler);
	};
} // namespace Durin
