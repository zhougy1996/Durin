#pragma once

#include "Client/Viewport.h"
#include "Rendering/RenderingCommon.h"

#include "MonaFwd.h"

namespace Durin
{
	class FViewportClient;

	class FSceneViewport : public FViewport, public Mona::IMonaViewport
	{
	public:
		ENGINE_API FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<Mona::MWindow>& InWindow);

		ENGINE_API FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<Mona::MViewport>& InViewportWidget);

		ENGINE_API auto UpdateRHIViewport() -> void override;

		ENGINE_API auto GetRenderMode() const -> Mona::EMonaViewportRenderMode override;

		ENGINE_API auto IsWindowBacked() const -> bool override;

		ENGINE_API auto GetDesiredSize() const -> FVector2f override;

		ENGINE_API auto GetRenderTargetRHI() const -> const FTextureRHIRef&;

		ENGINE_API auto IsRenderTargetReady() const -> bool;

		ENGINE_API auto MarkRenderTargetReady() -> void;

	private:
		Mona::EMonaViewportRenderMode RenderMode = Mona::EMonaViewportRenderMode::Window;

		std::weak_ptr<Mona::MWindow> Window;

		std::weak_ptr<Mona::MViewport> ViewportWidget;

		FTextureRHIRef RenderTargetRHI;

		bool bRenderTargetReady = false;
	};
}
