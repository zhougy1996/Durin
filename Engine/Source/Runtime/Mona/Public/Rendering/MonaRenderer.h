#pragma once

#include "MonaAPI.h"
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

		MONA_API virtual auto CreateViewport(const std::shared_ptr<MWindow>& Window) -> void = 0;

		MONA_API virtual auto RequestResize(const std::shared_ptr<MWindow>& Window, uint32 Width, uint32 Height) -> void = 0;

		MONA_API virtual auto OnWindowDestroyed(const std::shared_ptr<MWindow>& Window) -> void = 0;

		// Consumes the latest resize request before returning the viewport used by
		// this draw. Callers must invoke this immediately before enqueueing work.
		MONA_API virtual auto PrepareViewportForDraw(const MWindow& Window) -> TRefCountPtr<FRHIViewport> = 0;
	};
}
