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
	auto FContactShadowGraphicsPassParameters::GetRenderGraphParametersMetadata()
		-> const FRenderGraphParametersMetadata*
	{
		using FParameters = FContactShadowGraphicsPassParameters;
		static const std::array Members = {
			MakeRenderGraphValueParameterMemberMetadata<FParameters,
				decltype(FParameters::DirectionalShadow),
				FDirectionalShadowPassResult>("DirectionalShadow",
					offsetof(FParameters, DirectionalShadow)),
			MakeRenderGraphValueParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferCompletion), FGBufferPassResult>(
				"GBufferCompletion", offsetof(FParameters, GBufferCompletion)),
			MakeRenderGraphValueParameterMemberMetadata<FParameters,
				decltype(FParameters::Completion),
				FContactShadowVisibilityPassResult>("Completion",
					offsetof(FParameters, Completion)),
			MakeRenderGraphShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferMaterial),
				FRenderGraphTextureParameter>("GBufferMaterial",
					offsetof(FParameters, GBufferMaterial),
				ERenderGraphParameterMemberKind::Texture,
				ERenderGraphResourceKind::Texture,
				ERenderGraphParameterRangeKind::TextureSubresource,
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead,
				ERHIBindingType::Texture),
			MakeRenderGraphShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferNormals),
				FRenderGraphTextureParameter>("GBufferNormals",
					offsetof(FParameters, GBufferNormals),
				ERenderGraphParameterMemberKind::Texture,
				ERenderGraphResourceKind::Texture,
				ERenderGraphParameterRangeKind::TextureSubresource,
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead,
				ERHIBindingType::Texture),
			MakeRenderGraphShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferSurface),
				FRenderGraphTextureParameter>("GBufferSurface",
					offsetof(FParameters, GBufferSurface),
				ERenderGraphParameterMemberKind::Texture,
				ERenderGraphResourceKind::Texture,
				ERenderGraphParameterRangeKind::TextureSubresource,
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead,
				ERHIBindingType::Texture),
			MakeRenderGraphShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferEmissive),
				FRenderGraphTextureParameter>("GBufferEmissive",
					offsetof(FParameters, GBufferEmissive),
				ERenderGraphParameterMemberKind::Texture,
				ERenderGraphResourceKind::Texture,
				ERenderGraphParameterRangeKind::TextureSubresource,
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead,
				ERHIBindingType::Texture),
			MakeRenderGraphShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::SceneDepth), FRenderGraphTextureParameter>(
				"SceneDepth", offsetof(FParameters, SceneDepth),
				ERenderGraphParameterMemberKind::Texture,
				ERenderGraphResourceKind::Texture,
				ERenderGraphParameterRangeKind::TextureSubresource,
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead,
				ERHIBindingType::Texture),
			MakeRenderGraphResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::Output),
				FRenderGraphColorAttachmentParameter>("Output",
					offsetof(FParameters, Output),
				ERenderGraphParameterMemberKind::ColorAttachment,
				ERenderGraphResourceKind::Texture,
				ERenderGraphParameterRangeKind::TextureSubresource,
				ERenderGraphUse::ReadWrite,
				ERHIAccess::ColorAttachmentReadWrite, true,
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store)};
		static const auto Metadata = MakeInlineRenderGraphParametersMetadata<
			FParameters>("FContactShadowGraphicsPassParameters", Members);
		return &Metadata;
	}

	auto FContactShadowComputePassParameters::GetRenderGraphParametersMetadata()
		-> const FRenderGraphParametersMetadata*
	{
		using FParameters = FContactShadowComputePassParameters;
		static const std::array Members = {
			MakeRenderGraphValueParameterMemberMetadata<FParameters,
				decltype(FParameters::DirectionalShadow),
				FDirectionalShadowPassResult>("DirectionalShadow",
					offsetof(FParameters, DirectionalShadow)),
			MakeRenderGraphValueParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferCompletion), FGBufferPassResult>(
				"GBufferCompletion", offsetof(FParameters, GBufferCompletion)),
			MakeRenderGraphValueParameterMemberMetadata<FParameters,
				decltype(FParameters::Completion),
				FContactShadowVisibilityPassResult>("Completion",
					offsetof(FParameters, Completion)),
			MakeRenderGraphShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferMaterial),
				FRenderGraphTextureParameter>("GBufferMaterial",
					offsetof(FParameters, GBufferMaterial),
				ERenderGraphParameterMemberKind::Texture,
				ERenderGraphResourceKind::Texture,
				ERenderGraphParameterRangeKind::TextureSubresource,
				ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead,
				ERHIBindingType::Texture),
			MakeRenderGraphShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferNormals),
				FRenderGraphTextureParameter>("GBufferNormals",
					offsetof(FParameters, GBufferNormals),
				ERenderGraphParameterMemberKind::Texture,
				ERenderGraphResourceKind::Texture,
				ERenderGraphParameterRangeKind::TextureSubresource,
				ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead,
				ERHIBindingType::Texture),
			MakeRenderGraphShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferSurface),
				FRenderGraphTextureParameter>("GBufferSurface",
					offsetof(FParameters, GBufferSurface),
				ERenderGraphParameterMemberKind::Texture,
				ERenderGraphResourceKind::Texture,
				ERenderGraphParameterRangeKind::TextureSubresource,
				ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead,
				ERHIBindingType::Texture),
			MakeRenderGraphShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferEmissive),
				FRenderGraphTextureParameter>("GBufferEmissive",
					offsetof(FParameters, GBufferEmissive),
				ERenderGraphParameterMemberKind::Texture,
				ERenderGraphResourceKind::Texture,
				ERenderGraphParameterRangeKind::TextureSubresource,
				ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead,
				ERHIBindingType::Texture),
			MakeRenderGraphShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::SceneDepth), FRenderGraphTextureParameter>(
				"SceneDepth", offsetof(FParameters, SceneDepth),
				ERenderGraphParameterMemberKind::Texture,
				ERenderGraphResourceKind::Texture,
				ERenderGraphParameterRangeKind::TextureSubresource,
				ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead,
				ERHIBindingType::Texture),
			MakeRenderGraphShaderResourceParameterMemberMetadata<FParameters,
				decltype(FParameters::ContactVisibilityOutput),
				FRenderGraphTextureParameter>("ContactVisibilityOutput",
					offsetof(FParameters, ContactVisibilityOutput),
				ERenderGraphParameterMemberKind::Texture,
				ERenderGraphResourceKind::Texture,
				ERenderGraphParameterRangeKind::TextureSubresource,
				ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite,
				ERHIBindingType::StorageImage, nullptr, true)};
		static const auto Metadata = MakeInlineRenderGraphParametersMetadata<
			FParameters>("FContactShadowComputePassParameters", Members);
		return &Metadata;
	}

	auto FContactShadowVisibilityGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FContactShadowVisibilityRecordInputs& RecordInputs) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Topology = Context.Topology;
		const auto& PreparedEditorAssistance =
			Context.EditorAssistance;
		const auto PreparedContactRoute = Context.ContactRoute;
		const auto PreparedCloudShadowRoute = Context.CloudShadowRoute;
		const auto PreparedCloudRoute = Context.CloudRoute;
		auto* CloudWeatherTexture = Context.CloudWeatherTexture;
		auto* DirectionalShadowTexture = Context.DirectionalShadowTexture;
		const uint32 Width = Context.Width;
		const uint32 Height = Context.Height;
		const bool bPresentOutput = Context.bPresentOutput;
		const bool bHasEditorAssistance =
			Context.bHasEditorAssistance;
		const bool bRequiresDeferredOpaque =
			Context.bRequiresDeferredOpaque;
		const bool bWantsIsolatedDeferred =
			Context.bWantsIsolatedDeferred;
		const bool bWantsGroundTruthAmbientOcclusion =
			Context.bWantsGroundTruthAmbientOcclusion;
		const bool bWantsDeferredInputs =
			Context.bWantsDeferredInputs;
		const bool bWantsProductionDeferred =
			Context.bWantsProductionDeferred;
		const bool bHybridRetainedResourcesReady =
			Context.bHybridRetainedResourcesReady;
		const bool bNeedsGBuffer = Context.bNeedsGBuffer;
		auto& DeferredParameters =
			Context.Composition.DeferredParameters;
		auto& ProductionDeferredParameters =
			Context.Composition.ProductionDeferredParameters;
		auto& GraphResources = Context.Composition.Resources;
		auto& Channels = Context.Composition.Channels;
		auto& DirectionalShadowValue = Channels.DirectionalShadow;
		auto& GBufferValue = Channels.GBuffer;
		auto& AmbientOcclusionValue = Channels.AmbientOcclusion;
		auto& ContactShadowVisibilityValue = Channels.ContactShadowVisibility;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredDirectionalLightingValue = Channels.DeferredDirectionalLighting;
		auto& BaseSceneValue = Channels.BaseScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto DeclarePersistentGraphicsInputs = [&](auto Pass) {
			std::vector<FRenderGraphTextureHandle> Declared;
			auto Declare = [&](const auto& Handle, FRHITexture* Physical) {
				if (!Handle || !Physical
					|| std::ranges::find(Declared, *Handle) != Declared.end())
					return;
				Declared.push_back(*Handle);
				Graph.UseTexture(Pass, *Handle,
					{GetTextureAspects(Physical->GetFormat()), 0,
						Physical->GetNumMips(), 0, Physical->GetArraySize()},
					ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
			};
			Declare(GraphResources.DefaultWhite,
				Services.DefaultTextures.Get_RenderThread(EDefaultTexture::White));
			Declare(GraphResources.DefaultShadowArray,
				Services.DefaultTextures.GetArray_RenderThread());
			Declare(GraphResources.EnvironmentIrradiance,
				GraphResources.SelectedEnvironmentIrradiance);
			Declare(GraphResources.EnvironmentPrefiltered,
				GraphResources.SelectedEnvironmentPrefiltered);
			Declare(GraphResources.EnvironmentBrdfLut,
				GraphResources.SelectedEnvironmentBrdfLut);
		};
		if (Topology.UsesContactShadowVisibilityFragment())
			GraphResources.ContactShadowVisibilityFragment = Graph.CreateTexture(
				"Scene.ContactShadowVisibility.Fragment",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DirectionalContactVisibility", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy)
					.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f)),
					.BackingClass = std::string(GetSceneFrameBackingClassName(
						ESceneFrameBackingClass::ContactShadowVisibilityFragment))},
				ERHIAccess::GraphicsShaderRead);
		if (Topology.UsesContactShadowVisibilityCompute())
			GraphResources.ContactShadowVisibilityCompute = Graph.CreateTexture(
				"Scene.ContactShadowVisibility.Compute",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DirectionalContactShadowVisibilityCompute", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::Storage
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.BackingClass = std::string(GetSceneFrameBackingClassName(
						ESceneFrameBackingClass::ContactShadowVisibilityCompute))},
				ERHIAccess::GraphicsShaderRead);
		auto FillCommonParameters = [&](auto& Parameters) {
			Parameters.DirectionalShadow = {.Value = DirectionalShadowValue.Handle};
			Parameters.GBufferCompletion = {.Value = GBufferValue.Handle};
			Parameters.Completion = {.Value = ContactShadowVisibilityValue.Handle};
			if (GraphResources.GBuffer[0])
			{
				const FRHITextureSubresourceRange ColorRange{
					ERHITextureAspect::Color, 0, 1, 0, 1};
				Parameters.GBufferMaterial = FRenderGraphTextureParameter{
					*GraphResources.GBuffer[0], ColorRange};
				Parameters.GBufferNormals = FRenderGraphTextureParameter{
					*GraphResources.GBuffer[1], ColorRange};
				Parameters.GBufferSurface = FRenderGraphTextureParameter{
					*GraphResources.GBuffer[2], ColorRange};
				Parameters.GBufferEmissive = FRenderGraphTextureParameter{
					*GraphResources.GBuffer[3], ColorRange};
				Parameters.SceneDepth = FRenderGraphTextureParameter{
					GraphResources.SceneDepth,
					{ERHITextureAspect::Depth, 0, 1, 0, 1}};
			}
		};
		auto Execute = [&Services, RecordInputs, &Options, Width, Height,
			bWantsProductionDeferred](FRHICommandListImmediate& Commands,
			const auto& Parameters,
			const FRenderGraphParameterResolver& Resolver) {
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
			if (GraphResources.ContactShadowVisibilityCompute)
				Parameters->ContactVisibilityOutput = FRenderGraphTextureParameter{
					*GraphResources.ContactShadowVisibilityCompute,
					{ERHITextureAspect::Color, 0, 1, 0, 1}};
			(void)AddSceneFrameFeaturePass<
				FContactShadowVisibilityGraphContributor>(Graph,
				ERenderGraphPassType::Compute, std::move(Parameters), Execute);
		}
		else
		{
			auto Parameters = Graph.AllocParameters<
				FContactShadowGraphicsPassParameters>();
			FillCommonParameters(Parameters.Get());
			if (GraphResources.ContactShadowVisibilityFragment)
				Parameters->Output = FRenderGraphColorAttachmentParameter{
					*GraphResources.ContactShadowVisibilityFragment,
					{ERHITextureAspect::Color, 0, 1, 0, 1}};
			(void)AddSceneFrameFeaturePass<
				FContactShadowVisibilityGraphContributor>(Graph,
				ERenderGraphPassType::Graphics, std::move(Parameters), Execute);
		}
	}

	auto FSceneFrameFeatureRecorders::RenderContactShadowVisibility_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FContactShadowVisibilityRecordInputs& Inputs,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FContactShadowVisibilityRenderer::FTargets*
			FragmentContactTargets,
		const FContactShadowVisibilityRenderer::FComputeTargets*
			ComputeContactTargets,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FRenderGraphShaderParameters* ShaderParameters,
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
				ContactShadowRenderer.GetRetainedTargetBytes_RenderThread();
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
