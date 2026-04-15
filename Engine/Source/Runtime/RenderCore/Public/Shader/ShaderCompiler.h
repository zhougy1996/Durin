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
}