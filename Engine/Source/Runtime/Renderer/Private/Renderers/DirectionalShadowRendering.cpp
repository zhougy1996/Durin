#include "Renderers/DirectionalShadowRendering.h"
#include "RDG/RDG.h"
#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"
#include <cstdlib>

namespace Durin
{
	namespace
	{
		struct FCascadeParameters
		{
			TRDGValueWrite<FStaticMeshRenderObservations> Completion;
			FDirectionalShadowPassResources Resources;
			static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
			{
				using P = FCascadeParameters;
				static const std::array Members = {
					MakeRDGValueParameterMemberMetadata<P, decltype(P::Completion), FStaticMeshRenderObservations>("Completion", offsetof(P, Completion)),
					MakeRDGNestedParameterMemberMetadata<P, decltype(P::Resources)>("Resources", offsetof(P, Resources), FDirectionalShadowPassResources::GetRDGParametersMetadata())};
				static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FDirectionalShadowCascadeParameters", Members);
				return &Metadata;
			}
		};
		struct FCompleteParameters
		{
			TRDGValueWrite<FDirectionalShadowPassResult> Completion;
			std::array<std::optional<TRDGValueRead<FStaticMeshRenderObservations>>, DirectionalShadowCascadeCount> Cascades;
			static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
			{
				using P = FCompleteParameters;
				static const std::array Members = {
					MakeRDGValueParameterMemberMetadata<P, decltype(P::Completion), FDirectionalShadowPassResult>("Completion", offsetof(P, Completion)),
					MakeRDGValueParameterMemberMetadata<P, decltype(P::Cascades), FStaticMeshRenderObservations>("Cascades", offsetof(P, Cascades))};
				static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FDirectionalShadowCompleteParameters", Members);
				return &Metadata;
			}
		};

