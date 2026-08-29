#include "RenderCore.h"

#include "Shader/ShaderPaths.h"
#include "Shader/Shader.h"
#include "Shader/GlobalShader.h"
#include "RHIGlobals.h"

namespace Durin
{
	auto InitShaderCompileService() -> void;
	auto ShutdownShaderCompileService() -> void;

	class FRenderCoreModule : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			FShaderPaths::InitDefaultMountPoints();
			InitShaderCompileService();
			RHIReleaseResourcesHandle = GetRHIReleaseResourcesDelegate().AddStatic(
				&FRenderCoreModule::ReleaseRHIResources);
		}

		auto ShutdownModule() -> void override
		{
			GetRHIReleaseResourcesDelegate().Remove(RHIReleaseResourcesHandle);
			RHIReleaseResourcesHandle = {};
			GetGlobalShaderMap().Shutdown_RenderThread();
			ClearShaderMapResourceCache();
			ShutdownShaderCompileService();
		}

	private:
		static auto ReleaseRHIResources() -> void
		{
			GetGlobalShaderMap().ReleaseDeviceResources_RenderThread();
		}

		FDelegateHandle RHIReleaseResourcesHandle;
	};

	IMPLEMENT_MODULE(FRenderCoreModule, RenderCore);
}
