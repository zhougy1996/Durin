#pragma once

#include "Client/Viewport.h"
#include "Rendering/RenderingCommon.h"

#include "MonaFwd.h"

namespace Durin
{
	class FViewportClient;
	class IScene;

	class FSceneViewport : public FViewport, public Mona::IMonaViewport
	{
	public:
		ENGINE_API FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<MWindow>& InWindow);

		ENGINE_API FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<MViewport>& InViewportWidget, IScene* InRenderScene = nullptr);

		ENGINE_API ~FSceneViewport() override;

		ENGINE_API auto UpdateRHIViewport() -> void override;

		ENGINE_API auto GetRenderMode() const -> Mona::EMonaViewportRenderMode override;

		ENGINE_API auto IsWindowBacked() const -> bool override;

		ENGINE_API auto GetDesiredSize() const -> FVector2f override;

		ENGINE_API auto GetRenderTargetRHI() const -> const FTextureRHIRef&;

		ENGINE_API auto GetDisplayTexture() const -> const FTextureRHIRef& override;

		// Auxiliary editor viewports may render an isolated scene instead of leaking preview primitives into the level.
		ENGINE_API auto GetRenderScene() const -> IScene* { return RenderScene; }

	private:
		ENGINE_API auto UnregisterDisplayTexture() -> void;

		Mona::EMonaViewportRenderMode RenderMode = Mona::EMonaViewportRenderMode::Window;

		std::weak_ptr<MWindow> Window;

		std::weak_ptr<MViewport> ViewportWidget;

		FTextureRHIRef RenderTargetRHI;

		IScene* RenderScene = nullptr;
	};
}
