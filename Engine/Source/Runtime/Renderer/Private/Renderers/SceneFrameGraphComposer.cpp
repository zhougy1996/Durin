#include "Renderers/SceneFrameGraphComposer.h"

#include "Renderers/SceneFrameGraphBackingProvider.h"
#include "Renderers/SceneFrameGraphContributors.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "Resources/RenderTargetLayouts.h"

namespace Durin
{
	auto FSceneFrameGraphComposer::Compose(
		FRenderGraphBuilder& Graph,
		const FSceneFrameGraphComposeInputs& Inputs,
		FSceneFrameGraphComposition& Composition) -> void
	{
		auto& Services = Inputs.Services;
		const auto& PreparedView = Inputs.PreparedView;
		const auto& View = Inputs.View;
		auto* OutputTarget = Inputs.OutputTarget;
		const auto& Options = Inputs.Options;
		auto& Topology = Inputs.Topology;
		const auto& PreparedEditorAssistance = Inputs.EditorAssistance;
		const auto PreparedContactRoute = Inputs.ContactRoute;
		const auto PreparedCloudShadowRoute = Inputs.CloudShadowRoute;
		const auto PreparedCloudRoute = Inputs.CloudRoute;
		auto* CloudWeatherTexture = Inputs.CloudWeatherTexture;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		const bool bPresentOutput = Inputs.bPresentOutput;
		const bool bHasEditorAssistance = Inputs.bHasEditorAssistance;
		const bool bRequiresDeferredOpaque = Inputs.bRequiresDeferredOpaque;
		const bool bWantsIsolatedDeferred = Inputs.bWantsIsolatedDeferred;
		const bool bWantsGroundTruthAmbientOcclusion =
			Inputs.bWantsGroundTruthAmbientOcclusion;
		const bool bWantsDeferredInputs = Inputs.bWantsDeferredInputs;
		const bool bWantsProductionDeferred = Inputs.bWantsProductionDeferred;
		const bool bHybridRetainedResourcesReady =
			Inputs.bHybridRetainedResourcesReady;
		const bool bNeedsGBuffer = Inputs.bNeedsGBuffer;
		auto& DeferredParameters = Composition.DeferredParameters;
		auto& ProductionDeferredParameters =
			Composition.ProductionDeferredParameters;
		auto& TargetResolutionResult = Composition.TargetResolutionResult;
		auto& GraphResources = Composition.Resources;
		auto& Channels = Composition.Channels;
		constexpr FRenderGraphBudget SceneFrameBudget{
			.MaxPasses = 256,
			.MaxDependencies = 4096,
			.MaxBufferTransitions = 4096,
			.MaxTextureTransitions = 4096,
			.RegressionMaxPasses = 12,
			.RegressionMaxDependencies = 28,
			.RegressionMaxTextureTransitions = 32,
			.MaxCompileMicroseconds = 5000,
			.MaxExecuteMicroseconds = 250000,
		};
		Graph.SetBudget(SceneFrameBudget);
		Graph.EnablePassCulling();
		auto ImportPersistentTexture = [&](std::string_view Name,
			FRHITexture* Texture) -> std::optional<FRenderGraphTextureHandle> {
			if (!Texture) return std::nullopt;
			return Graph.ImportTexture(Name, Texture,
				ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
		};
		FRHITexture* DirectionalShadowTexture =
			Services.DirectionalShadowRenderer.GetTexture_RenderThread();
		if (PreparedView.DirectionalShadow && Services.ResolvedFrame.DirectionalShadow
			&& Services.ResolvedFrame.DirectionalShadow->bEnabled
			&& DirectionalShadowTexture != nullptr)
			GraphResources.DirectionalShadow = ImportPersistentTexture(
				"Scene.DirectionalShadow", DirectionalShadowTexture);
		if (Services.ResolvedFrame.VolumetricCloud)
		{
			GraphResources.VolumetricCloudBaseDensity = ImportPersistentTexture(
				"Scene.VolumetricCloud.BaseDensity",
				Services.ResolvedFrame.VolumetricCloud->Textures.BaseDensity);
			GraphResources.VolumetricCloudDetailDensity = ImportPersistentTexture(
				"Scene.VolumetricCloud.DetailDensity",
				Services.ResolvedFrame.VolumetricCloud->Textures.DetailDensity);
			GraphResources.VolumetricCloudWeather = ImportPersistentTexture(
				"Scene.VolumetricCloud.Weather", CloudWeatherTexture);
		}
		GraphResources.DefaultWhite = ImportPersistentTexture(
			"Scene.Default.White",
			Services.DefaultTextures.Get_RenderThread(EDefaultTexture::White));
		GraphResources.DefaultShadowArray = ImportPersistentTexture(
			"Scene.Default.ShadowArray", Services.DefaultTextures.GetArray_RenderThread());
		FRHISampler* SelectedEnvironmentSampler = nullptr;
		if (bWantsDeferredInputs)
		{
			FRHITexture* Irradiance =
				Services.EnvironmentLighting.GetIrradiance_RenderThread();
			FRHITexture* Prefiltered =
				Services.EnvironmentLighting.GetPrefiltered_RenderThread();
			FRHITexture* BrdfLut =
				Services.EnvironmentLighting.GetBrdfLut_RenderThread();
			SelectedEnvironmentSampler =
				Services.EnvironmentLighting.GetSampler_RenderThread();
			if (Irradiance == nullptr || Prefiltered == nullptr
				|| BrdfLut == nullptr || SelectedEnvironmentSampler == nullptr)
			{
				Irradiance = Services.DefaultTextures.GetCube_RenderThread();
				Prefiltered = Services.DefaultTextures.GetCube_RenderThread();
				BrdfLut = Services.DefaultTextures.Get_RenderThread(
					EDefaultTexture::Black);
				SelectedEnvironmentSampler = nullptr;
			}
			GraphResources.EnvironmentIrradiance = Graph.ImportTexture(
				"Scene.Environment.Irradiance", Irradiance,
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			GraphResources.EnvironmentPrefiltered = Graph.ImportTexture(
				"Scene.Environment.Prefiltered", Prefiltered,
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			GraphResources.EnvironmentBrdfLut = Graph.ImportTexture(
				"Scene.Environment.BrdfLut", BrdfLut,
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			GraphResources.SelectedEnvironmentIrradiance = Irradiance;
			GraphResources.SelectedEnvironmentPrefiltered = Prefiltered;
			GraphResources.SelectedEnvironmentBrdfLut = BrdfLut;
		}
		GraphResources.SceneColor = Graph.CreateTexture("Scene.Color",
			FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
				"SceneColor", Width, Height, EPixelFormat::RGBA16_FLOAT)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::SourceCopy),
				.BackingClass = std::string(GetSceneFrameBackingClassName(
					ESceneFrameBackingClass::Scene))}, ERHIAccess::GraphicsShaderRead);
		GraphResources.SceneDepth = Graph.CreateTexture("Scene.Depth",
			FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
				"SceneDepth", Width, Height, EPixelFormat::D32)
				.SetFlags(ETextureCreateFlags::DepthStencilTargetable
					| ETextureCreateFlags::ShaderResource),
				.BackingClass = std::string(GetSceneFrameBackingClassName(
					ESceneFrameBackingClass::Scene))}, ERHIAccess::DepthStencilReadWrite);
		GraphResources.Output = Graph.ImportTexture("Scene.Output", OutputTarget,
			ERHIAccess::Discard,
			bPresentOutput ? ERHIAccess::Present : ERHIAccess::GraphicsShaderRead);
		Graph.SetBackingResolver([&Services, &Topology, &TargetResolutionResult,
			&GraphResources](auto Requests, auto& Backings,
			std::string& Error) {
			const auto Retained =
				FSceneFrameGraphBackingProvider::BuildRetainedTopology(
					Requests, Topology, Error);
			if (!Retained) return false;
			TargetResolutionResult = Services.ResolveTargets(*Retained);
			if (TargetResolutionResult != ERenderViewResult::Success)
			{
				Error = "renderer transient target preparation failed";
				return false;
			}
			return FSceneFrameGraphBackingProvider::Publish(Requests, Backings,
				GraphResources, Services.ResolvedFrame.Targets, Error);
		});
		Channels = {
			.DirectionalShadow = {Graph.CreateValue<FDirectionalShadowPassResult>(
				"Scene.DirectionalShadowValue", "directional-shadow-result")},
			.GBuffer = {Graph.CreateValue<FGBufferPassResult>(
				"Scene.GBufferValue", "gbuffer-result")},
			.AmbientOcclusion = {Graph.CreateValue<
				FGroundTruthAmbientOcclusionPassResult>(
					"Scene.AmbientOcclusionValue", "ambient-occlusion-result")},
			.ContactShadowVisibility = {
				Graph.CreateValue<FContactShadowVisibilityPassResult>(
					"Scene.ContactShadowVisibilityValue",
					"contact-shadow-visibility-result")},
			.CloudShadow = {Graph.CreateValue<FVolumetricCloudShadowPassResult>(
				"Scene.CloudShadowValue", "cloud-shadow-result")},
			.DeferredDirectionalLighting = {
				Graph.CreateValue<FIsolatedDeferredPassResult>(
					"Scene.DeferredDirectionalLightingValue",
					"deferred-directional-lighting-result")},
			.BaseScene = {Graph.CreateValue<FSceneColorPassResult>(
				"Scene.BaseValue", "scene-color-result")},
			.VolumetricCloudSpatial = {
				Graph.CreateValue<FVolumetricCloudSpatialPassResult>(
					"Scene.VolumetricCloudSpatialValue",
					"volumetric-cloud-spatial-result")},
			.VolumetricCloud = {Graph.CreateValue<FVolumetricCloudPassResult>(
				"Scene.VolumetricCloudValue", "volumetric-cloud-result")},
			.SceneColor = {Graph.CreateValue<FSceneColorPassResult>(
				"Scene.ColorValue", "scene-color-result")},
			.PostProcess = {Graph.CreateValue<FPostProcessPassResult>(
				"Scene.PostProcessValue", "post-process-result")},
			.OutputCompletion = Graph.CreateToken("Scene.OutputCompletion")};
		FSceneFrameGraphContributorContext Context{
			.Graph = Graph,
			.Services = Services,
			.Composition = Composition,
			.View = View,
			.OutputTarget = OutputTarget,
			.Options = Options,
			.Topology = Topology,
			.EditorAssistance = PreparedEditorAssistance,
			.ContactRoute = PreparedContactRoute,
			.CloudShadowRoute = PreparedCloudShadowRoute,
			.CloudRoute = PreparedCloudRoute,
			.CloudWeatherTexture = CloudWeatherTexture,
			.DirectionalShadowTexture = DirectionalShadowTexture,
			.EnvironmentSampler = SelectedEnvironmentSampler,
			.Width = Width,
			.Height = Height,
			.bPresentOutput = bPresentOutput,
			.bHasEditorAssistance = bHasEditorAssistance,
			.bRequiresDeferredOpaque = bRequiresDeferredOpaque,
			.bWantsIsolatedDeferred = bWantsIsolatedDeferred,
			.bWantsGroundTruthAmbientOcclusion = bWantsGroundTruthAmbientOcclusion,
			.bWantsDeferredInputs = bWantsDeferredInputs,
			.bWantsProductionDeferred = bWantsProductionDeferred,
			.bHybridRetainedResourcesReady = bHybridRetainedResourcesReady,
			.bNeedsGBuffer = bNeedsGBuffer};
		const FSceneView& PreparedRenderView = PreparedView.Context.View;
		const FPreparedDirectionalShadow* DirectionalShadow =
			PreparedView.DirectionalShadow ? &*PreparedView.DirectionalShadow : nullptr;
		const FPreparedVolumetricCloud* VolumetricCloud =
			PreparedView.VolumetricCloud ? &*PreparedView.VolumetricCloud : nullptr;
		const FPreparedEnvironment* Environment =
			PreparedView.Environment ? &*PreparedView.Environment : nullptr;
		FDirectionalShadowGraphContributor::AddPasses(Context,
			{DirectionalShadow});
		FGBufferGraphContributor::AddPasses(Context,
			{PreparedRenderView, PreparedView.Receiver});
		FAmbientOcclusionGraphContributor::AddPasses(Context, PreparedRenderView);
		FContactShadowVisibilityGraphContributor::AddPasses(Context,
			{PreparedRenderView, DirectionalShadow});
		FVolumetricCloudShadowGraphContributor::AddPasses(Context,
			{PreparedRenderView, VolumetricCloud, PreparedView.Lighting});
		FDeferredDirectionalLightingGraphContributor::AddPasses(
			Context, PreparedRenderView);
		const FSceneGeometryRecordInputs GeometryInputs{
			PreparedRenderView, Environment, PreparedView.Receiver};
		FBaseSceneGraphContributor::AddPasses(Context, GeometryInputs);
		const FVolumetricCloudRecordInputs CloudInputs{
			PreparedRenderView, VolumetricCloud};
		FVolumetricCloudSpatialGraphContributor::AddPasses(Context, CloudInputs);
		FVolumetricCloudCompositeGraphContributor::AddPasses(Context, CloudInputs);
		FSceneColorGraphContributor::AddPasses(Context, GeometryInputs);
		FPostProcessGraphContributor::AddPasses(Context, PreparedRenderView);
		FEditorAssistanceGraphContributor::AddPasses(Context, PreparedRenderView);
	}
} // namespace Durin
