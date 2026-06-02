#include "RenderCore.h"

#include "Shader/ShaderPaths.h"

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
			ShutdownShaderCompileService();
		}
	};

	IMPLEMENT_MODULE(FRenderCoreModule, RenderCore);
}