		auto RecordDirectionalShadow(
			FRHICommandListImmediate& CommandList,
			const FPreparedDirectionalShadow* Shadow,
			FRHITexture* DirectionalShadowTarget,
			FDirectionalShadowRenderer& Renderer,
			FResolvedSceneResources& Resolved,
			FSceneRenderTelemetry& Telemetry
		) -> FDirectionalShadowPassResult
		{
			if (Shadow == nullptr || !Resolved.DirectionalShadow
				|| !Resolved.DirectionalShadow->bEnabled)
				return {};
			const bool bRendered = Renderer.Render_RenderThread(CommandList,
				DirectionalShadowTarget,
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
			MakeRDGAttachmentMetadata<FParameters, decltype(FParameters::DirectionalShadowOutput)>(
				"DirectionalShadowOutput",
				offsetof(FParameters, DirectionalShadowOutput),
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
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

	auto AddDirectionalShadowPasses(
		FRDGBuilder& Graph, const FDirectionalShadowFeatureInputs& Inputs)
		-> FDirectionalShadowGraphOutput
	{
		const auto* ShadowRecord = Inputs.ShadowRecord;
		auto* Renderer = &Inputs.Renderer;
		auto* Resolved = &Inputs.Resolved;
		auto* Telemetry = &Inputs.Telemetry;
		const auto DirectionalShadow =
			Graph.CreateValue<FDirectionalShadowPassResult>(
				"Scene.DirectionalShadowValue", "directional-shadow-result");
		const char* Setting = std::getenv("DURIN_SHADOW_RECORDING");
		const std::string_view Policy = Setting ? Setting : "auto";
		// Timing queries span all cascades and therefore retain the immediate path.
		if (Inputs.Shadow && ShadowRecord && Resolved->DirectionalShadow
			&& Resolved->DirectionalShadow->bEnabled && !HasShadowDepthTimingQuerySink()
			&& Policy != "immediate")
		{
			const uint32 Count = ShadowRecord->View.CascadeCount;
			check(Count > 0 && Count <= DirectionalShadowCascadeCount);
			std::array<std::optional<FDirectionalShadowCascadeRecording>, DirectionalShadowCascadeCount> Recordings;
			size_t DrawCount = 0;
			bool bReady = true;
			for (uint32 Cascade = 0; Cascade < Count; ++Cascade)
			{
				Recordings[Cascade] = Renderer->CaptureCascade_RenderThread(Renderer->GetTexture_RenderThread(), Cascade);
				bReady = bReady && Recordings[Cascade].has_value();
				DrawCount += ShadowRecord->StaticMeshes[Cascade].GetNumSections();
			}
			if (bReady)
			{
				const auto RecordingPolicy = Policy == "parallel" || (Policy != "serial" && DrawCount >= 256)
					? ERDGRecordingPolicy::Parallel : ERDGRecordingPolicy::Serial;
				auto Complete = Graph.AllocParameters<FCompleteParameters>();
				Complete->Completion = {.Value = DirectionalShadow};
				for (uint32 Cascade = 0; Cascade < Count; ++Cascade)
				{
					const auto Value = Graph.CreateValue<FStaticMeshRenderObservations>(
						std::format("Scene.DirectionalShadow.Cascade{}", Cascade), "shadow-cascade-observations");
					Complete->Cascades[Cascade] = TRDGValueRead<FStaticMeshRenderObservations>{.Value = Value};
					auto Pass = Graph.AllocParameters<FCascadeParameters>();
					Pass->Completion = {.Value = Value};
					Pass->Resources.DirectionalShadowOutput = {
						.Texture = *Inputs.Shadow, .Range = {ERHITextureAspect::Depth, 0, 1, Cascade, 1}};
					const auto* Prepared = &ShadowRecord->StaticMeshes[Cascade];
					const FResolvedStaticMeshView* Bindings = &Resolved->DirectionalShadow->StaticMeshes[Cascade];
					(void)Graph.AddRecordingPass(std::format("Scene.DirectionalShadow.Cascade{}", Cascade),
						ERDGPassType::Graphics, std::move(Pass),
						[Recording = *Recordings[Cascade], Prepared, Bindings](FRHICommandList& Commands,
							const FCascadeParameters& Parameters, const FRDGParameterResolver& Resolver) {
							check(Resolver.GetDepthStencilAttachment(Parameters.Resources.DirectionalShadowOutput).Texture == Recording.Target);
							Resolver.WriteValue(Parameters.Completion) = FDirectionalShadowRenderer::RecordCascade(
								Commands, Recording, *Prepared, *Bindings);
						}, RecordingPolicy);
				}
				(void)Graph.AddPass(DirectionalShadowPassName, ERDGPassType::Graphics, std::move(Complete),
					[Target = Recordings[0]->Target, Count, Resolved, Telemetry](FRHICommandListImmediate& Commands,
						const FCompleteParameters& Parameters, const FRDGParameterResolver& Resolver) {
						std::array<FStaticMeshRenderObservations, DirectionalShadowCascadeCount> Counts;
						for (uint32 Cascade = 0; Cascade < Count; ++Cascade)
							Counts[Cascade] = Resolver.ReadValue(*Parameters.Cascades[Cascade]);
						FDirectionalShadowRenderer::Complete_RenderThread(Commands, Target,
							std::span(Counts).first(Count), *Resolved->DirectionalShadow, Telemetry->View);
						Resolver.WriteValue(Parameters.Completion) = {.Status = EScenePassStatus::Complete};
					});
				return {.Completion = DirectionalShadow, .Shadow = Inputs.Shadow};
			}
		}
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
		(void)Graph.AddPass(DirectionalShadowPassName, ERDGPassType::Graphics, std::move(Parameters),
			[Renderer, Resolved,
				Telemetry, ShadowRecord](FRHICommandListImmediate& Commands,
				const FDirectionalShadowPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				Resolver.WriteValue(PassParameters.Completion) =
					RecordDirectionalShadow(Commands, ShadowRecord,
						Resolver.GetDepthStencilAttachment(PassParameters.Resources
							.DirectionalShadowOutput).Texture, *Renderer,
						*Resolved,
						*Telemetry);
			});
		return {.Completion = DirectionalShadow, .Shadow = Inputs.Shadow};
	}
} // namespace Durin
