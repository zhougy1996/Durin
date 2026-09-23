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
		auto SupportsDynamicReloading() const -> bool override { return false; }
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
