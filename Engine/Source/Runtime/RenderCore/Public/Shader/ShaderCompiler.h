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
		std::string ShaderName;
		std::vector<const char8*> EntryPoints;
		std::vector<EShaderFrequency> Frequencies;
		bool bForceRecompile = false;
	};

	struct FShaderCompilerOutput
	{
		bool bSucceeded = false;
		std::vector<FCompiledShader> CompiledShaders;
		std::string ErrorMessage;

		operator bool() const { return bSucceeded; }
	};

	struct FShaderCompilerSettings
	{
		bool bForceRecompile = false;
	};

	class FShaderCompiler
	{
	public:
		FShaderCompiler();
		virtual ~FShaderCompiler() = default;

		RENDERCORE_API virtual auto Compile(std::string_view ShaderSourceFilePath, const FShaderCompileOptions& Options) -> FShaderCompilerOutput = 0;

		DURIN_NONCOPYABLE(FShaderCompiler);

	protected:
		FShaderCompilerSettings Settings;
	};

	RENDERCORE_API extern FShaderCompiler* GShaderCompiler;
} // namespace Durin