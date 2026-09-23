#include "Renderers/SceneRenderer.h"
#include "Renderers/SceneRenderingService.h"
#include "RDG/RDG.h"

#include "Renderers/AmbientOcclusionRendering.h"
#include "Renderers/BaseSceneRendering.h"
#include "Renderers/ContactShadowVisibilityRendering.h"
#include "Renderers/DeferredDirectionalLightingRendering.h"
#include "Renderers/DirectionalShadowRendering.h"
#include "Renderers/EditorAssistanceRendering.h"
#include "Renderers/GBufferRendering.h"
#include "Renderers/PostProcessRendering.h"
#include "Renderers/SceneColorRendering.h"
#include "Renderers/VolumetricCloudRendering.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "Resources/RenderTargetLayouts.h"

namespace Durin
{
	struct FSceneRenderer::FGraphResources
	{
		std::optional<FRDGTextureHandle> DirectionalShadow;
		FRDGTextureHandle SceneColor;
		FRDGTextureHandle SceneDepth;
		FRDGTextureHandle Output;
		std::optional<FRDGTextureHandle> VolumetricCloudBaseDensity;
		std::optional<FRDGTextureHandle> VolumetricCloudDetailDensity;
		std::optional<FRDGTextureHandle> VolumetricCloudWeather;
		std::optional<FRDGTextureHandle> DefaultWhite;
		std::optional<FRDGTextureHandle> DefaultShadowArray;
		FSceneEnvironmentInputs Environment;
	};

	auto FSceneRenderer::Render(FRDGBuilder& Graph) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.ComposeGraph");
		check(!bAuthored && Context.Logical.PreparedView.has_value());
		bAuthored = true;
		const auto& Logical = Context.Logical;
		auto& Resolved = Context.Resolved;
		const auto& PreparedView = *Logical.PreparedView;
		const auto& View = *Logical.CallerView;
		auto* OutputTarget = Logical.OutputTarget;
		const auto& Options = Logical.Options;
		const auto& Features = Context.Features;
		const auto& PreparedEditorAssistance = Logical.EditorAssistance;
		auto* CloudWeatherTexture = Resolved.CloudWeatherTexture;
		const uint32 Width = Logical.Width;
		const uint32 Height = Logical.Height;
		const bool bPresentOutput = Logical.bPresentOutput;
		const bool bHybridRetainedResourcesReady =
			Resolved.bHybridRetainedResourcesReady;
		auto& Composition = Context.Transaction.Composition;
		auto& Telemetry = Context.Observation.Telemetry;
		auto& DeferredParameters = Composition.DeferredParameters;
		auto& ProductionDeferredParameters =
			Composition.ProductionDeferredParameters;
		const auto GraphResources = PrepareGraphResources(Graph);
		OutputTexture = GraphResources.Output;

