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
		auto StartupModule() -> void override
		{
			FShaderPaths::InitDefaultMountPoints();
			InitShaderCompileService();
		}

		auto ShutdownModule() -> void override
		{
			ClearShaderMapResourceCache();
			ShutdownShaderCompileService();
		}
	};

	IMPLEMENT_MODULE(FRenderCoreModule, RenderCore);
}
