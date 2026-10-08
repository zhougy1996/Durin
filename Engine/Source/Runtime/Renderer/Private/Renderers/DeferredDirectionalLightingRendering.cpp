#include "Renderers/DeferredDirectionalLightingRendering.h"
#include "RDG/RDG.h"
#include "Renderers/SceneTextureGroupParameters.h"
#include "Renderers/VolumetricCloudRendering.h"
#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
#define DURIN_TEXTURE(Field)                                               \
	MakeRDGTextureReadMetadata<FParameters, decltype(FParameters::Field)>( \
		#Field, offsetof(FParameters, Field)                               \
	)
#define DURIN_DEFINE_METADATA(TypeName, ...)                                       \
	auto TypeName::GetRDGParametersMetadata() -> const FRDGParametersMetadata*     \
	{                                                                              \
		using FParameters = TypeName;                                              \
		static const std::array Members = {__VA_ARGS__};                           \
		static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>( \
			#TypeName, Members                                                     \
		);                                                                         \
		return &Metadata;                                                          \
	}

	DURIN_DEFINE_METADATA(FDeferredDirectionalLightingPassResources, DURIN_TEXTURE(DirectionalShadow), DURIN_TEXTURE(GBuffer), DURIN_TEXTURE(SceneDepth), DURIN_TEXTURE(AmbientOcclusion), DURIN_TEXTURE(ContactShadowFragment), DURIN_TEXTURE(ContactShadowCompute), DURIN_TEXTURE(CloudShadowFragment), DURIN_TEXTURE(CloudShadowCompute), DURIN_TEXTURE(DefaultWhite), DURIN_TEXTURE(DefaultShadowArray), DURIN_TEXTURE(EnvironmentIrradiance), DURIN_TEXTURE(EnvironmentPrefiltered), DURIN_TEXTURE(EnvironmentBrdfLut));

	DURIN_DEFINE_METADATA(FDeferredLightingInputParameters, MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::DirectionalShadow), FDirectionalShadowPassResult>("DirectionalShadow", offsetof(FParameters, DirectionalShadow)), MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::GBufferCompletion), FGBufferPassResult>("GBufferCompletion", offsetof(FParameters, GBufferCompletion)), MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::AmbientOcclusion), FGroundTruthAmbientOcclusionPassResult>("AmbientOcclusion", offsetof(FParameters, AmbientOcclusion)), MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::ContactShadow), FContactShadowVisibilityPassResult>("ContactShadow", offsetof(FParameters, ContactShadow)), MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::CloudShadow), FVolumetricCloudShadowPassResult>("CloudShadow", offsetof(FParameters, CloudShadow)), MakeRDGNestedParameterMemberMetadata<FParameters, decltype(FParameters::Resources)>("Resources", offsetof(FParameters, Resources), FDeferredDirectionalLightingPassResources::GetRDGParametersMetadata()));

	DURIN_DEFINE_METADATA(FDeferredDirectionalLightingPassParameters, MakeRDGNestedParameterMemberMetadata<FParameters, decltype(FParameters::Inputs)>("Inputs", offsetof(FParameters, Inputs), FDeferredLightingInputParameters::GetRDGParametersMetadata()), MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::Completion), FIsolatedDeferredPassResult>("Completion", offsetof(FParameters, Completion)), MakeRDGAttachmentMetadata<FParameters, decltype(FParameters::IsolatedDeferredOutput)>("IsolatedDeferredOutput", offsetof(FParameters, IsolatedDeferredOutput), ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store, ERHIAccess::GraphicsShaderRead));

