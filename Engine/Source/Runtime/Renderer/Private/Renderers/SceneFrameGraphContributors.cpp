#include "Renderers/SceneFrameGraphContributors.h"

#include "Renderers/SceneFrameFeatureRecorders.h"
#include "Renderers/SceneFrameGraphComposer.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"

namespace Durin
{
	auto FDirectionalShadowGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FDirectionalShadowRecordInputs& RecordInputs) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Requirements = Context.Topology;
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
		auto& ContactShadowValue = Channels.ContactShadow;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredValue = Channels.Deferred;
		auto& OpaqueSceneValue = Channels.OpaqueScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto& FinalOutputValue = Channels.FinalOutput;
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
		const auto DirectionalShadowPass =
			AddSceneFrameFeaturePass<FDirectionalShadowGraphContributor>(
				Graph, ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordInputs, &GraphResources](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				Channels.DirectionalShadow.Result =
					Services.Recorders.RenderDirectionalShadow_RenderThread(Commands,
						RecordInputs,
						GraphResources.DirectionalShadow
							? Resources.GetTexture(*GraphResources.DirectionalShadow)
							: nullptr);
			});
		Graph.UseToken(DirectionalShadowPass, DirectionalShadowValue.Handle,
			ERenderGraphUse::Write);
		if (GraphResources.DirectionalShadow)
			Graph.UseManagedDepthStencilAttachment(DirectionalShadowPass,
				*GraphResources.DirectionalShadow,
				{ERHITextureAspect::Depth, 0, 1, 0,
					DirectionalShadowCascadeCount},
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead);
	}

	auto FGBufferGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FGBufferRecordInputs& RecordInputs) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Requirements = Context.Topology;
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
		auto& ContactShadowValue = Channels.ContactShadow;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredValue = Channels.Deferred;
		auto& OpaqueSceneValue = Channels.OpaqueScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto& FinalOutputValue = Channels.FinalOutput;
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
		if (Requirements.bGBuffer)
		{
			const std::array Formats{EPixelFormat::RGBA8_UNORM,
				EPixelFormat::RGBA8_UNORM, EPixelFormat::RGBA8_UNORM,
				EPixelFormat::R11G11B10_FLOAT};
			const std::array Names{"Scene.GBuffer.Material", "Scene.GBuffer.Normals",
				"Scene.GBuffer.Surface", "Scene.GBuffer.Emissive"};
			for (uint32 Index = 0; Index < GraphResources.GBuffer.size(); ++Index)
				GraphResources.GBuffer[Index] = Graph.CreateTexture(Names[Index],
					FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
						Names[Index], Width, Height, Formats[Index])
						.SetFlags(ETextureCreateFlags::RenderTargetable
							| ETextureCreateFlags::ShaderResource
							| ETextureCreateFlags::SourceCopy),
						.BackingClass = std::string(GetSceneFrameBackingClassName(
							ESceneFrameBackingClass::GBuffer))},
					ERHIAccess::GraphicsShaderRead);
		}
		const auto GBufferPass = AddSceneFrameFeaturePass<FGBufferGraphContributor>(
			Graph, ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordInputs, &GraphResources, &Options,
				Width, Height, bNeedsGBuffer, bWantsIsolatedDeferred](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = GraphResources.GBuffer[0]
						? Resources.GetTexture(GraphResources.SceneDepth) : nullptr};
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0])
					GBufferTargets = {.Material = Resources.GetTexture(*GraphResources.GBuffer[0]),
						.Normals = Resources.GetTexture(*GraphResources.GBuffer[1]),
						.Surface = Resources.GetTexture(*GraphResources.GBuffer[2]),
						.Emissive = Resources.GetTexture(*GraphResources.GBuffer[3])};
				Channels.GBuffer.Result = Services.Recorders.RenderGBuffer_RenderThread(
					Commands, RecordInputs, SceneTargets,
					GBufferTargets ? &*GBufferTargets : nullptr,
					Options, Width, Height,
					bNeedsGBuffer, bWantsIsolatedDeferred);
			});
		Graph.UseToken(GBufferPass, GBufferValue.Handle, ERenderGraphUse::Write);
		if (GraphResources.GBuffer[0])
		{
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseManagedColorAttachment(GBufferPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1},
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store,
					ERHIAccess::GraphicsShaderRead);
			Graph.UseManagedDepthStencilAttachment(GBufferPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead);
		}
	}

	auto FAmbientOcclusionGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FSceneView& RecordView) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Requirements = Context.Topology;
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
		auto& ContactShadowValue = Channels.ContactShadow;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredValue = Channels.Deferred;
		auto& OpaqueSceneValue = Channels.OpaqueScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto& FinalOutputValue = Channels.FinalOutput;
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
		if (Requirements.bGroundTruthAmbientOcclusion)
		{
			const bool bHalfResolution = Requirements.AmbientOcclusionQuality
				== EGroundTruthAmbientOcclusionQuality::HalfResolution;
			const uint32 NativeWidth = bHalfResolution
				? FGroundTruthAmbientOcclusionRenderer::CalculateHalfExtent(Width)
				: Width;
			const uint32 NativeHeight = bHalfResolution
				? FGroundTruthAmbientOcclusionRenderer::CalculateHalfExtent(Height)
				: Height;
			const std::array Names{"Scene.AmbientOcclusion.Raw",
				"Scene.AmbientOcclusion.Scratch",
				"Scene.AmbientOcclusion.Selector",
				"Scene.AmbientOcclusion.Resolved"};
			for (uint32 Index = 0; Index < 2; ++Index)
				GraphResources.GroundTruthAmbientOcclusion[Index] =
					Graph.CreateTexture(Names[Index],
						FRenderGraphTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[Index],
								NativeWidth, NativeHeight, EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy)
							.SetClearValue(FClearValueBinding(
								1.0f, 1.0f, 1.0f, 1.0f)),
							.BackingClass = std::string(GetSceneFrameBackingClassName(
								ESceneFrameBackingClass::AmbientOcclusion))},
						ERHIAccess::GraphicsShaderRead);
			if (bHalfResolution)
			{
				GraphResources.GroundTruthAmbientOcclusion[2] =
					Graph.CreateTexture(Names[2],
						FRenderGraphTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[2],
								NativeWidth, NativeHeight, EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy)
							.SetClearValue(FClearValueBinding(
								0.0f, 0.0f, 0.0f, 0.0f)),
							.BackingClass = std::string(GetSceneFrameBackingClassName(
								ESceneFrameBackingClass::AmbientOcclusion))},
						ERHIAccess::GraphicsShaderRead);
				GraphResources.GroundTruthAmbientOcclusion[3] =
					Graph.CreateTexture(Names[3],
						FRenderGraphTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[3], Width,
								Height, EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy)
							.SetClearValue(FClearValueBinding(
								1.0f, 1.0f, 1.0f, 1.0f)),
							.BackingClass = std::string(GetSceneFrameBackingClassName(
								ESceneFrameBackingClass::AmbientOcclusion))},
						ERHIAccess::GraphicsShaderRead);
			}
		}
		const auto AmbientOcclusionPass =
			AddSceneFrameFeaturePass<FAmbientOcclusionGraphContributor>(
				Graph, ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordView = &RecordView, &GraphResources, &Requirements,
				&Options, Width, Height, bWantsGroundTruthAmbientOcclusion](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0]
					&& Requirements.bGroundTruthAmbientOcclusion)
					GBufferTargets = {
						.Material = Resources.GetTexture(*GraphResources.GBuffer[0]),
						.Normals = Resources.GetTexture(*GraphResources.GBuffer[1]),
						.Surface = Resources.GetTexture(*GraphResources.GBuffer[2]),
						.Emissive = Resources.GetTexture(*GraphResources.GBuffer[3])};
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = GBufferTargets
						? Resources.GetTexture(GraphResources.SceneDepth) : nullptr};
				std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
					AmbientOcclusionTargets;
				if (GraphResources.GroundTruthAmbientOcclusion[0])
					AmbientOcclusionTargets = {
						.Raw = Resources.GetTexture(
							*GraphResources.GroundTruthAmbientOcclusion[0]),
						.Scratch = Resources.GetTexture(
							*GraphResources.GroundTruthAmbientOcclusion[1]),
						.Selector = GraphResources.GroundTruthAmbientOcclusion[2]
							? Resources.GetTexture(
								*GraphResources.GroundTruthAmbientOcclusion[2])
							: nullptr,
						.Resolved = GraphResources.GroundTruthAmbientOcclusion[3]
							? Resources.GetTexture(
								*GraphResources.GroundTruthAmbientOcclusion[3])
							: nullptr,
						.Quality = Requirements.AmbientOcclusionQuality};
				Channels.AmbientOcclusion.Result =
					Services.Recorders.RenderGroundTruthAmbientOcclusion_RenderThread(
						Commands, *RecordView,
						GBufferTargets ? &*GBufferTargets : nullptr,
						AmbientOcclusionTargets ? &*AmbientOcclusionTargets : nullptr,
						SceneTargets, Options, Width, Height,
						bWantsGroundTruthAmbientOcclusion,
						Channels.GBuffer.Result.IsComplete());
			});
		Graph.UseToken(AmbientOcclusionPass, GBufferValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(AmbientOcclusionPass, AmbientOcclusionValue.Handle,
			ERenderGraphUse::Write);
		if (GraphResources.GBuffer[0] && Requirements.bGroundTruthAmbientOcclusion)
		{
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseTexture(AmbientOcclusionPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
					ERHIAccess::GraphicsShaderRead);
			Graph.UseTexture(AmbientOcclusionPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
			for (const auto& Texture :
				GraphResources.GroundTruthAmbientOcclusion)
			{
				if (!Texture) continue;
				Graph.UseManagedTexture(AmbientOcclusionPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1},
					ERenderGraphUse::ReadWrite,
					ERHIAccess::GraphicsShaderRead,
					ERHIAccess::GraphicsShaderRead, true);
			}
		}
	}

	auto FContactVisibilityGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FContactVisibilityRecordInputs& RecordInputs) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Requirements = Context.Topology;
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
		auto& ContactShadowValue = Channels.ContactShadow;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredValue = Channels.Deferred;
		auto& OpaqueSceneValue = Channels.OpaqueScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto& FinalOutputValue = Channels.FinalOutput;
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
		if (Requirements.UsesContactFragment())
			GraphResources.ContactFragment = Graph.CreateTexture(
				"Scene.ContactVisibility.Fragment",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DirectionalContactVisibility", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy)
					.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f)),
					.BackingClass = std::string(GetSceneFrameBackingClassName(
						ESceneFrameBackingClass::ContactVisibilityFragment))},
				ERHIAccess::GraphicsShaderRead);
		if (Requirements.UsesContactCompute())
			GraphResources.ContactCompute = Graph.CreateTexture(
				"Scene.ContactVisibility.Compute",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DirectionalContactVisibilityCompute", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::Storage
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.BackingClass = std::string(GetSceneFrameBackingClassName(
						ESceneFrameBackingClass::ContactVisibilityCompute))},
				ERHIAccess::GraphicsShaderRead);
		const auto ContactShadowPass =
			AddSceneFrameFeaturePass<FContactVisibilityGraphContributor>(Graph,
			PreparedContactRoute.Route
					== FContactShadowVisibilityRenderer::ERoute::Compute
				? ERenderGraphPassType::Compute : ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordInputs, &GraphResources, &Requirements,
				&Options, Width, Height, bWantsProductionDeferred](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0]
					&& Requirements.ContactVisibility != ESceneFrameRoute::Disabled)
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
				if (GraphResources.ContactFragment)
					FragmentContactTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.ContactFragment)};
				std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
					ComputeContactTargets;
				if (GraphResources.ContactCompute)
					ComputeContactTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.ContactCompute)};
				Channels.ContactShadow.Result = Services.Recorders.RenderContactShadows_RenderThread(
					Commands,
					RecordInputs,
					GBufferTargets ? &*GBufferTargets : nullptr,
					FragmentContactTargets ? &*FragmentContactTargets : nullptr,
					ComputeContactTargets ? &*ComputeContactTargets : nullptr,
					SceneTargets, Options, Width, Height,
					bWantsProductionDeferred, Channels.GBuffer.Result.IsComplete(),
					Channels.GBuffer.Result.bRenderedGeometry);
			});
		Graph.UseToken(ContactShadowPass, DirectionalShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(ContactShadowPass, GBufferValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(ContactShadowPass, ContactShadowValue.Handle,
			ERenderGraphUse::Write);
		if (GraphResources.GBuffer[0])
		{
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseTexture(ContactShadowPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
					PreparedContactRoute.Route
							== FContactShadowVisibilityRenderer::ERoute::Compute
						? ERHIAccess::ComputeShaderRead
						: ERHIAccess::GraphicsShaderRead);
			Graph.UseTexture(ContactShadowPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				PreparedContactRoute.Route
						== FContactShadowVisibilityRenderer::ERoute::Compute
					? ERHIAccess::ComputeShaderRead
					: ERHIAccess::GraphicsShaderRead);
		}
		if (GraphResources.ContactFragment)
			Graph.UseColorAttachment(ContactShadowPass,
				*GraphResources.ContactFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store);
		if (GraphResources.ContactCompute)
			Graph.UseTexture(ContactShadowPass,
				*GraphResources.ContactCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
	}

	auto FVolumetricCloudShadowGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FVolumetricCloudShadowRecordInputs& RecordInputs) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Requirements = Context.Topology;
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
		auto& ContactShadowValue = Channels.ContactShadow;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredValue = Channels.Deferred;
		auto& OpaqueSceneValue = Channels.OpaqueScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto& FinalOutputValue = Channels.FinalOutput;
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
		if (Requirements.UsesCloudShadowFragment())
			GraphResources.VolumetricCloudShadowFragment = Graph.CreateTexture(
				"Scene.VolumetricCloudShadow.Fragment",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudVisibility", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback)
					.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f)),
					.BackingClass = std::string(GetSceneFrameBackingClassName(
						ESceneFrameBackingClass::VolumetricCloudShadowFragment))},
				ERHIAccess::GraphicsShaderRead);
		if (Requirements.UsesCloudShadowCompute())
			GraphResources.VolumetricCloudShadowCompute = Graph.CreateTexture(
				"Scene.VolumetricCloudShadow.Compute",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudVisibilityCompute", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::Storage
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback),
					.BackingClass = std::string(GetSceneFrameBackingClassName(
						ESceneFrameBackingClass::VolumetricCloudShadowCompute))},
				ERHIAccess::GraphicsShaderRead);
		const auto CloudShadowPass =
			AddSceneFrameFeaturePass<FVolumetricCloudShadowGraphContributor>(Graph,
			PreparedCloudShadowRoute
					== FVolumetricCloudShadowRenderer::ERoute::Compute
				? ERenderGraphPassType::Compute : ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordInputs, &GraphResources, &Requirements,
				Width, Height, bWantsProductionDeferred](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FVolumetricCloudShadowRenderer::FTargets>
					FragmentTargets;
				if (GraphResources.VolumetricCloudShadowFragment)
					FragmentTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowFragment)};
				std::optional<FVolumetricCloudShadowRenderer::FComputeTargets>
					ComputeTargets;
				if (GraphResources.VolumetricCloudShadowCompute)
					ComputeTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowCompute)};
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = Requirements.VolumetricCloudShadow
							!= ESceneFrameRoute::Disabled
						? Resources.GetTexture(GraphResources.SceneDepth) : nullptr};
				Channels.CloudShadow.Result =
					Services.Recorders.RenderVolumetricCloudShadows_RenderThread(
						Commands,
						RecordInputs,
						FragmentTargets ? &*FragmentTargets : nullptr,
						ComputeTargets ? &*ComputeTargets : nullptr,
						SceneTargets,
						GraphResources.VolumetricCloudBaseDensity
							? Resources.GetTexture(
								*GraphResources.VolumetricCloudBaseDensity) : nullptr,
						GraphResources.VolumetricCloudDetailDensity
							? Resources.GetTexture(
								*GraphResources.VolumetricCloudDetailDensity) : nullptr,
						GraphResources.VolumetricCloudWeather
							? Resources.GetTexture(
								*GraphResources.VolumetricCloudWeather) : nullptr,
						Width, Height, bWantsProductionDeferred,
						Channels.GBuffer.Result.IsComplete());
			});
		Graph.UseToken(CloudShadowPass, GBufferValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(CloudShadowPass, CloudShadowValue.Handle,
			ERenderGraphUse::Write);
		if (Requirements.VolumetricCloudShadow != ESceneFrameRoute::Disabled)
			Graph.UseTexture(CloudShadowPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				PreparedCloudShadowRoute
						== FVolumetricCloudShadowRenderer::ERoute::Compute
					? ERHIAccess::ComputeShaderRead
					: ERHIAccess::GraphicsShaderRead);
		auto DeclareCloudShadowInput = [&](const auto& Texture, FRHITexture* Physical) {
			if (!Texture || !Physical) return;
			Graph.UseTexture(CloudShadowPass, *Texture,
				{GetTextureAspects(Physical->GetFormat()), 0,
					Physical->GetNumMips(), 0, Physical->GetArraySize()},
				ERenderGraphUse::Read,
				PreparedCloudShadowRoute
						== FVolumetricCloudShadowRenderer::ERoute::Compute
					? ERHIAccess::ComputeShaderRead
					: ERHIAccess::GraphicsShaderRead);
		};
		if (Services.ResolvedFrame.VolumetricCloud)
		{
			DeclareCloudShadowInput(GraphResources.VolumetricCloudBaseDensity,
				Services.ResolvedFrame.VolumetricCloud->Textures.BaseDensity);
			DeclareCloudShadowInput(GraphResources.VolumetricCloudDetailDensity,
				Services.ResolvedFrame.VolumetricCloud->Textures.DetailDensity);
			DeclareCloudShadowInput(GraphResources.VolumetricCloudWeather,
				CloudWeatherTexture);
		}
		if (GraphResources.VolumetricCloudShadowFragment)
			Graph.UseManagedTexture(CloudShadowPass,
				*GraphResources.VolumetricCloudShadowFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::ReadWrite, ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead, true);
		if (GraphResources.VolumetricCloudShadowCompute)
			Graph.UseTexture(CloudShadowPass,
				*GraphResources.VolumetricCloudShadowCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
	}

	auto FDeferredLightingGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FSceneView& RecordView) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Requirements = Context.Topology;
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
		auto& ContactShadowValue = Channels.ContactShadow;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredValue = Channels.Deferred;
		auto& OpaqueSceneValue = Channels.OpaqueScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto& FinalOutputValue = Channels.FinalOutput;
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
		if (Requirements.bIsolatedDeferred)
			GraphResources.IsolatedDeferred = Graph.CreateTexture(
				"Scene.Deferred.Isolated",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DeferredDirectionalColor", Width, Height,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.BackingClass = std::string(GetSceneFrameBackingClassName(
						ESceneFrameBackingClass::Deferred))},
				ERHIAccess::GraphicsShaderRead);
		const auto DeferredPass =
			AddSceneFrameFeaturePass<FDeferredLightingGraphContributor>(
				Graph, ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordView = &RecordView, &GraphResources, &Requirements,
				&Options, &DeferredParameters, &ProductionDeferredParameters,
				Width, Height, bWantsDeferredInputs, bWantsIsolatedDeferred,
				bWantsProductionDeferred, bHybridRetainedResourcesReady](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0])
					GBufferTargets = {
						.Material = Resources.GetTexture(*GraphResources.GBuffer[0]),
						.Normals = Resources.GetTexture(*GraphResources.GBuffer[1]),
						.Surface = Resources.GetTexture(*GraphResources.GBuffer[2]),
						.Emissive = Resources.GetTexture(*GraphResources.GBuffer[3])};
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = GBufferTargets
						? Resources.GetTexture(GraphResources.SceneDepth) : nullptr};
				std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
					AmbientOcclusionTargets;
				if (GraphResources.GroundTruthAmbientOcclusion[0])
					AmbientOcclusionTargets = {
						.Raw = Resources.GetTexture(
							*GraphResources.GroundTruthAmbientOcclusion[0]),
						.Scratch = Resources.GetTexture(
							*GraphResources.GroundTruthAmbientOcclusion[1]),
						.Selector = GraphResources.GroundTruthAmbientOcclusion[2]
							? Resources.GetTexture(
								*GraphResources.GroundTruthAmbientOcclusion[2])
							: nullptr,
						.Resolved = GraphResources.GroundTruthAmbientOcclusion[3]
							? Resources.GetTexture(
								*GraphResources.GroundTruthAmbientOcclusion[3])
							: nullptr,
						.Quality = Requirements.AmbientOcclusionQuality};
				std::optional<FContactShadowVisibilityRenderer::FTargets>
					FragmentContactTargets;
				if (GraphResources.ContactFragment)
					FragmentContactTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.ContactFragment)};
				std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
					ComputeContactTargets;
				if (GraphResources.ContactCompute)
					ComputeContactTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.ContactCompute)};
				std::optional<FVolumetricCloudShadowRenderer::FTargets>
					FragmentCloudShadowTargets;
				if (GraphResources.VolumetricCloudShadowFragment)
					FragmentCloudShadowTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowFragment)};
				std::optional<FVolumetricCloudShadowRenderer::FComputeTargets>
					ComputeCloudShadowTargets;
				if (GraphResources.VolumetricCloudShadowCompute)
					ComputeCloudShadowTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowCompute)};
				DeferredParameters = bWantsDeferredInputs
					? Services.Recorders.BuildDeferredParameters(
						*RecordView, Channels.DirectionalShadow.Result,
						GraphResources.DirectionalShadow
							? Resources.GetTexture(*GraphResources.DirectionalShadow)
							: nullptr,
						Channels.GBuffer.Result,
						GBufferTargets ? &*GBufferTargets : nullptr,
						Channels.AmbientOcclusion.Result,
						AmbientOcclusionTargets ? &*AmbientOcclusionTargets : nullptr,
						Channels.ContactShadow.Result,
						FragmentContactTargets ? &*FragmentContactTargets : nullptr,
						ComputeContactTargets ? &*ComputeContactTargets : nullptr,
						Channels.CloudShadow.Result,
						FragmentCloudShadowTargets
							? &*FragmentCloudShadowTargets : nullptr,
						ComputeCloudShadowTargets
							? &*ComputeCloudShadowTargets : nullptr,
						SceneTargets, Options)
					: std::nullopt;
				if (DeferredParameters)
				{
					std::optional<FDeferredDirectionalLightingRenderer::FTargets>
						IsolatedTargets;
					if (GraphResources.IsolatedDeferred)
						IsolatedTargets = {.Color = Resources.GetTexture(
							*GraphResources.IsolatedDeferred)};
					Channels.Deferred.Result = Services.Recorders.RenderIsolatedDeferred_RenderThread(
						Commands, IsolatedTargets ? &*IsolatedTargets : nullptr,
						*DeferredParameters, Options, Width, Height,
						bWantsIsolatedDeferred);
				}
				else if (bWantsIsolatedDeferred)
				{
					Channels.Deferred.Result.Status = EScenePassStatus::Failed;
					++Services.Telemetry.View.Deferred.DeferredDirectionalUnavailableViews;
				}
				const bool bProductionResourcesReady =
					!bWantsProductionDeferred
					|| (Channels.GBuffer.Result.IsComplete()
						&& bHybridRetainedResourcesReady
						&& DeferredParameters.has_value());
				if (bWantsProductionDeferred && bProductionResourcesReady)
				{
					ProductionDeferredParameters = *DeferredParameters;
					ProductionDeferredParameters->DiagnosticMode = 0;
				}
			});
		Graph.UseToken(DeferredPass, DirectionalShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, GBufferValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, AmbientOcclusionValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, ContactShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, CloudShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, DeferredValue.Handle, ERenderGraphUse::Write);
		if (GraphResources.DirectionalShadow)
			Graph.UseTexture(DeferredPass, *GraphResources.DirectionalShadow,
				{ERHITextureAspect::Depth, 0, 1, 0,
					DirectionalShadowCascadeCount},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.GBuffer[0])
		{
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseTexture(DeferredPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
					ERHIAccess::GraphicsShaderRead);
			Graph.UseTexture(DeferredPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		}
		for (const auto& Texture : GraphResources.GroundTruthAmbientOcclusion)
		{
			if (!Texture) continue;
			Graph.UseTexture(DeferredPass, *Texture,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		}
		if (GraphResources.ContactFragment)
			Graph.UseTexture(DeferredPass, *GraphResources.ContactFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.ContactCompute)
			Graph.UseTexture(DeferredPass, *GraphResources.ContactCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudShadowFragment)
			Graph.UseTexture(DeferredPass,
				*GraphResources.VolumetricCloudShadowFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudShadowCompute)
			Graph.UseTexture(DeferredPass,
				*GraphResources.VolumetricCloudShadowCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		DeclarePersistentGraphicsInputs(DeferredPass);
		if (GraphResources.IsolatedDeferred)
			Graph.UseManagedColorAttachment(DeferredPass,
				*GraphResources.IsolatedDeferred,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead);
	}

	auto FOpaqueSceneGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FSceneGeometryRecordInputs& RecordInputs) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Requirements = Context.Topology;
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
		auto& ContactShadowValue = Channels.ContactShadow;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredValue = Channels.Deferred;
		auto& OpaqueSceneValue = Channels.OpaqueScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto& FinalOutputValue = Channels.FinalOutput;
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
		const auto OpaqueScenePass =
			AddSceneFrameFeaturePass<FOpaqueSceneGraphContributor>(
				Graph, ERenderGraphPassType::Graphics,
			[&Services, RecordInputs, &GraphResources, &ProductionDeferredParameters,
				&Channels](FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = Resources.GetTexture(GraphResources.SceneColor),
					.Depth = Resources.GetTexture(GraphResources.SceneDepth)};
				const FSceneColorTimingQuerySink TimingSink =
					GetSceneColorTimingQuerySink();
				TScopedRendererGPUTimingQuery Timing(Commands, TimingSink);
				Channels.OpaqueScene.Result = Services.Recorders.RenderSceneOpaque_RenderThread(
					Commands,
					RecordInputs,
					SceneTargets.Color, SceneTargets.Depth,
					ProductionDeferredParameters
						? &*ProductionDeferredParameters : nullptr);
				Timing.Commit();
			});
		Graph.UseToken(OpaqueScenePass, DeferredValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(OpaqueScenePass, OpaqueSceneValue.Handle,
			ERenderGraphUse::Write);
		if (GraphResources.DirectionalShadow)
			Graph.UseTexture(OpaqueScenePass, *GraphResources.DirectionalShadow,
				{ERHITextureAspect::Depth, 0, 1, 0,
					DirectionalShadowCascadeCount},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		DeclarePersistentGraphicsInputs(OpaqueScenePass);
		Graph.UseManagedColorAttachment(OpaqueScenePass,
			GraphResources.SceneColor,
			{ERHITextureAspect::Color, 0, 1, 0, 1},
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			ERHIAccess::GraphicsShaderRead);
		Graph.UseManagedTexture(OpaqueScenePass, GraphResources.SceneDepth,
			{ERHITextureAspect::Depth, 0, 1, 0, 1},
			ERenderGraphUse::ReadWrite,
			bNeedsGBuffer ? ERHIAccess::GraphicsShaderRead
				: ERHIAccess::DepthStencilReadWrite,
			bRequiresDeferredOpaque ? ERHIAccess::GraphicsShaderRead
				: ERHIAccess::DepthStencilReadWrite,
			!bNeedsGBuffer);

	}

	auto FVolumetricCloudSpatialGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FVolumetricCloudRecordInputs& RecordInputs) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Requirements = Context.Topology;
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
		auto& ContactShadowValue = Channels.ContactShadow;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredValue = Channels.Deferred;
		auto& OpaqueSceneValue = Channels.OpaqueScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto& FinalOutputValue = Channels.FinalOutput;
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
		const uint32 CloudWidth = static_cast<uint32>(
			std::max(Requirements.VolumetricCloudExtent.x, 0));
		const uint32 CloudHeight = static_cast<uint32>(
			std::max(Requirements.VolumetricCloudExtent.y, 0));
		if (Requirements.UsesCloudFragment())
			GraphResources.VolumetricCloudFragment = Graph.CreateTexture(
				"Scene.VolumetricCloud.Fragment",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudFragment", CloudWidth, CloudHeight,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback)
					.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f)),
					.BackingClass = std::string(GetSceneFrameBackingClassName(
						ESceneFrameBackingClass::VolumetricCloudFragment))},
				ERHIAccess::GraphicsShaderRead);
		if (Requirements.UsesCloudCompute())
			GraphResources.VolumetricCloudCompute = Graph.CreateTexture(
				"Scene.VolumetricCloud.Compute",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudCompute", CloudWidth, CloudHeight,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::Storage
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback),
					.BackingClass = std::string(GetSceneFrameBackingClassName(
						ESceneFrameBackingClass::VolumetricCloudCompute))},
				ERHIAccess::GraphicsShaderRead);
		if (Requirements.bVolumetricCloudComposite)
			GraphResources.VolumetricCloudComposite = Graph.CreateTexture(
				"Scene.VolumetricCloud.Composite",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudComposite", Width, Height,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback)
					.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f)),
					.BackingClass = std::string(GetSceneFrameBackingClassName(
						ESceneFrameBackingClass::VolumetricCloudComposite))},
				ERHIAccess::GraphicsShaderRead);
		const auto VolumetricCloudSpatialPass =
			AddSceneFrameFeaturePass<FVolumetricCloudSpatialGraphContributor>(Graph,
			PreparedCloudRoute == FVolumetricCloudRenderer::ERoute::Compute
				? ERenderGraphPassType::Compute : ERenderGraphPassType::Graphics,
			[&Services, RecordInputs, &GraphResources, &Requirements,
				&Channels](FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FVolumetricCloudRenderer::FTargets> FragmentTargets;
				if (GraphResources.VolumetricCloudFragment)
					FragmentTargets = {.Cloud = Resources.GetTexture(
						*GraphResources.VolumetricCloudFragment)};
				std::optional<FVolumetricCloudRenderer::FComputeTargets> ComputeTargets;
				if (GraphResources.VolumetricCloudCompute)
					ComputeTargets = {.Cloud = Resources.GetTexture(
						*GraphResources.VolumetricCloudCompute)};
				const FVolumetricCloudTimingQuerySink TimingSink =
					GetVolumetricCloudTimingQuerySink();
				TScopedRendererGPUTimingQuery Timing(Commands, TimingSink);
				Channels.VolumetricCloudSpatial.Result =
					Services.Recorders.RenderVolumetricCloudSpatial_RenderThread(
						Commands,
						RecordInputs,
						FragmentTargets ? &*FragmentTargets : nullptr,
						ComputeTargets ? &*ComputeTargets : nullptr,
						GraphResources.VolumetricCloudBaseDensity
							? Resources.GetTexture(
								*GraphResources.VolumetricCloudBaseDensity) : nullptr,
						GraphResources.VolumetricCloudDetailDensity
							? Resources.GetTexture(
								*GraphResources.VolumetricCloudDetailDensity) : nullptr,
						GraphResources.VolumetricCloudWeather
							? Resources.GetTexture(
								*GraphResources.VolumetricCloudWeather) : nullptr,
					Requirements.VolumetricCloud != ESceneFrameRoute::Disabled
							? Resources.GetTexture(GraphResources.SceneDepth) : nullptr);
				Timing.Commit();
			});
		Graph.UseToken(VolumetricCloudSpatialPass, OpaqueSceneValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(VolumetricCloudSpatialPass,
			VolumetricCloudSpatialValue.Handle, ERenderGraphUse::Write);
		auto DeclareCloudSpatialInput = [&](const auto& Texture,
			FRHITexture* Physical) {
			if (!Texture || !Physical) return;
			Graph.UseTexture(VolumetricCloudSpatialPass, *Texture,
				{GetTextureAspects(Physical->GetFormat()), 0,
					Physical->GetNumMips(), 0, Physical->GetArraySize()},
				ERenderGraphUse::Read,
				PreparedCloudRoute == FVolumetricCloudRenderer::ERoute::Compute
					? ERHIAccess::ComputeShaderRead
					: ERHIAccess::GraphicsShaderRead);
		};
		if (Services.ResolvedFrame.VolumetricCloud)
		{
			DeclareCloudSpatialInput(GraphResources.VolumetricCloudBaseDensity,
				Services.ResolvedFrame.VolumetricCloud->Textures.BaseDensity);
			DeclareCloudSpatialInput(GraphResources.VolumetricCloudDetailDensity,
				Services.ResolvedFrame.VolumetricCloud->Textures.DetailDensity);
			DeclareCloudSpatialInput(GraphResources.VolumetricCloudWeather,
				CloudWeatherTexture);
		}
		if (Requirements.VolumetricCloud != ESceneFrameRoute::Disabled)
			Graph.UseTexture(VolumetricCloudSpatialPass,
				GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				PreparedCloudRoute == FVolumetricCloudRenderer::ERoute::Compute
					? ERHIAccess::ComputeShaderRead
					: ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudFragment)
			Graph.UseManagedTexture(VolumetricCloudSpatialPass,
				*GraphResources.VolumetricCloudFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::ReadWrite, ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead, true);
		if (GraphResources.VolumetricCloudCompute)
			Graph.UseTexture(VolumetricCloudSpatialPass,
				*GraphResources.VolumetricCloudCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);

	}

	auto FVolumetricCloudCompositeGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FVolumetricCloudRecordInputs& RecordInputs) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Requirements = Context.Topology;
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
		auto& ContactShadowValue = Channels.ContactShadow;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredValue = Channels.Deferred;
		auto& OpaqueSceneValue = Channels.OpaqueScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto& FinalOutputValue = Channels.FinalOutput;
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
		const auto VolumetricCloudPass =
			AddSceneFrameFeaturePass<FVolumetricCloudCompositeGraphContributor>(
				Graph, ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordInputs, &GraphResources, &Requirements](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				if (!Requirements.bVolumetricCloudComposite) return;
				std::optional<FVolumetricCloudRenderer::FTargets> FragmentTargets;
				if (GraphResources.VolumetricCloudFragment)
					FragmentTargets = {.Cloud = Resources.GetTexture(
						*GraphResources.VolumetricCloudFragment)};
				std::optional<FVolumetricCloudRenderer::FComputeTargets> ComputeTargets;
				if (GraphResources.VolumetricCloudCompute)
					ComputeTargets = {.Cloud = Resources.GetTexture(
						*GraphResources.VolumetricCloudCompute)};
				std::optional<FVolumetricCloudRenderer::FTargets> CompositeTargets;
				if (GraphResources.VolumetricCloudComposite)
					CompositeTargets = {.Cloud = Resources.GetTexture(
						*GraphResources.VolumetricCloudComposite)};
				FRHITexture* ShadowVisibility = nullptr;
				if (Channels.CloudShadow.Result.Route
					== EVolumetricCloudShadowPassRoute::Compute
					&& GraphResources.VolumetricCloudShadowCompute)
					ShadowVisibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowCompute);
				else if (Channels.CloudShadow.Result.Route
					== EVolumetricCloudShadowPassRoute::Fragment
					&& GraphResources.VolumetricCloudShadowFragment)
					ShadowVisibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowFragment);
				Channels.VolumetricCloud.Result =
					Services.Recorders.RenderVolumetricCloudComposite_RenderThread(
						Commands,
						RecordInputs,
						Channels.VolumetricCloudSpatial.Result,
						FragmentTargets ? &*FragmentTargets : nullptr,
						ComputeTargets ? &*ComputeTargets : nullptr,
						CompositeTargets ? &*CompositeTargets : nullptr,
						Resources.GetTexture(GraphResources.SceneColor),
						Resources.GetTexture(GraphResources.SceneDepth),
						ShadowVisibility);
			});
		Graph.UseToken(VolumetricCloudPass, OpaqueSceneValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(VolumetricCloudPass, VolumetricCloudSpatialValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(VolumetricCloudPass, CloudShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(VolumetricCloudPass, VolumetricCloudValue.Handle,
			ERenderGraphUse::Write);
		if (Requirements.bVolumetricCloudComposite)
		{
			Graph.UseTexture(VolumetricCloudPass, GraphResources.SceneColor,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
			Graph.UseTexture(VolumetricCloudPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		}
		auto DeclareCloudCompositeInput = [&](const auto& Texture,
			FRHITexture* Physical) {
			if (!Texture || !Physical) return;
			Graph.UseTexture(VolumetricCloudPass, *Texture,
				{GetTextureAspects(Physical->GetFormat()), 0,
					Physical->GetNumMips(), 0, Physical->GetArraySize()},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		};
		if (Services.ResolvedFrame.VolumetricCloud
			&& Requirements.bVolumetricCloudComposite)
		{
			DeclareCloudCompositeInput(GraphResources.VolumetricCloudBaseDensity,
				Services.ResolvedFrame.VolumetricCloud->Textures.BaseDensity);
			DeclareCloudCompositeInput(GraphResources.VolumetricCloudDetailDensity,
				Services.ResolvedFrame.VolumetricCloud->Textures.DetailDensity);
			DeclareCloudCompositeInput(GraphResources.VolumetricCloudWeather,
				CloudWeatherTexture);
		}
		if (GraphResources.VolumetricCloudShadowFragment)
			Graph.UseTexture(VolumetricCloudPass,
				*GraphResources.VolumetricCloudShadowFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudShadowCompute)
			Graph.UseTexture(VolumetricCloudPass,
				*GraphResources.VolumetricCloudShadowCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudFragment)
			Graph.UseTexture(VolumetricCloudPass,
				*GraphResources.VolumetricCloudFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudCompute)
			Graph.UseTexture(VolumetricCloudPass,
				*GraphResources.VolumetricCloudCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudComposite)
			Graph.UseManagedTexture(VolumetricCloudPass,
				*GraphResources.VolumetricCloudComposite,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::ReadWrite, ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead, true);

	}

	auto FSortedTranslucencyGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FSceneGeometryRecordInputs& RecordInputs) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Requirements = Context.Topology;
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
		auto& ContactShadowValue = Channels.ContactShadow;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredValue = Channels.Deferred;
		auto& OpaqueSceneValue = Channels.OpaqueScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto& FinalOutputValue = Channels.FinalOutput;
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
		const auto SceneColorPass =
			AddSceneFrameFeaturePass<FSortedTranslucencyGraphContributor>(
				Graph, ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordInputs, &GraphResources, &Requirements,
				bRequiresDeferredOpaque](FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				if (!bRequiresDeferredOpaque)
					Channels.SceneColor.Result = Channels.OpaqueScene.Result;
				else
				{
					FSceneColorPassResult Input = Channels.OpaqueScene.Result;
					if (Requirements.bVolumetricCloudComposite
						&& !Channels.VolumetricCloud.Result.bCompositeOutputValid)
						Input.Result = ERenderViewResult::RendererResourcesUnavailable;
					FRHITexture* Color = Requirements.bVolumetricCloudComposite
						&& GraphResources.VolumetricCloudComposite
						? Resources.GetTexture(
							*GraphResources.VolumetricCloudComposite)
						: Resources.GetTexture(GraphResources.SceneColor);
					Channels.SceneColor.Result = Services.Recorders.RenderSceneTranslucency_RenderThread(
						Commands,
						RecordInputs,
						Color,
						Resources.GetTexture(GraphResources.SceneDepth), Input,
						Channels.VolumetricCloud.Result);
				}
				if (!Channels.SceneColor.Result.IsSuccess()) return;
				ReduceStaticMeshTelemetry(RecordInputs.Receiver.StaticMeshes,
					Services.ResolvedFrame.Receiver.StaticMeshes, Services.Telemetry.View);
				ReduceSkeletalMeshTelemetry(RecordInputs.Receiver.SkeletalMeshes,
					Services.ResolvedFrame.Receiver.SkeletalMeshes,
					Services.ResolvedFrame.Receiver.SkeletalPalettes, Services.Telemetry.View);
				ReduceTerrainTelemetry(RecordInputs.Receiver.Terrains,
					Services.ResolvedFrame.Receiver.Terrains, Services.Telemetry.View);
			});
		Graph.UseToken(SceneColorPass, OpaqueSceneValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(SceneColorPass, VolumetricCloudValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(SceneColorPass, SceneColorValue.Handle,
			ERenderGraphUse::Write);
		if (bRequiresDeferredOpaque)
		{
			const FRenderGraphTextureHandle Color =
				Requirements.bVolumetricCloudComposite
					&& GraphResources.VolumetricCloudComposite
				? *GraphResources.VolumetricCloudComposite
				: GraphResources.SceneColor;
			Graph.UseManagedTexture(SceneColorPass, Color,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::ReadWrite,
				ERHIAccess::ColorAttachmentReadWrite,
				ERHIAccess::GraphicsShaderRead);
			Graph.UseManagedTexture(SceneColorPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1},
				ERenderGraphUse::ReadWrite, ERHIAccess::GraphicsShaderRead,
				ERHIAccess::DepthStencilReadWrite);
		}
	}

	auto FPostProcessGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FSceneView& RecordView) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Requirements = Context.Topology;
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
		auto& ContactShadowValue = Channels.ContactShadow;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredValue = Channels.Deferred;
		auto& OpaqueSceneValue = Channels.OpaqueScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto& FinalOutputValue = Channels.FinalOutput;
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
		if (Requirements.bGBufferDebug)
			GraphResources.GBufferDebug = Graph.CreateTexture(
				"Scene.GBuffer.Debug",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"GBufferDebugColor", Width, Height,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.BackingClass = std::string(GetSceneFrameBackingClassName(
						ESceneFrameBackingClass::GBufferDebug))},
				ERHIAccess::GraphicsShaderRead);
		const auto PostProcessPass =
			AddSceneFrameFeaturePass<FPostProcessGraphContributor>(
				Graph, ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordView = &RecordView, &View, &GraphResources,
				&Requirements, &Options, bPresentOutput,
				bHasEditorAssistance](FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				if (!Channels.SceneColor.Result.IsSuccess()) return;
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = Resources.GetTexture(GraphResources.SceneColor),
					.Depth = Requirements.bGBufferDebug
						? Resources.GetTexture(GraphResources.SceneDepth)
						: nullptr};
				FRHITexture* SceneColorInput = SceneTargets.Color;
				if (Channels.SceneColor.Result.bUsesVolumetricCloudComposite
					&& GraphResources.VolumetricCloudComposite)
					SceneColorInput = Resources.GetTexture(
						*GraphResources.VolumetricCloudComposite);
				std::optional<FGBufferDebugRenderer::FTargets> DebugTargets;
				if (GraphResources.GBufferDebug)
					DebugTargets = {.Color = Resources.GetTexture(
						*GraphResources.GBufferDebug)};
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0] && Requirements.bGBufferDebug)
					GBufferTargets = {
						.Material = Resources.GetTexture(*GraphResources.GBuffer[0]),
						.Normals = Resources.GetTexture(*GraphResources.GBuffer[1]),
						.Surface = Resources.GetTexture(*GraphResources.GBuffer[2]),
						.Emissive = Resources.GetTexture(*GraphResources.GBuffer[3])};
				FRHITexture* IsolatedDeferredOutput = nullptr;
				if (GraphResources.IsolatedDeferred
					&& Channels.Deferred.Result.bOutputValid)
					IsolatedDeferredOutput = Resources.GetTexture(
						*GraphResources.IsolatedDeferred);
				Channels.PostProcess.Result = Services.Recorders.RenderPostProcess_RenderThread(
					Commands, *RecordView, View,
					Resources.GetTexture(GraphResources.Output),
					bPresentOutput, Options, SceneTargets,
					GBufferTargets ? &*GBufferTargets : nullptr,
					DebugTargets ? &*DebugTargets : nullptr,
					SceneColorInput, IsolatedDeferredOutput,
					bHasEditorAssistance);
			});
		Graph.UseToken(PostProcessPass, SceneColorValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(PostProcessPass, GBufferValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(PostProcessPass, PostProcessValue.Handle,
			ERenderGraphUse::Write);
		Graph.UseTexture(PostProcessPass, GraphResources.SceneColor,
			{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
			ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudComposite)
			Graph.UseTexture(PostProcessPass,
				*GraphResources.VolumetricCloudComposite,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (Requirements.bGBufferDebug)
			Graph.UseTexture(PostProcessPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		Graph.UseManagedColorAttachment(PostProcessPass, GraphResources.Output,
			{GetTextureAspects(OutputTarget->GetFormat()), 0,
				OutputTarget->GetNumMips(), 0, OutputTarget->GetArraySize()},
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			bHasEditorAssistance
				? ERHIAccess::ColorAttachmentReadWrite
				: (bPresentOutput ? ERHIAccess::Present
								  : ERHIAccess::GraphicsShaderRead));
		if (GraphResources.GBufferDebug)
			Graph.UseManagedColorAttachment(PostProcessPass,
				*GraphResources.GBufferDebug,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead);
		if (GraphResources.GBuffer[0] && Requirements.bGBufferDebug)
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseTexture(PostProcessPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1},
					ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.IsolatedDeferred)
			Graph.UseTexture(PostProcessPass, *GraphResources.IsolatedDeferred,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (!bHasEditorAssistance)
		{
			Graph.UseToken(PostProcessPass, FinalOutputValue.Handle,
				ERenderGraphUse::Write);
			Graph.MarkPassRoot(PostProcessPass,
				bPresentOutput ? "present" : "offscreen-output");
		}
	}

	auto FEditorAssistanceGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FSceneView& RecordView) -> void
	{
		if (!Context.bHasEditorAssistance) return;
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		auto* OutputTarget = Context.OutputTarget;
		const bool bPresentOutput = Context.bPresentOutput;
		const auto& PreparedEditorAssistance =
			Context.EditorAssistance;
		auto& GraphResources = Context.Composition.Resources;
		auto& Channels = Context.Composition.Channels;
		auto& PostProcessValue = Channels.PostProcess;
			auto& FinalOutputValue = Channels.FinalOutput;
			const auto EditorAssistancePass =
				AddSceneFrameFeaturePass<FEditorAssistanceGraphContributor>(
					Graph, ERenderGraphPassType::Graphics,
				[&Services, &Channels, RecordView = &RecordView, &GraphResources,
					&PreparedEditorAssistance, bPresentOutput](
					FRHICommandListImmediate& Commands,
					const FRenderGraphPassResources& Resources) {
					if (Channels.PostProcess.Result.Result != ERenderViewResult::Success) return;
					Channels.PostProcess.Result.bEditorAssistance =
						Services.Recorders.RenderEditorAssistance_RenderThread(
							Commands, *RecordView,
							Resources.GetTexture(GraphResources.Output),
							Resources.GetTexture(GraphResources.SceneDepth),
							bPresentOutput, PreparedEditorAssistance);
				});
			Graph.UseToken(EditorAssistancePass, PostProcessValue.Handle,
				ERenderGraphUse::Read);
			Graph.UseToken(EditorAssistancePass, FinalOutputValue.Handle,
				ERenderGraphUse::Write);
			Graph.UseManagedColorAttachment(EditorAssistancePass,
				GraphResources.Output,
				{GetTextureAspects(OutputTarget->GetFormat()), 0,
					OutputTarget->GetNumMips(), 0, OutputTarget->GetArraySize()},
				ERHIRenderTargetLoadAction::Load,
				ERHIRenderTargetStoreAction::Store,
				bPresentOutput ? ERHIAccess::Present
								 : ERHIAccess::GraphicsShaderRead);
			Graph.UseManagedDepthStencilAttachment(EditorAssistancePass,
				GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Load,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::DepthStencilReadWrite);
			Graph.MarkPassRoot(EditorAssistancePass,
				bPresentOutput ? "present" : "offscreen-output");
	}
} // namespace Durin
