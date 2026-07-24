#pragma once

#include "MonaCoreAPI.h"
#include "RHIResources.h"

namespace Durin
{
	class MWindow;
}

namespace Durin::Mona
{
	// Defines the renderer services required by Mona application windows.
	class FMonaRenderer
	{
	public:
		virtual ~FMonaRenderer() = default;

		MONACORE_API virtual auto CreateViewport(const std::shared_ptr<MWindow>& Window) -> void = 0;

		MONACORE_API virtual auto RequestResize(const std::shared_ptr<MWindow>& Window, uint32 Width, uint32 Height) -> void = 0;

		MONACORE_API virtual auto RenderViewports() -> void = 0;

		MONACORE_API virtual auto OnWindowDestroyed(const std::shared_ptr<MWindow>& Window) -> void = 0;

		MONACORE_API virtual auto GetRHIViewport(const MWindow& Window) -> TRefCountPtr<FRHIViewport> = 0;
	};
}
