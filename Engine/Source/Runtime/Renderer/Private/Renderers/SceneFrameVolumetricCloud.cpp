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
		const FCloudShadowGraphInputs& Inputs) -> FCloudShadowGraphOutput
	{
		auto& Graph = Inputs.Graph;
		auto& Services = Inputs.Services;
		const auto RecordInputs = Inputs.Record;
		const auto PreparedCloudShadowRoute = Inputs.Route;
		auto* CloudWeatherTexture = Inputs.WeatherTexture;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		const bool bWantsProductionDeferred = Inputs.bProductionDeferred;
		FSceneFrameTopology Topology;
		Topology.VolumetricCloudShadow = Inputs.GraphRoute;
		struct {
			FRDGTextureHandle SceneDepth;
			std::array<std::optional<FRDGTextureHandle>, 4> GBuffer;
			std::optional<FRDGTextureHandle> VolumetricCloudBaseDensity;
			std::optional<FRDGTextureHandle> VolumetricCloudDetailDensity;
			std::optional<FRDGTextureHandle> VolumetricCloudWeather;
			std::optional<FRDGTextureHandle>
				VolumetricCloudShadowFragment;
			std::optional<FRDGTextureHandle>
				VolumetricCloudShadowCompute;
		} GraphResources;
		GraphResources.GBuffer = Inputs.GBuffer.Textures;
		GraphResources.SceneDepth = Inputs.SceneDepth;
		GraphResources.VolumetricCloudBaseDensity = Inputs.BaseDensity;
		GraphResources.VolumetricCloudDetailDensity = Inputs.DetailDensity;
		GraphResources.VolumetricCloudWeather = Inputs.Weather;
		struct {
			TRDGValueHandle<FGBufferPassResult> GBuffer;
			TRDGValueHandle<FVolumetricCloudShadowPassResult> CloudShadow;
		} Channels;
		Channels.GBuffer = Inputs.GBuffer.Completion;
		Channels.CloudShadow = Graph.CreateValue<
			FVolumetricCloudShadowPassResult>(
				"Scene.CloudShadowValue", "cloud-shadow-result");
		if (Topology.UsesCloudShadowFragment())
			GraphResources.VolumetricCloudShadowFragment = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudVisibility", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback)
					.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f)),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::VolumetricCloudShadowFragment)},
				"Scene.VolumetricCloudShadow.Fragment",
				ERHIAccess::GraphicsShaderRead);
		if (Topology.UsesCloudShadowCompute())
			GraphResources.VolumetricCloudShadowCompute = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudVisibilityCompute", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::Storage
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::VolumetricCloudShadowCompute)},
				"Scene.VolumetricCloudShadow.Compute",
				ERHIAccess::GraphicsShaderRead);
		const bool bCompute = PreparedCloudShadowRoute
			== FVolumetricCloudShadowRenderer::ERoute::Compute;
		auto Parameters = Graph.AllocParameters<
			FVolumetricCloudShadowPassParameters>();
		Parameters->GBufferCompletion = {.Value = Channels.GBuffer};
		Parameters->Completion = {.Value = Channels.CloudShadow};
		if (Topology.VolumetricCloudShadow != ESceneFrameRoute::Disabled)
		{
			FRDGTextureParameter Depth{GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}};
			if (bCompute) Parameters->Resources.SceneDepthCompute = Depth;
			else Parameters->Resources.SceneDepth = Depth;
		}
		auto AssignCloudInput = [&](auto& Graphics, auto& Compute,
			const auto& Texture, FRHITexture* Physical) {
			if (!Texture || !Physical) return;
			FRDGTextureParameter Parameter{*Texture,
				{GetTextureAspects(Physical->GetFormat()), 0,
					Physical->GetNumMips(), 0, Physical->GetArraySize()}};
			if (bCompute) Compute = Parameter;
			else Graphics = Parameter;
		};
		if (Services.ResolvedFrame.VolumetricCloud)
		{
			AssignCloudInput(Parameters->Resources.CloudBaseDensity,
				Parameters->Resources.CloudBaseDensityCompute,
				GraphResources.VolumetricCloudBaseDensity,
				Services.ResolvedFrame.VolumetricCloud->Textures.BaseDensity);
			AssignCloudInput(Parameters->Resources.CloudDetailDensity,
				Parameters->Resources.CloudDetailDensityCompute,
				GraphResources.VolumetricCloudDetailDensity,
				Services.ResolvedFrame.VolumetricCloud->Textures.DetailDensity);
			AssignCloudInput(Parameters->Resources.CloudWeather,
				Parameters->Resources.CloudWeatherCompute,
				GraphResources.VolumetricCloudWeather, CloudWeatherTexture);
		}
		if (GraphResources.VolumetricCloudShadowFragment)
			Parameters->Resources.CloudShadowFragmentOutput = {
				*GraphResources.VolumetricCloudShadowFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.VolumetricCloudShadowCompute)
			Parameters->Resources.CloudShadowComputeOutput = {
				*GraphResources.VolumetricCloudShadowCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		(void)AddSceneFrameFeaturePass<FVolumetricCloudShadowGraphContributor>(Graph,
			bCompute ? ERDGPassType::Compute : ERDGPassType::Graphics,
			std::move(Parameters),
			[&Services, RecordInputs, Topology, Width, Height,
				bWantsProductionDeferred](FRHICommandListImmediate& Commands,
				const FVolumetricCloudShadowPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				std::optional<FVolumetricCloudShadowRenderer::FTargets>
					FragmentTargets;
				if (PassParameters.Resources.CloudShadowFragmentOutput)
					FragmentTargets = {.Visibility = Resolver.GetTexture(
						PassParameters.Resources.CloudShadowFragmentOutput)};
				std::optional<FVolumetricCloudShadowRenderer::FComputeTargets>
					ComputeTargets;
				if (PassParameters.Resources.CloudShadowComputeOutput)
					ComputeTargets = {.Visibility = Resolver.GetTexture(
						PassParameters.Resources.CloudShadowComputeOutput)};
				FRHITexture* Depth = Resolver.GetTexture(
					PassParameters.Resources.SceneDepth);
				if (Depth == nullptr) Depth = Resolver.GetTexture(
					PassParameters.Resources.SceneDepthCompute);
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = Topology.VolumetricCloudShadow
							!= ESceneFrameRoute::Disabled
						? Depth : nullptr};
				auto GetCloudInput = [&](const auto& Graphics, const auto& Compute) {
					FRHITexture* Texture = Resolver.GetTexture(Graphics);
					return Texture != nullptr ? Texture : Resolver.GetTexture(Compute);
				};
				Resolver.WriteValue(PassParameters.Completion) =
					Services.Recorders.RenderVolumetricCloudShadows_RenderThread(
						Commands,
						RecordInputs,
						FragmentTargets ? &*FragmentTargets : nullptr,
						ComputeTargets ? &*ComputeTargets : nullptr,
						SceneTargets,
						GetCloudInput(PassParameters.Resources.CloudBaseDensity,
							PassParameters.Resources.CloudBaseDensityCompute),
						GetCloudInput(PassParameters.Resources.CloudDetailDensity,
							PassParameters.Resources.CloudDetailDensityCompute),
						GetCloudInput(PassParameters.Resources.CloudWeather,
							PassParameters.Resources.CloudWeatherCompute),
						Width, Height, bWantsProductionDeferred,
						Resolver.ReadValue(PassParameters.GBufferCompletion).IsComplete());
			});
		return {.Completion = Channels.CloudShadow,
			.Fragment = GraphResources.VolumetricCloudShadowFragment,
			.Compute = GraphResources.VolumetricCloudShadowCompute};
	}

	auto FVolumetricCloudSpatialGraphContributor::AddPasses(
		const FCloudSpatialGraphInputs& Inputs) -> FCloudSpatialGraphOutput
	{
		auto& Graph = Inputs.Graph;
		auto& Services = Inputs.Services;
		const auto RecordInputs = Inputs.Record;
		const auto PreparedCloudRoute = Inputs.Route;
		auto* CloudWeatherTexture = Inputs.WeatherTexture;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		FSceneFrameTopology Topology;
		Topology.VolumetricCloud = Inputs.GraphRoute;
		Topology.VolumetricCloudExtent = Inputs.Extent;
		Topology.bVolumetricCloudComposite = Inputs.bComposite;
		struct {
			FRDGTextureHandle SceneColor;
			FRDGTextureHandle SceneDepth;
			std::optional<FRDGTextureHandle> VolumetricCloudBaseDensity;
			std::optional<FRDGTextureHandle> VolumetricCloudDetailDensity;
			std::optional<FRDGTextureHandle> VolumetricCloudWeather;
			std::optional<FRDGTextureHandle> VolumetricCloudFragment;
			std::optional<FRDGTextureHandle> VolumetricCloudCompute;
			std::optional<FRDGTextureHandle> VolumetricCloudComposite;
		} GraphResources;
		GraphResources.SceneColor = Inputs.BaseScene.Color;
		GraphResources.SceneDepth = Inputs.BaseScene.Depth;
		GraphResources.VolumetricCloudBaseDensity = Inputs.BaseDensity;
		GraphResources.VolumetricCloudDetailDensity = Inputs.DetailDensity;
		GraphResources.VolumetricCloudWeather = Inputs.Weather;
		struct {
			TRDGValueHandle<FSceneColorPassResult> BaseScene;
			TRDGValueHandle<FVolumetricCloudSpatialPassResult>
				VolumetricCloudSpatial;
		} Channels;
		Channels.BaseScene = Inputs.BaseScene.Completion;
		Channels.VolumetricCloudSpatial = Graph.CreateValue<
			FVolumetricCloudSpatialPassResult>("Scene.VolumetricCloudSpatialValue",
				"volumetric-cloud-spatial-result");
		const uint32 CloudWidth = static_cast<uint32>(
			std::max(Topology.VolumetricCloudExtent.x, 0));
		const uint32 CloudHeight = static_cast<uint32>(
			std::max(Topology.VolumetricCloudExtent.y, 0));
		if (Topology.UsesCloudFragment())
			GraphResources.VolumetricCloudFragment = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudFragment", CloudWidth, CloudHeight,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback)
					.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f)),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::VolumetricCloudFragment)},
				"Scene.VolumetricCloud.Fragment",
				ERHIAccess::GraphicsShaderRead);
		if (Topology.UsesCloudCompute())
			GraphResources.VolumetricCloudCompute = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudCompute", CloudWidth, CloudHeight,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::Storage
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::VolumetricCloudCompute)},
				"Scene.VolumetricCloud.Compute",
				ERHIAccess::GraphicsShaderRead);
		if (Topology.bVolumetricCloudComposite)
			GraphResources.VolumetricCloudComposite = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudComposite", Width, Height,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback)
					.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f)),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::VolumetricCloudComposite)},
				"Scene.VolumetricCloud.Composite",
				ERHIAccess::GraphicsShaderRead);
		const bool bCompute = PreparedCloudRoute
			== FVolumetricCloudRenderer::ERoute::Compute;
		auto Parameters = Graph.AllocParameters<
			FVolumetricCloudSpatialPassParameters>();
		Parameters->BaseScene = {.Value = Channels.BaseScene};
		Parameters->Completion = {.Value = Channels.VolumetricCloudSpatial};
		auto AssignCloudInput = [&](auto& Graphics, auto& Compute,
			const auto& Texture, FRHITexture* Physical) {
			if (!Texture || !Physical) return;
			FRDGTextureParameter Parameter{*Texture,
				{GetTextureAspects(Physical->GetFormat()), 0,
					Physical->GetNumMips(), 0, Physical->GetArraySize()}};
			if (bCompute) Compute = Parameter;
			else Graphics = Parameter;
		};
		if (Services.ResolvedFrame.VolumetricCloud)
		{
			AssignCloudInput(Parameters->Resources.CloudBaseDensity,
				Parameters->Resources.CloudBaseDensityCompute,
				GraphResources.VolumetricCloudBaseDensity,
				Services.ResolvedFrame.VolumetricCloud->Textures.BaseDensity);
			AssignCloudInput(Parameters->Resources.CloudDetailDensity,
				Parameters->Resources.CloudDetailDensityCompute,
				GraphResources.VolumetricCloudDetailDensity,
				Services.ResolvedFrame.VolumetricCloud->Textures.DetailDensity);
			AssignCloudInput(Parameters->Resources.CloudWeather,
				Parameters->Resources.CloudWeatherCompute,
				GraphResources.VolumetricCloudWeather, CloudWeatherTexture);
		}
		if (Topology.VolumetricCloud != ESceneFrameRoute::Disabled)
		{
			FRDGTextureParameter Depth{GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}};
			if (bCompute) Parameters->Resources.SceneDepthCompute = Depth;
			else Parameters->Resources.SceneDepth = Depth;
		}
		if (GraphResources.VolumetricCloudFragment)
			Parameters->Resources.CloudFragmentOutput = {
				*GraphResources.VolumetricCloudFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.VolumetricCloudCompute)
			Parameters->Resources.CloudComputeOutput = {
				*GraphResources.VolumetricCloudCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		(void)AddSceneFrameFeaturePass<FVolumetricCloudSpatialGraphContributor>(Graph,
			bCompute ? ERDGPassType::Compute : ERDGPassType::Graphics,
			std::move(Parameters),
			[&Services, RecordInputs, Topology](FRHICommandListImmediate& Commands,
				const FVolumetricCloudSpatialPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				std::optional<FVolumetricCloudRenderer::FTargets> FragmentTargets;
				if (PassParameters.Resources.CloudFragmentOutput)
					FragmentTargets = {.Cloud = Resolver.GetTexture(
						PassParameters.Resources.CloudFragmentOutput)};
				std::optional<FVolumetricCloudRenderer::FComputeTargets> ComputeTargets;
				if (PassParameters.Resources.CloudComputeOutput)
					ComputeTargets = {.Cloud = Resolver.GetTexture(
						PassParameters.Resources.CloudComputeOutput)};
				auto GetCloudInput = [&](const auto& Graphics, const auto& Compute) {
					FRHITexture* Texture = Resolver.GetTexture(Graphics);
					return Texture != nullptr ? Texture : Resolver.GetTexture(Compute);
				};
				const FVolumetricCloudTimingQuerySink TimingSink =
					GetVolumetricCloudTimingQuerySink();
				TScopedRendererGPUTimingQuery Timing(Commands, TimingSink);
				Resolver.WriteValue(PassParameters.Completion) =
					Services.Recorders.RenderVolumetricCloudSpatial_RenderThread(
						Commands,
						RecordInputs,
						FragmentTargets ? &*FragmentTargets : nullptr,
						ComputeTargets ? &*ComputeTargets : nullptr,
						GetCloudInput(PassParameters.Resources.CloudBaseDensity,
							PassParameters.Resources.CloudBaseDensityCompute),
						GetCloudInput(PassParameters.Resources.CloudDetailDensity,
							PassParameters.Resources.CloudDetailDensityCompute),
						GetCloudInput(PassParameters.Resources.CloudWeather,
							PassParameters.Resources.CloudWeatherCompute),
					Topology.VolumetricCloud != ESceneFrameRoute::Disabled
							? GetCloudInput(PassParameters.Resources.SceneDepth,
								PassParameters.Resources.SceneDepthCompute) : nullptr);
				Timing.Commit();
			});
		return {.Completion = Channels.VolumetricCloudSpatial,
			.Fragment = GraphResources.VolumetricCloudFragment,
			.Compute = GraphResources.VolumetricCloudCompute,
			.Composite = GraphResources.VolumetricCloudComposite};
	}

	auto FVolumetricCloudCompositeGraphContributor::AddPasses(
		const FCloudCompositeGraphInputs& Inputs) -> FCloudCompositeGraphOutput
	{
		auto& Graph = Inputs.Graph;
		auto& Services = Inputs.Services;
		const auto RecordInputs = Inputs.Record;
		auto* CloudWeatherTexture = Inputs.WeatherTexture;
		FSceneFrameTopology Topology;
		Topology.bVolumetricCloudComposite = Inputs.bEnabled;
		struct {
			FRDGTextureHandle SceneColor;
			FRDGTextureHandle SceneDepth;
			std::optional<FRDGTextureHandle> VolumetricCloudBaseDensity;
			std::optional<FRDGTextureHandle> VolumetricCloudDetailDensity;
			std::optional<FRDGTextureHandle> VolumetricCloudWeather;
			std::optional<FRDGTextureHandle>
				VolumetricCloudShadowFragment;
			std::optional<FRDGTextureHandle>
				VolumetricCloudShadowCompute;
			std::optional<FRDGTextureHandle> VolumetricCloudFragment;
			std::optional<FRDGTextureHandle> VolumetricCloudCompute;
			std::optional<FRDGTextureHandle> VolumetricCloudComposite;
		} GraphResources;
		GraphResources.SceneColor = Inputs.BaseScene.Color;
		GraphResources.SceneDepth = Inputs.BaseScene.Depth;
		GraphResources.VolumetricCloudBaseDensity = Inputs.BaseDensity;
		GraphResources.VolumetricCloudDetailDensity = Inputs.DetailDensity;
		GraphResources.VolumetricCloudWeather = Inputs.Weather;
		GraphResources.VolumetricCloudFragment = Inputs.Spatial.Fragment;
		GraphResources.VolumetricCloudCompute = Inputs.Spatial.Compute;
		GraphResources.VolumetricCloudComposite = Inputs.Spatial.Composite;
		GraphResources.VolumetricCloudShadowFragment = Inputs.CloudShadow.Fragment;
		GraphResources.VolumetricCloudShadowCompute = Inputs.CloudShadow.Compute;
		struct {
			TRDGValueHandle<FSceneColorPassResult> BaseScene;
			TRDGValueHandle<FVolumetricCloudSpatialPassResult>
				VolumetricCloudSpatial;
			TRDGValueHandle<FVolumetricCloudShadowPassResult> CloudShadow;
			TRDGValueHandle<FVolumetricCloudPassResult> VolumetricCloud;
		} Channels;
		Channels.BaseScene = Inputs.BaseScene.Completion;
		Channels.VolumetricCloudSpatial = Inputs.Spatial.Completion;
		Channels.CloudShadow = Inputs.CloudShadow.Completion;
		Channels.VolumetricCloud = Graph.CreateValue<
			FVolumetricCloudPassResult>("Scene.VolumetricCloudValue",
				"volumetric-cloud-result");
		auto Parameters = Graph.AllocParameters<
			FVolumetricCloudCompositePassParameters>();
		Parameters->BaseScene = {.Value = Channels.BaseScene};
		Parameters->Spatial = {.Value = Channels.VolumetricCloudSpatial};
		Parameters->CloudShadow = {.Value = Channels.CloudShadow};
		Parameters->Completion = {.Value = Channels.VolumetricCloud};
		if (Topology.bVolumetricCloudComposite)
		{
			Parameters->Resources.SceneColor = {GraphResources.SceneColor,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
			Parameters->Resources.SceneDepth = {GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}};
			auto AssignCloudInput = [](auto& Parameter, const auto& Texture,
				FRHITexture* Physical) {
				if (!Texture || !Physical) return;
				Parameter = FRDGTextureParameter{*Texture,
					{GetTextureAspects(Physical->GetFormat()), 0,
						Physical->GetNumMips(), 0, Physical->GetArraySize()}};
			};
			if (Services.ResolvedFrame.VolumetricCloud)
			{
				AssignCloudInput(Parameters->Resources.CloudBaseDensity,
					GraphResources.VolumetricCloudBaseDensity,
					Services.ResolvedFrame.VolumetricCloud->Textures.BaseDensity);
				AssignCloudInput(Parameters->Resources.CloudDetailDensity,
					GraphResources.VolumetricCloudDetailDensity,
					Services.ResolvedFrame.VolumetricCloud->Textures.DetailDensity);
				AssignCloudInput(Parameters->Resources.CloudWeather,
					GraphResources.VolumetricCloudWeather, CloudWeatherTexture);
			}
		}
		if (GraphResources.VolumetricCloudShadowFragment)
			Parameters->Resources.CloudShadowFragment = {
				*GraphResources.VolumetricCloudShadowFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.VolumetricCloudShadowCompute)
			Parameters->Resources.CloudShadowCompute = {
				*GraphResources.VolumetricCloudShadowCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.VolumetricCloudFragment)
			Parameters->Resources.CloudFragment = {
				*GraphResources.VolumetricCloudFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.VolumetricCloudCompute)
			Parameters->Resources.CloudCompute = {
				*GraphResources.VolumetricCloudCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.VolumetricCloudComposite)
			Parameters->Resources.CloudCompositeOutput = {
				*GraphResources.VolumetricCloudComposite,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		(void)AddSceneFrameFeaturePass<FVolumetricCloudCompositeGraphContributor>(
			Graph, ERDGPassType::Graphics, std::move(Parameters),
			[&Services, RecordInputs, Topology](
				FRHICommandListImmediate& Commands,
				const FVolumetricCloudCompositePassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				if (!Topology.bVolumetricCloudComposite) return;
				std::optional<FVolumetricCloudRenderer::FTargets> FragmentTargets;
				if (PassParameters.Resources.CloudFragment)
					FragmentTargets = {.Cloud = Resolver.GetTexture(
						PassParameters.Resources.CloudFragment)};
				std::optional<FVolumetricCloudRenderer::FComputeTargets> ComputeTargets;
				if (PassParameters.Resources.CloudCompute)
					ComputeTargets = {.Cloud = Resolver.GetTexture(
						PassParameters.Resources.CloudCompute)};
				std::optional<FVolumetricCloudRenderer::FTargets> CompositeTargets;
				if (PassParameters.Resources.CloudCompositeOutput)
					CompositeTargets = {.Cloud = Resolver.GetTexture(
						PassParameters.Resources.CloudCompositeOutput)};
				FRHITexture* ShadowVisibility = nullptr;
				const auto& CloudShadowResult = Resolver.ReadValue(
					PassParameters.CloudShadow);
				if (CloudShadowResult.Route
					== EVolumetricCloudShadowPassRoute::Compute
					&& PassParameters.Resources.CloudShadowCompute)
					ShadowVisibility = Resolver.GetTexture(
						PassParameters.Resources.CloudShadowCompute);
				else if (CloudShadowResult.Route
					== EVolumetricCloudShadowPassRoute::Fragment
					&& PassParameters.Resources.CloudShadowFragment)
					ShadowVisibility = Resolver.GetTexture(
						PassParameters.Resources.CloudShadowFragment);
				Resolver.WriteValue(PassParameters.Completion) =
					Services.Recorders.RenderVolumetricCloudComposite_RenderThread(
						Commands,
						RecordInputs,
						Resolver.ReadValue(PassParameters.Spatial),
						FragmentTargets ? &*FragmentTargets : nullptr,
						ComputeTargets ? &*ComputeTargets : nullptr,
						CompositeTargets ? &*CompositeTargets : nullptr,
						Resolver.GetTexture(PassParameters.Resources.SceneColor),
						Resolver.GetTexture(PassParameters.Resources.SceneDepth),
						ShadowVisibility);
			});
		return {.Completion = Channels.VolumetricCloud,
			.Composite = GraphResources.VolumetricCloudComposite};
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
			TransientTargets.GetObservedRetainedBytes_RenderThread(
				ERDGAllocationObservation::VolumetricCloudShadowFragment)
			+ TransientTargets.GetObservedRetainedBytes_RenderThread(
				ERDGAllocationObservation::VolumetricCloudShadowCompute);
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
			TransientTargets.GetObservedRetainedBytes_RenderThread(
				ERDGAllocationObservation::VolumetricCloudFragment)
			+ TransientTargets.GetObservedRetainedBytes_RenderThread(
				ERDGAllocationObservation::VolumetricCloudCompute)
			+ TransientTargets.GetObservedRetainedBytes_RenderThread(
				ERDGAllocationObservation::VolumetricCloudComposite);
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
