#include "RenderCore.h"

#include "Shader/ShaderPaths.h"
#include "Shader/SlangShaderCompiler.h"

namespace Durin
{
	auto InitShaderCompiler() -> void
	{
		GShaderCompiler = new FSlangShaderCompiler();
	}

	auto ShutdownShaderCompiler() -> void
	{
		if (!GShaderCompiler)
		{
			return;
		}

		FShaderCompiler* ShaderCompiler = GShaderCompiler;
		GShaderCompiler = nullptr;
		delete ShaderCompiler;
	}

	class FRenderCoreModule : public FDefaultModuleImpl
	{
	public:
		auto StartupModule() -> void override
		{
			FShaderPaths::InitDefaultMountPoints();
			InitShaderCompiler();
		}

		auto ShutdownModule() -> void override
		{
			ShutdownShaderCompiler();
		}
	};

	IMPLEMENT_MODULE(FRenderCoreModule, RenderCore);
}
