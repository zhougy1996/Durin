#pragma once

#include "Client/Viewport.h"
#include "Rendering/RenderingCommon.h"

#include "MonaFwd.h"

namespace Doge
{
	class FViewportClient;

	class ENGINE_API FSceneViewport : public FViewport, public Mona::IMonaViewport
	{
	public:
		FSceneViewport(FViewportClient* InViewportClient, const TSharedPtr<Mona::MViewport>& InViewportWidget);

		auto UpdateRHIViewport() -> void override;

	private:
		TWeakPtr<Mona::MViewport> ViewportWidget;
	};
}