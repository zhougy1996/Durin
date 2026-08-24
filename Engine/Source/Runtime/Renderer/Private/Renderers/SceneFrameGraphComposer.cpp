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
		auto& Requirements = Inputs.Topology;
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
		std::vector<std::pair<FRHITexture*, FRenderGraphTextureHandle>>
			PersistentTextureImports;
		auto ImportPersistentTexture = [&](std::string_view Name,
			FRHITexture* Texture) -> std::optional<FRenderGraphTextureHandle> {
			if (!Texture) return std::nullopt;
			const auto Existing = std::ranges::find(PersistentTextureImports,
				Texture,
				&std::pair<FRHITexture*, FRenderGraphTextureHandle>::first);
			if (Existing != PersistentTextureImports.end()) return Existing->second;
			const auto Handle = Graph.ImportTexture(Name, Texture,
				ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
			PersistentTextureImports.emplace_back(Texture, Handle);
			return Handle;
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
		GraphResources.EnvironmentIrradiance = ImportPersistentTexture(
			"Scene.Environment.Irradiance",
			Services.EnvironmentLighting.GetIrradiance_RenderThread());
		GraphResources.EnvironmentPrefiltered = ImportPersistentTexture(
			"Scene.Environment.Prefiltered",
			Services.EnvironmentLighting.GetPrefiltered_RenderThread());
		GraphResources.EnvironmentBrdfLut = ImportPersistentTexture(
			"Scene.Environment.BrdfLut",
			Services.EnvironmentLighting.GetBrdfLut_RenderThread());
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
		Graph.SetBackingResolver([&Services, &Requirements, &TargetResolutionResult,
			&GraphResources](auto Requests, auto& Backings,
			std::string& Error) {
			const auto Retained =
				FSceneFrameGraphBackingProvider::BuildRetainedTopology(
					Requests, Requirements, Error);
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
			.DirectionalShadow = {Graph.CreateToken("Scene.DirectionalShadowValue")},
			.GBuffer = {Graph.CreateToken("Scene.GBufferValue")},
			.AmbientOcclusion = {Graph.CreateToken("Scene.AmbientOcclusionValue")},
			.ContactShadow = {Graph.CreateToken("Scene.ContactShadowValue")},
			.CloudShadow = {Graph.CreateToken("Scene.CloudShadowValue")},
			.Deferred = {Graph.CreateToken("Scene.DeferredValue")},
			.OpaqueScene = {Graph.CreateToken("Scene.OpaqueValue")},
			.VolumetricCloudSpatial = {
				Graph.CreateToken("Scene.VolumetricCloudSpatialValue")},
			.VolumetricCloud = {Graph.CreateToken("Scene.VolumetricCloudValue")},
			.SceneColor = {Graph.CreateToken("Scene.ColorValue")},
			.PostProcess = {Graph.CreateToken("Scene.PostProcessValue")},
			.FinalOutput = {Graph.CreateToken("Scene.FinalOutputValue")}};
		FSceneFrameGraphContributorContext Context{
			.Graph = Graph,
			.Services = Services,
			.Composition = Composition,
			.View = View,
			.OutputTarget = OutputTarget,
			.Options = Options,
			.Topology = Requirements,
			.EditorAssistance = PreparedEditorAssistance,
			.ContactRoute = PreparedContactRoute,
			.CloudShadowRoute = PreparedCloudShadowRoute,
			.CloudRoute = PreparedCloudRoute,
			.CloudWeatherTexture = CloudWeatherTexture,
			.DirectionalShadowTexture = DirectionalShadowTexture,
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
		FContactVisibilityGraphContributor::AddPasses(Context,
			{PreparedRenderView, DirectionalShadow});
		FVolumetricCloudShadowGraphContributor::AddPasses(Context,
			{PreparedRenderView, VolumetricCloud, PreparedView.Lighting});
		FDeferredLightingGraphContributor::AddPasses(Context, PreparedRenderView);
		const FSceneGeometryRecordInputs GeometryInputs{
			PreparedRenderView, Environment, PreparedView.Receiver};
		FOpaqueSceneGraphContributor::AddPasses(Context, GeometryInputs);
		const FVolumetricCloudRecordInputs CloudInputs{
			PreparedRenderView, VolumetricCloud};
		FVolumetricCloudSpatialGraphContributor::AddPasses(Context, CloudInputs);
		FVolumetricCloudCompositeGraphContributor::AddPasses(Context, CloudInputs);
		FSortedTranslucencyGraphContributor::AddPasses(Context, GeometryInputs);
		FPostProcessGraphContributor::AddPasses(Context, PreparedRenderView);
		FEditorAssistanceGraphContributor::AddPasses(Context, PreparedRenderView);
	}
} // namespace Durin
