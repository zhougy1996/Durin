#include "Renderers/GBufferRendering.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/GBufferDebugRenderer.h"
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
			FRHITexture* SceneColor,
			const FGBufferRenderer::FTargets* GBufferTargets,
			const FSceneViewRenderOptions& Options,
			uint32 Width,
			uint32 Height,
			bool bWantsIsolatedDeferred,
			const RendererPrivate::FMeshDrawCommandOverrideArgs& OverrideArgs
		)
			-> FGBufferPassResult;
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
			MakeRDGNestedParameterMemberMetadata<FGBufferPassParameters, decltype(InstanceCulling)>("InstanceCulling", offsetof(FGBufferPassParameters, InstanceCulling), FInstanceCullingDrawParams::GetRDGParametersMetadata()),
		};
		static const auto Metadata =
			MakeInlineRDGParametersMetadata<FGBufferPassParameters>(
				"FGBufferPassParameters", Members);
		return &Metadata;
	}

	struct FBasePassDebugPassParameters final
	{
		TRDGValueRead<FGBufferPassResult> GBuffer;
		TRDGValueWrite<FGBufferPassResult> Completion;
		std::array<std::optional<FRDGTextureParameter>, 3> Textures;
		std::optional<FRDGTextureParameter> Seed;
		std::optional<FRDGTextureParameter> Depth;
		std::optional<FRDGColorAttachmentParameter> Output;
		static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
		{
			using P = FBasePassDebugPassParameters;
			static const std::array Members{
				MakeRDGValueParameterMemberMetadata<P, decltype(GBuffer), FGBufferPassResult>("GBuffer", offsetof(P, GBuffer)),
				MakeRDGValueParameterMemberMetadata<P, decltype(Completion), FGBufferPassResult>("Completion", offsetof(P, Completion)),
				MakeRDGTextureReadMetadata<P, decltype(Textures)>("Textures", offsetof(P, Textures)),
				MakeRDGTextureReadMetadata<P, decltype(Seed)>("Seed", offsetof(P, Seed)),
				MakeRDGTextureReadMetadata<P, decltype(Depth)>("Depth", offsetof(P, Depth)),
				MakeRDGAttachmentMetadata<P, decltype(Output)>("Output", offsetof(P, Output), ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store, ERHIAccess::GraphicsShaderRead)
			};
			static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FBasePassDebugPassParameters", Members);
			return &Metadata;
		}
	};

	auto AddGBufferPasses(
		FRDGBuilder& Graph, const FGBufferFeatureInputs& Inputs) -> FGBufferGraphOutput
	{
		const auto& Options = Inputs.Options;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		if (!Inputs.Feature.IsEnabled()) return {.Depth = Inputs.Depth, .BasePassColor = Inputs.SceneColor};
		// Factory classification belongs to the consuming feature, after final draw admission.
		auto& Meshes = Inputs.Resolved.Receiver.StaticMeshes;
		const auto& Prepared = Inputs.Receiver.StaticMeshes;
		Meshes.GBufferPassStats = Inputs.MeshDrawCommandStats.CreatePassStats({RendererPrivate::EMeshPipelinePass::GBuffer});
		Inputs.Telemetry.View.GBuffer.bDetailedDrawStats = Meshes.GBufferPassStats != nullptr;
		Meshes.GBufferSourceFamilies.assign(Meshes.GBufferPassStats ? Meshes.Draws.size() : 0, 0);
		Meshes.GBufferSkippedSources = {};
		for (uint32 Index = 0; Meshes.GBufferPassStats && Index < Prepared.GetNumSections(); ++Index)
		{
			const auto& Draw = Prepared.GetDraw(Index);
			const uint8 Family = Draw.Command->PipelineKey.VertexDomain == EVertexDeformationDomain::Spline ? 1 : 0;
			Meshes.GBufferSourceFamilies[Draw.ResolvedIndex] = Family;
			if (Index >= Prepared.Opaque.size() + Prepared.Masked.size()
				|| !RendererPrivate::FPassProcessorManager::GetMeshPassProcessor(RendererPrivate::EMeshPipelinePass::GBuffer)
					.IsMeshRelevant(Draw.Command->Material.PlanningPassIdentity, {}, Draw.Command->bSupportsGBuffer))
				++Meshes.GBufferSkippedSources[Family];
		}

		const bool bWantsIsolatedDeferred =
			Inputs.DeferredFeature.HasPurpose(ESceneFeaturePurpose::Debug)
			|| Inputs.DeferredFeature.HasPurpose(
				ESceneFeaturePurpose::Qualification);
		std::optional<FGBufferTextureHandles> GBuffer;
		const auto GBufferCompletion = Graph.CreateValue<FGBufferPassResult>(
			"Scene.GBufferValue", "gbuffer-result");
		GBuffer.emplace();
		const std::array Formats{EPixelFormat::RGBA8_UNORM, EPixelFormat::RGBA8_UNORM, EPixelFormat::RGBA8_UNORM};
		const std::array Names{"Scene.GBuffer.Material", "Scene.GBuffer.Normals", "Scene.GBuffer.Surface"};
		for (uint32 Index = 0; Index < GBuffer->Colors.size(); ++Index)
			GBuffer->Colors[Index] = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					Names[Index], Width, Height, Formats[Index])
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::GBuffer)}, Names[Index]);
		if (FStaticMeshRenderer::NeedsGBufferModelData(Prepared))
			GBuffer->ModelData = Graph.CreateTexture({.Texture = FRHITextureCreateDesc::Create2D("Scene.GBuffer.ModelData", Width, Height, EPixelFormat::RGBA8_UNORM).SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy), .ObservationTag = static_cast<uint32>(ERDGAllocationObservation::GBuffer)}, "Scene.GBuffer.ModelData");
		const auto CullingMode = Inputs.Feature.InstanceCullingMode;
		const bool bUseGPUCulling = CullingMode == EInstanceCullingMode::Enabled;
		size_t OverflowCandidates = 0;
		FInstanceCullingAdmissionDiagnostics Admission;
        auto& CullingManager = Inputs.CullingManager;
        CullingManager.ResetViews();
		auto& MeshPass = Inputs.Resolved.Receiver.StaticMeshes.GetMeshDrawCommandPass({RendererPrivate::EMeshPipelinePass::GBuffer});
		bool bHasGPUCandidates = false;
		if (bUseGPUCulling)
		{
			const auto ViewId = CullingManager.RegisterView(Inputs.View, Inputs.Receiver.CullingFrustum);
			const auto Sources = RendererPrivate::BuildMeshDrawCommandCullingPayloads(Inputs.Receiver.StaticMeshes,
				Inputs.Resolved.Receiver.StaticMeshes, {RendererPrivate::EMeshPipelinePass::GBuffer});
			bHasGPUCandidates = MeshPass.SetupDrawCommands(CullingManager, ViewId, Sources,
				&OverflowCandidates, Inputs.Failure == EGPUCullingFailure::MetadataBudget ? 0 : FInstanceCullingContext::MaximumGeneratedBytes, &Admission);
		}
		auto* GPUCullingPlan = MeshPass.GetInstanceCullingContext();
		Inputs.Telemetry.View.GBuffer.GPUCullingPolicyDisabledViews += CullingMode == EInstanceCullingMode::Disabled ? 1u : 0u;
		Inputs.Telemetry.View.GBuffer.GPUCullingBelowThresholdCandidates += Admission.BelowThresholdCandidates;
		Inputs.Telemetry.View.GBuffer.GPUCullingIncompatibleCandidates += Admission.IncompatibleCandidates;
		Inputs.Telemetry.View.GBuffer.GPUCullingPreparationUnavailableCandidates += Admission.PreparationUnavailableCandidates;
		Inputs.Telemetry.View.GBuffer.GPUCullingOverflowCandidates +=
			OverflowCandidates;
		if (CullingMode != EInstanceCullingMode::Disabled) ++Inputs.Telemetry.View.GBuffer.GPUCullingRequestedViews;
		switch (CullingMode)
		{
		case EInstanceCullingMode::Unsupported: ++Inputs.Telemetry.View.GBuffer.GPUCullingUnsupportedViews; break;
		case EInstanceCullingMode::ResourcesUnavailable: ++Inputs.Telemetry.View.GBuffer.GPUCullingResourcesUnavailableViews; break;
		case EInstanceCullingMode::Enabled:
			if (!bHasGPUCandidates) ++Inputs.Telemetry.View.GBuffer.GPUCullingNoEligibleGroupViews;
			break;
		case EInstanceCullingMode::Disabled: break;
		}
        const bool bObserveVisibility = Inputs.MeshDrawCommandStats.CollectGPUVisibilityStats();
        const auto DrawParams = GPUCullingPlan
			? GPUCullingPlan->BuildRenderingCommands(Graph, Inputs.Telemetry.View.GBuffer, Options.bEnableAsyncCompute, bObserveVisibility, Inputs.Failure)
			: FInstanceCullingDrawParams{};

		auto Parameters = Graph.AllocParameters<FGBufferPassParameters>();
		Parameters->Completion = {GBufferCompletion};
		Parameters->GPUScene = Inputs.Resolved.Receiver.StaticMeshes.GetGPUSceneParameters(Graph);
		for (uint32 Index = 0; Index < GBuffer->Colors.size(); ++Index)
			Parameters->Colors[Index] = FRDGColorAttachmentParameter{GBuffer->Colors[Index], {ERHITextureAspect::Color, 0, 1, 0, 1}};
		Parameters->Colors[3] = FRDGColorAttachmentParameter{Inputs.SceneColor, {ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GBuffer->ModelData) Parameters->Colors[4] = FRDGColorAttachmentParameter{*GBuffer->ModelData, {ERHITextureAspect::Color, 0, 1, 0, 1}};
		Parameters->Depth = FRDGDepthStencilAttachmentParameter{
			.Texture = Inputs.Depth,
			.Range = {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		Parameters->InstanceCulling = DrawParams;
		const auto GBufferPass = Graph.AddPass(GBufferPassName, ERDGPassType::Graphics,
			std::move(Parameters),
			[&StaticMeshes = Inputs.StaticMeshes,
				&Resolved = Inputs.Resolved,
				&Telemetry = Inputs.Telemetry,
				&View = Inputs.View, &Receiver = Inputs.Receiver, &Options,
				Width, Height, bWantsIsolatedDeferred](
				FRHICommandListImmediate& Commands,
				const FGBufferPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				const FRDGAttachmentView Depth =
					Resolver.GetDepthStencilAttachment(PassParameters.Depth);
				FRHITexture* SceneDepth = Depth.Texture;
				auto GBufferTargets = SceneTextureGroups::ResolveGBuffer(
					Resolver, PassParameters.Colors[0], PassParameters.Colors[1], PassParameters.Colors[2]
				);
				if (GBufferTargets) GBufferTargets->ModelData = Resolver.GetColorAttachment(PassParameters.Colors[4]).Texture;
				const auto OverrideArgs = GetMeshDrawCommandOverrideArgs(PassParameters.InstanceCulling, Resolver);
				Resolver.WriteValue(PassParameters.Completion) =
					RecordGBuffer_RenderThread(
						Commands, View, Receiver, Resolved, Telemetry,
						StaticMeshes, SceneDepth, Resolver.GetColorAttachment(PassParameters.Colors[3]).Texture,
						GBufferTargets ? &*GBufferTargets : nullptr,
						Options, Width, Height,
						bWantsIsolatedDeferred, OverrideArgs
					);
			});
		if (bHasGPUCandidates && bObserveVisibility)
			Inputs.Telemetry.View.GBuffer.GPUCullingVisibility = Inputs.MeshDrawCommandStats.QueueDrawRDGIndirectArgsReadback(
				Graph, {RendererPrivate::EMeshPipelinePass::GBuffer}, *DrawParams.DrawIndirectArgsBuffer,
				static_cast<uint32>(GPUCullingPlan->GetCandidates().size()), GPUCullingPlan->GetArguments(), GBufferCompletion,
				[Plan = MeshPass.GetInstanceCullingContext()->shared_from_this()](const FGBufferPassResult& Result) {
					return Plan->IsCullingDispatched() && Result.IsComplete();
				});
		if (GetGBufferCaptureSink() || GetGBufferModelDataCaptureSink() || GetGBufferTimingQuerySink())
			Graph.MarkPassRoot(GBufferPass, "GBuffer qualification observation");
		FGBufferGraphOutput Output{.Completion = GBufferCompletion, .Textures = GBuffer, .Depth = Inputs.Depth, .BasePassColor = Inputs.SceneColor};
		if (Inputs.Options.GBufferDebugMode == EGBufferDebugMode::Emissive
			|| Inputs.Options.GBufferDebugMode == EGBufferDebugMode::MaterialInputs)
		{
			Output.BasePassDebug = Graph.CreateTexture({.Texture = FRHITextureCreateDesc::Create2D("Scene.BasePassDebug", Width, Height, EPixelFormat::RGBA16_FLOAT).SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy), .ObservationTag = static_cast<uint32>(ERDGAllocationObservation::GBufferDebug)}, "Scene.BasePassDebug");
			Output.BasePassDebugCompletion = Graph.CreateValue<FGBufferPassResult>("Scene.BasePassDebugValue", "gbuffer-result");
			auto Debug = Graph.AllocParameters<FBasePassDebugPassParameters>();
			Debug->GBuffer = {GBufferCompletion};
			Debug->Completion = {*Output.BasePassDebugCompletion};
			SceneTextureGroups::FillGBuffer(GBuffer, Debug->Textures);
			Debug->Seed = FRDGTextureParameter{Inputs.SceneColor, {ERHITextureAspect::Color, 0, 1, 0, 1}};
			Debug->Depth = FRDGTextureParameter{Inputs.Depth, {ERHITextureAspect::Depth, 0, 1, 0, 1}};
			Debug->Output = FRDGColorAttachmentParameter{*Output.BasePassDebug, {ERHITextureAspect::Color, 0, 1, 0, 1}};
			(void)Graph.AddPass("Scene.BasePassDebug", ERDGPassType::Graphics, std::move(Debug), [&Renderer = Inputs.DebugRenderer, &View = Inputs.View, Width, Height, Mode = Inputs.Options.GBufferDebugMode](FRHICommandListImmediate& Commands, const FBasePassDebugPassParameters& P, const FRDGParameterResolver& Resolver) {
				auto& Result = Resolver.WriteValue(P.Completion);
				Result.Status = EScenePassStatus::Failed;
				if (!Resolver.ReadValue(P.GBuffer).IsComplete()) return;
				const auto G = SceneTextureGroups::ResolveGBuffer(Resolver, P.Textures);
				if (G && Renderer.Render_RenderThread(Commands, G->Material, G->Normals, G->Surface, Resolver.GetTexture(P.Seed), Resolver.GetTexture(P.Depth), Resolver.GetColorAttachment(P.Output).Texture, View, Mode, Width, Height)) Result.Status = EScenePassStatus::Complete;
			});
		}
		return Output;
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
			FRHITexture* SceneColor,
			const FGBufferRenderer::FTargets* GBufferTargets,
			const FSceneViewRenderOptions& Options,
			uint32 Width,
			uint32 Height,
			bool bWantsIsolatedDeferred,
			const RendererPrivate::FMeshDrawCommandOverrideArgs& OverrideArgs
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
					RenderTargetLayouts::MakeGBufferTargets(GBufferTargets->ModelData != nullptr);
				GBufferPassInfo.ColorRenderTargets[0] =
					GBufferTargets->Material;
				GBufferPassInfo.ColorRenderTargets[1] =
					GBufferTargets->Normals;
				GBufferPassInfo.ColorRenderTargets[2] =
					GBufferTargets->Surface;
				GBufferPassInfo.ColorRenderTargets[3] =
					SceneColor;
				GBufferPassInfo.DepthStencilRenderTarget = SceneDepth;
				if (GBufferTargets->ModelData) GBufferPassInfo.ColorRenderTargets[4] = GBufferTargets->ModelData;
				for (uint32 Index = 0; Index < GBufferPassInfo.RenderTargetLayout.NumColorRenderTargets; ++Index)
				{
					GBufferPassInfo.ColorClearValues[Index] =
						FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f);
				}
				GBufferPassInfo.ColorClearValues[3] = FClearValueBinding(RenderView.ClearColor.r, RenderView.ClearColor.g, RenderView.ClearColor.b, RenderView.ClearColor.a);
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
					CommandList, ResolvedSceneResources.Receiver.StaticMeshes,
					RenderView,
					OverrideArgs
				);
				if (Options.bEnableGPUCulling)
				{
					const auto& Submissions = ResolvedSceneResources.Receiver.StaticMeshes.Observations;
					Telemetry.View.GBuffer.GPUCullingIndirectDraws += Submissions.GBufferIndirectSubmissions;
					Telemetry.View.GBuffer.GPUCullingDirectDraws += Submissions.GBufferLocalDirectSubmissions;
				}
				CommandList.EndRenderPass();
				Result.Status = StaticResult.bComplete ? EScenePassStatus::Complete : EScenePassStatus::Failed;
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
						SceneColor,
						SceneDepth
					);
				}
				if (const auto Sink = GetGBufferModelDataCaptureSink()) Sink(CommandList, GBufferTargets->ModelData);
				++Telemetry.View.GBuffer.GBufferEnabledViews;
				Telemetry.View.GBuffer.GBufferAttachmentBytes =
					FGBufferRenderer::CalculateTargetBytes(Width, Height, GBufferTargets->ModelData != nullptr);
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
