#include "RenderCore.h"

#include "Shader/ShaderPaths.h"
#include "Shader/Shader.h"

namespace Durin
{
	auto InitShaderCompileService() -> void;
	auto ShutdownShaderCompileService() -> void;

	class FRenderCoreModule : public FDefaultModuleImpl
	{
	public:
		auto StartupModule(FModuleContext&) -> void override
		{
			FShaderPaths::InitDefaultMountPoints();
			InitShaderCompileService();
		}

		auto ShutdownModule(FModuleShutdownContext&) -> void override
		{
			ClearShaderMapResourceCache();
			ShutdownShaderCompileService();
		}
	};

	IMPLEMENT_MODULE(FRenderCoreModule, RenderCore);
}
