#include "Renderers/SceneRenderGraphExecutor.h"

#include "Renderers/SceneRendererProfiling.h"
#include "RHICommandList.h"

namespace Durin
{
	FSceneRenderGraphExecutor::FSceneRenderGraphExecutor(
		FSceneRenderer& Renderer)
		: Pipeline(Renderer), Allocator(Renderer.RDGAllocator)
	{
	}

	auto FSceneRenderGraphExecutor::Execute_RenderThread(
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		FSceneViewStatistics* OutStatistics,
		FRDGCapture* OutRenderGraphCapture
	) -> ERenderViewResult
	{
		return Pipeline.Execute_RenderThread(CommandList, Scene, View, OutputTarget,
			bPresentOutput, Options, OutStatistics,
			[this, &CommandList, OutRenderGraphCapture](FRDGBuilder& Graph) {
				return CompileAndExecuteGraph_RenderThread(
					Graph, CommandList, OutRenderGraphCapture);
			});
	}

	auto FSceneRenderGraphExecutor::CompileAndExecuteGraph_RenderThread(
		FRDGBuilder& Graph,
		FRHICommandListImmediate& CommandList,
		FRDGCapture* OutRenderGraphCapture
	) -> ESceneRenderGraphExecutionStatus
	{
		auto CompiledGraph = Graph.Compile();
		if (!CompiledGraph.IsSuccess())
		{
			DURIN_WARN("Scene frame graph compilation failed: {}",
				CompiledGraph.Error);
			return ESceneRenderGraphExecutionStatus::CompileFailed;
		}
		const FRDGStatistics Statistics =
			CompiledGraph.Graph->GetStatistics();
		if (Statistics.IsStructuralRegressionBudgetExceeded()
			&& !bReportedRegressionOverage)
		{
			const FRDGBudget& Budget = CompiledGraph.Graph->GetBudget();
			DURIN_WARN(
				"Scene frame graph regression budget exceeded: passes={}/{} "
				"dependencies={}/{} buffer-transitions={}/{} "
				"texture-transitions={}/{}",
				Statistics.DeclaredPasses, Budget.RegressionMaxPasses,
				Statistics.Dependencies, Budget.RegressionMaxDependencies,
				Statistics.BufferTransitions,
				Budget.RegressionMaxBufferTransitions,
				Statistics.TextureTransitions,
				Budget.RegressionMaxTextureTransitions);
			bReportedRegressionOverage = true;
		}
		std::string ExecutionError;
		FRDGExecutionContext ExecutionContext{Allocator};
		const bool Executed = CompiledGraph.Graph->Execute(
			CommandList, ExecutionContext, &ExecutionError);
		if (!Executed && !std::exchange(bReportedExecutionFailure, true))
		{
			DURIN_WARN("Scene frame graph execution failed: {}",
				ExecutionError.empty() ? "unspecified error" : ExecutionError);
		}
		PublishSceneRenderGraphCapture(
			*CompiledGraph.Graph, OutRenderGraphCapture);
		return Executed ? ESceneRenderGraphExecutionStatus::Executed
			: ESceneRenderGraphExecutionStatus::ExecutionFailed;
	}
} // namespace Durin
