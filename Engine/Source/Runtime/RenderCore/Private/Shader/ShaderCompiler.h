#pragma once

#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	class FShaderCompiler
	{
	public:
		FShaderCompiler() = default;
		virtual ~FShaderCompiler() = default;

		virtual auto Compile(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput = 0;

		DURIN_NONCOPYABLE(FShaderCompiler);
	};
} // namespace Durin
