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
	// Tracks one window viewport and a resize request consumed on RHI access.
	struct FMonaViewportInfo
	{
		TRefCountPtr<FRHIViewport> ViewportRHI;
		bool bFullScreen;
		uint32 CurrentWidth = 0;
		uint32 CurrentHeight = 0;
		uint32 PendingResizeWidth = 0;
		uint32 PendingResizeHeight = 0;
		bool bResizeRequested = false;
	};


	// Owns and lazily resizes the RHI viewports backing Mona windows.
	class FMonaRHIRenderer : public FMonaRenderer
	{
	public:
		MONACORE_API ~FMonaRHIRenderer() override;

		MONACORE_API auto CreateViewport(const std::shared_ptr<MWindow>& Window) -> void override;
		MONACORE_API auto RequestResize(const std::shared_ptr<MWindow>& Window, uint32 Width, uint32 Height) -> void override;
		MONACORE_API auto RenderViewports() -> void override;
		MONACORE_API auto OnWindowDestroyed(const std::shared_ptr<MWindow>& Window) -> void override;

		MONACORE_API auto GetRHIViewport(const MWindow& Window) -> TRefCountPtr<FRHIViewport> override;

		// Window keys are non-owning; this renderer owns every mapped viewport record.
		std::unordered_map<const MWindow*, FMonaViewportInfo*> WindowToViewportInfoMap;
	};
}
