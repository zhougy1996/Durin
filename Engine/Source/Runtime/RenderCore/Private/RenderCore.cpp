#include "RenderCore.h"

#include "Shader/Shader.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderData.h"
#include "RHIGlobals.h"

namespace Durin
{
	class FRenderCoreModule : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			RHIReleaseResourcesHandle = GetRHIReleaseResourcesDelegate().AddStatic(
				&FRenderCoreModule::ReleaseRHIResources);
		}

			auto ShutdownModule() -> void override
		{
			GetRHIReleaseResourcesDelegate().Remove(RHIReleaseResourcesHandle);
			RHIReleaseResourcesHandle = {};
			GetGlobalShaderMap().Shutdown_RenderThread();
			ClearShaderMapResourceCache();
			ShutdownShaderData();
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
