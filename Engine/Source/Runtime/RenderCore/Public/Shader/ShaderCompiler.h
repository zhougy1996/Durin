#pragma once

namespace Doge
{
	class FShaderCompiler {
	public:
		FShaderCompiler() = default;
		virtual ~FShaderCompiler() = default;

		DOGE_NONCOPYABLE(FShaderCompiler);
	};

	RENDERCORE_API extern FShaderCompiler* GShaderCompiler;

	RENDERCORE_API extern void InitShaderCompiler();

	RENDERCORE_API extern void DestroyShaderCompiler();
}