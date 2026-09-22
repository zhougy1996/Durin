#include "Renderers/DirectionalShadowRenderer.h"

#include "RenderResourceCreation.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/ForwardLighting.h"
#include "Renderers/SceneRenderPlan.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Renderers/StaticMeshRenderer.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"

#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		std::atomic<FShadowDepthTimingQuerySink> GShadowDepthTimingQuerySink = nullptr;
		std::atomic<FShadowDepthCaptureSink> GShadowDepthCaptureSink = nullptr;
	}

	struct FDirectionalShadowRenderer::FState
	{
		struct FResources
		{
			FTextureRHIRef Target;
			FTextureViewRHIRef SampledView;
			std::array<FTextureViewRHIRef,
				DirectionalShadowCascadeCount> DepthAttachmentViews;
			FSamplerRHIRef Sampler;
		};

		TRenderResourceCreationSlot<FResources> Resources{
			ERenderResourceGenerationDependency::Device
				| ERenderResourceGenerationDependency::Manual};
		FRHIUniformBufferRange FallbackLighting;
	};

	auto SetShadowDepthTimingQuerySink(FShadowDepthTimingQuerySink Sink) -> void
	{
		GShadowDepthTimingQuerySink.store(Sink, std::memory_order_release);
	}

	auto SetShadowDepthCaptureSink(FShadowDepthCaptureSink Sink) -> void
	{
		GShadowDepthCaptureSink.store(Sink, std::memory_order_release);
	}

	auto HasShadowDepthTimingQuerySink() -> bool
	{
		return GShadowDepthTimingQuerySink.load(std::memory_order_acquire) != nullptr;
	}

	FDirectionalShadowRenderer::FDirectionalShadowRenderer(
		FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator), State(std::make_unique<FState>())
	{
	}

	FDirectionalShadowRenderer::~FDirectionalShadowRenderer() = default;

	auto FDirectionalShadowRenderer::PrepareResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		FStaticMeshRenderer& StaticMeshes,
		const FPreparedDirectionalShadow& Shadow,
		FResolvedDirectionalShadow& ResolvedShadow,
		FViewRenderTelemetry& Telemetry) -> bool
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const auto PreparationStart = std::chrono::steady_clock::now();
		struct FPreparationTimingScope
		{
			std::chrono::steady_clock::time_point Start;
			uint64& Nanoseconds;
			~FPreparationTimingScope()
			{
				Nanoseconds = static_cast<uint64>(std::chrono::duration_cast<
					std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - Start).count());
			}
		} PreparationTimingScope{
			PreparationStart, Telemetry.DirectionalShadow.ShadowResourcePreparationNanoseconds};
		using FResult = TRenderResourceCreateResult<FState::FResources>;
		FState::FResources* Resources = State->Resources.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[&CommandList]() -> FResult {
				FState::FResources Candidate;
				FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2DArray(
					"DirectionalShadowDepthArray")
					.SetExtent(DirectionalShadowResolution,
						DirectionalShadowResolution)
					.SetArraySize(DirectionalShadowCascadeCount)
					.SetFormat(EPixelFormat::D32)
					.SetFlags(ETextureCreateFlags::DepthStencilTargetable
						| ETextureCreateFlags::ShaderResource)
					.SetClearValue(FClearValueBinding(1.0f, 0u));
				Candidate.Target = GDynamicRHI != nullptr
					? GDynamicRHI->RHICreateTexture(CommandList, Desc) : nullptr;
				if (Candidate.Target != nullptr)
				{
					Candidate.SampledView = GDynamicRHI->RHICreateTextureView(
						Candidate.Target,
						MakeDefaultTextureViewDesc(
							*Candidate.Target, ERHITextureViewUsage::Sampled));
					for (uint32 Layer = 0;
						Layer < DirectionalShadowCascadeCount; ++Layer)
					{
						FRHITextureViewDesc Attachment = MakeDefaultTextureViewDesc(
							*Candidate.Target,
							ERHITextureViewUsage::DepthStencilAttachment);
						Attachment.Dimension = ERHITextureViewDimension::Texture2D;
						Attachment.Range.FirstArrayLayer = Layer;
						Attachment.Range.NumArrayLayers = 1;
						Candidate.DepthAttachmentViews[Layer] =
							GDynamicRHI->RHICreateTextureView(
								Candidate.Target, Attachment);
					}
				}
				Candidate.Sampler = RHICreateSampler(
					MakeDirectionalShadowSamplerDesc());
				const bool bHasAllAttachmentViews = std::ranges::all_of(
					Candidate.DepthAttachmentViews,
					[](const FTextureViewRHIRef& View) { return View != nullptr; });
				if (!Candidate.Target || !Candidate.SampledView
					|| !bHasAllAttachmentViews || !Candidate.Sampler)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"DirectionalShadow", "3x2048-D32-array",
						ERenderResourceCreateErrorReason::ResourceCreationFailed,
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				const std::array InitialTransition{FRHITextureTransition::Whole(
					Candidate.Target, ERHIAccess::Discard,
					ERHIAccess::GraphicsShaderRead)};
				CommandList.TransitionTextures(InitialTransition);
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		ResolvedShadow.bEnabled = false;
		if (!Shadow.View.bEnabled) return false;
		++Telemetry.DirectionalShadow.ShadowResourceAttempts;
		if (Resources == nullptr)
		{
			++Telemetry.DirectionalShadow.ShadowResourceFailures;
			return false;
		}
		++Telemetry.DirectionalShadow.ShadowResourceSuccesses;
		Telemetry.DirectionalShadow.ShadowTargetLogicalBytes = DirectionalShadowLogicalBytes;
		Telemetry.DirectionalShadow.ShadowTargetBackendBytes = static_cast<size_t>(
			Resources->Target->GetBackendAllocationBytes());

		const FForwardLightingUniform FullyUnlit{};
		State->FallbackLighting = CommandList.AllocateDynamicUniformBuffer(
			&FullyUnlit, sizeof(FullyUnlit));
		bool bReady = State->FallbackLighting.Buffer != nullptr;
		for (uint32 Cascade = 0;
			Cascade < Shadow.View.CascadeCount; ++Cascade)
		{
			bReady = !Shadow.StaticMeshes[Cascade].bResourceFailure && StaticMeshes.PrepareShadowResources_RenderThread(
				CommandList, Shadow.StaticMeshes[Cascade],
				ResolvedShadow.StaticMeshes[Cascade]) && bReady;
			if (bReady) bReady = StaticMeshes.PrepareUniforms_RenderThread(CommandList,
				Shadow.View.Cascades[Cascade].CasterView, Shadow.StaticMeshes[Cascade],
				ResolvedShadow.StaticMeshes[Cascade], false, false, true);
			if (bReady) bReady = StaticMeshes.PrepareBindings_RenderThread(CommandList, nullptr,
				Shadow.StaticMeshes[Cascade], ResolvedShadow.StaticMeshes[Cascade], State->FallbackLighting, true);
		}
		if (!bReady)
		{
			++Telemetry.DirectionalShadow.ShadowPreparationFailures;
		}
		ResolvedShadow.bEnabled = bReady;
		return bReady;
	}

	auto FDirectionalShadowRenderer::CaptureCascade_RenderThread(FRHITexture* Target, uint32 Cascade) const
		-> std::optional<FDirectionalShadowCascadeRecording>
	{
		check(IsInRenderingThread());
		const auto* Resources = State->Resources.GetPayload();
		if (!Resources || !Resources->Target || Target != Resources->Target
			|| Cascade >= Resources->DepthAttachmentViews.size() || !Resources->DepthAttachmentViews[Cascade]) return {};
		return FDirectionalShadowCascadeRecording{Resources->Target, Resources->DepthAttachmentViews[Cascade]};
	}

	auto FDirectionalShadowRenderer::RecordCascade(FRHICommandList& Commands,
		const FDirectionalShadowCascadeRecording& Recording,
		const FPreparedStaticMeshView& Prepared, const FResolvedStaticMeshView& Resolved)
		-> FStaticMeshRenderObservations
	{
		check(!Commands.IsInsideRenderPass());
		check(Recording.Target && Recording.DepthAttachment);
		Commands.SwitchPipeline(ERHIPipeline::Graphics);
		FRHIRenderPassInfo Pass{};
		Pass.RenderTargetLayout = RenderTargetLayouts::MakeDirectionalShadowDepth();
		Pass.DepthStencilRenderTarget = Recording.Target;
		Pass.DepthStencilRenderTargetView = Recording.DepthAttachment;
		Pass.DepthStencilClearValue = FClearValueBinding(1.0f, 0u);
		Commands.BeginRenderPass(Pass, "DirectionalShadowCascadeDepthRenderPass");
		Commands.SetViewport(0.0f, 0.0f, 0.0f, static_cast<float>(DirectionalShadowResolution),
			static_cast<float>(DirectionalShadowResolution), 1.0f);
		Commands.SetScissor(0.0f, 0.0f, static_cast<float>(DirectionalShadowResolution), static_cast<float>(DirectionalShadowResolution));
		auto Counts = FStaticMeshRenderer::RecordShadow(Commands, Prepared, Resolved);
		Commands.EndRenderPass();
		return Counts;
	}

	auto FDirectionalShadowRenderer::Render_RenderThread(
		FRHICommandListImmediate& CommandList,
		FRHITexture* Target,
		const FPreparedDirectionalShadow& Shadow,
		FResolvedDirectionalShadow& ResolvedShadow,
		FViewRenderTelemetry& Telemetry) -> bool
	{
		check(!CommandList.IsInsideRenderPass());
		FState::FResources* Resources = State->Resources.GetPayload();
		if (!ResolvedShadow.bEnabled || Resources == nullptr
			|| Resources->Target == nullptr || Target != Resources->Target)
			return false;
		FGPUTimingQueryRHIRef TimingQuery;
		const FShadowDepthTimingQuerySink Sink =
			GShadowDepthTimingQuerySink.load(std::memory_order_acquire);
		if (Sink != nullptr && GDynamicRHI != nullptr)
		{
			TimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
			if (TimingQuery) CommandList.BeginGPUTimingQuery(TimingQuery);
		}
		std::array<FStaticMeshRenderObservations, DirectionalShadowCascadeCount> Counts;
		for (uint32 CascadeIndex = 0;
			CascadeIndex < Shadow.View.CascadeCount; ++CascadeIndex)
		{
			const auto Recording = CaptureCascade_RenderThread(Target, CascadeIndex);
			require(Recording.has_value());
			Counts[CascadeIndex] = RecordCascade(CommandList, *Recording, Shadow.StaticMeshes[CascadeIndex],
				ResolvedShadow.StaticMeshes[CascadeIndex]);
		}
		if (TimingQuery)
		{
			CommandList.EndGPUTimingQuery(TimingQuery);
			Sink(TimingQuery);
		}
		Complete_RenderThread(CommandList, Target, std::span(Counts).first(Shadow.View.CascadeCount),
			ResolvedShadow, Telemetry);
		return true;
	}

	auto FDirectionalShadowRenderer::Complete_RenderThread(FRHICommandListImmediate& Commands,
		FRHITexture* Target, std::span<const FStaticMeshRenderObservations> Counts,
		FResolvedDirectionalShadow& ResolvedShadow, FViewRenderTelemetry& Telemetry) -> void
	{
		check(IsInRenderingThread());
		for (uint32 CascadeIndex = 0; CascadeIndex < Counts.size(); ++CascadeIndex)
		{
			Telemetry.DirectionalShadow.ShadowWorkerRecordingChunks += Counts[CascadeIndex].bRecordedOnWorker;
			auto& Observations = ResolvedShadow.StaticMeshes[CascadeIndex].Observations;
			Observations.AttemptedDraws += Counts[CascadeIndex].AttemptedDraws;
			Observations.SuccessfulDraws += Counts[CascadeIndex].SuccessfulDraws;
			Observations.RejectedDraws += Counts[CascadeIndex].RejectedDraws;
			auto& CascadeTelemetry = Telemetry.DirectionalShadow.ShadowCascades[CascadeIndex];
			CascadeTelemetry.AttemptedDraws =
				ResolvedShadow.StaticMeshes[CascadeIndex].Observations.AttemptedDraws;
			CascadeTelemetry.SuccessfulDraws =
				ResolvedShadow.StaticMeshes[CascadeIndex].Observations.SuccessfulDraws;
			CascadeTelemetry.RejectedDraws =
				CascadeTelemetry.AttemptedDraws
					- CascadeTelemetry.SuccessfulDraws;
			Telemetry.DirectionalShadow.ShadowAttemptedDraws += CascadeTelemetry.AttemptedDraws;
			Telemetry.DirectionalShadow.ShadowSuccessfulDraws += CascadeTelemetry.SuccessfulDraws;
		}
		if (const auto Capture = GShadowDepthCaptureSink.load(std::memory_order_acquire))
			Capture(Commands, Target, static_cast<uint32>(Counts.size()));
		Telemetry.DirectionalShadow.ShadowRejectedDraws =
			Telemetry.DirectionalShadow.ShadowAttemptedDraws
				- Telemetry.DirectionalShadow.ShadowSuccessfulDraws;
	}

	auto FDirectionalShadowRenderer::GetTexture_RenderThread() const
		-> FRHITexture*
	{
		const FState::FResources* Resources = State->Resources.GetPayload();
		return Resources != nullptr ? Resources->Target.GetReference() : nullptr;
	}

	auto FDirectionalShadowRenderer::GetSampledView_RenderThread() const
		-> FRHITextureView*
	{
		const FState::FResources* Resources = State->Resources.GetPayload();
		return Resources != nullptr ? Resources->SampledView.GetReference() : nullptr;
	}

	auto FDirectionalShadowRenderer::GetSampler_RenderThread() const
		-> FRHISampler*
	{
		const FState::FResources* Resources = State->Resources.GetPayload();
		return Resources != nullptr ? Resources->Sampler.GetReference() : nullptr;
	}

	auto FDirectionalShadowRenderer::ReleaseResources_RenderThread() -> void
	{
		State->Resources.Reset();
		State->FallbackLighting = {};
	}
}
