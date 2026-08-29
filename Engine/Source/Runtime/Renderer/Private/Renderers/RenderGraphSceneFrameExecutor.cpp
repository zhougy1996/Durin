#include "Renderers/RenderGraphSceneFrameExecutor.h"

#include "Renderers/SceneRendererProfiling.h"
#include "RHICommandList.h"

namespace Durin
{
	FRenderGraphSceneFrameExecutor::FRenderGraphSceneFrameExecutor(
		FSceneRenderer& Renderer)
		: Pipeline(Renderer)
	{
	}

	auto FRenderGraphSceneFrameExecutor::Execute_RenderThread(
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		FSceneViewStatistics* OutStatistics,
		FRenderGraphCapture* OutRenderGraphCapture
	) -> ERenderViewResult
	{
		return Pipeline.Execute_RenderThread(CommandList, Scene, View, OutputTarget,
			bPresentOutput, Options, OutStatistics,
			[this, &CommandList, OutRenderGraphCapture](FRenderGraphBuilder& Graph) {
				return CompileAndExecuteGraph_RenderThread(
					Graph, CommandList, OutRenderGraphCapture);
			});
	}

	auto FRenderGraphSceneFrameExecutor::CompileAndExecuteGraph_RenderThread(
		FRenderGraphBuilder& Graph,
		FRHICommandListImmediate& CommandList,
		FRenderGraphCapture* OutRenderGraphCapture
	) -> ESceneFrameGraphExecutionStatus
	{
		auto CompiledGraph = Graph.Compile();
		if (!CompiledGraph.IsSuccess())
		{
			DURIN_WARN("Scene frame graph compilation failed: {}",
				CompiledGraph.Error);
			return ESceneFrameGraphExecutionStatus::CompileFailed;
		}
		const FRenderGraphStatistics Statistics =
			CompiledGraph.Graph->GetStatistics();
		if (Statistics.IsStructuralRegressionBudgetExceeded()
			&& !bReportedRegressionOverage)
		{
			const FRenderGraphBudget& Budget = CompiledGraph.Graph->GetBudget();
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
		const bool Executed =
			CompiledGraph.Graph->Execute(CommandList, &ExecutionError);
		if (!Executed && !std::exchange(bReportedExecutionFailure, true))
		{
			DURIN_WARN("Scene frame graph execution failed: {}",
				ExecutionError.empty() ? "unspecified error" : ExecutionError);
		}
		if (OutRenderGraphCapture != nullptr)
			*OutRenderGraphCapture = CompiledGraph.Graph->Capture();
		PublishSceneRenderGraphCapture(*CompiledGraph.Graph);
		return Executed ? ESceneFrameGraphExecutionStatus::Executed
			: ESceneFrameGraphExecutionStatus::ExecutionFailed;
	}
} // namespace Durin
