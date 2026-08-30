#include "Renderers/SceneFrameGraphContributors.h"

#include "Renderers/SceneFrameFeatureRecorders.h"
#include "Renderers/SceneFrameGraphComposer.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	auto FDirectionalShadowGraphContributor::AddPasses(
		const FDirectionalShadowGraphInputs& Inputs)
		-> FDirectionalShadowGraphOutput
	{
		auto& Graph = Inputs.Graph;
		auto& Services = Inputs.Services;
		const auto RecordInputs = Inputs.Record;
		struct { std::optional<FRDGTextureHandle> DirectionalShadow; }
			GraphResources;
		GraphResources.DirectionalShadow = Inputs.Shadow;
		struct { TRDGValueHandle<FDirectionalShadowPassResult>
			DirectionalShadow; } Channels;
		Channels.DirectionalShadow =
			Graph.CreateValue<FDirectionalShadowPassResult>(
				"Scene.DirectionalShadowValue", "directional-shadow-result");
		auto Parameters = Graph.AllocParameters<FDirectionalShadowPassParameters>();
		Parameters->Completion = {.Value = Channels.DirectionalShadow};
		if (GraphResources.DirectionalShadow)
			Parameters->Resources.DirectionalShadowOutput = {
				.Texture = *GraphResources.DirectionalShadow,
				.Range = {ERHITextureAspect::Depth, 0, 1, 0,
					DirectionalShadowCascadeCount}};
		(void)AddSceneFrameFeaturePass<FDirectionalShadowGraphContributor>(
			Graph, ERDGPassType::Graphics, std::move(Parameters),
			[&Services, RecordInputs](FRHICommandListImmediate& Commands,
				const FDirectionalShadowPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				Resolver.WriteValue(PassParameters.Completion) =
					Services.Recorders.RenderDirectionalShadow_RenderThread(Commands,
						RecordInputs, Resolver.GetDepthStencilAttachment(
							PassParameters.Resources.DirectionalShadowOutput).Texture);
			});
		return {.Completion = Channels.DirectionalShadow,
			.Shadow = GraphResources.DirectionalShadow};
	}

	auto FSceneFrameFeatureRecorders::RenderDirectionalShadow_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FDirectionalShadowRecordInputs& Inputs,
		FRHITexture* DirectionalShadowTarget
	) -> FDirectionalShadowPassResult
	{
		if (Inputs.Shadow == nullptr
			|| !ResolvedFrame.DirectionalShadow
			|| !ResolvedFrame.DirectionalShadow->bEnabled)
			return {};
		const bool bRendered = DirectionalShadowRenderer.Render_RenderThread(
			CommandList, DirectionalShadowTarget, StaticMeshRenderer, SkeletalMeshRenderer,
			TerrainRenderer, *Inputs.Shadow,
			*ResolvedFrame.DirectionalShadow, Telemetry.View);
		return {
			.Status = bRendered
				? EScenePassStatus::Complete : EScenePassStatus::Failed};
	}
} // namespace Durin
