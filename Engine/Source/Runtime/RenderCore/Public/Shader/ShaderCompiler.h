#pragma once

#include "RenderCoreAPI.h"

#include "Shader/Shader.h"

namespace Durin
{
	struct FShaderMacroDefinition
	{
		std::string Name;
		std::string Value;
		// When false, the macro behaves like a presence define and is materialized as Name=1 for Slang.
		bool bHasExplicitValue = false;
	};

	struct FShaderCompileOptions
	{
		// Stable cache identity resolved by the caller. Leave empty to disable disk-backed shader cache reads and writes.
		std::string VirtualShaderPath;
		std::vector<const char8*> EntryPoints;
		std::vector<EShaderFrequency> Frequencies;
		std::vector<FShaderMacroDefinition> Macros;
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