		const FCloudDensityInputs CloudDensity{
			.BaseDensity = GraphResources.VolumetricCloudBaseDensity,
			.DetailDensity = GraphResources.VolumetricCloudDetailDensity,
			.Weather = GraphResources.VolumetricCloudWeather,
			.WeatherTexture = CloudWeatherTexture};
		const FSceneView& PreparedRenderView = PreparedView.Context.View;
		const FPreparedDirectionalShadow* DirectionalShadow =
			PreparedView.DirectionalShadow ? &*PreparedView.DirectionalShadow : nullptr;
		const FPreparedVolumetricCloud* VolumetricCloud =
			PreparedView.VolumetricCloud ? &*PreparedView.VolumetricCloud : nullptr;
		const FPreparedEnvironment* Environment =
			PreparedView.Environment ? &*PreparedView.Environment : nullptr;
		const auto DirectionalShadowOutput =
			AddDirectionalShadowPasses(Graph, { .ShadowRecord = DirectionalShadow,
				.Shadow = GraphResources.DirectionalShadow,
				.Renderer = Service.DirectionalShadowRenderer,
				.Resolved = Resolved.Scene, .Telemetry = Telemetry});
		const auto GBufferOutput = AddGBufferPasses(Graph, { .View = PreparedRenderView,
			.Receiver = PreparedView.Receiver,
			.Resolved = Resolved.Scene, .Telemetry = Telemetry,
			.Renderer = Service.GBufferRenderer,
			.StaticMeshes = Service.StaticMeshRenderer,
			.Depth = GraphResources.SceneDepth, .Options = Options,
			.Width = Width, .Height = Height, .Feature = Features.GBuffer,
			.DeferredFeature = Features.Deferred});
		const auto AmbientOcclusionOutput =
			AddAmbientOcclusionPasses(Graph, { .View = PreparedRenderView,
				.Options = Options, .GBuffer = GBufferOutput,
				.Allocator = Service.RDGAllocator,
				.Renderer = Service.GroundTruthAmbientOcclusionRenderer,
				.Telemetry = Telemetry,
				.Width = Width, .Height = Height,
				.Feature = Features.AmbientOcclusion});
		const auto ContactShadowOutput =
			AddContactShadowVisibilityPasses(Graph, { .View = PreparedRenderView,
				.Shadow = DirectionalShadow,
				.Resolved = Resolved.Scene, .Telemetry = Telemetry,
				.Allocator = Service.RDGAllocator,
				.Renderer = Service.ContactShadowRenderer,
				.DirectionalShadow = DirectionalShadowOutput,
				.GBuffer = GBufferOutput, .Feature = Features.ContactVisibility,
				.Width = Width, .Height = Height});
		const auto CloudShadowOutput =
			AddVolumetricCloudShadowPasses(Graph, {
				.Record = {PreparedRenderView, VolumetricCloud, PreparedView.Lighting},
				.GBuffer = GBufferOutput,
				.Allocator = Service.RDGAllocator,
				.Renderer = Service.VolumetricCloudShadowRenderer,
				.Resolved = Resolved.Scene, .Telemetry = Telemetry,
				.Qualification = Context.Logical.Qualification,
				.SceneDepth = GraphResources.SceneDepth,
				.Density = CloudDensity,
				.Feature = Features.CloudShadow,
				.Width = Width, .Height = Height});
		const auto DeferredOutput =
			AddDeferredDirectionalLightingPasses(Graph, { .View = PreparedRenderView,
				.Options = Options, .DirectionalShadow = DirectionalShadowOutput,
				.GBuffer = GBufferOutput, .AmbientOcclusion = AmbientOcclusionOutput,
				.ContactShadow = ContactShadowOutput, .CloudShadow = CloudShadowOutput,
				.DefaultTextures = Service.DefaultTextures,
				.DirectionalShadowRenderer = Service.DirectionalShadowRenderer,
				.Renderer = Service.DeferredDirectionalLightingRenderer,
				.Resolved = Resolved.Scene, .Telemetry = Telemetry,
				.DefaultWhite = GraphResources.DefaultWhite,
				.DefaultShadowArray = GraphResources.DefaultShadowArray,
				.Environment = GraphResources.Environment,
				.DeferredParameters = DeferredParameters,
				.ProductionDeferredParameters = ProductionDeferredParameters,
				.Width = Width, .Height = Height,
				.Feature = Features.Deferred,
				.AmbientOcclusionFeature = Features.AmbientOcclusion,
				.bHybridRetainedResourcesReady = bHybridRetainedResourcesReady});
		const FSceneGeometryRecordInputs GeometryInputs{
			PreparedRenderView, Environment, PreparedView.Receiver};
		const auto BaseSceneOutput = AddBaseScenePasses(Graph, { .Record = GeometryInputs,
			.Deferred = DeferredOutput, .SceneColor = GraphResources.SceneColor,
			.SceneDepth = GraphResources.SceneDepth,
			.DirectionalShadow = DirectionalShadowOutput,
			.DefaultTextures = Service.DefaultTextures,
			.DirectionalShadowRenderer = Service.DirectionalShadowRenderer,
			.DeferredRenderer = Service.DeferredDirectionalLightingRenderer,
			.StaticMeshes = Service.StaticMeshRenderer,
			.SkyBox = Service.SkyBoxRenderer,
			.Resolved = Resolved.Scene, .Telemetry = Telemetry,
			.DefaultWhite = GraphResources.DefaultWhite,
			.DefaultShadowArray = GraphResources.DefaultShadowArray,
			.Environment = GraphResources.Environment,
			.ProductionDeferredParameters = ProductionDeferredParameters,
			.DeferredFeature = Features.Deferred,
			.GBufferFeature = Features.GBuffer});
		const FVolumetricCloudRecordInputs CloudInputs{
			PreparedRenderView, VolumetricCloud};
		const auto CloudSpatialOutput =
			AddVolumetricCloudSpatialPasses(Graph, { .Record = CloudInputs,
				.BaseScene = BaseSceneOutput,
				.Allocator = Service.RDGAllocator,
				.Renderer = Service.VolumetricCloudRenderer,
				.Resolved = Resolved.Scene, .Telemetry = Telemetry,
				.Temporal = Context.Transaction.Temporal,
				.ViewState = Context.Transaction.ViewState,
				.Qualification = Context.Logical.Qualification,
				.Density = CloudDensity,
				.Feature = Features.CloudSpatial,
				.Width = Width, .Height = Height});
		const auto CloudCompositeOutput =
			AddVolumetricCloudCompositePasses(Graph, { .Record = CloudInputs,
				.BaseScene = BaseSceneOutput, .Spatial = CloudSpatialOutput,
				.CloudShadow = CloudShadowOutput,
				.Allocator = Service.RDGAllocator,
				.Renderer = Service.VolumetricCloudRenderer,
				.Resolved = Resolved.Scene, .Telemetry = Telemetry,
				.Temporal = Context.Transaction.Temporal,
				.ViewState = Context.Transaction.ViewState,
				.Density = CloudDensity,
				.Feature = Features.CloudSpatial});
		const auto SceneColorOutput = AddSceneColorPasses(Graph, { .Record = GeometryInputs,
			.BaseScene = BaseSceneOutput, .VolumetricCloud = CloudCompositeOutput,
			.StaticMeshes = Service.StaticMeshRenderer,
			.Resolved = Resolved.Scene, .Telemetry = Telemetry,
			.Publication = Composition.SceneColorPublication,
			.DeferredFeature = Features.Deferred,
			.CloudFeature = Features.CloudSpatial});
		const auto PostProcessOutput = AddPostProcessPasses(Graph, { .RecordView = PreparedRenderView,
			.View = View, .Options = Options, .SceneColor = SceneColorOutput,
			.GBuffer = GBufferOutput, .Deferred = DeferredOutput,
			.GBufferDebug = Service.GBufferDebugRenderer,
			.Renderer = Service.PostProcessRenderer, .Telemetry = Telemetry,
			.Output = GraphResources.Output, .OutputTarget = OutputTarget,
			.Publication = Composition.PostProcessPublication,
			.Width = Width, .Height = Height,
			.GBufferDebugFeature = Features.GBufferDebug,
			.EditorAssistanceFeature = Features.EditorAssistance,
			.bPresentOutput = bPresentOutput});
		AddEditorAssistancePasses(Graph, { .View = PreparedRenderView,
			.Prepared = PreparedEditorAssistance, .PostProcess = PostProcessOutput,
			.Renderer = Service.EditorAssistanceRenderer,
			.SceneDepth = GraphResources.SceneDepth, .OutputTarget = OutputTarget,
			.Publication = Composition.PostProcessPublication,
			.Feature = Features.EditorAssistance,
			.bPresentOutput = bPresentOutput});
	}

	auto FSceneRenderer::PrepareGraphResources(FRDGBuilder& Graph) -> FGraphResources
	{
		const auto& Logical = Context.Logical;
		const auto& PreparedView = *Logical.PreparedView;
		const auto& Resolved = Context.Resolved;
		const auto& Features = Context.Features;
		const bool bWantsDeferredInputs = Features.RequiresDeferredInputs();
		const uint32 Width = Logical.Width;
		const uint32 Height = Logical.Height;
		auto* OutputTarget = Logical.OutputTarget;
		const bool bPresentOutput = Logical.bPresentOutput;
		auto* CloudWeatherTexture = Resolved.CloudWeatherTexture;
		FGraphResources GraphResources;
		FRDGBudget SceneRenderBudget{
			.MaxPasses = 256,
			.MaxDependencies = 4096,
			.MaxBufferTransitions = 4096,
			.MaxTextureTransitions = 4096,
			// Three shadow layer passes and their typed-result consumer replace
			// the single shadow pass (+3 passes, +8 edges, +2 barrier calls).
			.RegressionMaxPasses = 15,
			.RegressionMaxDependencies = 36,
			.RegressionMaxTextureTransitions = 34,
			.MaxCompileMicroseconds = 5000,
			.MaxExecuteMicroseconds = 250000,
		};
		if (Features.ContactVisibility.HasPurpose(ESceneFeaturePurpose::Production)
			&& Features.ContactVisibility.Decision.Route
				== FContactShadowVisibilityRenderer::ERoute::Compute)
		{
			// Four GBuffer textures and scene depth switch to compute reads
			// and back to graphics reads; the fragment baseline has neither.
			SceneRenderBudget.RegressionMaxTextureTransitions += 2 * 5;
		}
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
			Service.DirectionalShadowRenderer.GetTexture_RenderThread();
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
			Service.DefaultTextures.Get_RenderThread(EDefaultTexture::White));
		GraphResources.DefaultShadowArray = ImportPersistentTexture(
			"Scene.Default.ShadowArray", Service.DefaultTextures.GetArray_RenderThread());
		if (bWantsDeferredInputs)
		{
			FRHITexture* Irradiance =
				Service.EnvironmentLighting.GetIrradiance_RenderThread();
			FRHITexture* Prefiltered =
				Service.EnvironmentLighting.GetPrefiltered_RenderThread();
			FRHITexture* BrdfLut =
				Service.EnvironmentLighting.GetBrdfLut_RenderThread();
			GraphResources.Environment.Sampler =
				Service.EnvironmentLighting.GetSampler_RenderThread();
			if (Irradiance == nullptr || Prefiltered == nullptr
				|| BrdfLut == nullptr || GraphResources.Environment.Sampler == nullptr)
			{
				Irradiance = Service.DefaultTextures.GetCube_RenderThread();
				Prefiltered = Service.DefaultTextures.GetCube_RenderThread();
				BrdfLut = Service.DefaultTextures.Get_RenderThread(
					EDefaultTexture::Black);
				GraphResources.Environment.Sampler = nullptr;
			}
			GraphResources.Environment.Irradiance = Graph.RegisterExternalTexture(
				FTextureRHIRef(Irradiance), "Scene.Environment.Irradiance",
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			GraphResources.Environment.Prefiltered = Graph.RegisterExternalTexture(
				FTextureRHIRef(Prefiltered), "Scene.Environment.Prefiltered",
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			GraphResources.Environment.BrdfLut = Graph.RegisterExternalTexture(
				FTextureRHIRef(BrdfLut), "Scene.Environment.BrdfLut",
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			GraphResources.Environment.SelectedIrradiance = Irradiance;
			GraphResources.Environment.SelectedPrefiltered = Prefiltered;
			GraphResources.Environment.SelectedBrdfLut = BrdfLut;
		}
		GraphResources.SceneColor = Graph.CreateTexture(
			FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
				"SceneColor", Width, Height, EPixelFormat::RGBA16_FLOAT)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::SourceCopy),
			.ObservationTag = static_cast<uint32>(
				ERDGAllocationObservation::Scene)}, "Scene.Color");
		GraphResources.SceneDepth = Graph.CreateTexture(
			FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
				"SceneDepth", Width, Height, EPixelFormat::D32)
				.SetFlags(ETextureCreateFlags::DepthStencilTargetable
					| ETextureCreateFlags::ShaderResource),
			.ObservationTag = static_cast<uint32>(
				ERDGAllocationObservation::Scene)}, "Scene.Depth");
		GraphResources.Output = Graph.RegisterExternalTexture(
			FTextureRHIRef(OutputTarget), "Scene.Output",
			ERHIAccess::Discard,
			bPresentOutput ? ERHIAccess::Present : ERHIAccess::GraphicsShaderRead);
		return GraphResources;
	}

} // namespace Durin
