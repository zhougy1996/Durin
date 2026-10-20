#include "RenderCore.h"

#include "Shader/Shader.h"
#include "Shader/ShaderData.h"

namespace Durin
{
	class FRenderCoreModule : public IModuleInterface
	{
	public:
		auto SupportsDynamicReloading() const -> bool override { return false; }
		auto ShutdownModule() -> void override
		{
			ClearShaderMapResourceCache();
			ShutdownShaderData();
		}
	};

	IMPLEMENT_MODULE(FRenderCoreModule, RenderCore);
}
