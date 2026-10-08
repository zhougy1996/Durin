#include "Renderers/GBufferRendering.h"
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
#include "Renderers/StaticMeshDrawExecution.h"

namespace Durin
{
	auto BuildGBufferGPUCullingPlan(const FPreparedStaticMeshView& Prepared,
		const FResolvedStaticMeshView& Resolved,
		const FSceneView& View,
		bool bRequested, bool bIndirectDrawSupported,
		size_t* OutOverflowCandidates)
		-> std::shared_ptr<FGBufferGPUCullingPlan>
	{
			if (OutOverflowCandidates) *OutOverflowCandidates = 0;
			if (!bRequested || !bIndirectDrawSupported) return {};
			auto Plan = std::make_shared<FGBufferGPUCullingPlan>();
			Plan->ArgumentByResolvedDraw.assign(Resolved.Draws.size(),
				FGBufferGPUCullingPlan::InvalidArgument);
			using FGroupKey = std::tuple<const void*, const void*, const void*,
				const void*>;
			struct FPendingCandidate final
			{
				const FPreparedStaticMeshDraw* Draw = nullptr;
				const FPreparedStaticMeshPrimitive* Primitive = nullptr;
				FVector3f BoundsMin{};
				FVector3f BoundsMax{};
			};
			std::map<FGroupKey, size_t> GroupByKey;
			std::vector<std::vector<FPendingCandidate>> Groups;
			for (const FPreparedStaticMeshDraw& Draw : Prepared.Opaque)
			{
				const FPreparedStaticMeshPrimitive* Primitive =
					Prepared.GetPrimitive(Draw);
				if (Primitive == nullptr || !Resolved.IsReady(Draw)
					|| Primitive->VertexDomain != EVertexDeformationDomain::Local
					|| !Draw.Command || !Draw.Command->bSupportsGBuffer
					|| !Draw.Command->Geometry.bIndexed
					|| Draw.Command->Geometry.InstanceCount != 1
					|| Draw.ResolvedIndex >= Plan->ArgumentByResolvedDraw.size()
					|| !Primitive->WorldBounds.bIsValid
					|| !Math::IsFinite(Primitive->WorldBounds.Min)
					|| !Math::IsFinite(Primitive->WorldBounds.Max))
					continue;
				const FVector3 Center = Primitive->WorldBounds.GetCenter();
				const FVector3 Extent = (Primitive->WorldBounds.Max
					- Primitive->WorldBounds.Min) * 0.5;
				const double RadiusSquared = Math::Dot(Extent, Extent);
				if (!Math::IsFinite(Center) || !std::isfinite(RadiusSquared)
					|| RadiusSquared < 0.0)
					continue;
				const FVector3f BoundsMin(View.TranslateWorldPosition(Primitive->WorldBounds.Min));
				const FVector3f BoundsMax(View.TranslateWorldPosition(Primitive->WorldBounds.Max));
				if (!Math::IsFinite(BoundsMin) || !Math::IsFinite(BoundsMax))
					continue;
				const auto& Record = Resolved.Draws[Draw.ResolvedIndex];
				if (!Record.GBufferGPUCullingPipeline
					|| !Record.GBufferGPUCullingBindings
					|| !Primitive->CollectedBinding)
					continue;
				const FGroupKey Key{Draw.Command.get(),
					Primitive->CollectedBinding.get(),
					Record.GBufferGPUCullingPipeline.get(),
					Record.GBufferGPUCullingBindings.get()};
				auto [GroupIt, bInserted] = GroupByKey.try_emplace(
					Key, Groups.size());
				if (bInserted) Groups.emplace_back();
				Groups[GroupIt->second].push_back(
					{&Draw, Primitive, BoundsMin, BoundsMax});
			}

			constexpr size_t PerCandidateBytes =
				sizeof(FGBufferGPUCullingCandidate) + sizeof(uint32)
				+ sizeof(RendererPrivate::FStaticMeshTransformUniform);
			for (const auto& Group : Groups)
			{
				if (Group.size() < FGBufferGPUCullingPlan::MinimumGroupCandidates)
					continue;
				const size_t RequiredBytes = Group.size() * PerCandidateBytes
					+ sizeof(FRHIDrawIndexedIndirectArguments);
				const size_t CurrentBytes = Plan->Candidates.size()
					* PerCandidateBytes + Plan->Arguments.size()
					* sizeof(FRHIDrawIndexedIndirectArguments);
				if (Group.size() > FGBufferGPUCullingPlan::MaximumCandidates
					- Plan->Candidates.size()
					|| RequiredBytes > FGBufferGPUCullingPlan::MaximumGeneratedBytes
					- CurrentBytes)
				{
					if (OutOverflowCandidates)
						*OutOverflowCandidates += Group.size();
					continue;
				}
				const uint32 ArgumentIndex = static_cast<uint32>(
					Plan->Arguments.size());
				const FRHIDrawIndexedArguments Direct =
					Group.front().Draw->Command->Geometry.GetIndexedDrawArguments();
				Plan->Arguments.push_back({.IndexCount = Direct.IndexCount,
					.InstanceCount = 0, .FirstIndex = Direct.FirstIndex,
					.VertexOffset = Direct.VertexOffset,
					.FirstInstance = static_cast<uint32>(Plan->Candidates.size())});
				for (size_t GroupIndex = 0; GroupIndex < Group.size(); ++GroupIndex)
				{
					const FPreparedStaticMeshDraw& Draw = *Group[GroupIndex].Draw;
					const FPreparedStaticMeshPrimitive& Primitive =
						*Group[GroupIndex].Primitive;
					Plan->ArgumentByResolvedDraw[Draw.ResolvedIndex] = GroupIndex == 0
						? ArgumentIndex : FGBufferGPUCullingPlan::GroupedMember;
					const uint32 Index = static_cast<uint32>(Plan->Candidates.size());
					Plan->Candidates.push_back({
						.BoundsMin = Group[GroupIndex].BoundsMin,
						.ArgumentIndex = ArgumentIndex,
						.BoundsMax = Group[GroupIndex].BoundsMax,
						.TransformIndex = Index});
					const auto Transform = RendererPrivate::BuildMeshTransformUniform(View, Primitive);
					Plan->Transforms.push_back(Transform);
				}
			}
			return Plan->Candidates.empty() ? nullptr : Plan;
	}

