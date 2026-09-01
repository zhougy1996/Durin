#include "Renderers/DeferredDirectionalLightingRendering.h"
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
	#define DURIN_RESOURCE_MEMBER(Field, Wrapper, Kind, Use, Access, ...) \
		MakeRDGResourceParameterMemberMetadata<FParameters, \
			decltype(FParameters::Field), Wrapper>(#Field, offsetof(FParameters, Field), \
				Kind, ERDGResourceKind::Texture, \
				ERDGParameterRangeKind::TextureSubresource, Use, Access \
				__VA_OPT__(,) __VA_ARGS__)
	#define DURIN_TEXTURE(Field) DURIN_RESOURCE_MEMBER(Field, FRDGTextureParameter, \
		ERDGParameterMemberKind::Texture, ERDGUse::Read, \
		ERHIAccess::GraphicsShaderRead)
	#define DURIN_DEFINE_METADATA(TypeName, ...) \
		auto TypeName::GetRDGParametersMetadata() -> const FRDGParametersMetadata* \
		{ using FParameters = TypeName; static const std::array Members = {__VA_ARGS__}; \
		static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>( \
			#TypeName, Members); return &Metadata; }

	DURIN_DEFINE_METADATA(FDeferredDirectionalLightingPassResources,
		DURIN_TEXTURE(DirectionalShadow), DURIN_TEXTURE(GBuffer),
		DURIN_TEXTURE(SceneDepth), DURIN_TEXTURE(AmbientOcclusion),
		DURIN_TEXTURE(ContactShadowFragment), DURIN_TEXTURE(ContactShadowCompute),
		DURIN_TEXTURE(CloudShadowFragment), DURIN_TEXTURE(CloudShadowCompute),
		DURIN_TEXTURE(DefaultWhite), DURIN_TEXTURE(DefaultShadowArray),
		DURIN_TEXTURE(EnvironmentIrradiance),
		DURIN_TEXTURE(EnvironmentPrefiltered), DURIN_TEXTURE(EnvironmentBrdfLut),
		DURIN_RESOURCE_MEMBER(IsolatedDeferredOutput,
			FRDGColorAttachmentParameter,
			ERDGParameterMemberKind::ManagedColorAttachment, ERDGUse::ReadWrite,
			ERHIAccess::ColorAttachmentReadWrite, true,
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store, true,
			ERHIAccess::GraphicsShaderRead));

	DURIN_DEFINE_METADATA(FDeferredDirectionalLightingPassParameters,
		MakeRDGValueParameterMemberMetadata<FParameters,
			decltype(FParameters::DirectionalShadow), FDirectionalShadowPassResult>(
				"DirectionalShadow", offsetof(FParameters, DirectionalShadow)),
		MakeRDGValueParameterMemberMetadata<FParameters,
			decltype(FParameters::GBufferCompletion), FGBufferPassResult>(
				"GBufferCompletion", offsetof(FParameters, GBufferCompletion)),
		MakeRDGValueParameterMemberMetadata<FParameters,
			decltype(FParameters::AmbientOcclusion),
			FGroundTruthAmbientOcclusionPassResult>("AmbientOcclusion",
				offsetof(FParameters, AmbientOcclusion)),
		MakeRDGValueParameterMemberMetadata<FParameters,
			decltype(FParameters::ContactShadow), FContactShadowVisibilityPassResult>(
				"ContactShadow", offsetof(FParameters, ContactShadow)),
		MakeRDGValueParameterMemberMetadata<FParameters,
			decltype(FParameters::CloudShadow), FVolumetricCloudShadowPassResult>(
				"CloudShadow", offsetof(FParameters, CloudShadow)),
		MakeRDGValueParameterMemberMetadata<FParameters,
			decltype(FParameters::Completion), FIsolatedDeferredPassResult>(
				"Completion", offsetof(FParameters, Completion)),
		MakeRDGNestedParameterMemberMetadata<FParameters,
			decltype(FParameters::Resources)>("Resources",
				offsetof(FParameters, Resources),
				FDeferredDirectionalLightingPassResources::GetRDGParametersMetadata()));

	#undef DURIN_DEFINE_METADATA
	#undef DURIN_TEXTURE
	#undef DURIN_RESOURCE_MEMBER

	namespace
	{
		struct FDeferredDirectionalLightingRecorder final
		{
			FDefaultTextureResources& DefaultTextures;
			FDirectionalShadowRenderer& DirectionalShadowRenderer;
			FDeferredDirectionalLightingRenderer& DeferredDirectionalLightingRenderer;
			FSceneRenderTelemetry& Telemetry;
			FResolvedSceneResources& ResolvedSceneResources;

			auto BuildDeferredParameters(
				const FSceneView&, FRHITexture*, FRHITexture*, FRHITexture*,
				FRHISampler*, const FDirectionalShadowPassResult&, FRHITexture*,
				const FGBufferPassResult&, const FGBufferRenderer::FTargets*,
				const FGroundTruthAmbientOcclusionPassResult&,
				const FGroundTruthAmbientOcclusionRenderer::FTargets*,
				const FContactShadowVisibilityPassResult&,
				const FContactShadowVisibilityRenderer::FTargets*,
				const FContactShadowVisibilityRenderer::FComputeTargets*,
				const FVolumetricCloudShadowPassResult&,
				const FVolumetricCloudShadowRenderer::FTargets*,
				const FVolumetricCloudShadowRenderer::FComputeTargets*,
				const FPostProcessRenderer::FSceneTargets&,
				const FSceneViewRenderOptions&)
				-> std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>;
			auto RenderIsolatedDeferred_RenderThread(
				FRHICommandListImmediate&,
				const FDeferredDirectionalLightingRenderer::FTargets*,
				const FDeferredDirectionalLightingRenderer::FRenderParameters&,
				const FSceneViewRenderOptions&, uint32, uint32, bool)
				-> FIsolatedDeferredPassResult;
		};
	} // namespace

	auto FDeferredDirectionalLightingRendering::AddPasses(
		const FDeferredLightingFeatureInputs& Inputs)
		-> FDeferredLightingGraphOutput
	{
		auto& Graph = Inputs.Graph;
		FDeferredDirectionalLightingRecorder Recorder{
			Inputs.DefaultTextures, Inputs.DirectionalShadowRenderer,
			Inputs.Renderer, Inputs.Telemetry, Inputs.Resolved};
		const auto& RecordView = Inputs.View;
		const auto& Options = Inputs.Options;
		auto* DirectionalShadowTexture =
			Inputs.DirectionalShadowRenderer.GetTexture_RenderThread();
		auto* EnvironmentSampler = Inputs.EnvironmentSampler;
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
		auto& DeferredParameters = Inputs.DeferredParameters;
		auto& ProductionDeferredParameters = Inputs.ProductionDeferredParameters;
		const bool bIsolated = bWantsIsolatedDeferred;
		const auto AmbientOcclusionQuality = Inputs.AmbientOcclusion.Quality;
		std::optional<FRDGTextureHandle> IsolatedDeferred;
		const auto DeferredDirectionalLightingCompletion = Graph.CreateValue<
			FIsolatedDeferredPassResult>("Scene.DeferredDirectionalLightingValue",
				"deferred-directional-lighting-result");
		if (bIsolated)
			IsolatedDeferred = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DeferredDirectionalColor", Width, Height,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::DeferredDirectional)},
				"Scene.DeferredDirectionalLighting.Isolated",
				ERHIAccess::GraphicsShaderRead);
		auto Parameters = Graph.AllocParameters<
			FDeferredDirectionalLightingPassParameters>();
		Parameters->DirectionalShadow = {
			.Value = Inputs.DirectionalShadow.Completion};
		Parameters->GBufferCompletion = {.Value = Inputs.GBuffer.Completion};
		Parameters->AmbientOcclusion = {
			.Value = Inputs.AmbientOcclusion.Completion};
		Parameters->ContactShadow = {
			.Value = Inputs.ContactShadow.Completion};
		Parameters->CloudShadow = {.Value = Inputs.CloudShadow.Completion};
		Parameters->Completion = {
			.Value = DeferredDirectionalLightingCompletion};
		std::vector<FRDGTextureHandle> DeclaredPersistentInputs;
		auto AssignRead = [&DeclaredPersistentInputs](auto& Parameter, const auto& Handle,
			FRHITexture* Physical) {
			if (!Handle || !Physical
				|| std::ranges::find(DeclaredPersistentInputs, *Handle)
					!= DeclaredPersistentInputs.end()) return;
			DeclaredPersistentInputs.push_back(*Handle);
			Parameter = FRDGTextureParameter{*Handle,
				{GetTextureAspects(Physical->GetFormat()), 0,
					Physical->GetNumMips(), 0, Physical->GetArraySize()}};
		};
		AssignRead(Parameters->Resources.DirectionalShadow,
			Inputs.DirectionalShadow.Shadow, DirectionalShadowTexture);
		if (Inputs.GBuffer.Textures[0])
		{
			for (uint32 Index = 0; Index < Inputs.GBuffer.Textures.size(); ++Index)
				Parameters->Resources.GBuffer[Index] = {
					*Inputs.GBuffer.Textures[Index],
					{ERHITextureAspect::Color, 0, 1, 0, 1}};
			Parameters->Resources.SceneDepth = {Inputs.GBuffer.Depth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}};
		}
		for (uint32 Index = 0;
			Index < Inputs.AmbientOcclusion.Textures.size(); ++Index)
			if (Inputs.AmbientOcclusion.Textures[Index])
				Parameters->Resources.AmbientOcclusion[Index] = {
					*Inputs.AmbientOcclusion.Textures[Index],
					{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Inputs.ContactShadow.Fragment)
			Parameters->Resources.ContactShadowFragment = {
				*Inputs.ContactShadow.Fragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Inputs.ContactShadow.Compute)
			Parameters->Resources.ContactShadowCompute = {
				*Inputs.ContactShadow.Compute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Inputs.CloudShadow.Fragment)
			Parameters->Resources.CloudShadowFragment = {
				*Inputs.CloudShadow.Fragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Inputs.CloudShadow.Compute)
			Parameters->Resources.CloudShadowCompute = {
				*Inputs.CloudShadow.Compute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		AssignRead(Parameters->Resources.DefaultWhite, Inputs.DefaultWhite,
			Inputs.DefaultTextures.Get_RenderThread(EDefaultTexture::White));
		AssignRead(Parameters->Resources.DefaultShadowArray,
			Inputs.DefaultShadowArray,
			Inputs.DefaultTextures.GetArray_RenderThread());
		AssignRead(Parameters->Resources.EnvironmentIrradiance,
			Inputs.EnvironmentIrradiance,
			Inputs.SelectedEnvironmentIrradiance);
		AssignRead(Parameters->Resources.EnvironmentPrefiltered,
			Inputs.EnvironmentPrefiltered,
			Inputs.SelectedEnvironmentPrefiltered);
		AssignRead(Parameters->Resources.EnvironmentBrdfLut,
			Inputs.EnvironmentBrdfLut,
			Inputs.SelectedEnvironmentBrdfLut);
		if (IsolatedDeferred)
			Parameters->Resources.IsolatedDeferredOutput = {
				*IsolatedDeferred,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		(void)Graph.AddPass(Name, ERDGPassType::Graphics, std::move(Parameters),
			[Recorder, RecordView = &RecordView, AmbientOcclusionQuality,
				&Options, &DeferredParameters, &ProductionDeferredParameters,
				Width, Height, bWantsDeferredInputs, bWantsIsolatedDeferred,
				bWantsProductionDeferred, bHybridRetainedResourcesReady,
				EnvironmentSampler,
				EnvironmentIrradiance = Inputs.SelectedEnvironmentIrradiance,
				EnvironmentPrefiltered = Inputs.SelectedEnvironmentPrefiltered,
				EnvironmentBrdfLut = Inputs.SelectedEnvironmentBrdfLut](
				FRHICommandListImmediate& Commands,
				const FDeferredDirectionalLightingPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) mutable {
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (PassParameters.Resources.GBuffer[0])
					GBufferTargets = {
						.Material = Resolver.GetTexture(PassParameters.Resources.GBuffer[0]),
						.Normals = Resolver.GetTexture(PassParameters.Resources.GBuffer[1]),
						.Surface = Resolver.GetTexture(PassParameters.Resources.GBuffer[2]),
						.Emissive = Resolver.GetTexture(PassParameters.Resources.GBuffer[3])};
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = GBufferTargets
						? Resolver.GetTexture(PassParameters.Resources.SceneDepth) : nullptr};
				std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
					AmbientOcclusionTargets;
				if (PassParameters.Resources.AmbientOcclusion[0])
					AmbientOcclusionTargets = {
						.Raw = Resolver.GetTexture(PassParameters.Resources.AmbientOcclusion[0]),
						.Scratch = Resolver.GetTexture(PassParameters.Resources.AmbientOcclusion[1]),
						.Selector = PassParameters.Resources.AmbientOcclusion[2]
							? Resolver.GetTexture(PassParameters.Resources.AmbientOcclusion[2])
							: nullptr,
						.Resolved = PassParameters.Resources.AmbientOcclusion[3]
							? Resolver.GetTexture(PassParameters.Resources.AmbientOcclusion[3])
							: nullptr,
						.Quality = AmbientOcclusionQuality};
				std::optional<FContactShadowVisibilityRenderer::FTargets>
					FragmentContactTargets;
				if (PassParameters.Resources.ContactShadowFragment)
					FragmentContactTargets = {.Visibility = Resolver.GetTexture(
						PassParameters.Resources.ContactShadowFragment)};
				std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
					ComputeContactTargets;
				if (PassParameters.Resources.ContactShadowCompute)
					ComputeContactTargets = {.Visibility = Resolver.GetTexture(
						PassParameters.Resources.ContactShadowCompute)};
				std::optional<FVolumetricCloudShadowRenderer::FTargets>
					FragmentCloudShadowTargets;
				if (PassParameters.Resources.CloudShadowFragment)
					FragmentCloudShadowTargets = {.Visibility = Resolver.GetTexture(
						PassParameters.Resources.CloudShadowFragment)};
				std::optional<FVolumetricCloudShadowRenderer::FComputeTargets>
					ComputeCloudShadowTargets;
				if (PassParameters.Resources.CloudShadowCompute)
					ComputeCloudShadowTargets = {.Visibility = Resolver.GetTexture(
						PassParameters.Resources.CloudShadowCompute)};
				const auto& DirectionalShadowResult = Resolver.ReadValue(
					PassParameters.DirectionalShadow);
				const auto& GBufferResult = Resolver.ReadValue(
					PassParameters.GBufferCompletion);
				const auto& AmbientOcclusionResult = Resolver.ReadValue(
					PassParameters.AmbientOcclusion);
				const auto& ContactShadowResult = Resolver.ReadValue(
					PassParameters.ContactShadow);
				const auto& CloudShadowResult = Resolver.ReadValue(
					PassParameters.CloudShadow);
				auto& DeferredResult = Resolver.WriteValue(PassParameters.Completion);
				DeferredParameters = bWantsDeferredInputs
					? Recorder.BuildDeferredParameters(
						*RecordView,
						PassParameters.Resources.EnvironmentIrradiance
							? Resolver.GetTexture(
								PassParameters.Resources.EnvironmentIrradiance)
							: EnvironmentIrradiance,
						PassParameters.Resources.EnvironmentPrefiltered
							? Resolver.GetTexture(
								PassParameters.Resources.EnvironmentPrefiltered)
							: EnvironmentPrefiltered,
						PassParameters.Resources.EnvironmentBrdfLut
							? Resolver.GetTexture(
								PassParameters.Resources.EnvironmentBrdfLut)
							: EnvironmentBrdfLut,
						EnvironmentSampler, DirectionalShadowResult,
						Resolver.GetTexture(PassParameters.Resources.DirectionalShadow),
						GBufferResult,
						GBufferTargets ? &*GBufferTargets : nullptr,
						AmbientOcclusionResult,
						AmbientOcclusionTargets ? &*AmbientOcclusionTargets : nullptr,
						ContactShadowResult,
						FragmentContactTargets ? &*FragmentContactTargets : nullptr,
						ComputeContactTargets ? &*ComputeContactTargets : nullptr,
						CloudShadowResult,
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
					if (PassParameters.Resources.IsolatedDeferredOutput)
						IsolatedTargets = {.Color = Resolver.GetColorAttachment(
							PassParameters.Resources.IsolatedDeferredOutput).Texture};
					DeferredResult = Recorder.RenderIsolatedDeferred_RenderThread(
						Commands, IsolatedTargets ? &*IsolatedTargets : nullptr,
						*DeferredParameters, Options, Width, Height,
						bWantsIsolatedDeferred);
				}
				else if (bWantsIsolatedDeferred)
				{
					DeferredResult.Status = EScenePassStatus::Failed;
					++Recorder.Telemetry.View.Deferred.DeferredDirectionalUnavailableViews;
				}
				const bool bProductionResourcesReady =
					!bWantsProductionDeferred
					|| (GBufferResult.IsComplete()
						&& bHybridRetainedResourcesReady
						&& DeferredParameters.has_value());
				if (bWantsProductionDeferred && bProductionResourcesReady)
				{
					ProductionDeferredParameters = *DeferredParameters;
					ProductionDeferredParameters->DiagnosticMode = 0;
				}
			});
		return {.Completion = DeferredDirectionalLightingCompletion,
			.Isolated = IsolatedDeferred};
	}

	auto FDeferredDirectionalLightingRecorder::BuildDeferredParameters(
		const FSceneView& RenderView,
		FRHITexture* EnvironmentIrradiance,
		FRHITexture* EnvironmentPrefiltered,
		FRHITexture* EnvironmentBrdfLut,
		FRHISampler* EnvironmentSampler,
		const FDirectionalShadowPassResult& DirectionalShadow,
		FRHITexture* DirectionalShadowTexture,
		const FGBufferPassResult& GBuffer,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FGroundTruthAmbientOcclusionPassResult& AmbientOcclusion,
		const FGroundTruthAmbientOcclusionRenderer::FTargets*
			AmbientOcclusionTargets,
		const FContactShadowVisibilityPassResult& ContactShadow,
		const FContactShadowVisibilityRenderer::FTargets*
			FragmentContactTargets,
		const FContactShadowVisibilityRenderer::FComputeTargets*
			ComputeContactTargets,
		const FVolumetricCloudShadowPassResult& CloudShadow,
		const FVolumetricCloudShadowRenderer::FTargets*
			FragmentCloudShadowTargets,
		const FVolumetricCloudShadowRenderer::FComputeTargets*
			ComputeCloudShadowTargets,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FSceneViewRenderOptions& Options
	) -> std::optional<
		FDeferredDirectionalLightingRenderer::FRenderParameters>
	{
		if (!GBuffer.IsComplete() || GBufferTargets == nullptr) return std::nullopt;
		FRHITexture* White =
			DefaultTextures.Get_RenderThread(EDefaultTexture::White);
		const bool bAmbientOcclusionComplete = AmbientOcclusion.IsComplete()
			&& AmbientOcclusionTargets != nullptr;
		FRHITexture* ContactVisibility = White;
		bool bContactVisibilityComplete = false;
		if (ContactShadow.IsComplete())
		{
			if (ContactShadow.Route == EContactShadowVisibilityPassRoute::Compute
				&& ComputeContactTargets != nullptr)
			{
				ContactVisibility = ComputeContactTargets->Visibility;
				bContactVisibilityComplete = true;
			}
			else if (ContactShadow.Route == EContactShadowVisibilityPassRoute::Fragment
				&& FragmentContactTargets != nullptr)
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
				&& ComputeCloudShadowTargets != nullptr)
			{
				CloudShadowVisibility = ComputeCloudShadowTargets->Visibility;
				bCloudShadowVisibilityComplete = true;
			}
			else if (CloudShadow.Route == EVolumetricCloudShadowPassRoute::Fragment
				&& FragmentCloudShadowTargets != nullptr)
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
			.EnvironmentIrradiance = EnvironmentIrradiance,
			.EnvironmentPrefiltered = EnvironmentPrefiltered,
			.EnvironmentBrdfLut = EnvironmentBrdfLut,
			.EnvironmentSampler = EnvironmentSampler,
			.DirectionalShadowTexture = DirectionalShadow.IsComplete()
				&& DirectionalShadowTexture != nullptr
				? DirectionalShadowTexture
				: DefaultTextures.GetArray_RenderThread(),
			.DirectionalShadowSampler = DirectionalShadow.IsComplete()
				? DirectionalShadowRenderer.GetSampler_RenderThread() : nullptr,
			.GroundTruthAmbientOcclusionRaw = bAmbientOcclusionComplete
				? (AmbientOcclusion.bRawDiagnosticUsesScratch
					? AmbientOcclusionTargets->Scratch.GetReference()
					: AmbientOcclusionTargets->Raw.GetReference())
				: White,
			.GroundTruthAmbientOcclusionFiltered =
				bAmbientOcclusionComplete
					? AmbientOcclusionTargets->Raw.GetReference() : White,
			.GroundTruthAmbientOcclusionResolved =
				bAmbientOcclusionComplete
					? (AmbientOcclusion.bHalfResolution
						? AmbientOcclusionTargets->Resolved.GetReference()
						: AmbientOcclusionTargets->Raw.GetReference())
					: White,
			.GroundTruthAmbientOcclusionSelector =
				bAmbientOcclusionComplete && AmbientOcclusion.bHalfResolution
					? AmbientOcclusionTargets->Selector.GetReference() : White,
			.ContactVisibility = ContactVisibility,
			.VolumetricCloudVisibility = CloudShadowVisibility,
			.Lighting = ResolvedSceneResources.Lighting.UniformBuffer,
			.View = &RenderView,
			.DiagnosticMode = static_cast<uint32>(
				Options.DeferredDirectionalDebugMode),
			.bGroundTruthAmbientOcclusionEnabled = bAmbientOcclusionComplete,
			.bGroundTruthAmbientOcclusionHalfResolution =
				AmbientOcclusion.bHalfResolution,
			.bContactVisibilityEnabled = bContactVisibilityComplete,
			.bContactVisibilityDebug = ContactShadow.bDebug,
			.bVolumetricCloudVisibilityEnabled =
				bCloudShadowVisibilityComplete};
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
