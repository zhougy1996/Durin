#include "Renderers/SceneFrameGraphComposer.h"

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
		struct {
			std::optional<FRenderGraphTextureHandle> DirectionalShadow;
			FRenderGraphTextureHandle SceneColor;
			FRenderGraphTextureHandle SceneDepth;
			FRenderGraphTextureHandle Output;
			std::optional<FRenderGraphTextureHandle> VolumetricCloudBaseDensity;
			std::optional<FRenderGraphTextureHandle> VolumetricCloudDetailDensity;
			std::optional<FRenderGraphTextureHandle> VolumetricCloudWeather;
			std::optional<FRenderGraphTextureHandle> DefaultWhite;
			std::optional<FRenderGraphTextureHandle> DefaultShadowArray;
			std::optional<FRenderGraphTextureHandle> EnvironmentIrradiance;
			std::optional<FRenderGraphTextureHandle> EnvironmentPrefiltered;
			std::optional<FRenderGraphTextureHandle> EnvironmentBrdfLut;
			FRHITexture* SelectedEnvironmentIrradiance = nullptr;
			FRHITexture* SelectedEnvironmentPrefiltered = nullptr;
			FRHITexture* SelectedEnvironmentBrdfLut = nullptr;
		} GraphResources;
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
			return Graph.RegisterExternalTexture(FTextureRHIRef(Texture), Name,
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
			GraphResources.EnvironmentIrradiance = Graph.RegisterExternalTexture(
				FTextureRHIRef(Irradiance), "Scene.Environment.Irradiance",
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			GraphResources.EnvironmentPrefiltered = Graph.RegisterExternalTexture(
				FTextureRHIRef(Prefiltered), "Scene.Environment.Prefiltered",
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			GraphResources.EnvironmentBrdfLut = Graph.RegisterExternalTexture(
				FTextureRHIRef(BrdfLut), "Scene.Environment.BrdfLut",
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			GraphResources.SelectedEnvironmentIrradiance = Irradiance;
			GraphResources.SelectedEnvironmentPrefiltered = Prefiltered;
			GraphResources.SelectedEnvironmentBrdfLut = BrdfLut;
		}
		GraphResources.SceneColor = Graph.CreateTexture(
			FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
				"SceneColor", Width, Height, EPixelFormat::RGBA16_FLOAT)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::SourceCopy),
			.ObservationTag = static_cast<uint32>(
				ERDGAllocationObservation::Scene)}, "Scene.Color",
			ERHIAccess::GraphicsShaderRead);
		GraphResources.SceneDepth = Graph.CreateTexture(
			FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
				"SceneDepth", Width, Height, EPixelFormat::D32)
				.SetFlags(ETextureCreateFlags::DepthStencilTargetable
					| ETextureCreateFlags::ShaderResource),
			.ObservationTag = static_cast<uint32>(
				ERDGAllocationObservation::Scene)}, "Scene.Depth",
			ERHIAccess::DepthStencilReadWrite);
		GraphResources.Output = Graph.RegisterExternalTexture(
			FTextureRHIRef(OutputTarget), "Scene.Output",
			ERHIAccess::Discard,
			bPresentOutput ? ERHIAccess::Present : ERHIAccess::GraphicsShaderRead);
		const FSceneView& PreparedRenderView = PreparedView.Context.View;
		const FPreparedDirectionalShadow* DirectionalShadow =
			PreparedView.DirectionalShadow ? &*PreparedView.DirectionalShadow : nullptr;
		const FPreparedVolumetricCloud* VolumetricCloud =
			PreparedView.VolumetricCloud ? &*PreparedView.VolumetricCloud : nullptr;
		const FPreparedEnvironment* Environment =
			PreparedView.Environment ? &*PreparedView.Environment : nullptr;
		const auto DirectionalShadowOutput =
			FDirectionalShadowGraphContributor::AddPasses({
				.Graph = Graph, .Services = Services,
				.Record = {DirectionalShadow}, .Shadow = GraphResources.DirectionalShadow});
		const auto GBufferOutput = FGBufferGraphContributor::AddPasses({
			.Graph = Graph, .Services = Services,
			.Record = {PreparedRenderView, PreparedView.Receiver},
			.Depth = GraphResources.SceneDepth, .Options = Options,
			.Width = Width, .Height = Height, .bEnabled = Topology.bGBuffer,
			.bNeedsGBuffer = bNeedsGBuffer,
			.bWantsIsolatedDeferred = bWantsIsolatedDeferred});
		const auto AmbientOcclusionOutput =
			FAmbientOcclusionGraphContributor::AddPasses({
				.Graph = Graph, .Services = Services, .View = PreparedRenderView,
				.Options = Options, .GBuffer = GBufferOutput,
				.Width = Width, .Height = Height,
				.bEnabled = Topology.bGroundTruthAmbientOcclusion,
				.bRequested = bWantsGroundTruthAmbientOcclusion,
				.Quality = Topology.AmbientOcclusionQuality});
		const auto ContactShadowOutput =
			FContactShadowVisibilityGraphContributor::AddPasses({
				.Graph = Graph, .Services = Services,
				.Record = {PreparedRenderView, DirectionalShadow},
				.Options = Options, .DirectionalShadow = DirectionalShadowOutput,
				.GBuffer = GBufferOutput, .Route = PreparedContactRoute,
				.GraphRoute = Topology.ContactShadowVisibility,
				.Width = Width, .Height = Height,
				.bProductionDeferred = bWantsProductionDeferred});
		const auto CloudShadowOutput =
			FVolumetricCloudShadowGraphContributor::AddPasses({
				.Graph = Graph, .Services = Services,
				.Record = {PreparedRenderView, VolumetricCloud, PreparedView.Lighting},
				.GBuffer = GBufferOutput, .SceneDepth = GraphResources.SceneDepth,
				.BaseDensity = GraphResources.VolumetricCloudBaseDensity,
				.DetailDensity = GraphResources.VolumetricCloudDetailDensity,
				.Weather = GraphResources.VolumetricCloudWeather,
				.WeatherTexture = CloudWeatherTexture,
				.Route = PreparedCloudShadowRoute,
				.GraphRoute = Topology.VolumetricCloudShadow,
				.Width = Width, .Height = Height,
				.bProductionDeferred = bWantsProductionDeferred});
		const auto DeferredOutput =
			FDeferredDirectionalLightingGraphContributor::AddPasses({
				.Graph = Graph, .Services = Services, .View = PreparedRenderView,
				.Options = Options, .DirectionalShadow = DirectionalShadowOutput,
				.GBuffer = GBufferOutput, .AmbientOcclusion = AmbientOcclusionOutput,
				.ContactShadow = ContactShadowOutput, .CloudShadow = CloudShadowOutput,
				.DefaultWhite = GraphResources.DefaultWhite,
				.DefaultShadowArray = GraphResources.DefaultShadowArray,
				.EnvironmentIrradiance = GraphResources.EnvironmentIrradiance,
				.EnvironmentPrefiltered = GraphResources.EnvironmentPrefiltered,
				.EnvironmentBrdfLut = GraphResources.EnvironmentBrdfLut,
				.SelectedEnvironmentIrradiance = GraphResources.SelectedEnvironmentIrradiance,
				.SelectedEnvironmentPrefiltered = GraphResources.SelectedEnvironmentPrefiltered,
				.SelectedEnvironmentBrdfLut = GraphResources.SelectedEnvironmentBrdfLut,
				.EnvironmentSampler = SelectedEnvironmentSampler,
				.DeferredParameters = DeferredParameters,
				.ProductionDeferredParameters = ProductionDeferredParameters,
				.Width = Width, .Height = Height,
				.bIsolated = Topology.bIsolatedDeferred,
				.bWantsDeferredInputs = bWantsDeferredInputs,
				.bWantsIsolatedDeferred = bWantsIsolatedDeferred,
				.bWantsProductionDeferred = bWantsProductionDeferred,
				.bHybridRetainedResourcesReady = bHybridRetainedResourcesReady});
		const FSceneGeometryRecordInputs GeometryInputs{
			PreparedRenderView, Environment, PreparedView.Receiver};
		const auto BaseSceneOutput = FBaseSceneGraphContributor::AddPasses({
			.Graph = Graph, .Services = Services, .Record = GeometryInputs,
			.Deferred = DeferredOutput, .SceneColor = GraphResources.SceneColor,
			.SceneDepth = GraphResources.SceneDepth,
			.DirectionalShadow = DirectionalShadowOutput,
			.DefaultWhite = GraphResources.DefaultWhite,
			.DefaultShadowArray = GraphResources.DefaultShadowArray,
			.EnvironmentIrradiance = GraphResources.EnvironmentIrradiance,
			.EnvironmentPrefiltered = GraphResources.EnvironmentPrefiltered,
			.EnvironmentBrdfLut = GraphResources.EnvironmentBrdfLut,
			.SelectedEnvironmentIrradiance = GraphResources.SelectedEnvironmentIrradiance,
			.SelectedEnvironmentPrefiltered = GraphResources.SelectedEnvironmentPrefiltered,
			.SelectedEnvironmentBrdfLut = GraphResources.SelectedEnvironmentBrdfLut,
			.ProductionDeferredParameters = ProductionDeferredParameters,
			.bRequiresDeferredOpaque = bRequiresDeferredOpaque,
			.bNeedsGBuffer = bNeedsGBuffer});
		const FVolumetricCloudRecordInputs CloudInputs{
			PreparedRenderView, VolumetricCloud};
		const auto CloudSpatialOutput =
			FVolumetricCloudSpatialGraphContributor::AddPasses({
				.Graph = Graph, .Services = Services, .Record = CloudInputs,
				.BaseScene = BaseSceneOutput,
				.BaseDensity = GraphResources.VolumetricCloudBaseDensity,
				.DetailDensity = GraphResources.VolumetricCloudDetailDensity,
				.Weather = GraphResources.VolumetricCloudWeather,
				.WeatherTexture = CloudWeatherTexture, .Route = PreparedCloudRoute,
				.GraphRoute = Topology.VolumetricCloud,
				.Extent = Topology.VolumetricCloudExtent,
				.Width = Width, .Height = Height,
				.bComposite = Topology.bVolumetricCloudComposite});
		const auto CloudCompositeOutput =
			FVolumetricCloudCompositeGraphContributor::AddPasses({
				.Graph = Graph, .Services = Services, .Record = CloudInputs,
				.BaseScene = BaseSceneOutput, .Spatial = CloudSpatialOutput,
				.CloudShadow = CloudShadowOutput,
				.BaseDensity = GraphResources.VolumetricCloudBaseDensity,
				.DetailDensity = GraphResources.VolumetricCloudDetailDensity,
				.Weather = GraphResources.VolumetricCloudWeather,
				.WeatherTexture = CloudWeatherTexture,
				.bEnabled = Topology.bVolumetricCloudComposite});
		const auto SceneColorOutput = FSceneColorGraphContributor::AddPasses({
			.Graph = Graph, .Services = Services, .Record = GeometryInputs,
			.BaseScene = BaseSceneOutput, .VolumetricCloud = CloudCompositeOutput,
			.Publication = Composition.SceneColorPublication,
			.bRequiresDeferredOpaque = bRequiresDeferredOpaque,
			.bVolumetricCloudComposite = Topology.bVolumetricCloudComposite});
		const auto PostProcessOutput = FPostProcessGraphContributor::AddPasses({
			.Graph = Graph, .Services = Services, .RecordView = PreparedRenderView,
			.View = View, .Options = Options, .SceneColor = SceneColorOutput,
			.GBuffer = GBufferOutput, .Deferred = DeferredOutput,
			.Output = GraphResources.Output, .OutputTarget = OutputTarget,
			.Publication = Composition.PostProcessPublication,
			.Width = Width, .Height = Height,
			.bGBufferDebug = Topology.bGBufferDebug,
			.bPresentOutput = bPresentOutput,
			.bHasEditorAssistance = bHasEditorAssistance});
		(void)FEditorAssistanceGraphContributor::AddPasses({
			.Graph = Graph, .Services = Services, .View = PreparedRenderView,
			.Prepared = PreparedEditorAssistance, .PostProcess = PostProcessOutput,
			.SceneDepth = GraphResources.SceneDepth, .OutputTarget = OutputTarget,
			.Publication = Composition.PostProcessPublication,
			.bPresentOutput = bPresentOutput, .bEnabled = bHasEditorAssistance});
	}
} // namespace Durin
