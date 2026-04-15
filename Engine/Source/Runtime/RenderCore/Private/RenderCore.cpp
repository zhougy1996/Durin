#include "RenderCore.h"

#include "Misc/Paths.h"
#include "Shader/SlangShaderCompiler.h"
#include "Misc/StringConvert.h"

namespace Doge
{
	auto InitShaderCompiler() -> void
	{
		GShaderCompiler = new FSlangShaderCompiler();
	}

	auto ShutdownShaderCompiler() -> void
	{
		delete GShaderCompiler;
		GShaderCompiler = nullptr;
	}

	class FRenderCoreModule : public FDefaultModuleImpl
	{
	public:
		auto StartupModule() -> void override
		{
			InitShaderCompiler();
		}

		auto ShutdownModule() -> void override
		{
			ShutdownShaderCompiler();
		}
	};

	IMPLEMENT_MODULE(FRenderCoreModule, RenderCore);
}