#undef DURIN_DEFINE_METADATA
#undef DURIN_TEXTURE

	namespace
	{
		struct FDeferredDirectionalLightingRecorder final
		{
			FDeferredDirectionalLightingRenderer& DeferredDirectionalLightingRenderer;
			FSceneRenderTelemetry& Telemetry;
			FResolvedSceneResources& ResolvedSceneResources;

			auto RenderIsolatedDeferred_RenderThread(
				FRHICommandListImmediate&,
				const FDeferredDirectionalLightingRenderer::FTargets*,
				const FDeferredDirectionalLightingRenderer::FRenderParameters&,
				const FSceneViewRenderOptions&,
				uint32,
				uint32,
				bool
			)
				-> FIsolatedDeferredPassResult;
		};
	} // namespace

	auto AddDeferredDirectionalLightingPasses(
		FRDGBuilder& Graph, const FDeferredLightingFeatureInputs& Inputs
	)
		-> FDeferredLightingGraphOutput
	{
		FDeferredDirectionalLightingRecorder Recorder{
			Inputs.Renderer, Inputs.Telemetry, Inputs.Resolved
		};
		const auto& RecordView = Inputs.View;
		const auto& Options = Inputs.Options;
		auto* DirectionalShadowTexture =
			Inputs.DirectionalShadowRenderer.GetTexture_RenderThread();
		auto* EnvironmentSampler = Inputs.Environment.Sampler;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		const bool bWantsIsolatedDeferred =
			Inputs.Feature.HasPurpose(ESceneFeaturePurpose::Debug)
			|| Inputs.Feature.HasPurpose(ESceneFeaturePurpose::Qualification);
		const bool bWantsDeferredInputs = Inputs.Feature.IsEnabled()
										  || Inputs.AmbientOcclusionFeature.IsEnabled();
		const bool bWantsProductionDeferred =
			Inputs.Feature.HasPurpose(ESceneFeaturePurpose::Production);
		const bool bHybridRetainedResourcesReady =
			Inputs.bHybridRetainedResourcesReady;
		const bool bIsolated = bWantsIsolatedDeferred;
		const auto AmbientOcclusionQuality = Inputs.AmbientOcclusion.Quality;
		std::optional<FRDGTextureHandle> IsolatedDeferred;
		std::optional<TRDGValueHandle<FIsolatedDeferredPassResult>> DeferredDirectionalLightingCompletion;
		if (bIsolated)
			DeferredDirectionalLightingCompletion = Graph.CreateValue<FIsolatedDeferredPassResult>(
				"Scene.DeferredDirectionalLightingValue", "deferred-directional-lighting-result"
			);
		if (bIsolated)
			IsolatedDeferred = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D("DeferredDirectionalColor", Width, Height, EPixelFormat::RGBA16_FLOAT).SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy), .ObservationTag = static_cast<uint32>(ERDGAllocationObservation::DeferredDirectional)},
				"Scene.DeferredDirectionalLighting.Isolated"
			);
		auto Parameters = Graph.AllocParameters<
			FDeferredDirectionalLightingPassParameters>();
		Parameters->Inputs.DirectionalShadow = {
			.Value = Inputs.DirectionalShadow.Completion
		};
		if (Inputs.GBuffer.Completion)
			Parameters->Inputs.GBufferCompletion = TRDGValueRead<FGBufferPassResult>{
				.Value = *Inputs.GBuffer.Completion
			};
		if (Inputs.AmbientOcclusion.Completion)
			Parameters->Inputs.AmbientOcclusion = TRDGValueRead<FGroundTruthAmbientOcclusionPassResult>{
				.Value = *Inputs.AmbientOcclusion.Completion
			};
		if (Inputs.ContactShadow.Completion)
			Parameters->Inputs.ContactShadow = TRDGValueRead<FContactShadowVisibilityPassResult>{
				.Value = *Inputs.ContactShadow.Completion
			};
		if (Inputs.CloudShadow.Completion)
			Parameters->Inputs.CloudShadow = TRDGValueRead<FVolumetricCloudShadowPassResult>{
				.Value = *Inputs.CloudShadow.Completion
			};
		if (DeferredDirectionalLightingCompletion)
			Parameters->Completion = {.Value = *DeferredDirectionalLightingCompletion};
		SceneTextureGroups::FPersistentTextureReads PersistentReads;
		PersistentReads.Assign(Parameters->Inputs.Resources.DirectionalShadow, Inputs.DirectionalShadow.Shadow, DirectionalShadowTexture);
		if (Inputs.GBuffer.Textures)
		{
			SceneTextureGroups::FillGBuffer(Inputs.GBuffer.Textures, Parameters->Inputs.Resources.GBuffer);
			Parameters->Inputs.Resources.SceneDepth = {Inputs.GBuffer.Depth, {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		}
		if (Inputs.AmbientOcclusion.Textures)
			SceneTextureGroups::FillAmbientOcclusion(*Inputs.AmbientOcclusion.Textures, Parameters->Inputs.Resources.AmbientOcclusion);
		if (Inputs.ContactShadow.Fragment)
			Parameters->Inputs.Resources.ContactShadowFragment = {
				*Inputs.ContactShadow.Fragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}
			};
		if (Inputs.ContactShadow.Compute)
			Parameters->Inputs.Resources.ContactShadowCompute = {
				*Inputs.ContactShadow.Compute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}
			};
		if (Inputs.CloudShadow.Fragment)
			Parameters->Inputs.Resources.CloudShadowFragment = {
				*Inputs.CloudShadow.Fragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}
			};
		if (Inputs.CloudShadow.Compute)
			Parameters->Inputs.Resources.CloudShadowCompute = {
				*Inputs.CloudShadow.Compute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}
			};
		PersistentReads.Assign(Parameters->Inputs.Resources.DefaultWhite, Inputs.DefaultWhite, Inputs.DefaultTextures.Get_RenderThread(EDefaultTexture::White));
		PersistentReads.Assign(Parameters->Inputs.Resources.DefaultShadowArray, Inputs.DefaultShadowArray, Inputs.DefaultTextures.GetArray_RenderThread());
		PersistentReads.Assign(Parameters->Inputs.Resources.EnvironmentIrradiance, Inputs.Environment.Irradiance, Inputs.Environment.SelectedIrradiance);
		PersistentReads.Assign(Parameters->Inputs.Resources.EnvironmentPrefiltered, Inputs.Environment.Prefiltered, Inputs.Environment.SelectedPrefiltered);
		PersistentReads.Assign(Parameters->Inputs.Resources.EnvironmentBrdfLut, Inputs.Environment.BrdfLut, Inputs.Environment.SelectedBrdfLut);
		if (IsolatedDeferred)
			Parameters->IsolatedDeferredOutput = {
				*IsolatedDeferred,
				{ERHITextureAspect::Color, 0, 1, 0, 1}
			};

		FDeferredLightingPolicy Policy{
			.PersistentAliases = {Inputs.DirectionalShadow.Shadow, Inputs.DefaultWhite, Inputs.DefaultShadowArray, Inputs.Environment.Irradiance, Inputs.Environment.Prefiltered, Inputs.Environment.BrdfLut},
			.EnvironmentSampler = EnvironmentSampler,
			.DirectionalShadowSampler = Inputs.DirectionalShadowRenderer.GetSampler_RenderThread(),
			.AmbientOcclusionQuality = AmbientOcclusionQuality,
			.bProduction = bWantsProductionDeferred,
			.bRetainedResourcesReady = bHybridRetainedResourcesReady
		};
		auto ProductionInputs = Parameters->Inputs;
		if (!bIsolated)
			return {.Inputs = ProductionInputs, .Policy = Policy};
		(void)Graph.AddPass(DeferredDirectionalLightingPassName, ERDGPassType::Graphics, std::move(Parameters), [Recorder, RecordView = &RecordView, &Options, Width, Height, bWantsDeferredInputs, bWantsIsolatedDeferred, Policy](FRHICommandListImmediate& Commands, const FDeferredDirectionalLightingPassParameters& PassParameters, const FRDGParameterResolver& Resolver) mutable {
			auto& DeferredResult = Resolver.WriteValue(PassParameters.Completion);
			auto IsolatedPolicy = Policy;
			IsolatedPolicy.bProduction = false;
			const auto DeferredParameters = bWantsDeferredInputs ? ResolveDeferredLightingParameters(Resolver, PassParameters.Inputs, IsolatedPolicy, *RecordView, Options, Recorder.ResolvedSceneResources.Lighting.UniformBuffer) : std::nullopt;
			if (DeferredParameters)
			{
				std::optional<FDeferredDirectionalLightingRenderer::FTargets>
					IsolatedTargets;
				if (PassParameters.IsolatedDeferredOutput)
					IsolatedTargets = {.Color = Resolver.GetColorAttachment(PassParameters.IsolatedDeferredOutput).Texture};
				DeferredResult = Recorder.RenderIsolatedDeferred_RenderThread(
					Commands, IsolatedTargets ? &*IsolatedTargets : nullptr,
					*DeferredParameters, Options, Width, Height,
					bWantsIsolatedDeferred
				);
			}
			else if (bWantsIsolatedDeferred)
			{
				DeferredResult.Status = EScenePassStatus::Failed;
				++Recorder.Telemetry.View.Deferred.DeferredDirectionalUnavailableViews;
			}
		});
		return {.Completion = DeferredDirectionalLightingCompletion, .Inputs = ProductionInputs, .Policy = Policy, .Isolated = IsolatedDeferred};
	}

	auto ResolveDeferredLightingParameters(const FRDGParameterResolver& Resolver, const FDeferredLightingInputParameters& PassParameters, const FDeferredLightingPolicy& Policy, const FSceneView& RenderView, const FSceneViewRenderOptions& Options, const FRHIUniformBufferRange& Lighting)
		-> std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>
	{
		const auto& DirectionalShadow = Resolver.ReadValue(
			PassParameters.DirectionalShadow
		);
		const auto* GBufferValue = Resolver.ReadValue(
			PassParameters.GBufferCompletion
		);
		const auto GBuffer = GBufferValue ? *GBufferValue : FGBufferPassResult{};
		const auto* AmbientOcclusionValue = Resolver.ReadValue(PassParameters.AmbientOcclusion);
		const auto AmbientOcclusion = AmbientOcclusionValue ? *AmbientOcclusionValue : FGroundTruthAmbientOcclusionPassResult{};
		const auto* ContactShadowValue = Resolver.ReadValue(PassParameters.ContactShadow);
		const auto ContactShadow = ContactShadowValue ? *ContactShadowValue : FContactShadowVisibilityPassResult{};
		const auto* CloudShadowValue = Resolver.ReadValue(PassParameters.CloudShadow);
		const auto CloudShadow = CloudShadowValue ? *CloudShadowValue : FVolumetricCloudShadowPassResult{};
		return ResolveDeferredLightingResources(Resolver, PassParameters.Resources, {DirectionalShadow, GBuffer, AmbientOcclusion, ContactShadow, CloudShadow}, Policy, RenderView, Options, Lighting);
	}

	auto ResolveDeferredLightingResources(const FRDGParameterResolver& Resolver, const FDeferredDirectionalLightingPassResources& Resources, const FDeferredLightingOutcomes& Outcomes, const FDeferredLightingPolicy& Policy, const FSceneView& RenderView, const FSceneViewRenderOptions& Options, const FRHIUniformBufferRange& Lighting)
		-> std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>
	{
		const auto& [DirectionalShadow, GBuffer, AmbientOcclusion, ContactShadow, CloudShadow] = Outcomes;
		const auto AmbientOcclusionQuality = Policy.AmbientOcclusionQuality;
		const auto GBufferTargets = SceneTextureGroups::ResolveGBuffer(
			Resolver, Resources.GBuffer
		);
		const FPostProcessRenderer::FSceneTargets SceneTargets{
			.Color = nullptr,
			.Depth = GBufferTargets ? Resolver.GetTexture(Resources.SceneDepth) : nullptr
		};
		const auto AmbientOcclusionTargets = SceneTextureGroups::ResolveOptionalAmbientOcclusion(
			Resolver, Resources.AmbientOcclusion, AmbientOcclusionQuality
		);
		std::optional<FContactShadowVisibilityRenderer::FTargets>
			FragmentContactTargets;
		if (Resources.ContactShadowFragment)
			FragmentContactTargets = {.Visibility = Resolver.GetTexture(Resources.ContactShadowFragment)};
		std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
			ComputeContactTargets;
		if (Resources.ContactShadowCompute)
			ComputeContactTargets = {.Visibility = Resolver.GetTexture(Resources.ContactShadowCompute)};
		std::optional<FVolumetricCloudShadowRenderer::FTargets>
			FragmentCloudShadowTargets;
		if (Resources.CloudShadowFragment)
			FragmentCloudShadowTargets = {.Visibility = Resolver.GetTexture(Resources.CloudShadowFragment)};
		std::optional<FVolumetricCloudShadowRenderer::FComputeTargets>
			ComputeCloudShadowTargets;
		if (Resources.CloudShadowCompute)
			ComputeCloudShadowTargets = {.Visibility = Resolver.GetTexture(Resources.CloudShadowCompute)};
		const auto ResolvePersistent = [&](const std::optional<FRDGTextureHandle>& Handle) -> FRHITexture* {
			if (!Handle) return nullptr;
			for (const auto* Field : {&Resources.DirectionalShadow, &Resources.DefaultWhite, &Resources.DefaultShadowArray, &Resources.EnvironmentIrradiance, &Resources.EnvironmentPrefiltered, &Resources.EnvironmentBrdfLut})
				if (*Field && (*Field)->Texture == *Handle) return Resolver.GetTexture(*Field);
			return nullptr;
		};
		FRHITexture* DirectionalShadowTexture = ResolvePersistent(Policy.PersistentAliases.DirectionalShadow);
		FRHITexture* White = ResolvePersistent(Policy.PersistentAliases.White);
		if (Policy.bProduction && !Policy.bRetainedResourcesReady) return std::nullopt;

		if (!GBuffer.IsComplete() || !GBufferTargets) return std::nullopt;
		const bool bAmbientOcclusionComplete = AmbientOcclusion.IsComplete()
											   && AmbientOcclusionTargets.has_value();
		FRHITexture* ContactVisibility = White;
		bool bContactVisibilityComplete = false;
		if (ContactShadow.IsComplete())
		{
			if (ContactShadow.Route == EContactShadowVisibilityPassRoute::Compute
				&& ComputeContactTargets.has_value())
			{
				ContactVisibility = ComputeContactTargets->Visibility;
				bContactVisibilityComplete = true;
			}
			else if (ContactShadow.Route == EContactShadowVisibilityPassRoute::Fragment
					 && FragmentContactTargets.has_value())
			{
				ContactVisibility = FragmentContactTargets->Visibility;
				bContactVisibilityComplete = true;
			}
		}
		FRHITexture* CloudShadowVisibility = White;
		bool bCloudShadowVisibilityComplete = false;
		if (CloudShadow.IsComplete())
		{
			if (CloudShadow.Route == EVolumetricCloudShadowPassRoute::Compute
				&& ComputeCloudShadowTargets.has_value())
			{
				CloudShadowVisibility = ComputeCloudShadowTargets->Visibility;
				bCloudShadowVisibilityComplete = true;
			}
			else if (CloudShadow.Route == EVolumetricCloudShadowPassRoute::Fragment
					 && FragmentCloudShadowTargets.has_value())
			{
				CloudShadowVisibility = FragmentCloudShadowTargets->Visibility;
				bCloudShadowVisibilityComplete = true;
			}
		}
		return FDeferredDirectionalLightingRenderer::FRenderParameters{
			.Material = GBufferTargets->Material,
			.Normals = GBufferTargets->Normals,
			.Surface = GBufferTargets->Surface,
			.Emissive = GBufferTargets->Emissive,
			.Depth = SceneTargets.Depth,
			.EnvironmentIrradiance = ResolvePersistent(Policy.PersistentAliases.Irradiance),
			.EnvironmentPrefiltered = ResolvePersistent(Policy.PersistentAliases.Prefiltered),
			.EnvironmentBrdfLut = ResolvePersistent(Policy.PersistentAliases.BrdfLut),
			.EnvironmentSampler = Policy.EnvironmentSampler,
			.DirectionalShadowTexture = DirectionalShadow.IsComplete()
												&& DirectionalShadowTexture != nullptr ?
											DirectionalShadowTexture :
											ResolvePersistent(Policy.PersistentAliases.ShadowArray),
			.DirectionalShadowSampler = DirectionalShadow.IsComplete() ? Policy.DirectionalShadowSampler : nullptr,
			.GroundTruthAmbientOcclusionRaw = bAmbientOcclusionComplete ? (AmbientOcclusion.bRawDiagnosticUsesScratch ? AmbientOcclusionTargets->Scratch.GetReference() : AmbientOcclusionTargets->Raw.GetReference()) : White,
			.GroundTruthAmbientOcclusionFiltered =
				bAmbientOcclusionComplete ? AmbientOcclusionTargets->Raw.GetReference() : White,
			.GroundTruthAmbientOcclusionResolved =
				bAmbientOcclusionComplete ? (AmbientOcclusion.bHalfResolution ? AmbientOcclusionTargets->Resolved.GetReference() : AmbientOcclusionTargets->Raw.GetReference()) : White,
			.GroundTruthAmbientOcclusionSelector =
				bAmbientOcclusionComplete && AmbientOcclusion.bHalfResolution ? AmbientOcclusionTargets->Selector.GetReference() : White,
			.ContactVisibility = ContactVisibility,
			.VolumetricCloudVisibility = CloudShadowVisibility,
			.Lighting = Lighting,
			.View = &RenderView,
			.DiagnosticMode = static_cast<uint32>(
				Policy.bProduction ? EDeferredDirectionalDebugMode::Disabled : Options.DeferredDirectionalDebugMode
			),
			.bGroundTruthAmbientOcclusionEnabled = bAmbientOcclusionComplete,
			.bGroundTruthAmbientOcclusionHalfResolution =
				AmbientOcclusion.bHalfResolution,
			.bContactVisibilityEnabled = bContactVisibilityComplete,
			.bContactVisibilityDebug = ContactShadow.bDebug,
			.bVolumetricCloudVisibilityEnabled =
				bCloudShadowVisibilityComplete
		};
	}

	auto FDeferredDirectionalLightingRecorder::RenderIsolatedDeferred_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FDeferredDirectionalLightingRenderer::FTargets* Targets,
		const FDeferredDirectionalLightingRenderer::FRenderParameters& DeferredParameters,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsIsolatedDeferred
	) -> FIsolatedDeferredPassResult
	{
		FIsolatedDeferredPassResult Result;
		if (bWantsIsolatedDeferred)
		{
			Result.Status = EScenePassStatus::Failed;
			if (Targets == nullptr)
				++Telemetry.View.Deferred.DeferredDirectionalUnavailableViews;
			else
			{
				auto Parameters = DeferredParameters;
				Parameters.GroundTruthAmbientOcclusionDebugMode =
					static_cast<uint32>(
						Options.GroundTruthAmbientOcclusionDebugMode
					);
				const FDeferredDirectionalTimingQuerySink DeferredTimingSink =
					GetDeferredDirectionalTimingQuerySink();
				TScopedRendererGPUTimingQuery DeferredTiming(
					CommandList, DeferredTimingSink
				);
				const bool bRendered =
					DeferredDirectionalLightingRenderer.Render_RenderThread(
						CommandList, *Targets, Parameters
					);
				DeferredTiming.End();
				if (bRendered)
				{
					Result.Status = EScenePassStatus::Complete;
					++Telemetry.View.Deferred.DeferredDirectionalEnabledViews;
					Telemetry.View.Deferred.DeferredDirectionalOutputBytes =
						FDeferredDirectionalLightingRenderer::
							CalculateTargetBytes(Width, Height);
					if (Options.DeferredDirectionalDebugMode
						!= EDeferredDirectionalDebugMode::Disabled)
					{
						++Telemetry.View.Deferred.DeferredDirectionalDebugViews;
					}
					DeferredTiming.Commit();
					const FDeferredDirectionalCaptureSink CaptureSink =
						GetDeferredDirectionalCaptureSink();
					if (CaptureSink != nullptr)
						CaptureSink(CommandList, Targets->Color);
					if (Options.GroundTruthAmbientOcclusionDebugMode
						!= EGroundTruthAmbientOcclusionDebugMode::Disabled)
					{
						Result.bOutputValid = true;
					}
				}
				else
				{
					++Telemetry.View.Deferred.DeferredDirectionalPassFailures;
				}
			}
		}
		return Result;
	}
} // namespace Durin
