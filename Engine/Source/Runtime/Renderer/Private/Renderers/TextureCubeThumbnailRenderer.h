#pragma once

#include "RHIResources.h"

#include <memory>

namespace Durin
{
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	class FSkyBoxRenderer;
	class FTextureCubePreviewSceneProxy;
	struct FSceneView;

	// Owns TextureCube thumbnail resources and the preview-proxy draw path.
	class FTextureCubeThumbnailRenderer final
	{
	public:
		explicit FTextureCubeThumbnailRenderer(
			FRendererResourceCoordinator& InCoordinator);
		~FTextureCubeThumbnailRenderer();

		FTextureCubeThumbnailRenderer(
			const FTextureCubeThumbnailRenderer&) = delete;
		auto operator=(const FTextureCubeThumbnailRenderer&)
			-> FTextureCubeThumbnailRenderer& = delete;

		auto EnsureResources_RenderThread() -> bool;
		auto DrawProxy_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FTextureCubePreviewSceneProxy& Proxy,
			FSkyBoxRenderer& SkyBoxRenderer) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
