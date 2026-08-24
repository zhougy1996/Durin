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
	namespace
	{
		auto CanonicalizeRenderGraphFrameCloudQuality(
			EVolumetricCloudQuality Quality) -> EVolumetricCloudQuality
		{
			return Quality < EVolumetricCloudQuality::Count
				? Quality : EVolumetricCloudQuality::High;
		}
	} // namespace

	auto FVolumetricCloudShadowGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FVolumetricCloudShadowRecordInputs& RecordInputs) -> void
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
		if (Topology.UsesCloudShadowFragment())
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
		if (Topology.UsesCloudShadowCompute())
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
			[&Services, &Channels, RecordInputs, &GraphResources, &Topology,
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
					.Depth = Topology.VolumetricCloudShadow
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
		if (Topology.VolumetricCloudShadow != ESceneFrameRoute::Disabled)
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

	auto FVolumetricCloudSpatialGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FVolumetricCloudRecordInputs& RecordInputs) -> void
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
		const uint32 CloudWidth = static_cast<uint32>(
			std::max(Topology.VolumetricCloudExtent.x, 0));
		const uint32 CloudHeight = static_cast<uint32>(
			std::max(Topology.VolumetricCloudExtent.y, 0));
		if (Topology.UsesCloudFragment())
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
		if (Topology.UsesCloudCompute())
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
		if (Topology.bVolumetricCloudComposite)
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
			[&Services, RecordInputs, &GraphResources, &Topology,
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
					Topology.VolumetricCloud != ESceneFrameRoute::Disabled
							? Resources.GetTexture(GraphResources.SceneDepth) : nullptr);
				Timing.Commit();
			});
		Graph.UseToken(VolumetricCloudSpatialPass, BaseSceneValue.Handle,
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
		if (Topology.VolumetricCloud != ESceneFrameRoute::Disabled)
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
		const auto VolumetricCloudPass =
			AddSceneFrameFeaturePass<FVolumetricCloudCompositeGraphContributor>(
				Graph, ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordInputs, &GraphResources, &Topology](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				if (!Topology.bVolumetricCloudComposite) return;
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
		Graph.UseToken(VolumetricCloudPass, BaseSceneValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(VolumetricCloudPass, VolumetricCloudSpatialValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(VolumetricCloudPass, CloudShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(VolumetricCloudPass, VolumetricCloudValue.Handle,
			ERenderGraphUse::Write);
		if (Topology.bVolumetricCloudComposite)
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
			&& Topology.bVolumetricCloudComposite)
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

	auto FSceneFrameFeatureRecorders::RenderVolumetricCloudShadows_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FVolumetricCloudShadowRecordInputs& Inputs,
		const FVolumetricCloudShadowRenderer::FTargets* FragmentTargets,
		const FVolumetricCloudShadowRenderer::FComputeTargets* ComputeTargets,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		FRHITexture* BaseDensity,
		FRHITexture* DetailDensity,
		FRHITexture* Weather,
		uint32 Width,
		uint32 Height,
		bool bWantsProductionDeferred,
		bool bGBufferComplete
	) -> FVolumetricCloudShadowPassResult
	{
		FVolumetricCloudShadowPassResult PassResult;
		const FPreparedVolumetricCloud* Cloud = Inputs.Cloud;
		const FResolvedVolumetricCloud* ResolvedCloud =
			ResolvedFrame.VolumetricCloud
				? &*ResolvedFrame.VolumetricCloud : nullptr;
		const bool bRequested = bWantsProductionDeferred && bGBufferComplete
								&& Cloud != nullptr && ResolvedCloud != nullptr
								&& !Inputs.Lighting.Lights.Directional.empty()
								&& ResolvedCloud->Textures.BaseDensity
								&& ResolvedCloud->Textures.DetailDensity
								&& ResolvedCloud->Textures.DensitySampler
								&& SceneTargets.Depth;
		if (!bRequested) return PassResult;
		PassResult.Status = EScenePassStatus::Failed;
		const bool bForceFragment =
			Qualification.bForceFragmentVolumetricCloud;
		if (bForceFragment) ComputeTargets = nullptr;
		const auto QualityTier = CanonicalizeRenderGraphFrameCloudQuality(
			Inputs.View.Settings.VolumetricCloud.Quality);
		const auto Result = VolumetricCloudShadowRenderer.Render_RenderThread(
			CommandList, FragmentTargets, ComputeTargets,
			{.bRequested = true,
				 .BaseDensity = BaseDensity,
				 .DetailDensity = DetailDensity,
			 .Weather = Weather,
			 .SceneDepth = SceneTargets.Depth,
				 .DensitySampler = ResolvedCloud->Textures.DensitySampler,
			 .Parameters = Cloud->Parameters,
				 .View = &Inputs.View,
			 .QualityTier = QualityTier,
			 .Width = Width,
			 .Height = Height},
			{.bGraphManagedTextureAccess = true}
		);
		auto& ViewTelemetry = Telemetry.View;
		const size_t ReasonIndex = static_cast<size_t>(Result.Reason);
		if (ReasonIndex < ViewTelemetry.VolumetricCloud.VolumetricCloudShadowRouteReasons.size())
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowRouteReasons[ReasonIndex];
		ViewTelemetry.VolumetricCloud.VolumetricCloudShadowRetainedBytes =
			VolumetricCloudShadowRenderer.GetRetainedTargetBytes_RenderThread();
		if (!Result.Visibility)
		{
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowFactorOneViews;
			return PassResult;
		}
		PassResult.Status = EScenePassStatus::Complete;
		PassResult.Route = Result.Route
			== FVolumetricCloudShadowRenderer::ERoute::Compute
			? EVolumetricCloudShadowPassRoute::Compute
			: EVolumetricCloudShadowPassRoute::Fragment;
		ViewTelemetry.VolumetricCloud.VolumetricCloudShadowActiveBytes = Result.TargetBytes;
		ViewTelemetry.VolumetricCloud.VolumetricCloudShadowSamples = static_cast<uint64>(Width)
												* Height * Result.SampleCount;
		++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowEnabledViews;
		if (Result.Route == FVolumetricCloudShadowRenderer::ERoute::Compute)
		{
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowComputeViews;
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowDispatches;
		}
		else
		{
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowFragmentViews;
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowDraws;
		}
		return PassResult;
	}

	auto FSceneFrameFeatureRecorders::RenderVolumetricCloudSpatial_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FVolumetricCloudRecordInputs& Inputs,
		const FVolumetricCloudRenderer::FTargets* FragmentTargets,
		const FVolumetricCloudRenderer::FComputeTargets* ComputeTargets,
		FRHITexture* BaseDensity,
		FRHITexture* DetailDensity,
		FRHITexture* Weather,
		FRHITexture* Depth
	) -> FVolumetricCloudSpatialPassResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView& View = Inputs.View;
		const uint32 Width = Depth != nullptr ? Depth->GetSizeX() : 0;
		const uint32 Height = Depth != nullptr ? Depth->GetSizeY() : 0;
		const FPreparedVolumetricCloud* Cloud = Inputs.Cloud;
		const FResolvedVolumetricCloud* ResolvedCloud =
			ResolvedFrame.VolumetricCloud
				? &*ResolvedFrame.VolumetricCloud : nullptr;
		const bool bInputsPresent = Cloud != nullptr && ResolvedCloud != nullptr
									&& BaseDensity != nullptr
									&& DetailDensity != nullptr
									&& Weather != nullptr
									&& ResolvedCloud->Textures.DensitySampler != nullptr
									&& Depth != nullptr;
		const auto QualityTier = CanonicalizeRenderGraphFrameCloudQuality(
			View.Settings.VolumetricCloud.Quality);
		const auto Quality = FVolumetricCloudSpatialRenderer::ResolveQualityPolicy(
			QualityTier
		);
		const auto CloudExtent = FVolumetricCloudSpatialRenderer::CalculateScaledExtent(
			Width, Height, Quality
		);
		if (!bInputsPresent) FragmentTargets = nullptr;
		if (!bInputsPresent || Qualification.bForceFragmentVolumetricCloud)
			ComputeTargets = nullptr;
		auto Textures = ResolvedCloud != nullptr
			? ResolvedCloud->Textures
			: FVolumetricCloudRenderer::FTextureBindings{};
		Textures.BaseDensity = BaseDensity;
		Textures.DetailDensity = DetailDensity;
		Textures.Weather = Weather;
		Textures.SceneDepth = Depth;
		const FVolumetricCloudRenderer::FRenderResult Result =
			VolumetricCloudRenderer.Render_RenderThread(CommandList, FragmentTargets,
				ComputeTargets, {.bRequested = Cloud != nullptr, .Textures = Textures,
					.Parameters = Cloud != nullptr ? Cloud->Parameters
						: FVolumetricCloudRenderer::FParameters{}, .View = &View,
					.QualityTier = QualityTier,
					.SuccessfulSequence = TemporalContext.SuccessfulSequence,
					.Width = CloudExtent.Width, .Height = CloudExtent.Height,
					.OutputWidth = Width, .OutputHeight = Height},
				{.bGraphManagedTextureAccess = true});
		auto& ViewTelemetry = Telemetry.View;
		const auto RouteIndex = static_cast<size_t>(Result.Counters.Reason);
		if (RouteIndex < ViewTelemetry.VolumetricCloud.VolumetricCloudRouteReasons.size())
			++ViewTelemetry.VolumetricCloud.VolumetricCloudRouteReasons[RouteIndex];
		ViewTelemetry.VolumetricCloud.VolumetricCloudDispatches += Result.Counters.Dispatches;
		ViewTelemetry.VolumetricCloud.VolumetricCloudDraws += Result.Counters.Draws;
		ViewTelemetry.VolumetricCloud.VolumetricCloudPrimarySamples += Result.Counters.PrimarySamples;
		ViewTelemetry.VolumetricCloud.VolumetricCloudLightSamples += Result.Counters.LightSamples;
		ViewTelemetry.VolumetricCloud.VolumetricCloudTargetWidth = Result.Counters.TargetWidth;
		ViewTelemetry.VolumetricCloud.VolumetricCloudTargetHeight = Result.Counters.TargetHeight;
		ViewTelemetry.VolumetricCloud.VolumetricCloudOutputWidth = Result.Counters.OutputWidth;
		ViewTelemetry.VolumetricCloud.VolumetricCloudOutputHeight = Result.Counters.OutputHeight;
		ViewTelemetry.VolumetricCloud.VolumetricCloudActiveBytes = Result.Counters.TargetBytes;
		if (Result.Counters.Route == FVolumetricCloudRenderer::ERoute::Compute)
			++ViewTelemetry.VolumetricCloud.VolumetricCloudComputeViews;
		else if (Result.Counters.Route == FVolumetricCloudRenderer::ERoute::Fragment)
			++ViewTelemetry.VolumetricCloud.VolumetricCloudFragmentViews;
		else
			++ViewTelemetry.VolumetricCloud.VolumetricCloudDisabledViews;
		return {
			.Status = Result.Cloud != nullptr
				? EScenePassStatus::Complete
				: (Cloud != nullptr ? EScenePassStatus::Failed
					: EScenePassStatus::NotRequested),
			.Route = Result.Counters.Route};
	}

	auto FSceneFrameFeatureRecorders::RenderVolumetricCloudComposite_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FVolumetricCloudRecordInputs& Inputs,
		const FVolumetricCloudSpatialPassResult& Spatial,
		const FVolumetricCloudRenderer::FTargets* FragmentTargets,
		const FVolumetricCloudRenderer::FComputeTargets* ComputeTargets,
		const FVolumetricCloudRenderer::FTargets* CompositeTargets,
		FRHITexture* SceneColor,
		FRHITexture* Depth,
		FRHITexture* VolumetricCloudShadowVisibility
	) -> FVolumetricCloudPassResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView& View = Inputs.View;
		const FPreparedVolumetricCloud* Cloud = Inputs.Cloud;
		const auto QualityTier = CanonicalizeRenderGraphFrameCloudQuality(
			View.Settings.VolumetricCloud.Quality);
		auto& ViewTelemetry = Telemetry.View;
		FRHITexture* CurrentCloud = Spatial.Route
				== FVolumetricCloudRenderer::ERoute::Compute && ComputeTargets
			? ComputeTargets->Cloud.GetReference()
			: (Spatial.Route == FVolumetricCloudRenderer::ERoute::Fragment
				&& FragmentTargets ? FragmentTargets->Cloud.GetReference() : nullptr);
		const FVolumetricCloudRenderer::FTemporalReconstructionResult Temporal =
			CurrentCloud != nullptr ? VolumetricCloudRenderer.ReconstructTemporal_RenderThread(
										  CommandList, {.CurrentCloud = CurrentCloud, .View = &View, .TemporalContext = &TemporalContext, .ViewState = ViewState, .Parameters = Cloud != nullptr ? Cloud->Parameters : FVolumetricCloudRenderer::FParameters{}, .QualityTier = QualityTier, .CloudHistoryKey = Cloud != nullptr ? Cloud->HistoryKey : 0}
									  ) :
									  FVolumetricCloudRenderer::FTemporalReconstructionResult{};
		ViewTelemetry.VolumetricCloud.VolumetricCloudHistoryBytes = Temporal.HistoryBytes;
		if (Temporal.bCandidatePublished)
			++ViewTelemetry.VolumetricCloud.VolumetricCloudTemporalDraws;
		if (Temporal.bHistoryAccepted)
			++ViewTelemetry.VolumetricCloud.VolumetricCloudHistoryAccepted;
		else if (Temporal.bCandidatePublished)
			++ViewTelemetry.VolumetricCloud.VolumetricCloudHistoryRejected;
		FRHITexture* Composite = Temporal.Cloud != nullptr
			&& CompositeTargets != nullptr
			? VolumetricCloudRenderer.Composite_RenderThread(
				CommandList,
				*CompositeTargets,
				SceneColor, Temporal.Cloud, Depth,
				VolumetricCloudShadowVisibility,
				Temporal.bCandidatePublished,
				Temporal.bHistoryAccepted, View) :
			nullptr;
		ViewTelemetry.VolumetricCloud.VolumetricCloudRetainedBytes =
			VolumetricCloudRenderer.GetRetainedTargetBytes_RenderThread();
		if (Composite != nullptr)
		{
			++ViewTelemetry.VolumetricCloud.VolumetricCloudEnabledViews;
			++ViewTelemetry.VolumetricCloud.VolumetricCloudCompositeDraws;
			return {
				.Status = EScenePassStatus::Complete,
				.bCompositeOutputValid = true};
		}
		return {
			.Status = Spatial.Status == EScenePassStatus::Complete
				? EScenePassStatus::Failed : EScenePassStatus::NotRequested};
	}
} // namespace Durin
