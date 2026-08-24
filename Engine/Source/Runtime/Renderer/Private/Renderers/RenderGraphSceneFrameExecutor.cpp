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
		FSceneViewStatistics* OutStatistics
	) -> ERenderViewResult
	{
		return Pipeline.Execute_RenderThread(CommandList, Scene, View, OutputTarget,
			bPresentOutput, Options, OutStatistics,
			[this, &CommandList](FRenderGraphBuilder& Graph) {
				return CompileAndExecuteGraph_RenderThread(Graph, CommandList);
			});
	}

	auto FRenderGraphSceneFrameExecutor::CompileAndExecuteGraph_RenderThread(
		FRenderGraphBuilder& Graph,
		FRHICommandListImmediate& CommandList
	) -> ESceneFrameGraphExecutionStatus
	{
		auto CompiledGraph = Graph.Compile();
		if (!CompiledGraph.IsSuccess())
		{
			DURIN_WARN("Scene frame graph compilation failed: {}",
				CompiledGraph.Error);
			return ESceneFrameGraphExecutionStatus::CompileFailed;
		}
		std::string ExecutionError;
		const bool Executed =
			CompiledGraph.Graph->Execute(CommandList, &ExecutionError);
		PublishSceneRenderGraphCapture(*CompiledGraph.Graph);
		return Executed ? ESceneFrameGraphExecutionStatus::Executed
			: ESceneFrameGraphExecutionStatus::ExecutionFailed;
	}
} // namespace Durin
