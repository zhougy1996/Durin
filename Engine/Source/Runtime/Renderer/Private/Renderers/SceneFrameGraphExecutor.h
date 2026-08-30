#pragma once

#include "Renderers/SceneFrameExecutionPipeline.h"

namespace Durin
{
	// Owns the public frame-execution boundary. Scene graph authoring is delegated
	// to the renderer-private composer.
	class FSceneFrameGraphExecutor final
	{
	public:
		explicit FSceneFrameGraphExecutor(FSceneRenderer& Renderer);

		FSceneFrameGraphExecutor(const FSceneFrameGraphExecutor&) = delete;
		auto operator=(const FSceneFrameGraphExecutor&)
			-> FSceneFrameGraphExecutor& = delete;
		FSceneFrameGraphExecutor(FSceneFrameGraphExecutor&&) = delete;
		auto operator=(FSceneFrameGraphExecutor&&)
			-> FSceneFrameGraphExecutor& = delete;

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
		) -> ESceneFrameGraphExecutionStatus;

		FSceneFrameExecutionPipeline Pipeline;
		FRDGAllocator& Allocator;
		bool bReportedRegressionOverage = false;
		bool bReportedExecutionFailure = false;
	};
} // namespace Durin
