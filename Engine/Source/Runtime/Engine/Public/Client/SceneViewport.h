#pragma once

#include "Client/Viewport.h"
#include "Rendering/ViewportDisplaySource.h"

#include "MonaCoreFwd.h"

namespace Durin
{
	class FViewportClient;
	class IScene;

	// Owns Engine window or offscreen output while exposing only its offscreen texture through MonaCore.
	class FSceneViewport final : public FViewport, public IViewportDisplaySource
	{
	public:
		ENGINE_API static auto CreateWindowBacked(
			FViewportClient* InViewportClient,
			const std::shared_ptr<MWindow>& InWindow) -> std::shared_ptr<FSceneViewport>;

		ENGINE_API static auto CreateOffscreen(
			FViewportClient* InViewportClient,
			IScene* InRenderScene = nullptr) -> std::shared_ptr<FSceneViewport>;

		~FSceneViewport() override = default;

		ENGINE_API auto PrepareDisplay(const FVector2f& DesiredSize) -> void override;
		ENGINE_API auto GetDisplayTexture() const -> const FTextureRHIRef& override;

		ENGINE_API auto UpdateRHIViewport() -> void;
		ENGINE_API auto IsWindowBacked() const -> bool;
		ENGINE_API auto GetDesiredSize() const -> FVector2f;
		ENGINE_API auto GetRenderTargetRHI() const -> const FTextureRHIRef&;

		// Auxiliary editor viewports may render an isolated scene instead of leaking preview primitives into the level.
		ENGINE_API auto GetRenderScene() const -> IScene* { return RenderScene; }

	private:
		enum class EOutputPolicy
		{
			Window,
			Offscreen
		};

		FSceneViewport(FViewportClient* InViewportClient, const std::shared_ptr<MWindow>& InWindow);
		FSceneViewport(FViewportClient* InViewportClient, IScene* InRenderScene);

		static auto SanitizeDisplayExtent(const FVector2f& DesiredSize) -> FVector2f;

		EOutputPolicy OutputPolicy = EOutputPolicy::Window;
		std::weak_ptr<MWindow> Window;
		FVector2f OffscreenExtent = {640.0f, 360.0f};
		FTextureRHIRef RenderTargetRHI;
		IScene* RenderScene = nullptr;
	};
}
