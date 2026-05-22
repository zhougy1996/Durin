#pragma once

#include "RenderCoreAPI.h"

#include "Shader/Shader.h"

namespace Durin
{
	struct FCompiledShader
	{
		EShaderFrequency Frequency;
		std::shared_ptr<FShaderCode> Code;
		FXxHash64 Hash;
	};

	struct FShaderCompileOptions
	{
		std::vector<const char8*> EntryPoints;
		std::vector<EShaderFrequency> Frequencies;
	};

	struct FShaderCompilerOutput
	{
		bool bSucceeded = false;
		std::vector<FCompiledShader> CompiledShaders;
		std::string ErrorMessage;

		operator bool() const { return bSucceeded; }
	};

	class FShaderCompiler
	{
	public:
		FShaderCompiler() = default;
		virtual ~FShaderCompiler() = default;

		RENDERCORE_API virtual auto Compile(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput = 0;

		DURIN_NONCOPYABLE(FShaderCompiler);
	};

	RENDERCORE_API extern FShaderCompiler* GShaderCompiler;
} // namespace Durin