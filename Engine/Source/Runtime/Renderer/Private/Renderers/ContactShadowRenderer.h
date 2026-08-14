#pragma once

#include "Math/MathFwd.h"
#include "RHIResources.h"

#include <memory>

namespace Durin
{
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	class FFullscreenGeometryResources;
	struct FSceneView;

	// Owns the screen-space contact-shadow shader, pipeline, and full-screen
	// pass that removes occluded directional direct light near contact,
	// supplementing the directional shadow map. Render-thread only.
	class FScreenSpaceContactShadowRenderer final
	{
	public:
		FScreenSpaceContactShadowRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FScreenSpaceContactShadowRenderer();

		FScreenSpaceContactShadowRenderer(
			const FScreenSpaceContactShadowRenderer&) = delete;
		auto operator=(const FScreenSpaceContactShadowRenderer&)
			-> FScreenSpaceContactShadowRenderer& = delete;

		// Runs the contact-shadow full-screen pass into ContactColor. The
		// DirectionalDirect input contains only the selected directional light's
		// post-shadow contribution. Returns false when resources or inputs are
		// unavailable so the caller can keep SceneColor unchanged.
		// LightDirection is the light travel direction.
		auto Render_RenderThread(
			FRHICommandListImmediate& CommandList,
			FRHITexture* SceneColor,
			FRHITexture* DirectionalDirect,
			FRHITexture* SceneDepth,
			FRHITexture* ContactColor,
			const FSceneView& View,
			const FVector3& LightDirection,
			bool bShowDebug,
			uint32 Width,
			uint32 Height) -> bool;

		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
