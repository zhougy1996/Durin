#include "Renderers/VolumetricCloudRendering.h"
#include "Renderers/BaseSceneRendering.h"
#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	#define DURIN_RESOURCE_MEMBER(Field, Wrapper, Kind, Use, Access, ...) \
		MakeRDGResourceParameterMemberMetadata<FParameters, \
			decltype(FParameters::Field), Wrapper>(#Field, offsetof(FParameters, Field), \
				Kind, ERDGResourceKind::Texture, \
				ERDGParameterRangeKind::TextureSubresource, Use, Access \
				__VA_OPT__(,) __VA_ARGS__)
	#define DURIN_TEXTURE(Field, Use, Access, ...) \
		DURIN_RESOURCE_MEMBER(Field, FRDGTextureParameter, \
			ERDGParameterMemberKind::Texture, Use, Access __VA_OPT__(,) __VA_ARGS__)
	#define DURIN_MANAGED_TEXTURE(Field) \
		DURIN_RESOURCE_MEMBER(Field, FRDGManagedTextureParameter, \
			ERDGParameterMemberKind::ManagedTexture, ERDGUse::ReadWrite, \
			ERHIAccess::GraphicsShaderRead, true, ERHIRenderTargetLoadAction::Load, \
			ERHIRenderTargetStoreAction::Store, true, \
			ERHIAccess::GraphicsShaderRead)
	#define DURIN_DEFINE_METADATA(TypeName, ...) \
		auto TypeName::GetRDGParametersMetadata() -> const FRDGParametersMetadata* \
		{ using FParameters = TypeName; static const std::array Members = {__VA_ARGS__}; \
		static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>( \
			#TypeName, Members); return &Metadata; }
	#define DURIN_GRAPHICS_READ(Field) DURIN_TEXTURE(Field, ERDGUse::Read, \
		ERHIAccess::GraphicsShaderRead)
	#define DURIN_COMPUTE_READ(Field) DURIN_TEXTURE(Field, ERDGUse::Read, \
		ERHIAccess::ComputeShaderRead)

	DURIN_DEFINE_METADATA(FVolumetricCloudShadowPassResources,
		DURIN_GRAPHICS_READ(SceneDepth), DURIN_COMPUTE_READ(SceneDepthCompute),
		DURIN_GRAPHICS_READ(CloudBaseDensity),
		DURIN_GRAPHICS_READ(CloudDetailDensity), DURIN_GRAPHICS_READ(CloudWeather),
		DURIN_COMPUTE_READ(CloudBaseDensityCompute),
		DURIN_COMPUTE_READ(CloudDetailDensityCompute),
		DURIN_COMPUTE_READ(CloudWeatherCompute),
		DURIN_MANAGED_TEXTURE(CloudShadowFragmentOutput),
		DURIN_TEXTURE(CloudShadowComputeOutput, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true));
	DURIN_DEFINE_METADATA(FVolumetricCloudSpatialPassResources,
		DURIN_GRAPHICS_READ(SceneDepth), DURIN_COMPUTE_READ(SceneDepthCompute),
		DURIN_GRAPHICS_READ(CloudBaseDensity),
		DURIN_GRAPHICS_READ(CloudDetailDensity), DURIN_GRAPHICS_READ(CloudWeather),
		DURIN_COMPUTE_READ(CloudBaseDensityCompute),
		DURIN_COMPUTE_READ(CloudDetailDensityCompute),
		DURIN_COMPUTE_READ(CloudWeatherCompute),
		DURIN_MANAGED_TEXTURE(CloudFragmentOutput),
		DURIN_TEXTURE(CloudComputeOutput, ERDGUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true));
	DURIN_DEFINE_METADATA(FVolumetricCloudCompositePassResources,
		DURIN_GRAPHICS_READ(SceneColor), DURIN_GRAPHICS_READ(SceneDepth),
		DURIN_GRAPHICS_READ(CloudBaseDensity),
		DURIN_GRAPHICS_READ(CloudDetailDensity), DURIN_GRAPHICS_READ(CloudWeather),
		DURIN_GRAPHICS_READ(CloudShadowFragment),
		DURIN_GRAPHICS_READ(CloudShadowCompute), DURIN_GRAPHICS_READ(CloudFragment),
		DURIN_GRAPHICS_READ(CloudCompute),
		DURIN_MANAGED_TEXTURE(CloudCompositeOutput));

	#define DURIN_VALUE(Field, Type) \
		MakeRDGValueParameterMemberMetadata<FParameters, \
			decltype(FParameters::Field), Type>(#Field, offsetof(FParameters, Field))
	#define DURIN_NESTED(Type) MakeRDGNestedParameterMemberMetadata<FParameters, \
		decltype(FParameters::Resources)>("Resources", offsetof(FParameters, Resources), \
			Type::GetRDGParametersMetadata())
	DURIN_DEFINE_METADATA(FVolumetricCloudShadowPassParameters,
		DURIN_VALUE(GBufferCompletion, FGBufferPassResult),
		DURIN_VALUE(Completion, FVolumetricCloudShadowPassResult),
		DURIN_NESTED(FVolumetricCloudShadowPassResources));
	DURIN_DEFINE_METADATA(FVolumetricCloudSpatialPassParameters,
		DURIN_VALUE(BaseScene, FSceneColorPassResult),
		DURIN_VALUE(Completion, FVolumetricCloudSpatialPassResult),
		DURIN_NESTED(FVolumetricCloudSpatialPassResources));
	DURIN_DEFINE_METADATA(FVolumetricCloudCompositePassParameters,
		DURIN_VALUE(BaseScene, FSceneColorPassResult),
		DURIN_VALUE(Spatial, FVolumetricCloudSpatialPassResult),
		DURIN_VALUE(CloudShadow, FVolumetricCloudShadowPassResult),
		DURIN_VALUE(Completion, FVolumetricCloudPassResult),
		DURIN_NESTED(FVolumetricCloudCompositePassResources));

	#undef DURIN_NESTED
	#undef DURIN_VALUE
	#undef DURIN_COMPUTE_READ
	#undef DURIN_GRAPHICS_READ
	#undef DURIN_DEFINE_METADATA
	#undef DURIN_MANAGED_TEXTURE
	#undef DURIN_TEXTURE
	#undef DURIN_RESOURCE_MEMBER

	namespace
	{
		struct FCloudShadowRecorder final
		{
			FRendererRDGAllocator& RDGAllocator;
			FVolumetricCloudShadowRenderer& VolumetricCloudShadowRenderer;
			FRendererQualificationPolicy Qualification;
			FSceneRenderTelemetry& Telemetry;
			FResolvedSceneResources& ResolvedSceneResources;

			auto RenderVolumetricCloudShadows_RenderThread(
				FRHICommandListImmediate&, const FVolumetricCloudShadowRecordInputs&,
				const FVolumetricCloudShadowRenderer::FRouteDecision&,
				const FVolumetricCloudShadowRenderer::FTargets*,
				const FVolumetricCloudShadowRenderer::FComputeTargets*,
				const FPostProcessRenderer::FSceneTargets&, FRHITexture*, FRHITexture*,
				FRHITexture*, uint32, uint32, bool, bool)
				-> FVolumetricCloudShadowPassResult;
		};

		struct FCloudSpatialRecorder final
		{
			FRendererRDGAllocator& RDGAllocator;
			FVolumetricCloudRenderer& VolumetricCloudRenderer;
			FRendererQualificationPolicy Qualification;
			FSceneRenderTelemetry& Telemetry;
			FResolvedSceneResources& ResolvedSceneResources;
			FSceneViewTemporalContext& TemporalContext;
			FSceneViewState*& ViewState;

			auto RenderVolumetricCloudSpatial_RenderThread(
				FRHICommandListImmediate&, const FVolumetricCloudRecordInputs&,
				const FVolumetricCloudSpatialRenderer::FRouteDecision&,
				const FVolumetricCloudRenderer::FTargets*,
				const FVolumetricCloudRenderer::FComputeTargets*, FRHITexture*,
				FRHITexture*, FRHITexture*, FRHITexture*)
				-> FVolumetricCloudSpatialPassResult;
		};

		struct FCloudCompositeRecorder final
		{
			FRendererRDGAllocator& RDGAllocator;
			FVolumetricCloudRenderer& VolumetricCloudRenderer;
			FSceneRenderTelemetry& Telemetry;
			FSceneViewTemporalContext& TemporalContext;
			FSceneViewState*& ViewState;

			auto RenderVolumetricCloudComposite_RenderThread(
				FRHICommandListImmediate&, const FVolumetricCloudRecordInputs&,
				const FVolumetricCloudSpatialPassResult&,
				const FVolumetricCloudRenderer::FTargets*,
				const FVolumetricCloudRenderer::FComputeTargets*,
				const FVolumetricCloudRenderer::FTargets*, FRHITexture*, FRHITexture*,
				FRHITexture*) -> FVolumetricCloudPassResult;
		};
	} // namespace

	auto FVolumetricCloudShadowRendering::AddPasses(
		const FCloudShadowFeatureInputs& Inputs) -> FCloudShadowGraphOutput
	{
		auto& Graph = Inputs.Graph;
		FCloudShadowRecorder Recorder{Inputs.Allocator, Inputs.Renderer,
			Inputs.Qualification, Inputs.Telemetry, Inputs.Resolved};
		const auto RecordInputs = Inputs.Record;
		const auto PreparedCloudShadowRoute = Inputs.Feature.Decision.Route;
		const auto PreparedCloudShadowDecision = Inputs.Feature.Decision;
		auto* CloudWeatherTexture = Inputs.WeatherTexture;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		const bool bWantsProductionDeferred =
			Inputs.DeferredFeature.HasPurpose(ESceneFeaturePurpose::Production);
		std::optional<FRDGTextureHandle> VolumetricCloudShadowFragment;
		std::optional<FRDGTextureHandle> VolumetricCloudShadowCompute;
		const auto CloudShadowCompletion = Graph.CreateValue<
			FVolumetricCloudShadowPassResult>(
				"Scene.CloudShadowValue", "cloud-shadow-result");
		if (PreparedCloudShadowRoute
			== FVolumetricCloudShadowRenderer::ERoute::Fragment)
			VolumetricCloudShadowFragment = Graph.CreateTexture(
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
		if (PreparedCloudShadowRoute
			== FVolumetricCloudShadowRenderer::ERoute::Compute)
			VolumetricCloudShadowCompute = Graph.CreateTexture(
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
		Parameters->GBufferCompletion = {.Value = Inputs.GBuffer.Completion};
		Parameters->Completion = {.Value = CloudShadowCompletion};
		if (PreparedCloudShadowRoute
			!= FVolumetricCloudShadowRenderer::ERoute::FactorOne)
		{
			FRDGTextureParameter Depth{Inputs.SceneDepth,
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
		if (Inputs.Resolved.VolumetricCloud)
		{
			AssignCloudInput(Parameters->Resources.CloudBaseDensity,
				Parameters->Resources.CloudBaseDensityCompute,
				Inputs.BaseDensity,
				Inputs.Resolved.VolumetricCloud->Textures.BaseDensity);
			AssignCloudInput(Parameters->Resources.CloudDetailDensity,
				Parameters->Resources.CloudDetailDensityCompute,
				Inputs.DetailDensity,
				Inputs.Resolved.VolumetricCloud->Textures.DetailDensity);
			AssignCloudInput(Parameters->Resources.CloudWeather,
				Parameters->Resources.CloudWeatherCompute,
				Inputs.Weather, CloudWeatherTexture);
		}
		if (VolumetricCloudShadowFragment)
			Parameters->Resources.CloudShadowFragmentOutput = {
				*VolumetricCloudShadowFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (VolumetricCloudShadowCompute)
			Parameters->Resources.CloudShadowComputeOutput = {
				*VolumetricCloudShadowCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		(void)Graph.AddPass(Name,
			bCompute ? ERDGPassType::Compute : ERDGPassType::Graphics,
			std::move(Parameters),
			[Recorder, RecordInputs, PreparedCloudShadowDecision,
				PreparedCloudShadowRoute, Width, Height,
				bWantsProductionDeferred](FRHICommandListImmediate& Commands,
				const FVolumetricCloudShadowPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) mutable {
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
					.Depth = PreparedCloudShadowRoute
							!= FVolumetricCloudShadowRenderer::ERoute::FactorOne
						? Depth : nullptr};
				auto GetCloudInput = [&](const auto& Graphics, const auto& Compute) {
					FRHITexture* Texture = Resolver.GetTexture(Graphics);
					return Texture != nullptr ? Texture : Resolver.GetTexture(Compute);
				};
				Resolver.WriteValue(PassParameters.Completion) =
					Recorder.RenderVolumetricCloudShadows_RenderThread(
						Commands,
						RecordInputs, PreparedCloudShadowDecision,
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
		return {.Completion = CloudShadowCompletion,
			.Fragment = VolumetricCloudShadowFragment,
			.Compute = VolumetricCloudShadowCompute};
	}

	auto FVolumetricCloudSpatialRendering::AddPasses(
		const FCloudSpatialFeatureInputs& Inputs) -> FCloudSpatialGraphOutput
	{
		auto& Graph = Inputs.Graph;
		FCloudSpatialRecorder Recorder{Inputs.Allocator, Inputs.Renderer,
			Inputs.Qualification, Inputs.Telemetry,
			Inputs.Resolved, Inputs.Temporal, Inputs.ViewState};
		const auto RecordInputs = Inputs.Record;
		const auto PreparedCloudRoute = Inputs.Feature.Decision.Route;
		const auto PreparedCloudDecision = Inputs.Feature.Decision;
		auto* CloudWeatherTexture = Inputs.WeatherTexture;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		std::optional<FRDGTextureHandle> VolumetricCloudFragment;
		std::optional<FRDGTextureHandle> VolumetricCloudCompute;
		std::optional<FRDGTextureHandle> VolumetricCloudComposite;
		const auto VolumetricCloudSpatialCompletion = Graph.CreateValue<
			FVolumetricCloudSpatialPassResult>("Scene.VolumetricCloudSpatialValue",
				"volumetric-cloud-spatial-result");
		const uint32 CloudWidth = static_cast<uint32>(
			std::max(Inputs.Feature.Extent.x, 0));
		const uint32 CloudHeight = static_cast<uint32>(
			std::max(Inputs.Feature.Extent.y, 0));
		if (PreparedCloudRoute == FVolumetricCloudRenderer::ERoute::Fragment)
			VolumetricCloudFragment = Graph.CreateTexture(
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
		if (PreparedCloudRoute == FVolumetricCloudRenderer::ERoute::Compute)
			VolumetricCloudCompute = Graph.CreateTexture(
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
		if (PreparedCloudRoute != FVolumetricCloudRenderer::ERoute::Disabled)
			VolumetricCloudComposite = Graph.CreateTexture(
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
		Parameters->BaseScene = {.Value = Inputs.BaseScene.Completion};
		Parameters->Completion = {.Value = VolumetricCloudSpatialCompletion};
		auto AssignCloudInput = [&](auto& Graphics, auto& Compute,
			const auto& Texture, FRHITexture* Physical) {
			if (!Texture || !Physical) return;
			FRDGTextureParameter Parameter{*Texture,
				{GetTextureAspects(Physical->GetFormat()), 0,
					Physical->GetNumMips(), 0, Physical->GetArraySize()}};
			if (bCompute) Compute = Parameter;
			else Graphics = Parameter;
		};
		if (Inputs.Resolved.VolumetricCloud)
		{
			AssignCloudInput(Parameters->Resources.CloudBaseDensity,
				Parameters->Resources.CloudBaseDensityCompute,
				Inputs.BaseDensity,
				Inputs.Resolved.VolumetricCloud->Textures.BaseDensity);
			AssignCloudInput(Parameters->Resources.CloudDetailDensity,
				Parameters->Resources.CloudDetailDensityCompute,
				Inputs.DetailDensity,
				Inputs.Resolved.VolumetricCloud->Textures.DetailDensity);
			AssignCloudInput(Parameters->Resources.CloudWeather,
				Parameters->Resources.CloudWeatherCompute,
				Inputs.Weather, CloudWeatherTexture);
		}
		if (PreparedCloudRoute != FVolumetricCloudRenderer::ERoute::Disabled)
		{
			FRDGTextureParameter Depth{Inputs.BaseScene.Depth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}};
			if (bCompute) Parameters->Resources.SceneDepthCompute = Depth;
			else Parameters->Resources.SceneDepth = Depth;
		}
		if (VolumetricCloudFragment)
			Parameters->Resources.CloudFragmentOutput = {
				*VolumetricCloudFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (VolumetricCloudCompute)
			Parameters->Resources.CloudComputeOutput = {
				*VolumetricCloudCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		(void)Graph.AddPass(Name,
			bCompute ? ERDGPassType::Compute : ERDGPassType::Graphics,
			std::move(Parameters),
			[Recorder, RecordInputs, PreparedCloudDecision,
				PreparedCloudRoute](
				FRHICommandListImmediate& Commands,
				const FVolumetricCloudSpatialPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) mutable {
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
					Recorder.RenderVolumetricCloudSpatial_RenderThread(
						Commands,
						RecordInputs, PreparedCloudDecision,
						FragmentTargets ? &*FragmentTargets : nullptr,
						ComputeTargets ? &*ComputeTargets : nullptr,
						GetCloudInput(PassParameters.Resources.CloudBaseDensity,
							PassParameters.Resources.CloudBaseDensityCompute),
						GetCloudInput(PassParameters.Resources.CloudDetailDensity,
							PassParameters.Resources.CloudDetailDensityCompute),
						GetCloudInput(PassParameters.Resources.CloudWeather,
							PassParameters.Resources.CloudWeatherCompute),
					PreparedCloudRoute != FVolumetricCloudRenderer::ERoute::Disabled
							? GetCloudInput(PassParameters.Resources.SceneDepth,
								PassParameters.Resources.SceneDepthCompute) : nullptr);
				Timing.Commit();
			});
		return {.Completion = VolumetricCloudSpatialCompletion,
			.Fragment = VolumetricCloudFragment,
			.Compute = VolumetricCloudCompute,
			.Composite = VolumetricCloudComposite};
	}

	auto FVolumetricCloudCompositeRendering::AddPasses(
		const FCloudCompositeFeatureInputs& Inputs) -> FCloudCompositeGraphOutput
	{
		auto& Graph = Inputs.Graph;
		FCloudCompositeRecorder Recorder{Inputs.Allocator, Inputs.Renderer,
			Inputs.Telemetry, Inputs.Temporal, Inputs.ViewState};
		const auto RecordInputs = Inputs.Record;
		auto* CloudWeatherTexture = Inputs.WeatherTexture;
		const bool bEnabled = Inputs.Feature.Decision.Route
			!= FVolumetricCloudRenderer::ERoute::Disabled;
		const auto VolumetricCloudCompletion = Graph.CreateValue<
			FVolumetricCloudPassResult>("Scene.VolumetricCloudValue",
				"volumetric-cloud-result");
		auto Parameters = Graph.AllocParameters<
			FVolumetricCloudCompositePassParameters>();
		Parameters->BaseScene = {.Value = Inputs.BaseScene.Completion};
		Parameters->Spatial = {.Value = Inputs.Spatial.Completion};
		Parameters->CloudShadow = {.Value = Inputs.CloudShadow.Completion};
		Parameters->Completion = {.Value = VolumetricCloudCompletion};
		if (bEnabled)
		{
			Parameters->Resources.SceneColor = {Inputs.BaseScene.Color,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
			Parameters->Resources.SceneDepth = {Inputs.BaseScene.Depth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}};
			auto AssignCloudInput = [](auto& Parameter, const auto& Texture,
				FRHITexture* Physical) {
				if (!Texture || !Physical) return;
				Parameter = FRDGTextureParameter{*Texture,
					{GetTextureAspects(Physical->GetFormat()), 0,
						Physical->GetNumMips(), 0, Physical->GetArraySize()}};
			};
			if (Inputs.Resolved.VolumetricCloud)
			{
				AssignCloudInput(Parameters->Resources.CloudBaseDensity,
					Inputs.BaseDensity,
					Inputs.Resolved.VolumetricCloud->Textures.BaseDensity);
				AssignCloudInput(Parameters->Resources.CloudDetailDensity,
					Inputs.DetailDensity,
					Inputs.Resolved.VolumetricCloud->Textures.DetailDensity);
				AssignCloudInput(Parameters->Resources.CloudWeather,
					Inputs.Weather, CloudWeatherTexture);
			}
		}
		if (Inputs.CloudShadow.Fragment)
			Parameters->Resources.CloudShadowFragment = {
				*Inputs.CloudShadow.Fragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Inputs.CloudShadow.Compute)
			Parameters->Resources.CloudShadowCompute = {
				*Inputs.CloudShadow.Compute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Inputs.Spatial.Fragment)
			Parameters->Resources.CloudFragment = {
				*Inputs.Spatial.Fragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Inputs.Spatial.Compute)
			Parameters->Resources.CloudCompute = {
				*Inputs.Spatial.Compute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Inputs.Spatial.Composite)
			Parameters->Resources.CloudCompositeOutput = {
				*Inputs.Spatial.Composite,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		(void)Graph.AddPass(Name, ERDGPassType::Graphics, std::move(Parameters),
			[Recorder, RecordInputs, bEnabled](
				FRHICommandListImmediate& Commands,
				const FVolumetricCloudCompositePassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) mutable {
				if (!bEnabled) return;
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
					Recorder.RenderVolumetricCloudComposite_RenderThread(
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
		return {.Completion = VolumetricCloudCompletion,
			.Composite = Inputs.Spatial.Composite};
	}

	auto FCloudShadowRecorder::RenderVolumetricCloudShadows_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FVolumetricCloudShadowRecordInputs& Inputs,
		const FVolumetricCloudShadowRenderer::FRouteDecision& PreparedRoute,
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
			ResolvedSceneResources.VolumetricCloud
				? &*ResolvedSceneResources.VolumetricCloud : nullptr;
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
		const auto QualityTier = CanonicalizeVolumetricCloudQuality(
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
			{.PreparedRoute = PreparedRoute,
			 .bGraphManagedTextureAccess = true}
		);
		auto& ViewTelemetry = Telemetry.View;
		const size_t ReasonIndex = static_cast<size_t>(Result.Reason);
		if (ReasonIndex < ViewTelemetry.VolumetricCloud.VolumetricCloudShadowRouteReasons.size())
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowRouteReasons[ReasonIndex];
		ViewTelemetry.VolumetricCloud.VolumetricCloudShadowRetainedBytes =
			RDGAllocator.GetObservedRetainedBytes_RenderThread(
				ERDGAllocationObservation::VolumetricCloudShadowFragment)
			+ RDGAllocator.GetObservedRetainedBytes_RenderThread(
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

	auto FCloudSpatialRecorder::RenderVolumetricCloudSpatial_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FVolumetricCloudRecordInputs& Inputs,
		const FVolumetricCloudSpatialRenderer::FRouteDecision& PreparedRoute,
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
			ResolvedSceneResources.VolumetricCloud
				? &*ResolvedSceneResources.VolumetricCloud : nullptr;
		const bool bInputsPresent = Cloud != nullptr && ResolvedCloud != nullptr
									&& BaseDensity != nullptr
									&& DetailDensity != nullptr
									&& Weather != nullptr
									&& ResolvedCloud->Textures.DensitySampler != nullptr
									&& Depth != nullptr;
		const auto QualityTier = CanonicalizeVolumetricCloudQuality(
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
			{.PreparedRoute = PreparedRoute,
			 .bGraphManagedTextureAccess = true});
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

	auto FCloudCompositeRecorder::RenderVolumetricCloudComposite_RenderThread(
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
		const auto QualityTier = CanonicalizeVolumetricCloudQuality(
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
			RDGAllocator.GetObservedRetainedBytes_RenderThread(
				ERDGAllocationObservation::VolumetricCloudFragment)
			+ RDGAllocator.GetObservedRetainedBytes_RenderThread(
				ERDGAllocationObservation::VolumetricCloudCompute)
			+ RDGAllocator.GetObservedRetainedBytes_RenderThread(
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
