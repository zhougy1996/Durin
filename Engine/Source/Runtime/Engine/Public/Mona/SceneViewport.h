#pragma once

#include "Client/Viewport.h"
#include "Rendering/RenderingCommon.h"

#include "MonaFwd.h"

namespace Durin
{
	class FViewportClient;

	class ENGINE_API FSceneViewport : public FViewport, public Mona::IMonaViewport
	{
	public:
		FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<Mona::MViewport>& InViewportWidget);

		auto UpdateRHIViewport() -> void override;

	private:
		std::weak_ptr<Mona::MViewport> ViewportWidget;
	};
}