#pragma once

#include "RenderCoreAPI.h"

namespace Durin
{
	using FShaderCode = std::vector<std::byte>;

	struct FShaderCompileOptions
	{
		std::vector<const char8*> EntryPoints;
	};

	struct FShaderCompilerOutput
	{
		bool bSucceeded = false;
		std::vector<FShaderCode> Codes;
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