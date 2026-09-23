#pragma once

#include "Renderers/SceneFrameContext.h"

namespace Durin
{
	class FSceneRenderingService;
	class FSceneRenderPipeline;
	// One stable submission; retain through graph execution and finalization.
	class FSceneRenderer final
	{
	public:
		explicit FSceneRenderer(FSceneRenderingService& Service) : Service(Service) {}
		FSceneRenderer(const FSceneRenderer&) = delete;
		auto operator=(const FSceneRenderer&) -> FSceneRenderer& = delete;
		FSceneRenderer(FSceneRenderer&&) = delete;
		auto operator=(FSceneRenderer&&) -> FSceneRenderer& = delete;
		// Authors once after preparation. Caller executes and finalizes the graph.
		RENDERER_API auto Render(FRDGBuilder& GraphBuilder) -> void;

		// Same-builder handle for appended consumers; honor the imported final access.
		auto GetOutputTexture() const -> FRDGTextureHandle
		{
			check(OutputTexture.has_value());
			return *OutputTexture;
		}

	private:
		friend class FSceneRenderPipeline;
		friend struct FSceneRendererTestAccess;
		struct FGraphResources;
		auto PrepareGraphResources(FRDGBuilder& Graph) -> FGraphResources;
		FSceneRenderingService& Service;
		FSceneFrameContext Context;
		bool bAuthored = false;
		std::optional<FRDGTextureHandle> OutputTexture;
	};
} // namespace Durin
