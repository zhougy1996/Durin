#include "Renderers/SceneRenderGraphComposer.h"

#include "Renderers/AmbientOcclusionRendering.h"
#include "Renderers/BaseSceneRendering.h"
#include "Renderers/ContactShadowVisibilityRendering.h"
#include "Renderers/DeferredDirectionalLightingRendering.h"
#include "Renderers/DirectionalShadowRendering.h"
#include "Renderers/EditorAssistanceRendering.h"
#include "Renderers/GBufferRendering.h"
#include "Renderers/PostProcessRendering.h"
#include "Renderers/SceneColorRendering.h"
#include "Renderers/SceneRenderPipeline.h"
#include "Renderers/VolumetricCloudRendering.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "Resources/RenderTargetLayouts.h"

namespace Durin
{
	auto FSceneRenderGraphComposer::Compose(
		FRDGBuilder& Graph,
		FSceneRenderer& Renderer,
		FSceneFrameContext& Context) -> void
	{
		const auto& Logical = Context.Logical;
		auto& Resolved = Context.Resolved;
		const auto& PreparedView = *Logical.PreparedView;
		const auto& View = *Logical.CallerView;
		auto* OutputTarget = Logical.OutputTarget;
		const auto& Options = Logical.Options;
		const auto& Features = Context.Features.Plan;
		const auto& PreparedEditorAssistance = Logical.EditorAssistance;
		auto* CloudWeatherTexture = Resolved.CloudWeatherTexture;
		const uint32 Width = Logical.Width;
		const uint32 Height = Logical.Height;
		const bool bPresentOutput = Logical.bPresentOutput;
		const bool bWantsDeferredInputs = Features.RequiresDeferredInputs();
		const bool bHybridRetainedResourcesReady =
			Resolved.bHybridRetainedResourcesReady;
		auto& Composition = Context.Transaction.Composition;
		auto& Telemetry = Context.Observation.Telemetry;
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
			Renderer.DirectionalShadowRenderer.GetTexture_RenderThread();
		if (PreparedView.DirectionalShadow && Resolved.Scene.DirectionalShadow
			&& Resolved.Scene.DirectionalShadow->bEnabled
			&& DirectionalShadowTexture != nullptr)
			GraphResources.DirectionalShadow = ImportPersistentTexture(
				"Scene.DirectionalShadow", DirectionalShadowTexture);
		if (Resolved.Scene.VolumetricCloud)
		{
			GraphResources.VolumetricCloudBaseDensity = ImportPersistentTexture(
				"Scene.VolumetricCloud.BaseDensity",
				Resolved.Scene.VolumetricCloud->Textures.BaseDensity);
			GraphResources.VolumetricCloudDetailDensity = ImportPersistentTexture(
				"Scene.VolumetricCloud.DetailDensity",
				Resolved.Scene.VolumetricCloud->Textures.DetailDensity);
			GraphResources.VolumetricCloudWeather = ImportPersistentTexture(
				"Scene.VolumetricCloud.Weather", CloudWeatherTexture);
		}
		GraphResources.DefaultWhite = ImportPersistentTexture(
			"Scene.Default.White",
			Renderer.DefaultTextures.Get_RenderThread(EDefaultTexture::White));
		GraphResources.DefaultShadowArray = ImportPersistentTexture(
			"Scene.Default.ShadowArray", Renderer.DefaultTextures.GetArray_RenderThread());
		FRHISampler* SelectedEnvironmentSampler = nullptr;
		if (bWantsDeferredInputs)
		{
			FRHITexture* Irradiance =
				Renderer.EnvironmentLighting.GetIrradiance_RenderThread();
			FRHITexture* Prefiltered =
				Renderer.EnvironmentLighting.GetPrefiltered_RenderThread();
			FRHITexture* BrdfLut =
				Renderer.EnvironmentLighting.GetBrdfLut_RenderThread();
			SelectedEnvironmentSampler =
				Renderer.EnvironmentLighting.GetSampler_RenderThread();
			if (Irradiance == nullptr || Prefiltered == nullptr
				|| BrdfLut == nullptr || SelectedEnvironmentSampler == nullptr)
			{
				Irradiance = Renderer.DefaultTextures.GetCube_RenderThread();
				Prefiltered = Renderer.DefaultTextures.GetCube_RenderThread();
				BrdfLut = Renderer.DefaultTextures.Get_RenderThread(
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
				.Graph = Graph, .ShadowRecord = DirectionalShadow,
				.Shadow = GraphResources.DirectionalShadow,
				.Renderer = Renderer.DirectionalShadowRenderer,
				.StaticMeshes = Renderer.StaticMeshRenderer,
				.SkeletalMeshes = Renderer.SkeletalMeshRenderer,
				.Terrains = Renderer.TerrainRenderer,
				.Resolved = Resolved.Scene, .Telemetry = Telemetry});
		const auto GBufferOutput = FGBufferRendering::AddPasses({
			.Graph = Graph, .View = PreparedRenderView,
			.Receiver = PreparedView.Receiver,
			.Resolved = Resolved.Scene, .Telemetry = Telemetry,
			.Renderer = Renderer.GBufferRenderer,
			.StaticMeshes = Renderer.StaticMeshRenderer,
			.SkeletalMeshes = Renderer.SkeletalMeshRenderer,
			.Terrains = Renderer.TerrainRenderer,
			.Depth = GraphResources.SceneDepth, .Options = Options,
			.Width = Width, .Height = Height, .Feature = Features.GBuffer,
			.DeferredFeature = Features.Deferred});
		const auto AmbientOcclusionOutput =
			FAmbientOcclusionRendering::AddPasses({
				.Graph = Graph, .View = PreparedRenderView,
				.Options = Options, .GBuffer = GBufferOutput,
				.Allocator = Renderer.RDGAllocator,
				.Renderer = Renderer.GroundTruthAmbientOcclusionRenderer,
				.Telemetry = Telemetry,
				.Width = Width, .Height = Height,
				.Feature = Features.AmbientOcclusion});
		const auto ContactShadowOutput =
			FContactShadowVisibilityRendering::AddPasses({
				.Graph = Graph, .View = PreparedRenderView,
				.Shadow = DirectionalShadow,
				.Resolved = Resolved.Scene, .Telemetry = Telemetry,
				.Allocator = Renderer.RDGAllocator,
				.Renderer = Renderer.ContactShadowRenderer,
				.DirectionalShadow = DirectionalShadowOutput,
				.GBuffer = GBufferOutput, .Feature = Features.ContactVisibility,
				.Width = Width, .Height = Height});
		const auto CloudShadowOutput =
			FVolumetricCloudShadowRendering::AddPasses({
				.Graph = Graph,
				.Record = {PreparedRenderView, VolumetricCloud, PreparedView.Lighting},
				.GBuffer = GBufferOutput,
				.Allocator = Renderer.RDGAllocator,
				.Renderer = Renderer.VolumetricCloudShadowRenderer,
				.Resolved = Resolved.Scene, .Telemetry = Telemetry,
				.Qualification = Context.Logical.Qualification,
				.SceneDepth = GraphResources.SceneDepth,
				.BaseDensity = GraphResources.VolumetricCloudBaseDensity,
				.DetailDensity = GraphResources.VolumetricCloudDetailDensity,
				.Weather = GraphResources.VolumetricCloudWeather,
				.WeatherTexture = CloudWeatherTexture,
				.Feature = Features.CloudShadow,
				.DeferredFeature = Features.Deferred,
				.Width = Width, .Height = Height});
		const auto DeferredOutput =
			FDeferredDirectionalLightingRendering::AddPasses({
				.Graph = Graph, .View = PreparedRenderView,
				.Options = Options, .DirectionalShadow = DirectionalShadowOutput,
				.GBuffer = GBufferOutput, .AmbientOcclusion = AmbientOcclusionOutput,
				.ContactShadow = ContactShadowOutput, .CloudShadow = CloudShadowOutput,
				.DefaultTextures = Renderer.DefaultTextures,
				.DirectionalShadowRenderer = Renderer.DirectionalShadowRenderer,
				.Renderer = Renderer.DeferredDirectionalLightingRenderer,
				.Resolved = Resolved.Scene, .Telemetry = Telemetry,
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
			.Graph = Graph, .Record = GeometryInputs,
			.Deferred = DeferredOutput, .SceneColor = GraphResources.SceneColor,
			.SceneDepth = GraphResources.SceneDepth,
			.DirectionalShadow = DirectionalShadowOutput,
			.DefaultTextures = Renderer.DefaultTextures,
			.DirectionalShadowRenderer = Renderer.DirectionalShadowRenderer,
			.DeferredRenderer = Renderer.DeferredDirectionalLightingRenderer,
			.StaticMeshes = Renderer.StaticMeshRenderer,
			.SkeletalMeshes = Renderer.SkeletalMeshRenderer,
			.Terrains = Renderer.TerrainRenderer, .SkyBox = Renderer.SkyBoxRenderer,
			.Resolved = Resolved.Scene, .Telemetry = Telemetry,
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
				.Graph = Graph, .Record = CloudInputs,
				.BaseScene = BaseSceneOutput,
				.Allocator = Renderer.RDGAllocator,
				.Renderer = Renderer.VolumetricCloudRenderer,
				.Resolved = Resolved.Scene, .Telemetry = Telemetry,
				.Temporal = Context.Transaction.Temporal,
				.ViewState = Context.Transaction.ViewState,
				.Qualification = Context.Logical.Qualification,
				.BaseDensity = GraphResources.VolumetricCloudBaseDensity,
				.DetailDensity = GraphResources.VolumetricCloudDetailDensity,
				.Weather = GraphResources.VolumetricCloudWeather,
				.WeatherTexture = CloudWeatherTexture,
				.Feature = Features.CloudSpatial,
				.Width = Width, .Height = Height});
		const auto CloudCompositeOutput =
			FVolumetricCloudCompositeRendering::AddPasses({
				.Graph = Graph, .Record = CloudInputs,
				.BaseScene = BaseSceneOutput, .Spatial = CloudSpatialOutput,
				.CloudShadow = CloudShadowOutput,
				.Allocator = Renderer.RDGAllocator,
				.Renderer = Renderer.VolumetricCloudRenderer,
				.Resolved = Resolved.Scene, .Telemetry = Telemetry,
				.Temporal = Context.Transaction.Temporal,
				.ViewState = Context.Transaction.ViewState,
				.BaseDensity = GraphResources.VolumetricCloudBaseDensity,
				.DetailDensity = GraphResources.VolumetricCloudDetailDensity,
				.Weather = GraphResources.VolumetricCloudWeather,
				.WeatherTexture = CloudWeatherTexture,
				.Feature = Features.CloudSpatial});
		const auto SceneColorOutput = FSceneColorRendering::AddPasses({
			.Graph = Graph, .Record = GeometryInputs,
			.BaseScene = BaseSceneOutput, .VolumetricCloud = CloudCompositeOutput,
			.StaticMeshes = Renderer.StaticMeshRenderer,
			.SkeletalMeshes = Renderer.SkeletalMeshRenderer,
			.Terrains = Renderer.TerrainRenderer,
			.Resolved = Resolved.Scene, .Telemetry = Telemetry,
			.Publication = Composition.SceneColorPublication,
			.DeferredFeature = Features.Deferred,
			.CloudFeature = Features.CloudSpatial});
		const auto PostProcessOutput = FPostProcessRendering::AddPasses({
			.Graph = Graph, .RecordView = PreparedRenderView,
			.View = View, .Options = Options, .SceneColor = SceneColorOutput,
			.GBuffer = GBufferOutput, .Deferred = DeferredOutput,
			.GBufferDebug = Renderer.GBufferDebugRenderer,
			.Renderer = Renderer.PostProcessRenderer, .Telemetry = Telemetry,
			.Output = GraphResources.Output, .OutputTarget = OutputTarget,
			.Publication = Composition.PostProcessPublication,
			.Width = Width, .Height = Height,
			.GBufferDebugFeature = Features.GBufferDebug,
			.EditorAssistanceFeature = Features.EditorAssistance,
			.bPresentOutput = bPresentOutput});
		FEditorAssistanceRendering::AddPasses({
			.Graph = Graph, .View = PreparedRenderView,
			.Prepared = PreparedEditorAssistance, .PostProcess = PostProcessOutput,
			.Renderer = Renderer.EditorAssistanceRenderer,
			.SceneDepth = GraphResources.SceneDepth, .OutputTarget = OutputTarget,
			.Publication = Composition.PostProcessPublication,
			.Feature = Features.EditorAssistance,
			.bPresentOutput = bPresentOutput});
	}
} // namespace Durin
