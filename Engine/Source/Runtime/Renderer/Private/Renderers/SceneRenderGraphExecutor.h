#pragma once

#include "Renderers/SceneRenderPipeline.h"

namespace Durin
{
	// Owns the public frame-execution boundary. Scene graph authoring is delegated
	// to the renderer-private composer.
	class FSceneRenderGraphExecutor final
	{
	public:
		explicit FSceneRenderGraphExecutor(FSceneRenderer& Renderer);

		FSceneRenderGraphExecutor(const FSceneRenderGraphExecutor&) = delete;
		auto operator=(const FSceneRenderGraphExecutor&)
			-> FSceneRenderGraphExecutor& = delete;
		FSceneRenderGraphExecutor(FSceneRenderGraphExecutor&&) = delete;
		auto operator=(FSceneRenderGraphExecutor&&)
			-> FSceneRenderGraphExecutor& = delete;

		auto Execute_RenderThread(
			FRHICommandListImmediate& CommandList,
			FScene* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FSceneViewStatistics* OutStatistics,
			FRDGCapture* OutRenderGraphCapture
		) -> ERenderViewResult;

	private:
		auto CompileAndExecuteGraph_RenderThread(
			FRDGBuilder& Graph,
			FRHICommandListImmediate& CommandList,
			FRDGCapture* OutRenderGraphCapture
		) -> ESceneRenderGraphExecutionStatus;

		FSceneRenderPipeline Pipeline;
		FRDGAllocator& Allocator;
		bool bReportedRegressionOverage = false;
		bool bReportedExecutionFailure = false;
	};
} // namespace Durin
