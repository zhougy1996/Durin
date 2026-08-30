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
			MakeRDGShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferMaterial),
				FRDGTextureParameter>("GBufferMaterial",
					offsetof(FParameters, GBufferMaterial),
				ERDGParameterMemberKind::Texture,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::Read, ERHIAccess::GraphicsShaderRead,
				ERHIBindingType::Texture),
			MakeRDGShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferNormals),
				FRDGTextureParameter>("GBufferNormals",
					offsetof(FParameters, GBufferNormals),
				ERDGParameterMemberKind::Texture,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::Read, ERHIAccess::GraphicsShaderRead,
				ERHIBindingType::Texture),
			MakeRDGShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferSurface),
				FRDGTextureParameter>("GBufferSurface",
					offsetof(FParameters, GBufferSurface),
				ERDGParameterMemberKind::Texture,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::Read, ERHIAccess::GraphicsShaderRead,
				ERHIBindingType::Texture),
			MakeRDGShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferEmissive),
				FRDGTextureParameter>("GBufferEmissive",
					offsetof(FParameters, GBufferEmissive),
				ERDGParameterMemberKind::Texture,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::Read, ERHIAccess::GraphicsShaderRead,
				ERHIBindingType::Texture),
			MakeRDGShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::SceneDepth), FRDGTextureParameter>(
				"SceneDepth", offsetof(FParameters, SceneDepth),
				ERDGParameterMemberKind::Texture,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::Read, ERHIAccess::GraphicsShaderRead,
				ERHIBindingType::Texture),
			MakeRDGResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::Output),
				FRDGColorAttachmentParameter>("Output",
					offsetof(FParameters, Output),
				ERDGParameterMemberKind::ColorAttachment,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::ReadWrite,
				ERHIAccess::ColorAttachmentReadWrite, true,
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
			MakeRDGShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferMaterial),
				FRDGTextureParameter>("GBufferMaterial",
					offsetof(FParameters, GBufferMaterial),
				ERDGParameterMemberKind::Texture,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::Read, ERHIAccess::ComputeShaderRead,
				ERHIBindingType::Texture),
			MakeRDGShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferNormals),
				FRDGTextureParameter>("GBufferNormals",
					offsetof(FParameters, GBufferNormals),
				ERDGParameterMemberKind::Texture,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::Read, ERHIAccess::ComputeShaderRead,
				ERHIBindingType::Texture),
			MakeRDGShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferSurface),
				FRDGTextureParameter>("GBufferSurface",
					offsetof(FParameters, GBufferSurface),
				ERDGParameterMemberKind::Texture,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::Read, ERHIAccess::ComputeShaderRead,
				ERHIBindingType::Texture),
			MakeRDGShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferEmissive),
				FRDGTextureParameter>("GBufferEmissive",
					offsetof(FParameters, GBufferEmissive),
				ERDGParameterMemberKind::Texture,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::Read, ERHIAccess::ComputeShaderRead,
				ERHIBindingType::Texture),
			MakeRDGShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::SceneDepth), FRDGTextureParameter>(
				"SceneDepth", offsetof(FParameters, SceneDepth),
				ERDGParameterMemberKind::Texture,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::Read, ERHIAccess::ComputeShaderRead,
				ERHIBindingType::Texture),
			MakeRDGShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::ContactVisibilityOutput),
				FRDGTextureParameter>("ContactVisibilityOutput",
					offsetof(FParameters, ContactVisibilityOutput),
				ERDGParameterMemberKind::Texture,
				ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource,
				ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite,
				ERHIBindingType::StorageImage, nullptr, true)};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FContactShadowComputePassParameters", Members);
		return &Metadata;
	}

	auto FContactShadowVisibilityGraphContributor::AddPasses(
		const FContactShadowGraphInputs& Inputs) -> FContactShadowGraphOutput
	{
		auto& Graph = Inputs.Graph;
		auto& Services = Inputs.Services;
		const auto RecordInputs = Inputs.Record;
		const auto& Options = Inputs.Options;
		const auto PreparedContactRoute = Inputs.Route;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		const bool bWantsProductionDeferred = Inputs.bProductionDeferred;
		FSceneRenderTopology Topology;
		Topology.ContactShadowVisibility = Inputs.GraphRoute;
		std::optional<FRDGTextureHandle> ContactShadowVisibilityFragment;
		std::optional<FRDGTextureHandle> ContactShadowVisibilityCompute;
		const auto ContactShadowVisibilityCompletion = Graph.CreateValue<
			FContactShadowVisibilityPassResult>(
				"Scene.ContactShadowVisibilityValue",
				"contact-shadow-visibility-result");
		if (Topology.UsesContactShadowVisibilityFragment())
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
				"Scene.ContactShadowVisibility.Fragment",
				ERHIAccess::GraphicsShaderRead);
		if (Topology.UsesContactShadowVisibilityCompute())
			ContactShadowVisibilityCompute = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DirectionalContactShadowVisibilityCompute", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::Storage
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::ContactCompute)},
				"Scene.ContactShadowVisibility.Compute",
				ERHIAccess::GraphicsShaderRead);
		auto FillCommonParameters = [&](auto& Parameters) {
			Parameters.DirectionalShadow = {
				.Value = Inputs.DirectionalShadow.Completion};
			Parameters.GBufferCompletion = {.Value = Inputs.GBuffer.Completion};
			Parameters.Completion = {
				.Value = ContactShadowVisibilityCompletion};
			if (Inputs.GBuffer.Textures[0])
			{
				const FRHITextureSubresourceRange ColorRange{
					ERHITextureAspect::Color, 0, 1, 0, 1};
				Parameters.GBufferMaterial = FRDGTextureParameter{
					*Inputs.GBuffer.Textures[0], ColorRange};
				Parameters.GBufferNormals = FRDGTextureParameter{
					*Inputs.GBuffer.Textures[1], ColorRange};
				Parameters.GBufferSurface = FRDGTextureParameter{
					*Inputs.GBuffer.Textures[2], ColorRange};
				Parameters.GBufferEmissive = FRDGTextureParameter{
					*Inputs.GBuffer.Textures[3], ColorRange};
				Parameters.SceneDepth = FRDGTextureParameter{
					Inputs.GBuffer.Depth,
					{ERHITextureAspect::Depth, 0, 1, 0, 1}};
			}
		};
		auto Execute = [&Services, RecordInputs, &Options, Width, Height,
			bWantsProductionDeferred](FRHICommandListImmediate& Commands,
			const auto& Parameters,
			const FRDGParameterResolver& Resolver) {
			std::optional<FGBufferRenderer::FTargets> GBufferTargets;
			if (Parameters.GBufferMaterial)
				GBufferTargets = {
					.Material = Resolver.GetTexture(Parameters.GBufferMaterial),
					.Normals = Resolver.GetTexture(Parameters.GBufferNormals),
					.Surface = Resolver.GetTexture(Parameters.GBufferSurface),
					.Emissive = Resolver.GetTexture(Parameters.GBufferEmissive)};
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
				Services.Recorders.RenderContactShadowVisibility_RenderThread(
					Commands, RecordInputs,
					GBufferTargets ? &*GBufferTargets : nullptr,
					FragmentContactTargets ? &*FragmentContactTargets : nullptr,
					ComputeContactTargets ? &*ComputeContactTargets : nullptr,
					SceneTargets, &ShaderParameters, Options, Width, Height,
					bWantsProductionDeferred, GBufferResult.IsComplete(),
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
			(void)AddSceneRenderFeaturePass<
				FContactShadowVisibilityGraphContributor>(Graph,
				ERDGPassType::Compute, std::move(Parameters), Execute);
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
			(void)AddSceneRenderFeaturePass<
				FContactShadowVisibilityGraphContributor>(Graph,
				ERDGPassType::Graphics, std::move(Parameters), Execute);
		}
		return {.Completion = ContactShadowVisibilityCompletion,
			.Fragment = ContactShadowVisibilityFragment,
			.Compute = ContactShadowVisibilityCompute};
	}

	auto FSceneRenderFeatureRecorders::RenderContactShadowVisibility_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FContactShadowVisibilityRecordInputs& Inputs,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FContactShadowVisibilityRenderer::FTargets*
			FragmentContactTargets,
		const FContactShadowVisibilityRenderer::FComputeTargets*
			ComputeContactTargets,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FRDGShaderParameterScope* ShaderParameters,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsProductionDeferred,
		bool bGBufferComplete,
		bool bGBufferHasGeometry
	) -> FContactShadowVisibilityPassResult
	{
		FContactShadowVisibilityPassResult PassResult;
		const FSceneView& RenderView = Inputs.View;
		const bool bWantsContactVisibility = bWantsProductionDeferred
											 && RenderView.Settings.DirectionalShadow.bEnableContactShadows
											 && Inputs.Shadow != nullptr
											 && ResolvedFrame.DirectionalShadow
											 && ResolvedFrame.DirectionalShadow->bEnabled;
		if (!bWantsContactVisibility) return PassResult;
		PassResult.Status = EScenePassStatus::Failed;
		if (bWantsContactVisibility && bGBufferComplete
			&& bGBufferHasGeometry)
		{
			const EContactShadowRoutePreference RoutePreference =
				RenderView.Settings.DirectionalShadow.ContactRoutePreference;
			const bool bForceFragment = Qualification.bForceFragmentContactVisibility
										|| RoutePreference == EContactShadowRoutePreference::Fragment;
			const bool bForceCompute = !Qualification.bForceFragmentContactVisibility
									   && RoutePreference == EContactShadowRoutePreference::Compute;
			if (bForceCompute) FragmentContactTargets = nullptr;
			if (bForceFragment) ComputeContactTargets = nullptr;
			Telemetry.View.ContactShadow.ContactShadowRetainedBytes =
				TransientTargets.GetObservedRetainedBytes_RenderThread(
					ERDGAllocationObservation::ContactFragment)
				+ TransientTargets.GetObservedRetainedBytes_RenderThread(
					ERDGAllocationObservation::ContactCompute);
			const auto ContactResult = ContactShadowRenderer.Render_RenderThread(
				CommandList, true, FragmentContactTargets, ComputeContactTargets,
				GBufferTargets->Material, GBufferTargets->Normals,
				GBufferTargets->Surface, GBufferTargets->Emissive,
				SceneTargets.Depth, RenderView,
				Inputs.Shadow->View.LightDirection, Width, Height,
				{.bGraphManagedTextureAccess = true,
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
} // namespace Durin