	namespace
	{
		auto RecordGBuffer_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& RenderView,
			const FPreparedReceiverGeometry& Receiver,
			FResolvedSceneResources& Resolved,
			FSceneRenderTelemetry& Telemetry,
			FGBufferRenderer& GBufferRenderer,
			FStaticMeshRenderer& StaticMeshRenderer,
			const FPostProcessRenderer::FSceneTargets& SceneTargets,
			const FGBufferRenderer::FTargets* GBufferTargets,
			const FSceneViewRenderOptions& Options,
			uint32 Width, uint32 Height,
			bool bWantsIsolatedDeferred,
			const std::shared_ptr<FGBufferGPUCullingPlan>& GPUCullingPlan,
			FRHIBuffer* IndirectArguments,
			FRHIBuffer* VisibleInstances,
			FRHIBuffer* InstanceTransforms)
			-> FGBufferPassResult;
	}

	auto FGBufferGPUCullingPassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FGBufferGPUCullingPassParameters;
		static const std::array Members{
			WithRDGShaderBinding(MakeRDGResourceParameterMemberMetadata<
				FParameters, decltype(Candidates), FRDGBufferParameter>(
					"Candidates", offsetof(FParameters, Candidates),
					ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer,
					ERDGParameterRangeKind::BufferBytes, ERDGUse::ReadWrite,
					ERHIAccess::ComputeShaderReadWrite), ERHIBindingType::StorageBuffer),
			WithRDGShaderBinding(MakeRDGResourceParameterMemberMetadata<
				FParameters, decltype(VisibleInstances), FRDGBufferParameter>(
					"VisibleInstances", offsetof(FParameters, VisibleInstances),
					ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer,
					ERDGParameterRangeKind::BufferBytes, ERDGUse::ReadWrite,
					ERHIAccess::ComputeShaderReadWrite),
				ERHIBindingType::StorageBuffer),
			WithRDGShaderBinding(MakeRDGResourceParameterMemberMetadata<
				FParameters, decltype(Arguments), FRDGBufferParameter>(
					"IndirectArguments", offsetof(FParameters, Arguments),
					ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer,
					ERDGParameterRangeKind::BufferBytes, ERDGUse::ReadWrite,
					ERHIAccess::ComputeShaderReadWrite),
				ERHIBindingType::StorageBuffer),
		};
		static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>(
			"FGBufferGPUCullingPassParameters", Members);
		return &Metadata;
	}

	auto FGBufferPassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		static const std::array Members{
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
			MakeRDGResourceParameterMemberMetadata<FGBufferPassParameters,
				decltype(InstanceTransforms), FRDGBufferParameter>(
					"InstanceTransforms", offsetof(FGBufferPassParameters, InstanceTransforms),
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
		const auto GPUCullingPlan = BuildGBufferGPUCullingPlan(
			Inputs.Receiver.StaticMeshes, Inputs.Resolved.Receiver.StaticMeshes,
			Inputs.View,
			Options.bEnableGPUCulling,
			Capabilities && Capabilities->bSupportsIndirectDraw,
			&OverflowCandidates);
		Inputs.Telemetry.View.GBuffer.GPUCullingOverflowCandidates +=
			OverflowCandidates;
		if (Options.bEnableGPUCulling)
		{
			++Inputs.Telemetry.View.GBuffer.GPUCullingRequestedViews;
			if (!GPUCullingPlan)
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
		std::optional<FRDGBufferHandle> VisibleInstances;
		std::optional<FRDGBufferHandle> InstanceTransforms;
		std::optional<FRDGBufferHandle> IndirectArguments;
		if (GPUCullingPlan)
		{
			const uint32 CandidateBytes = static_cast<uint32>(
				GPUCullingPlan->Candidates.size()
				* sizeof(FGBufferGPUCullingCandidate));
			const uint32 VisibleBytes = static_cast<uint32>(
				GPUCullingPlan->Candidates.size() * sizeof(uint32));
			const uint32 ArgumentBytes = static_cast<uint32>(
				GPUCullingPlan->Arguments.size()
				* sizeof(FRHIDrawIndexedIndirectArguments));
			const uint32 TransformBytes = static_cast<uint32>(
				GPUCullingPlan->Transforms.size()
				* sizeof(RendererPrivate::FStaticMeshTransformUniform));
			Inputs.Telemetry.View.GBuffer.GPUCullingCandidates +=
				GPUCullingPlan->Candidates.size();
			Inputs.Telemetry.View.GBuffer.GPUCullingGroups +=
				GPUCullingPlan->Arguments.size();
			Inputs.Telemetry.View.GBuffer.GPUCullingCommands +=
				GPUCullingPlan->Arguments.size();
			Inputs.Telemetry.View.GBuffer.GPUCullingBufferBytes +=
				CandidateBytes + VisibleBytes + ArgumentBytes + TransformBytes;
			Inputs.Telemetry.View.GBuffer.GPUCullingAsyncComputeEligibleViews +=
				Options.bEnableAsyncCompute ? 1u : 0u;
			const auto Candidates = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(
				CandidateBytes, sizeof(FGBufferGPUCullingCandidate),
				EBufferUsageFlags::StructuredBuffer
					| EBufferUsageFlags::UnorderedAccess
					| EBufferUsageFlags::DestinationCopy)},
				"Scene.GBuffer.GPUCulling.Candidates");
			VisibleInstances = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(
				VisibleBytes, sizeof(uint32), EBufferUsageFlags::StructuredBuffer
					| EBufferUsageFlags::UnorderedAccess
					| EBufferUsageFlags::DestinationCopy)},
				"Scene.GBuffer.GPUCulling.VisibleInstances");
			InstanceTransforms = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(
				TransformBytes,
				sizeof(RendererPrivate::FStaticMeshTransformUniform),
				EBufferUsageFlags::StructuredBuffer
					| EBufferUsageFlags::DestinationCopy)},
				"Scene.GBuffer.GPUCulling.InstanceTransforms");
			IndirectArguments = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(
				ArgumentBytes, sizeof(uint32), EBufferUsageFlags::UnorderedAccess
					| EBufferUsageFlags::DrawIndirect
					| EBufferUsageFlags::DestinationCopy)},
				"Scene.GBuffer.GPUCulling.Arguments");
			Graph.QueueBufferUpload(Candidates, 0,
				std::as_bytes(std::span(GPUCullingPlan->Candidates)));
			Graph.QueueBufferUploadOwned(*VisibleInstances, 0,
				FByteBuffer(VisibleBytes));
			Graph.QueueBufferUpload(*InstanceTransforms, 0,
				std::as_bytes(std::span(GPUCullingPlan->Transforms)));
			Graph.QueueBufferUpload(*IndirectArguments, 0,
				std::as_bytes(std::span(GPUCullingPlan->Arguments)));
			auto CullParameters = Graph.AllocParameters<
				FGBufferGPUCullingPassParameters>();
			CullParameters->Candidates = {Candidates, 0, CandidateBytes};
			CullParameters->VisibleInstances = {*VisibleInstances, 0, VisibleBytes};
			CullParameters->Arguments = {*IndirectArguments, 0, ArgumentBytes};
			const auto CullPass = Graph.AddPass("Scene.GBuffer.GPUCulling",
				ERDGPassType::Compute, std::move(CullParameters),
				[Plan = GPUCullingPlan, &Renderer = Inputs.Renderer,
					&Telemetry = Inputs.Telemetry,
					&View = Inputs.View](FRHICommandListImmediate& Commands,
						const FGBufferGPUCullingPassParameters& Parameters,
						const FRDGParameterResolver& Resolver) {
					const auto ShaderParameters = Resolver.GetShaderParameters(Parameters);
					TScopedRendererGPUTimingQuery GPUCullingTiming(
						Commands, GetGPUCullingTimingQuerySink());
					Plan->bDispatched = Renderer.DispatchGPUCulling_RenderThread(
						Commands, View, static_cast<uint32>(Plan->Candidates.size()),
						ShaderParameters);
					if (Plan->bDispatched)
					{
						GPUCullingTiming.Commit();
						++Telemetry.View.GBuffer.GPUCullingDispatchedViews;
					}
					else
					{
						++Telemetry.View.GBuffer.GPUCullingFallbackViews;
						++Telemetry.View.GBuffer.GPUCullingDispatchFailureViews;
					}
				});
			Graph.SetPassAsyncComputeEligible(CullPass);
		}
		auto Parameters = Graph.AllocParameters<FGBufferPassParameters>();
		Parameters->Completion = {GBufferCompletion};
		SceneTextureGroups::FillGBuffer(GBuffer, Parameters->Colors);
		Parameters->Depth = FRDGDepthStencilAttachmentParameter{
			.Texture = Inputs.Depth,
			.Range = {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		if (GPUCullingPlan)
		{
			Parameters->VisibleInstances = FRDGBufferParameter{
				*VisibleInstances, 0,
				GPUCullingPlan->Candidates.size() * sizeof(uint32)};
			Parameters->InstanceTransforms = FRDGBufferParameter{
				*InstanceTransforms, 0, GPUCullingPlan->Transforms.size()
					* sizeof(RendererPrivate::FStaticMeshTransformUniform)};
			Parameters->Arguments = FRDGBufferParameter{
				*IndirectArguments, 0, GPUCullingPlan->Arguments.size()
					* sizeof(FRHIDrawIndexedIndirectArguments)};
		}
		const auto GBufferPass = Graph.AddPass(GBufferPassName, ERDGPassType::Graphics,
			std::move(Parameters),
			[&Renderer = Inputs.Renderer,
				&StaticMeshes = Inputs.StaticMeshes,
				&Resolved = Inputs.Resolved,
				&Telemetry = Inputs.Telemetry,
				&View = Inputs.View, &Receiver = Inputs.Receiver, &Options,
				Width, Height, bWantsIsolatedDeferred, GPUCullingPlan](
				FRHICommandListImmediate& Commands,
				const FGBufferPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				const FRDGAttachmentView Depth =
					Resolver.GetDepthStencilAttachment(PassParameters.Depth);
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = Depth.Texture};
				const auto GBufferTargets = SceneTextureGroups::ResolveGBuffer(
					Resolver, PassParameters.Colors);
				FRHIBuffer* ArgumentBuffer = PassParameters.Arguments
					? Resolver.GetBuffer(*PassParameters.Arguments) : nullptr;
				FRHIBuffer* VisibleBuffer = PassParameters.VisibleInstances
					? Resolver.GetBuffer(*PassParameters.VisibleInstances) : nullptr;
				FRHIBuffer* TransformBuffer = PassParameters.InstanceTransforms
					? Resolver.GetBuffer(*PassParameters.InstanceTransforms) : nullptr;
				Resolver.WriteValue(PassParameters.Completion) =
					RecordGBuffer_RenderThread(
					Commands, View, Receiver, Resolved, Telemetry, Renderer,
					StaticMeshes, SceneTargets,
					GBufferTargets ? &*GBufferTargets : nullptr,
					Options, Width, Height,
					bWantsIsolatedDeferred, GPUCullingPlan, ArgumentBuffer,
					VisibleBuffer, TransformBuffer);
			});
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
		FGBufferRenderer& GBufferRenderer,
		FStaticMeshRenderer& StaticMeshRenderer,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsIsolatedDeferred,
		const std::shared_ptr<FGBufferGPUCullingPlan>& GPUCullingPlan,
		FRHIBuffer* IndirectArguments,
		FRHIBuffer* VisibleInstances,
		FRHIBuffer* InstanceTransforms
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
			GBufferPassInfo.DepthStencilRenderTarget = SceneTargets.Depth;
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
				CommandList, RenderView, GBufferRenderer,
				Receiver.StaticMeshes,
				ResolvedSceneResources.Receiver.StaticMeshes,
				GPUCullingPlan && GPUCullingPlan->bDispatched
					? &GPUCullingPlan->ArgumentByResolvedDraw : nullptr,
				GPUCullingPlan && GPUCullingPlan->bDispatched
					? IndirectArguments : nullptr,
				GPUCullingPlan && GPUCullingPlan->bDispatched
					? VisibleInstances : nullptr,
				GPUCullingPlan && GPUCullingPlan->bDispatched
					? InstanceTransforms : nullptr
			);
			if (GPUCullingPlan)
			{
				if (GPUCullingPlan->bDispatched)
					Telemetry.View.GBuffer.GPUCullingIndirectDraws +=
						GPUCullingPlan->Arguments.size();
				else
					Telemetry.View.GBuffer.GPUCullingDirectFallbackDraws +=
						GPUCullingPlan->Arguments.size();
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
					SceneTargets.Depth
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
