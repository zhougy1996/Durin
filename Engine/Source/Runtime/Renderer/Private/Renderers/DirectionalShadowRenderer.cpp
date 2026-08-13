#include "Renderers/DirectionalShadowRenderer.h"

#include "RenderResourceCreation.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/ForwardLighting.h"
#include "Renderers/PreparedSceneView.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Renderers/SkeletalMeshRenderer.h"
#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/TerrainRenderer.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"

#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

#include <atomic>

namespace Durin
{
	namespace
	{
		std::atomic<FShadowDepthTimingQuerySink> GShadowDepthTimingQuerySink = nullptr;
	}

	struct FDirectionalShadowRenderer::FState
	{
		struct FResources
		{
			FTextureRHIRef Target;
			FTextureViewRHIRef SampledView;
			FTextureViewRHIRef DepthAttachmentView;
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

	FDirectionalShadowRenderer::FDirectionalShadowRenderer(
		FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator), State(std::make_unique<FState>())
	{
	}

	FDirectionalShadowRenderer::~FDirectionalShadowRenderer() = default;

	auto FDirectionalShadowRenderer::PrepareResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		FStaticMeshRenderer& StaticMeshes,
		FSkeletalMeshRenderer& SkeletalMeshes,
		FTerrainRenderer& Terrains,
		FPreparedSceneView& View) -> bool
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (!View.DirectionalShadow.bEnabled) return false;
		++View.Counters.ShadowResourceAttempts;
		using FResult = TRenderResourceCreateResult<FState::FResources>;
		FState::FResources* Resources = State->Resources.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[&CommandList]() -> FResult {
				FState::FResources Candidate;
				FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
					"DirectionalShadowDepth", DirectionalShadowResolution,
					DirectionalShadowResolution, EPixelFormat::D32)
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
					Candidate.DepthAttachmentView = GDynamicRHI->RHICreateTextureView(
						Candidate.Target,
						MakeDefaultTextureViewDesc(*Candidate.Target,
							ERHITextureViewUsage::DepthStencilAttachment));
				}
				Candidate.Sampler = RHICreateSampler(
					MakeDirectionalShadowSamplerDesc());
				if (!Candidate.Target || !Candidate.SampledView
					|| !Candidate.DepthAttachmentView || !Candidate.Sampler)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"DirectionalShadow", "2048-D32",
						"Target, exact views, or comparison sampler creation returned null.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		if (Resources == nullptr)
		{
			++View.Counters.ShadowResourceFailures;
			View.DirectionalShadow.bEnabled = false;
			return false;
		}
		++View.Counters.ShadowResourceSuccesses;
		View.Counters.ShadowTargetLogicalBytes = DirectionalShadowLogicalBytes;
		View.Counters.ShadowTargetBackendBytes = static_cast<size_t>(
			Resources->Target->GetBackendAllocationBytes());

		const FForwardLightingUniform FullyUnlit{};
		State->FallbackLighting = CommandList.AllocateDynamicUniformBuffer(
			&FullyUnlit, sizeof(FullyUnlit));
		const bool bStaticReady = StaticMeshes.PrepareShadowResources_RenderThread(
			CommandList, View.ShadowStaticMeshes);
		const bool bSkeletalReady = SkeletalMeshes.PrepareShadowResources_RenderThread(
			CommandList, View.SkeletalMeshes, View.ShadowSkeletalMeshes);
		const bool bTerrainReady = Terrains.PrepareShadowResources_RenderThread(
			CommandList, View.ShadowTerrains);
		const bool bReady = State->FallbackLighting.Buffer != nullptr
			&& bStaticReady && bSkeletalReady && bTerrainReady;
		if (!bReady)
		{
			++View.Counters.ShadowPreparationFailures;
			View.DirectionalShadow.bEnabled = false;
		}
		return bReady;
	}

	auto FDirectionalShadowRenderer::Render_RenderThread(
		FRHICommandListImmediate& CommandList,
		FStaticMeshRenderer& StaticMeshes,
		FSkeletalMeshRenderer& SkeletalMeshes,
		FTerrainRenderer& Terrains,
		FPreparedSceneView& View) -> bool
	{
		check(!CommandList.IsInsideRenderPass());
		FState::FResources* Resources = State->Resources.GetPayload();
		if (!View.DirectionalShadow.bEnabled || Resources == nullptr
			|| Resources->Target == nullptr) return false;
		FRHIRenderPassInfo Pass{};
		Pass.RenderTargetLayout = RenderTargetLayouts::MakeDirectionalShadowDepth();
		Pass.DepthStencilRenderTarget = Resources->Target;
		Pass.DepthStencilClearValue = FClearValueBinding(1.0f, 0u);
		FGPUTimingQueryRHIRef TimingQuery;
		const FShadowDepthTimingQuerySink Sink =
			GShadowDepthTimingQuerySink.load(std::memory_order_acquire);
		if (Sink != nullptr && GDynamicRHI != nullptr)
		{
			TimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
			if (TimingQuery) CommandList.BeginGPUTimingQuery(TimingQuery);
		}
		CommandList.BeginRenderPass(Pass, "DirectionalShadowDepthRenderPass");
		CommandList.SetViewport(0.0f, 0.0f, 0.0f,
			static_cast<float>(DirectionalShadowResolution),
			static_cast<float>(DirectionalShadowResolution), 1.0f);
		CommandList.SetScissor(0.0f, 0.0f,
			static_cast<float>(DirectionalShadowResolution),
			static_cast<float>(DirectionalShadowResolution));
		StaticMeshes.ExecuteShadow_RenderThread(
			CommandList, View.DirectionalShadow.CasterView,
			State->FallbackLighting, View.ShadowStaticMeshes);
		SkeletalMeshes.ExecuteShadow_RenderThread(
			CommandList, View.DirectionalShadow.CasterView,
			State->FallbackLighting, View.ShadowSkeletalMeshes);
		Terrains.ExecuteShadow_RenderThread(
			CommandList, View.DirectionalShadow.CasterView,
			State->FallbackLighting, View.ShadowTerrains);
		CommandList.EndRenderPass();
		if (TimingQuery)
		{
			CommandList.EndGPUTimingQuery(TimingQuery);
			Sink(TimingQuery);
		}
		View.Counters.ShadowAttemptedDraws =
			View.ShadowStaticMeshes.AttemptedDraws
			+ View.ShadowSkeletalMeshes.AttemptedDraws
			+ View.ShadowTerrains.AttemptedDraws;
		View.Counters.ShadowSuccessfulDraws =
			View.ShadowStaticMeshes.SuccessfulDraws
			+ View.ShadowSkeletalMeshes.SuccessfulDraws
			+ View.ShadowTerrains.SuccessfulDraws;
		View.Counters.ShadowRejectedDraws =
			View.Counters.ShadowAttemptedDraws
				- View.Counters.ShadowSuccessfulDraws;
		return true;
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
