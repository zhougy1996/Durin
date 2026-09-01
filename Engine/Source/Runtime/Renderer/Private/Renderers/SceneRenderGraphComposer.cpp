#include "Renderers/SceneRenderGraphComposer.h"

#include "Renderers/SceneRenderGraphContributors.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "Resources/RenderTargetLayouts.h"

namespace Durin
{
	auto FSceneRenderGraphComposer::Compose(
		FRDGBuilder& Graph,
		const FSceneRenderGraphComposeInputs& Inputs,
		FSceneRenderGraphComposition& Composition) -> void
	{
		auto& Services = Inputs.Services;
		const auto& PreparedView = Inputs.PreparedView;
		const auto& View = Inputs.View;
		auto* OutputTarget = Inputs.OutputTarget;
		const auto& Options = Inputs.Options;
		const auto& Features = Inputs.Features;
		const auto& PreparedEditorAssistance = Inputs.EditorAssistance;
		auto* CloudWeatherTexture = Inputs.CloudWeatherTexture;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		const bool bPresentOutput = Inputs.bPresentOutput;
		const bool bWantsDeferredInputs = Features.RequiresDeferredInputs();
		const bool bHybridRetainedResourcesReady =
			Inputs.bHybridRetainedResourcesReady;
		auto& DeferredParameters = Composition.DeferredParameters;
		auto& ProductionDeferredParameters =
			Composition.ProductionDeferredParameters;
		struct {
			std::optional<FRDGTextureHandle> DirectionalShadow;
			FRDGTextureHandle SceneColor;
			FRDGTextureHandle SceneDepth;
			FRDGTextureHandle Output;
			std::optional<FRDGTextureHandle> VolumetricCloudBaseDensity;
			std::optional<FRDGTextureHandle> VolumetricCloudDetailDensity;
			std::optional<FRDGTextureHandle> VolumetricCloudWeather;
			std::optional<FRDGTextureHandle> DefaultWhite;
			std::optional<FRDGTextureHandle> DefaultShadowArray;
			std::optional<FRDGTextureHandle> EnvironmentIrradiance;
			std::optional<FRDGTextureHandle> EnvironmentPrefiltered;
			std::optional<FRDGTextureHandle> EnvironmentBrdfLut;
			FRHITexture* SelectedEnvironmentIrradiance = nullptr;
			FRHITexture* SelectedEnvironmentPrefiltered = nullptr;
			FRHITexture* SelectedEnvironmentBrdfLut = nullptr;
		} GraphResources;
		constexpr FRDGBudget SceneRenderBudget{
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
		Graph.SetBudget(SceneRenderBudget);
		Graph.EnablePassCulling();
		auto ImportPersistentTexture = [&](std::string_view Name,
			FRHITexture* Texture) -> std::optional<FRDGTextureHandle> {
			if (!Texture) return std::nullopt;
			return Graph.RegisterExternalTexture(FTextureRHIRef(Texture), Name,
				ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
		};
		FRHITexture* DirectionalShadowTexture =
			Services.DirectionalShadowRenderer.GetTexture_RenderThread();
		if (PreparedView.DirectionalShadow && Services.ResolvedSceneResources.DirectionalShadow
			&& Services.ResolvedSceneResources.DirectionalShadow->bEnabled
			&& DirectionalShadowTexture != nullptr)
			GraphResources.DirectionalShadow = ImportPersistentTexture(
				"Scene.DirectionalShadow", DirectionalShadowTexture);
		if (Services.ResolvedSceneResources.VolumetricCloud)
		{
			GraphResources.VolumetricCloudBaseDensity = ImportPersistentTexture(
				"Scene.VolumetricCloud.BaseDensity",
				Services.ResolvedSceneResources.VolumetricCloud->Textures.BaseDensity);
			GraphResources.VolumetricCloudDetailDensity = ImportPersistentTexture(
				"Scene.VolumetricCloud.DetailDensity",
				Services.ResolvedSceneResources.VolumetricCloud->Textures.DetailDensity);
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
			FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
				"SceneColor", Width, Height, EPixelFormat::RGBA16_FLOAT)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::SourceCopy),
			.ObservationTag = static_cast<uint32>(
				ERDGAllocationObservation::Scene)}, "Scene.Color",
			ERHIAccess::GraphicsShaderRead);
		GraphResources.SceneDepth = Graph.CreateTexture(
			FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
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
			FDirectionalShadowRendering::AddPasses({
				.Graph = Graph, .Services = Services,
				.Record = {DirectionalShadow}, .Shadow = GraphResources.DirectionalShadow});
		const auto GBufferOutput = FGBufferRendering::AddPasses({
			.Graph = Graph, .View = PreparedRenderView,
			.Receiver = PreparedView.Receiver,
			.Resolved = Services.ResolvedSceneResources,
			.Telemetry = Services.Telemetry,
			.Renderer = Services.GBufferRenderer,
			.StaticMeshes = Services.StaticMeshRenderer,
			.SkeletalMeshes = Services.SkeletalMeshRenderer,
			.Terrains = Services.TerrainRenderer,
			.Depth = GraphResources.SceneDepth, .Options = Options,
			.Width = Width, .Height = Height, .Feature = Features.GBuffer,
			.DeferredFeature = Features.Deferred});
		const auto AmbientOcclusionOutput =
			FAmbientOcclusionRendering::AddPasses({
				.Graph = Graph, .Services = Services, .View = PreparedRenderView,
				.Options = Options, .GBuffer = GBufferOutput,
				.Width = Width, .Height = Height,
				.Feature = Features.AmbientOcclusion});
		const auto ContactShadowOutput =
			FContactShadowVisibilityRendering::AddPasses({
				.Graph = Graph, .View = PreparedRenderView,
				.Shadow = DirectionalShadow,
				.Resolved = Services.ResolvedSceneResources,
				.Telemetry = Services.Telemetry,
				.Allocator = Services.RDGAllocator,
				.Renderer = Services.ContactShadowRenderer,
				.DirectionalShadow = DirectionalShadowOutput,
				.GBuffer = GBufferOutput, .Feature = Features.ContactVisibility,
				.Width = Width, .Height = Height});
		const auto CloudShadowOutput =
			FVolumetricCloudShadowRendering::AddPasses({
				.Graph = Graph, .Services = Services,
				.Record = {PreparedRenderView, VolumetricCloud, PreparedView.Lighting},
				.GBuffer = GBufferOutput, .SceneDepth = GraphResources.SceneDepth,
				.BaseDensity = GraphResources.VolumetricCloudBaseDensity,
				.DetailDensity = GraphResources.VolumetricCloudDetailDensity,
				.Weather = GraphResources.VolumetricCloudWeather,
				.WeatherTexture = CloudWeatherTexture,
				.Feature = Features.CloudShadow,
				.DeferredFeature = Features.Deferred,
				.Width = Width, .Height = Height});
		const auto DeferredOutput =
			FDeferredDirectionalLightingRendering::AddPasses({
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
				.Feature = Features.Deferred,
				.AmbientOcclusionFeature = Features.AmbientOcclusion,
				.bHybridRetainedResourcesReady = bHybridRetainedResourcesReady});
		const FSceneGeometryRecordInputs GeometryInputs{
			PreparedRenderView, Environment, PreparedView.Receiver};
		const auto BaseSceneOutput = FBaseSceneRendering::AddPasses({
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
			.DeferredFeature = Features.Deferred,
			.GBufferFeature = Features.GBuffer});
		const FVolumetricCloudRecordInputs CloudInputs{
			PreparedRenderView, VolumetricCloud};
		const auto CloudSpatialOutput =
			FVolumetricCloudSpatialRendering::AddPasses({
				.Graph = Graph, .Services = Services, .Record = CloudInputs,
				.BaseScene = BaseSceneOutput,
				.BaseDensity = GraphResources.VolumetricCloudBaseDensity,
				.DetailDensity = GraphResources.VolumetricCloudDetailDensity,
				.Weather = GraphResources.VolumetricCloudWeather,
				.WeatherTexture = CloudWeatherTexture,
				.Feature = Features.CloudSpatial,
				.Width = Width, .Height = Height});
		const auto CloudCompositeOutput =
			FVolumetricCloudCompositeRendering::AddPasses({
				.Graph = Graph, .Services = Services, .Record = CloudInputs,
				.BaseScene = BaseSceneOutput, .Spatial = CloudSpatialOutput,
				.CloudShadow = CloudShadowOutput,
				.BaseDensity = GraphResources.VolumetricCloudBaseDensity,
				.DetailDensity = GraphResources.VolumetricCloudDetailDensity,
				.Weather = GraphResources.VolumetricCloudWeather,
				.WeatherTexture = CloudWeatherTexture,
				.Feature = Features.CloudSpatial});
		const auto SceneColorOutput = FSceneColorRendering::AddPasses({
			.Graph = Graph, .Services = Services, .Record = GeometryInputs,
			.BaseScene = BaseSceneOutput, .VolumetricCloud = CloudCompositeOutput,
			.Publication = Composition.SceneColorPublication,
			.DeferredFeature = Features.Deferred,
			.CloudFeature = Features.CloudSpatial});
		const auto PostProcessOutput = FPostProcessRendering::AddPasses({
			.Graph = Graph, .Services = Services, .RecordView = PreparedRenderView,
			.View = View, .Options = Options, .SceneColor = SceneColorOutput,
			.GBuffer = GBufferOutput, .Deferred = DeferredOutput,
			.Output = GraphResources.Output, .OutputTarget = OutputTarget,
			.Publication = Composition.PostProcessPublication,
			.Width = Width, .Height = Height,
			.GBufferDebugFeature = Features.GBufferDebug,
			.EditorAssistanceFeature = Features.EditorAssistance,
			.bPresentOutput = bPresentOutput});
		FEditorAssistanceRendering::AddPasses({
			.Graph = Graph, .Services = Services, .View = PreparedRenderView,
			.Prepared = PreparedEditorAssistance, .PostProcess = PostProcessOutput,
			.SceneDepth = GraphResources.SceneDepth, .OutputTarget = OutputTarget,
			.Publication = Composition.PostProcessPublication,
			.Feature = Features.EditorAssistance,
			.bPresentOutput = bPresentOutput});
	}
} // namespace Durin
