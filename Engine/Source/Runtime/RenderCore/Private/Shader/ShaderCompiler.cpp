#include "Shader/ShaderCompiler.h"

#include "Shader/SlangShaderCompiler.h"

namespace Doge
{
	FShaderCompiler* GShaderCompiler = nullptr;

	void InitShaderCompiler()
	{
		GShaderCompiler = new FSlangShaderCompiler();
	}

	void DestroyShaderCompiler()
	{
		delete GShaderCompiler;
		GShaderCompiler = nullptr;
	}
}

