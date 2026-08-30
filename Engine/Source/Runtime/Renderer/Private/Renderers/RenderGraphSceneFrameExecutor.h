#pragma once

#include "Renderers/SceneFrameExecutionPipeline.h"

namespace Durin
{
	// Owns the public frame-execution boundary. Scene graph authoring is delegated
	// to the renderer-private composer.
	class FRenderGraphSceneFrameExecutor final
	{
	public:
		explicit FRenderGraphSceneFrameExecutor(FSceneRenderer& Renderer);

		FRenderGraphSceneFrameExecutor(const FRenderGraphSceneFrameExecutor&) = delete;
		auto operator=(const FRenderGraphSceneFrameExecutor&)
			-> FRenderGraphSceneFrameExecutor& = delete;
		FRenderGraphSceneFrameExecutor(FRenderGraphSceneFrameExecutor&&) = delete;
		auto operator=(FRenderGraphSceneFrameExecutor&&)
			-> FRenderGraphSceneFrameExecutor& = delete;

		auto Execute_RenderThread(
			FRHICommandListImmediate& CommandList,
			FScene* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FSceneViewStatistics* OutStatistics,
			FRenderGraphCapture* OutRenderGraphCapture
		) -> ERenderViewResult;

	private:
		auto CompileAndExecuteGraph_RenderThread(
			FRenderGraphBuilder& Graph,
			FRHICommandListImmediate& CommandList,
			FRenderGraphCapture* OutRenderGraphCapture
		) -> ESceneFrameGraphExecutionStatus;

		FSceneFrameExecutionPipeline Pipeline;
		FRDGAllocator& Allocator;
		bool bReportedRegressionOverage = false;
		bool bReportedExecutionFailure = false;
	};
} // namespace Durin
