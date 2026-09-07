#include "Renderers/DirectionalShadowRendering.h"
#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	namespace
	{
		auto RecordDirectionalShadow(
			FRHICommandListImmediate& CommandList,
			const FPreparedDirectionalShadow* Shadow,
			FRHITexture* DirectionalShadowTarget,
			FDirectionalShadowRenderer& Renderer,
			FStaticMeshRenderer& StaticMeshes,
			FResolvedSceneResources& Resolved,
			FSceneRenderTelemetry& Telemetry
		) -> FDirectionalShadowPassResult
		{
			if (Shadow == nullptr || !Resolved.DirectionalShadow
				|| !Resolved.DirectionalShadow->bEnabled)
				return {};
			const bool bRendered = Renderer.Render_RenderThread(CommandList,
				DirectionalShadowTarget, StaticMeshes,
				*Shadow, *Resolved.DirectionalShadow, Telemetry.View);
			return {.Status = bRendered ? EScenePassStatus::Complete
				: EScenePassStatus::Failed};
		}
	} // namespace

	auto FDirectionalShadowPassResources::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FDirectionalShadowPassResources;
		static const std::array Members = {
			MakeRDGResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::DirectionalShadowOutput),
				FRDGDepthStencilAttachmentParameter>("DirectionalShadowOutput",
				offsetof(FParameters, DirectionalShadowOutput),
				ERDGParameterMemberKind::ManagedDepthStencilAttachment,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::ReadWrite, ERHIAccess::DepthStencilReadWrite, true,
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store, true,
				ERHIAccess::GraphicsShaderRead)};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FDirectionalShadowPassResources", Members);
		return &Metadata;
	}

	auto FDirectionalShadowPassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FDirectionalShadowPassParameters;
		static const std::array Members = {
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::Completion), FDirectionalShadowPassResult>(
					"Completion", offsetof(FParameters, Completion)),
			MakeRDGNestedParameterMemberMetadata<FParameters,
				decltype(FParameters::Resources)>("Resources",
					offsetof(FParameters, Resources),
					FDirectionalShadowPassResources::GetRDGParametersMetadata())};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FDirectionalShadowPassParameters", Members);
		return &Metadata;
	}

	auto FDirectionalShadowRendering::AddPasses(
		const FDirectionalShadowFeatureInputs& Inputs)
		-> FDirectionalShadowGraphOutput
	{
		auto& Graph = Inputs.Graph;
		const auto* ShadowRecord = Inputs.ShadowRecord;
		auto* Renderer = &Inputs.Renderer;
		auto* StaticMeshes = &Inputs.StaticMeshes;
		auto* Resolved = &Inputs.Resolved;
		auto* Telemetry = &Inputs.Telemetry;
		const auto DirectionalShadow =
			Graph.CreateValue<FDirectionalShadowPassResult>(
				"Scene.DirectionalShadowValue", "directional-shadow-result");
		auto Parameters = Graph.AllocParameters<FDirectionalShadowPassParameters>();
		Parameters->Completion = {.Value = DirectionalShadow};
		if (Inputs.Shadow)
		{
			check(ShadowRecord != nullptr && ShadowRecord->View.CascadeCount > 0
				&& ShadowRecord->View.CascadeCount <= DirectionalShadowCascadeCount);
			// Only rendered layers receive the render pass's final read layout.
			// Unused array layers must retain their imported shader-read state.
			Parameters->Resources.DirectionalShadowOutput = {
				.Texture = *Inputs.Shadow,
				.Range = {ERHITextureAspect::Depth, 0, 1, 0,
					ShadowRecord->View.CascadeCount}};
		}
		(void)Graph.AddPass(Name, ERDGPassType::Graphics, std::move(Parameters),
			[Renderer, StaticMeshes, Resolved,
				Telemetry, ShadowRecord](FRHICommandListImmediate& Commands,
				const FDirectionalShadowPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				Resolver.WriteValue(PassParameters.Completion) =
					RecordDirectionalShadow(Commands, ShadowRecord,
						Resolver.GetDepthStencilAttachment(PassParameters.Resources
							.DirectionalShadowOutput).Texture, *Renderer,
						*StaticMeshes, *Resolved,
						*Telemetry);
			});
		return {.Completion = DirectionalShadow, .Shadow = Inputs.Shadow};
	}
} // namespace Durin
