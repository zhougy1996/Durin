#include "Renderers/GBufferRendering.h"
#include "Renderers/MeshPassProcessor.h"
#include "RDG/RDG.h"
#include "Renderers/SceneTextureGroupParameters.h"

#include "Renderers/SceneRenderingService.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"
#include "Renderers/StaticMeshRenderPreparation.h"

namespace Durin
{

	namespace
	{
		auto RecordGBuffer_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& RenderView,
			const FPreparedReceiverGeometry& Receiver,
			FResolvedSceneResources& Resolved,
			FSceneRenderTelemetry& Telemetry,
			FStaticMeshRenderer& StaticMeshRenderer,
			FRHITexture* SceneDepth,
			const FGBufferRenderer::FTargets* GBufferTargets,
			const FSceneViewRenderOptions& Options,
			uint32 Width, uint32 Height,
			bool bWantsIsolatedDeferred,
			const std::shared_ptr<FInstanceCullingContext>& GPUCullingPlan,
			FRHIBuffer* IndirectArguments,
			FRHIBuffer* VisibleInstances)
			-> FGBufferPassResult;
	}

	auto FGBufferGPUCullingCopyParameters::GetRDGParametersMetadata() -> const FRDGParametersMetadata*
	{
		using P = FGBufferGPUCullingCopyParameters;
		static const std::array Members{
			MakeRDGValueParameterMemberMetadata<P, decltype(P::Completion), FGBufferPassResult>("Completion", offsetof(P, Completion)),
			MakeRDGResourceParameterMemberMetadata<P, decltype(P::Arguments), FRDGBufferParameter>(
				"Arguments", offsetof(P, Arguments), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer,
				ERDGParameterRangeKind::BufferBytes, ERDGUse::Read, ERHIAccess::TransferRead),
			MakeRDGResourceParameterMemberMetadata<P, decltype(P::Target), FRDGTextureParameter>(
				"Target", offsetof(P, Target), ERDGParameterMemberKind::Texture, ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource, ERDGUse::Write, ERHIAccess::TransferWrite)};
		static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FGBufferGPUCullingCopyParameters", Members);
		return &Metadata;
	}

	auto FGBufferGPUCullingReadbackParameters::GetRDGParametersMetadata() -> const FRDGParametersMetadata*
	{
		using P = FGBufferGPUCullingReadbackParameters;
		static const std::array Members{
			MakeRDGResourceParameterMemberMetadata<P, decltype(P::Target), FRDGTextureParameter>(
				"Target", offsetof(P, Target), ERDGParameterMemberKind::Texture, ERDGResourceKind::Texture,
				ERDGParameterRangeKind::TextureSubresource, ERDGUse::Read, ERHIAccess::TransferRead)};
		static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FGBufferGPUCullingReadbackParameters", Members);
		return &Metadata;
	}

	auto FGBufferPassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		static const std::array Members{
            MakeRDGNestedParameterMemberMetadata<FGBufferPassParameters, decltype(GPUScene)>("GPUScene", offsetof(FGBufferPassParameters, GPUScene), FGPUSceneReadParameters::GetRDGParametersMetadata()),
			MakeRDGValueParameterMemberMetadata<
				FGBufferPassParameters, decltype(Completion),
				FGBufferPassResult>("Completion",
					offsetof(FGBufferPassParameters, Completion)),
			MakeRDGAttachmentMetadata<FGBufferPassParameters, decltype(Colors)>(
				"Colors",
				offsetof(FGBufferPassParameters, Colors),
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead),
			MakeRDGAttachmentMetadata<FGBufferPassParameters, decltype(Depth)>(
				"Depth",
				offsetof(FGBufferPassParameters, Depth),
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead),
			MakeRDGResourceParameterMemberMetadata<FGBufferPassParameters,
				decltype(VisibleInstances), FRDGBufferParameter>(
					"VisibleInstances", offsetof(FGBufferPassParameters, VisibleInstances),
					ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer,
					ERDGParameterRangeKind::BufferBytes, ERDGUse::Read,
					ERHIAccess::GraphicsShaderRead),
			MakeRDGIndirectArgumentMetadata<FGBufferPassParameters,
				decltype(Arguments)>("Arguments",
					offsetof(FGBufferPassParameters, Arguments)),
		};
		static const auto Metadata =
			MakeInlineRDGParametersMetadata<FGBufferPassParameters>(
				"FGBufferPassParameters", Members);
		return &Metadata;
	}

	auto AddGBufferPasses(
		FRDGBuilder& Graph, const FGBufferFeatureInputs& Inputs) -> FGBufferGraphOutput
	{
		const auto& Options = Inputs.Options;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		if (!Inputs.Feature.IsEnabled()) return {.Depth = Inputs.Depth};
		const bool bWantsIsolatedDeferred =
			Inputs.DeferredFeature.HasPurpose(ESceneFeaturePurpose::Debug)
			|| Inputs.DeferredFeature.HasPurpose(
				ESceneFeaturePurpose::Qualification);
		std::optional<FGBufferTextureHandles> GBuffer;
		const auto GBufferCompletion = Graph.CreateValue<FGBufferPassResult>(
			"Scene.GBufferValue", "gbuffer-result");
		GBuffer.emplace();
		const std::array Formats{EPixelFormat::RGBA8_UNORM,
			EPixelFormat::RGBA8_UNORM, EPixelFormat::RGBA8_UNORM,
			EPixelFormat::R11G11B10_FLOAT};
		const std::array Names{"Scene.GBuffer.Material", "Scene.GBuffer.Normals",
			"Scene.GBuffer.Surface", "Scene.GBuffer.Emissive"};
		for (uint32 Index = 0; Index < GBuffer->Colors.size(); ++Index)
			GBuffer->Colors[Index] = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					Names[Index], Width, Height, Formats[Index])
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::GBuffer)}, Names[Index]);
		const FRHICapabilities* Capabilities = GDynamicRHI
			? GDynamicRHI->RHIGetCapabilities() : nullptr;
		size_t OverflowCandidates = 0;
		FInstanceCullingAdmissionDiagnostics Admission;
        auto& CullingManager = Inputs.CullingManager;
        CullingManager.ResetViews();
        const auto ViewId = CullingManager.RegisterView(Inputs.View, Inputs.Receiver.CullingFrustum);
        const auto GPUCullingPlan = CullingManager.CreateContext(ViewId, {RendererPrivate::EMeshPipelinePass::GBuffer});
        const bool bHasGPUCandidates = GPUCullingPlan->SetupDrawCommands(
            RendererPrivate::CollectMeshDrawCommands(Inputs.Receiver.StaticMeshes, Inputs.Resolved.Receiver.StaticMeshes,
                {RendererPrivate::EMeshPipelinePass::GBuffer}, false), Options.bEnableGPUCulling, Capabilities && Capabilities->bSupportsIndirectDraw,
            &OverflowCandidates, Inputs.Failure == EGPUCullingFailure::MetadataBudget ? 0 : FInstanceCullingContext::MaximumGeneratedBytes, &Admission);
		Inputs.Telemetry.View.GBuffer.GPUCullingPolicyDisabledViews += Options.bEnableGPUCulling ? 0u : 1u;
		Inputs.Telemetry.View.GBuffer.GPUCullingBelowThresholdCandidates += Admission.BelowThresholdCandidates;
		Inputs.Telemetry.View.GBuffer.GPUCullingIncompatibleCandidates += Admission.IncompatibleCandidates;
		Inputs.Telemetry.View.GBuffer.GPUCullingPreparationUnavailableCandidates += Admission.PreparationUnavailableCandidates;
		Inputs.Telemetry.View.GBuffer.GPUCullingOverflowCandidates +=
			OverflowCandidates;
		if (Options.bEnableGPUCulling)
		{
			++Inputs.Telemetry.View.GBuffer.GPUCullingRequestedViews;
			if (!bHasGPUCandidates)
			{
				++Inputs.Telemetry.View.GBuffer.GPUCullingFallbackViews;
				if (!Capabilities || !Capabilities->bSupportsIndirectDraw)
					++Inputs.Telemetry.View.GBuffer
						.GPUCullingUnsupportedFallbackViews;
				else
					++Inputs.Telemetry.View.GBuffer
						.GPUCullingNoEligibleGroupFallbackViews;
			}
		}
        const bool bObserveVisibility = IsViewRenderTelemetryObserved();
        const auto DrawParams = GPUCullingPlan->BuildRenderingCommands(Graph,
            Inputs.Telemetry.View.GBuffer, Options.bEnableAsyncCompute, bObserveVisibility, Inputs.Failure);
        const auto& VisibleInstances = DrawParams.InstanceIdOffsetBuffer;
        const auto& IndirectArguments = DrawParams.DrawIndirectArgsBuffer;
		auto Parameters = Graph.AllocParameters<FGBufferPassParameters>();
		Parameters->Completion = {GBufferCompletion};
		Parameters->GPUScene.Add(Graph, Inputs.Resolved.Receiver.StaticMeshes);
		SceneTextureGroups::FillGBuffer(GBuffer, Parameters->Colors);
		Parameters->Depth = FRDGDepthStencilAttachmentParameter{
			.Texture = Inputs.Depth,
			.Range = {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		if (bHasGPUCandidates)
		{
			Parameters->VisibleInstances = FRDGBufferParameter{
				*VisibleInstances, 0,
				GPUCullingPlan->Candidates.size() * sizeof(uint32)};
			Parameters->Arguments = FRDGBufferParameter{
				*IndirectArguments, 0, GPUCullingPlan->Arguments.size()
					* sizeof(FRHIDrawIndexedIndirectArguments)};
		}
		const auto GBufferPass = Graph.AddPass(GBufferPassName, ERDGPassType::Graphics,
			std::move(Parameters),
			[&StaticMeshes = Inputs.StaticMeshes,
				&Resolved = Inputs.Resolved,
				&Telemetry = Inputs.Telemetry,
				&View = Inputs.View, &Receiver = Inputs.Receiver, &Options,
				Width, Height, bWantsIsolatedDeferred, GPUCullingPlan](
				FRHICommandListImmediate& Commands,
				const FGBufferPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				const FRDGAttachmentView Depth =
					Resolver.GetDepthStencilAttachment(PassParameters.Depth);
				FRHITexture* SceneDepth = Depth.Texture;
				const auto GBufferTargets = SceneTextureGroups::ResolveGBuffer(
					Resolver, PassParameters.Colors);
				FRHIBuffer* ArgumentBuffer = PassParameters.Arguments
					? Resolver.GetBuffer(*PassParameters.Arguments) : nullptr;
				FRHIBuffer* VisibleBuffer = PassParameters.VisibleInstances
					? Resolver.GetBuffer(*PassParameters.VisibleInstances) : nullptr;
				Resolver.WriteValue(PassParameters.Completion) =
					RecordGBuffer_RenderThread(
					Commands, View, Receiver, Resolved, Telemetry,
					StaticMeshes, SceneDepth,
					GBufferTargets ? &*GBufferTargets : nullptr,
					Options, Width, Height,
					bWantsIsolatedDeferred, GPUCullingPlan, ArgumentBuffer,
					VisibleBuffer);
			});
		if (bHasGPUCandidates && bObserveVisibility)
		{
			const uint32 Groups = static_cast<uint32>(GPUCullingPlan->Arguments.size());
			auto Query = std::make_shared<FGPUCullingVisibilityQuery>(
				static_cast<uint32>(GPUCullingPlan->Candidates.size()), GPUCullingPlan->Arguments);
			Inputs.Telemetry.View.GBuffer.GPUCullingVisibility = Query;
			const auto Target = Graph.CreateTexture({.Texture = FRHITextureCreateDesc::Create2D(
				"GBufferGPUCullingCounters", 5, Groups, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::SourceCopy | ETextureCreateFlags::DestinationCopy
					| ETextureCreateFlags::CPUReadback)}, "Scene.GBuffer.GPUCulling.Counters");
			auto Copy = Graph.AllocParameters<FGBufferGPUCullingCopyParameters>();
			Copy->Completion = {.Value = GBufferCompletion};
			Copy->Arguments = {*IndirectArguments, 0, Groups * sizeof(FRHIDrawIndexedIndirectArguments)};
			Copy->Target = {Target, {ERHITextureAspect::Color, 0, 1, 0, 1}};
			Graph.AddPass("Scene.GBuffer.GPUCulling.CopyCounters", ERDGPassType::Copy, std::move(Copy),
				[Query, Plan = GPUCullingPlan, Groups](FRHICommandListImmediate& Commands,
					const FGBufferGPUCullingCopyParameters& P, const FRDGParameterResolver& Resolver) {
					if (!Plan->bDispatched || !Resolver.ReadValue(P.Completion).IsComplete())
					{
						Query->GetReadback()->Cancel();
						return;
					}
					const std::array Regions{FRHIBufferTextureCopyRegion{.TextureExtent = {5, Groups, 1}}};
					Commands.CopyBufferToTexture(Resolver.GetBuffer(P.Arguments), Resolver.GetTexture(P.Target), Regions);
				});
			auto Readback = Graph.AllocParameters<FGBufferGPUCullingReadbackParameters>();
			Readback->Target = {Target, {ERHITextureAspect::Color, 0, 1, 0, 1}};
			const auto Pass = Graph.AddPass("Scene.GBuffer.GPUCulling.Observe", ERDGPassType::Copy, std::move(Readback),
				[Query](FRHICommandListImmediate& Commands, const FGBufferGPUCullingReadbackParameters& P,
					const FRDGParameterResolver& Resolver) {
					if (Query->GetState() == ERHITextureReadbackState::Pending)
						Commands.EnqueueTextureReadback(Resolver.GetTexture(P.Target), 0, 0, Query->GetReadback());
				});
			Graph.MarkPassRoot(Pass, "Explicit GPU visibility observer");
		}
		if (GetGBufferCaptureSink() || GetGBufferTimingQuerySink())
			Graph.MarkPassRoot(GBufferPass, "GBuffer qualification observation");
		return {.Completion = GBufferCompletion,
			.Textures = GBuffer, .Depth = Inputs.Depth};
	}

	namespace
	{
	auto RecordGBuffer_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& RenderView,
		const FPreparedReceiverGeometry& Receiver,
		FResolvedSceneResources& ResolvedSceneResources,
		FSceneRenderTelemetry& Telemetry,
		FStaticMeshRenderer& StaticMeshRenderer,
		FRHITexture* SceneDepth,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsIsolatedDeferred,
		const std::shared_ptr<FInstanceCullingContext>& GPUCullingPlan,
		FRHIBuffer* IndirectArguments,
		FRHIBuffer* VisibleInstances
	) -> FGBufferPassResult
	{
		FGBufferPassResult Result;
		if (GBufferTargets == nullptr)
		{
			Result.Status = EScenePassStatus::Failed;
			++Telemetry.View.GBuffer.GBufferUnavailableViews;
			if (bWantsIsolatedDeferred)
				++Telemetry.View.Deferred.DeferredDirectionalUnavailableViews;
			if (Options.GBufferDebugMode != EGBufferDebugMode::Disabled)
				++Telemetry.View.GBuffer.GBufferDebugFailures;
		}
		else
		{
			FRHIRenderPassInfo GBufferPassInfo{};
			GBufferPassInfo.RenderTargetLayout =
				RenderTargetLayouts::MakeGBufferTargets();
			GBufferPassInfo.ColorRenderTargets[0] =
				GBufferTargets->Material;
			GBufferPassInfo.ColorRenderTargets[1] =
				GBufferTargets->Normals;
			GBufferPassInfo.ColorRenderTargets[2] =
				GBufferTargets->Surface;
			GBufferPassInfo.ColorRenderTargets[3] =
				GBufferTargets->Emissive;
			GBufferPassInfo.DepthStencilRenderTarget = SceneDepth;
			for (uint32 Index = 0; Index < 4; ++Index)
			{
				GBufferPassInfo.ColorClearValues[Index] =
					FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f);
			}
			GBufferPassInfo.DepthStencilClearValue = FClearValueBinding(
				RenderView.DepthConvention == ESceneDepthConvention::ReversedZ ? 0.0f : 1.0f,
				0u
			);
			const FGBufferTimingQuerySink GBufferTimingSink =
				GetGBufferTimingQuerySink();
			TScopedRendererGPUTimingQuery GBufferTiming(
				CommandList, GBufferTimingSink
			);
			CommandList.BeginRenderPass(
				GBufferPassInfo, "GBufferQualificationRenderPass"
			);
			CommandList.SetViewport(
				static_cast<float>(RenderView.ViewportX),
				static_cast<float>(RenderView.ViewportY),
				0.0f,
				static_cast<float>(RenderView.ViewportX + RenderView.ViewportWidth),
				static_cast<float>(RenderView.ViewportY + RenderView.ViewportHeight),
				1.0f
			);
			CommandList.SetScissor(
				static_cast<float>(RenderView.ViewportX),
				static_cast<float>(RenderView.ViewportY),
				static_cast<float>(RenderView.ViewportWidth),
				static_cast<float>(RenderView.ViewportHeight)
			);
			const FGeometryExecutionResult StaticResult = StaticMeshRenderer.ExecuteGBuffer_RenderThread(
				CommandList,
				Receiver.StaticMeshes,
				ResolvedSceneResources.Receiver.StaticMeshes,
                GPUCullingPlan.get(),
                {.InstanceBuffer = GPUCullingPlan->bDispatched ? VisibleInstances : nullptr,
                    .IndirectArgsBuffer = GPUCullingPlan->bDispatched ? IndirectArguments : nullptr}
			);
			if (Options.bEnableGPUCulling)
			{
				const auto& Submissions = ResolvedSceneResources.Receiver.StaticMeshes.Observations;
				Telemetry.View.GBuffer.GPUCullingIndirectDraws += Submissions.GBufferIndirectSubmissions;
				Telemetry.View.GBuffer.GPUCullingDirectFallbackDraws += Submissions.GBufferLocalDirectSubmissions;
			}
			CommandList.EndRenderPass();
			Result.Status = StaticResult.bComplete
				? EScenePassStatus::Complete
				: EScenePassStatus::Failed;
			Result.bRenderedGeometry = StaticResult.bRenderedGeometry;
			GBufferTiming.Commit();
			const FGBufferCaptureSink GBufferCaptureSink =
				GetGBufferCaptureSink();
			if (GBufferCaptureSink != nullptr)
			{
				GBufferCaptureSink(
					CommandList,
					GBufferTargets->Material,
					GBufferTargets->Normals,
					GBufferTargets->Surface,
					GBufferTargets->Emissive,
					SceneDepth
				);
			}
			++Telemetry.View.GBuffer.GBufferEnabledViews;
			Telemetry.View.GBuffer.GBufferAttachmentBytes =
				FGBufferRenderer::CalculateTargetBytes(Width, Height);
			Telemetry.View.GBuffer.GBufferAttemptedDraws =
				ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferAttemptedDraws;
			Telemetry.View.GBuffer.GBufferSuccessfulDraws =
				ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferSuccessfulDraws;
			Telemetry.View.GBuffer.GBufferRejectedDraws =
				ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferRejectedDraws;
			Telemetry.View.GBuffer.GBufferSkippedDraws =
				ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferSkippedDraws;
			Telemetry.View.GBuffer.GBufferStaticMeshAttemptedDraws =
				ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferLocalAttemptedDraws;
			Telemetry.View.GBuffer.GBufferStaticMeshSuccessfulDraws =
				ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferLocalSuccessfulDraws;
			Telemetry.View.GBuffer.GBufferStaticMeshRejectedDraws =
				ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferLocalRejectedDraws;
			Telemetry.View.GBuffer.GBufferStaticMeshSkippedDraws =
				ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferLocalSkippedDraws;
			Telemetry.View.GBuffer.GBufferSplineMeshAttemptedDraws =
				ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferSplineAttemptedDraws;
			Telemetry.View.GBuffer.GBufferSplineMeshSuccessfulDraws =
				ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferSplineSuccessfulDraws;
			Telemetry.View.GBuffer.GBufferSplineMeshRejectedDraws =
				ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferSplineRejectedDraws;
			Telemetry.View.GBuffer.GBufferSplineMeshSkippedDraws =
				ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferSplineSkippedDraws;
		}
		return Result;
	}
	} // namespace
} // namespace Durin
