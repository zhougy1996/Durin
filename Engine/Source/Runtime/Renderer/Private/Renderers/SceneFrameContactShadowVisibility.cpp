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
				Services.EnvironmentLighting.GetIrradiance_RenderThread());
			Declare(GraphResources.EnvironmentPrefiltered,
				Services.EnvironmentLighting.GetPrefiltered_RenderThread());
			Declare(GraphResources.EnvironmentBrdfLut,
				Services.EnvironmentLighting.GetBrdfLut_RenderThread());
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
		const auto ContactShadowVisibilityPass =
			AddSceneFrameFeaturePass<FContactShadowVisibilityGraphContributor>(Graph,
			PreparedContactRoute.Route
					== FContactShadowVisibilityRenderer::ERoute::Compute
				? ERenderGraphPassType::Compute : ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordInputs, &GraphResources, &Topology,
				&Options, Width, Height, bWantsProductionDeferred](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0]
					&& Topology.ContactShadowVisibility != ESceneFrameRoute::Disabled)
					GBufferTargets = {
						.Material = Resources.GetTexture(*GraphResources.GBuffer[0]),
						.Normals = Resources.GetTexture(*GraphResources.GBuffer[1]),
						.Surface = Resources.GetTexture(*GraphResources.GBuffer[2]),
						.Emissive = Resources.GetTexture(*GraphResources.GBuffer[3])};
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = GBufferTargets
						? Resources.GetTexture(GraphResources.SceneDepth) : nullptr};
				std::optional<FContactShadowVisibilityRenderer::FTargets>
					FragmentContactTargets;
				if (GraphResources.ContactShadowVisibilityFragment)
					FragmentContactTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.ContactShadowVisibilityFragment)};
				std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
					ComputeContactTargets;
				if (GraphResources.ContactShadowVisibilityCompute)
					ComputeContactTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.ContactShadowVisibilityCompute)};
				Channels.ContactShadowVisibility.Result = Services.Recorders.RenderContactShadowVisibility_RenderThread(
					Commands,
					RecordInputs,
					GBufferTargets ? &*GBufferTargets : nullptr,
					FragmentContactTargets ? &*FragmentContactTargets : nullptr,
					ComputeContactTargets ? &*ComputeContactTargets : nullptr,
					SceneTargets, Options, Width, Height,
					bWantsProductionDeferred, Channels.GBuffer.Result.IsComplete(),
					Channels.GBuffer.Result.bRenderedGeometry);
			});
		Graph.UseToken(ContactShadowVisibilityPass, DirectionalShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(ContactShadowVisibilityPass, GBufferValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(ContactShadowVisibilityPass, ContactShadowVisibilityValue.Handle,
			ERenderGraphUse::Write);
		if (GraphResources.GBuffer[0])
		{
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseTexture(ContactShadowVisibilityPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
					PreparedContactRoute.Route
							== FContactShadowVisibilityRenderer::ERoute::Compute
						? ERHIAccess::ComputeShaderRead
						: ERHIAccess::GraphicsShaderRead);
			Graph.UseTexture(ContactShadowVisibilityPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				PreparedContactRoute.Route
						== FContactShadowVisibilityRenderer::ERoute::Compute
					? ERHIAccess::ComputeShaderRead
					: ERHIAccess::GraphicsShaderRead);
		}
		if (GraphResources.ContactShadowVisibilityFragment)
			Graph.UseColorAttachment(ContactShadowVisibilityPass,
				*GraphResources.ContactShadowVisibilityFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store);
		if (GraphResources.ContactShadowVisibilityCompute)
			Graph.UseTexture(ContactShadowVisibilityPass,
				*GraphResources.ContactShadowVisibilityCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
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
				{.bGraphManagedTextureAccess = true}
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
