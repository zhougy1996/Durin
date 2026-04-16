#pragma once

namespace Doge
{
	class FShaderCompiler
	{
	public:
		FShaderCompiler() = default;
		virtual ~FShaderCompiler() = default;

		RENDERCORE_API virtual auto CompileShader(const char8* InShaderFilename, const char8* InEntryPoint) -> bool = 0;

		DOGE_NONCOPYABLE(FShaderCompiler);
	};

	RENDERCORE_API extern FShaderCompiler* GShaderCompiler;
} // namespace Doge