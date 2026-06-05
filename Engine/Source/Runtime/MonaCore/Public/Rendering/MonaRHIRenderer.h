#pragma once

#include "MonaCoreAPI.h"
#include "RHIResources.h"
#include "Rendering/MonaRenderer.h"

namespace Durin
{
	class MWindow;
}

namespace Durin::Mona
{
	struct FMonaViewportInfo
	{
		TRefCountPtr<FRHIViewport> ViewportRHI;
		bool bFullScreen;
	};


	class FMonaRHIRenderer : public FMonaRenderer
	{
	public:
		MONACORE_API ~FMonaRHIRenderer() override;

		MONACORE_API auto CreateViewport(const std::shared_ptr<MWindow>& Window) -> void override;
		MONACORE_API auto RequestResize(const std::shared_ptr<MWindow>& Window, uint32 Width, uint32 Height) -> void override;
		MONACORE_API auto RenderViewports() -> void override;
		MONACORE_API auto OnWindowDestroyed(const std::shared_ptr<MWindow>& Window) -> void override;

		MONACORE_API auto GetRHIViewport(const MWindow& Window) -> TRefCountPtr<FRHIViewport> override;

		std::unordered_map<const MWindow*, FMonaViewportInfo*> WindowToViewportInfoMap;
	};
}
