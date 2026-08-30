#include "Renderers/SceneRenderGraphContributors.h"

#include "Renderers/SceneRenderFeatureRecorders.h"
#include "Renderers/SceneRenderGraphComposer.h"
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
		const auto DirectionalShadow =
			Graph.CreateValue<FDirectionalShadowPassResult>(
				"Scene.DirectionalShadowValue", "directional-shadow-result");
		auto Parameters = Graph.AllocParameters<FDirectionalShadowPassParameters>();
		Parameters->Completion = {.Value = DirectionalShadow};
		if (Inputs.Shadow)
			Parameters->Resources.DirectionalShadowOutput = {
				.Texture = *Inputs.Shadow,
				.Range = {ERHITextureAspect::Depth, 0, 1, 0,
					DirectionalShadowCascadeCount}};
		(void)AddSceneRenderFeaturePass<FDirectionalShadowGraphContributor>(
			Graph, ERDGPassType::Graphics, std::move(Parameters),
			[&Services, RecordInputs](FRHICommandListImmediate& Commands,
				const FDirectionalShadowPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				Resolver.WriteValue(PassParameters.Completion) =
					Services.Recorders.RenderDirectionalShadow_RenderThread(Commands,
						RecordInputs, Resolver.GetDepthStencilAttachment(
							PassParameters.Resources.DirectionalShadowOutput).Texture);
			});
		return {.Completion = DirectionalShadow, .Shadow = Inputs.Shadow};
	}

	auto FSceneRenderFeatureRecorders::RenderDirectionalShadow_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FDirectionalShadowRecordInputs& Inputs,
		FRHITexture* DirectionalShadowTarget
	) -> FDirectionalShadowPassResult
	{
		if (Inputs.Shadow == nullptr
			|| !ResolvedSceneResources.DirectionalShadow
			|| !ResolvedSceneResources.DirectionalShadow->bEnabled)
			return {};
		const bool bRendered = DirectionalShadowRenderer.Render_RenderThread(
			CommandList, DirectionalShadowTarget, StaticMeshRenderer, SkeletalMeshRenderer,
			TerrainRenderer, *Inputs.Shadow,
			*ResolvedSceneResources.DirectionalShadow, Telemetry.View);
		return {
			.Status = bRendered
				? EScenePassStatus::Complete : EScenePassStatus::Failed};
	}
} // namespace Durin
