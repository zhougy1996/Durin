#include "Renderers/ContactShadowVisibilityRendering.h"
#include "Renderers/SceneTextureGroupParameters.h"

#include "Renderers/DirectionalShadowRendering.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/SceneRenderer.h"
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
		auto RecordContactShadowVisibility_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& RenderView,
			const FPreparedDirectionalShadow* Shadow,
			FResolvedSceneResources& Resolved,
			FSceneRenderTelemetry& Telemetry,
			FRendererRDGAllocator& Allocator,
			FContactShadowVisibilityRenderer& Renderer,
			const FContactShadowVisibilityRenderer::FRouteDecision& PreparedRoute,
			const FGBufferRenderer::FTargets* GBufferTargets,
			const FContactShadowVisibilityRenderer::FTargets* FragmentTargets,
			const FContactShadowVisibilityRenderer::FComputeTargets* ComputeTargets,
			const FPostProcessRenderer::FSceneTargets& SceneTargets,
			const FRDGShaderParameterScope* ShaderParameters,
			uint32 Width, uint32 Height,
			bool bGBufferComplete, bool bGBufferHasGeometry)
			-> FContactShadowVisibilityPassResult;
	}

	auto FContactShadowGraphicsPassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FContactShadowGraphicsPassParameters;
		static const std::array Members = {
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::DirectionalShadow),
				FDirectionalShadowPassResult>("DirectionalShadow",
					offsetof(FParameters, DirectionalShadow)),
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferCompletion), FGBufferPassResult>(
				"GBufferCompletion", offsetof(FParameters, GBufferCompletion)),
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::Completion),
				FContactShadowVisibilityPassResult>("Completion",
					offsetof(FParameters, Completion)),
			WithRDGShaderBinding(MakeRDGTextureReadMetadata<FParameters,
				decltype(FParameters::GBufferMaterial)>("GBufferMaterial", offsetof(FParameters, GBufferMaterial)), ERHIBindingType::Texture),
			WithRDGShaderBinding(MakeRDGTextureReadMetadata<FParameters,
				decltype(FParameters::GBufferNormals)>("GBufferNormals", offsetof(FParameters, GBufferNormals)), ERHIBindingType::Texture),
			WithRDGShaderBinding(MakeRDGTextureReadMetadata<FParameters,
				decltype(FParameters::GBufferSurface)>("GBufferSurface", offsetof(FParameters, GBufferSurface)), ERHIBindingType::Texture),
			WithRDGShaderBinding(MakeRDGTextureReadMetadata<FParameters,
				decltype(FParameters::GBufferEmissive)>("GBufferEmissive", offsetof(FParameters, GBufferEmissive)), ERHIBindingType::Texture),
			WithRDGShaderBinding(MakeRDGTextureReadMetadata<FParameters, decltype(FParameters::SceneDepth)>(
				"SceneDepth", offsetof(FParameters, SceneDepth)),
				ERHIBindingType::Texture),
			MakeRDGAttachmentMetadata<FParameters, decltype(FParameters::Output)>(
				"Output",
				offsetof(FParameters, Output),
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store)};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FContactShadowGraphicsPassParameters", Members);
		return &Metadata;
	}

	auto FContactShadowComputePassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FContactShadowComputePassParameters;
		static const std::array Members = {
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::DirectionalShadow),
				FDirectionalShadowPassResult>("DirectionalShadow",
					offsetof(FParameters, DirectionalShadow)),
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferCompletion), FGBufferPassResult>(
				"GBufferCompletion", offsetof(FParameters, GBufferCompletion)),
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::Completion),
				FContactShadowVisibilityPassResult>("Completion",
					offsetof(FParameters, Completion)),
			WithRDGShaderBinding(MakeRDGTextureReadMetadata<FParameters,
				decltype(FParameters::GBufferMaterial), ERDGPassType::Compute>("GBufferMaterial", offsetof(FParameters, GBufferMaterial)), ERHIBindingType::Texture),
			WithRDGShaderBinding(MakeRDGTextureReadMetadata<FParameters,
				decltype(FParameters::GBufferNormals), ERDGPassType::Compute>("GBufferNormals", offsetof(FParameters, GBufferNormals)), ERHIBindingType::Texture),
			WithRDGShaderBinding(MakeRDGTextureReadMetadata<FParameters,
				decltype(FParameters::GBufferSurface), ERDGPassType::Compute>("GBufferSurface", offsetof(FParameters, GBufferSurface)), ERHIBindingType::Texture),
			WithRDGShaderBinding(MakeRDGTextureReadMetadata<FParameters,
				decltype(FParameters::GBufferEmissive), ERDGPassType::Compute>("GBufferEmissive", offsetof(FParameters, GBufferEmissive)), ERHIBindingType::Texture),
			WithRDGShaderBinding(MakeRDGTextureReadMetadata<FParameters, decltype(FParameters::SceneDepth), ERDGPassType::Compute>(
				"SceneDepth", offsetof(FParameters, SceneDepth)),
				ERHIBindingType::Texture),
			WithRDGShaderBinding(MakeRDGComputeTextureWriteMetadata<FParameters, decltype(FParameters::ContactVisibilityOutput)>(
				"ContactVisibilityOutput", offsetof(FParameters, ContactVisibilityOutput)),
				ERHIBindingType::StorageImage, nullptr)};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FContactShadowComputePassParameters", Members);
		return &Metadata;
	}

	auto FContactShadowVisibilityRendering::AddPasses(
		const FContactShadowFeatureInputs& Inputs) -> FContactShadowGraphOutput
	{
		if (!Inputs.Feature.HasPurpose(ESceneFeaturePurpose::Production)) return {};
		auto& Graph = Inputs.Graph;
		const auto PreparedContactRoute = Inputs.Feature.Decision;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		std::optional<FRDGTextureHandle> ContactShadowVisibilityFragment;
		std::optional<FRDGTextureHandle> ContactShadowVisibilityCompute;
		const auto ContactShadowVisibilityCompletion = Graph.CreateValue<
			FContactShadowVisibilityPassResult>(
				"Scene.ContactShadowVisibilityValue",
				"contact-shadow-visibility-result");
		if (PreparedContactRoute.Route
			== FContactShadowVisibilityRenderer::ERoute::Fragment)
			ContactShadowVisibilityFragment = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DirectionalContactVisibility", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy)
					.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f)),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::ContactFragment)},
				"Scene.ContactShadowVisibility.Fragment");
		if (PreparedContactRoute.Route
			== FContactShadowVisibilityRenderer::ERoute::Compute)
			ContactShadowVisibilityCompute = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DirectionalContactShadowVisibilityCompute", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::Storage
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::ContactCompute)},
				"Scene.ContactShadowVisibility.Compute");
		auto FillCommonParameters = [&](auto& Parameters) {
			Parameters.DirectionalShadow = {
				.Value = Inputs.DirectionalShadow.Completion};
			Parameters.GBufferCompletion = {.Value = *Inputs.GBuffer.Completion};
			Parameters.Completion = {
				.Value = ContactShadowVisibilityCompletion};
			if (Inputs.GBuffer.Textures)
			{
				SceneTextureGroups::FillGBuffer(Inputs.GBuffer.Textures,
					Parameters.GBufferMaterial, Parameters.GBufferNormals,
					Parameters.GBufferSurface, Parameters.GBufferEmissive);
				Parameters.SceneDepth = FRDGTextureParameter{
					Inputs.GBuffer.Depth,
					{ERHITextureAspect::Depth, 0, 1, 0, 1}};
			}
		};
		auto Execute = [&Resolved = Inputs.Resolved,
			&Telemetry = Inputs.Telemetry, &Allocator = Inputs.Allocator,
			&Renderer = Inputs.Renderer, &View = Inputs.View,
			Shadow = Inputs.Shadow, PreparedContactRoute, Width, Height](FRHICommandListImmediate& Commands,
			const auto& Parameters,
			const FRDGParameterResolver& Resolver) {
			const auto GBufferTargets = SceneTextureGroups::ResolveGBuffer(Resolver,
				Parameters.GBufferMaterial, Parameters.GBufferNormals,
				Parameters.GBufferSurface, Parameters.GBufferEmissive);
			const FPostProcessRenderer::FSceneTargets SceneTargets{
				.Color = nullptr,
				.Depth = Resolver.GetTexture(Parameters.SceneDepth)};
			std::optional<FContactShadowVisibilityRenderer::FTargets>
				FragmentContactTargets;
			std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
				ComputeContactTargets;
			if constexpr (std::same_as<std::remove_cvref_t<decltype(Parameters)>,
				FContactShadowGraphicsPassParameters>)
			{
				const auto Output = Resolver.GetColorAttachment(Parameters.Output);
				if (Output) FragmentContactTargets = {.Visibility = Output.Texture};
			}
			else
			{
				if (FRHITexture* Output = Resolver.GetTexture(
					Parameters.ContactVisibilityOutput))
					ComputeContactTargets = {.Visibility = Output};
			}
			const auto ShaderParameters = Resolver.GetShaderParameters(Parameters);
			const auto& GBufferResult = Resolver.ReadValue(
				Parameters.GBufferCompletion);
			Resolver.WriteValue(Parameters.Completion) =
				RecordContactShadowVisibility_RenderThread(
					Commands, View, Shadow, Resolved, Telemetry, Allocator,
					Renderer, PreparedContactRoute,
					GBufferTargets ? &*GBufferTargets : nullptr,
					FragmentContactTargets ? &*FragmentContactTargets : nullptr,
					ComputeContactTargets ? &*ComputeContactTargets : nullptr,
					SceneTargets, &ShaderParameters, Width, Height,
					GBufferResult.IsComplete(),
					GBufferResult.bRenderedGeometry);
		};

		if (PreparedContactRoute.Route
			== FContactShadowVisibilityRenderer::ERoute::Compute)
		{
			auto Parameters = Graph.AllocParameters<
				FContactShadowComputePassParameters>();
			FillCommonParameters(Parameters.Get());
			if (ContactShadowVisibilityCompute)
				Parameters->ContactVisibilityOutput = FRDGTextureParameter{
					*ContactShadowVisibilityCompute,
					{ERHITextureAspect::Color, 0, 1, 0, 1}};
			(void)Graph.AddPass(Name, ERDGPassType::Compute,
				std::move(Parameters), Execute);
		}
		else
		{
			auto Parameters = Graph.AllocParameters<
				FContactShadowGraphicsPassParameters>();
			FillCommonParameters(Parameters.Get());
			if (ContactShadowVisibilityFragment)
				Parameters->Output = FRDGColorAttachmentParameter{
					*ContactShadowVisibilityFragment,
					{ERHITextureAspect::Color, 0, 1, 0, 1}};
			(void)Graph.AddPass(Name, ERDGPassType::Graphics,
				std::move(Parameters), Execute);
		}
		return {.Completion = ContactShadowVisibilityCompletion,
			.Fragment = ContactShadowVisibilityFragment,
			.Compute = ContactShadowVisibilityCompute};
	}

	namespace
	{
	auto RecordContactShadowVisibility_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& RenderView,
		const FPreparedDirectionalShadow* Shadow,
		FResolvedSceneResources& ResolvedSceneResources,
		FSceneRenderTelemetry& Telemetry,
		FRendererRDGAllocator& RDGAllocator,
		FContactShadowVisibilityRenderer& ContactShadowRenderer,
		const FContactShadowVisibilityRenderer::FRouteDecision& PreparedRoute,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FContactShadowVisibilityRenderer::FTargets*
			FragmentContactTargets,
		const FContactShadowVisibilityRenderer::FComputeTargets*
			ComputeContactTargets,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FRDGShaderParameterScope* ShaderParameters,
		uint32 Width,
		uint32 Height,
		bool bGBufferComplete,
		bool bGBufferHasGeometry
	) -> FContactShadowVisibilityPassResult
	{
		FContactShadowVisibilityPassResult PassResult;
		if (Shadow == nullptr || !ResolvedSceneResources.DirectionalShadow
			|| !ResolvedSceneResources.DirectionalShadow->bEnabled) return PassResult;
		PassResult.Status = EScenePassStatus::Failed;
		if (bGBufferComplete
			&& bGBufferHasGeometry)
		{
			Telemetry.View.ContactShadow.ContactShadowRetainedBytes =
				RDGAllocator.GetObservedRetainedBytes_RenderThread(
					ERDGAllocationObservation::ContactFragment)
				+ RDGAllocator.GetObservedRetainedBytes_RenderThread(
					ERDGAllocationObservation::ContactCompute);
			const auto ContactResult = ContactShadowRenderer.Render_RenderThread(
				CommandList, true, FragmentContactTargets, ComputeContactTargets,
				GBufferTargets->Material, GBufferTargets->Normals,
				GBufferTargets->Surface, GBufferTargets->Emissive,
				SceneTargets.Depth, RenderView,
				Shadow->View.LightDirection, Width, Height,
				{.PreparedRoute = PreparedRoute,
				 .bGraphManagedTextureAccess = true,
				 .GraphShaderParameters = ShaderParameters}
			);
			const size_t ReasonIndex = static_cast<size_t>(ContactResult.Reason);
			if (ReasonIndex < Telemetry.View.ContactShadow.ContactShadowRouteReasons.size())
				++Telemetry.View.ContactShadow.ContactShadowRouteReasons[ReasonIndex];
			if (ContactResult.Visibility != nullptr)
			{
				Telemetry.View.ContactShadow.ContactShadowActiveBytes =
					FContactShadowVisibilityRenderer::CalculateTargetBytes(Width, Height);
				PassResult.Status = EScenePassStatus::Complete;
				PassResult.Route = ContactResult.Route
					== FContactShadowVisibilityRenderer::ERoute::Compute
					? EContactShadowVisibilityPassRoute::Compute
					: EContactShadowVisibilityPassRoute::Fragment;
				PassResult.bDebug =
					RenderView.Settings.DirectionalShadow.bShowContactDebug;
				++Telemetry.View.ContactShadow.ContactShadowEnabledViews;
				if (ContactResult.Route
					== FContactShadowVisibilityRenderer::ERoute::Compute)
				{
					++Telemetry.View.ContactShadow.ContactShadowComputeViews;
					++Telemetry.View.ContactShadow.ContactShadowDispatches;
				}
				else
				{
					++Telemetry.View.ContactShadow.ContactShadowFragmentViews;
					++Telemetry.View.ContactShadow.ContactShadowDraws;
				}
			}
			else
			{
				++Telemetry.View.ContactShadow.ContactShadowPassFailures;
				++Telemetry.View.ContactShadow.ContactShadowFactorOneViews;
			}
		}
		return PassResult;
	}
	} // namespace
} // namespace Durin
