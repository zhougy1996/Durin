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
						"Array target, exact views, or comparison sampler creation returned null.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				const std::array InitialTransition{FRHITextureTransition::Whole(
					Candidate.Target, ERHIAccess::Discard,
					ERHIAccess::GraphicsShaderRead)};
				CommandList.TransitionTextures(InitialTransition);
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		if (!View.DirectionalShadow.bEnabled) return false;
		++View.Counters.ShadowResourceAttempts;
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
		bool bReady = State->FallbackLighting.Buffer != nullptr;
		for (uint32 Cascade = 0;
			Cascade < View.DirectionalShadow.CascadeCount; ++Cascade)
		{
			bReady = StaticMeshes.PrepareShadowResources_RenderThread(
				CommandList, View.ShadowStaticMeshes[Cascade]) && bReady;
			bReady = SkeletalMeshes.PrepareShadowResources_RenderThread(
				CommandList, View.SkeletalPalettes,
				View.ShadowSkeletalMeshes[Cascade]) && bReady;
			bReady = Terrains.PrepareShadowResources_RenderThread(
				CommandList, View.ShadowTerrains[Cascade]) && bReady;
		}
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
		FGPUTimingQueryRHIRef TimingQuery;
		const FShadowDepthTimingQuerySink Sink =
			GShadowDepthTimingQuerySink.load(std::memory_order_acquire);
		if (Sink != nullptr && GDynamicRHI != nullptr)
		{
			TimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
			if (TimingQuery) CommandList.BeginGPUTimingQuery(TimingQuery);
		}
		for (uint32 CascadeIndex = 0;
			CascadeIndex < View.DirectionalShadow.CascadeCount; ++CascadeIndex)
		{
			const auto& Cascade = View.DirectionalShadow.Cascades[CascadeIndex];
			FRHIRenderPassInfo Pass{};
			Pass.RenderTargetLayout =
				RenderTargetLayouts::MakeDirectionalShadowDepth();
			Pass.DepthStencilRenderTarget = Resources->Target;
			Pass.DepthStencilRenderTargetView =
				Resources->DepthAttachmentViews[CascadeIndex];
			Pass.DepthStencilClearValue = FClearValueBinding(1.0f, 0u);
			CommandList.BeginRenderPass(Pass,
				"DirectionalShadowCascadeDepthRenderPass");
			CommandList.SetViewport(0.0f, 0.0f, 0.0f,
				static_cast<float>(DirectionalShadowResolution),
				static_cast<float>(DirectionalShadowResolution), 1.0f);
			CommandList.SetScissor(0.0f, 0.0f,
				static_cast<float>(DirectionalShadowResolution),
				static_cast<float>(DirectionalShadowResolution));
			StaticMeshes.ExecuteShadow_RenderThread(
				CommandList, Cascade.CasterView, State->FallbackLighting,
				View.ShadowStaticMeshes[CascadeIndex]);
			SkeletalMeshes.ExecuteShadow_RenderThread(
				CommandList, Cascade.CasterView, State->FallbackLighting,
				View.ShadowSkeletalMeshes[CascadeIndex]);
			Terrains.ExecuteShadow_RenderThread(
				CommandList, Cascade.CasterView, State->FallbackLighting,
				View.ShadowTerrains[CascadeIndex]);
			CommandList.EndRenderPass();
			auto& Counters = View.Counters.ShadowCascades[CascadeIndex];
			Counters.AttemptedDraws =
				View.ShadowStaticMeshes[CascadeIndex].AttemptedDraws
				+ View.ShadowSkeletalMeshes[CascadeIndex].AttemptedDraws
				+ View.ShadowTerrains[CascadeIndex].AttemptedDraws;
			Counters.SuccessfulDraws =
				View.ShadowStaticMeshes[CascadeIndex].SuccessfulDraws
				+ View.ShadowSkeletalMeshes[CascadeIndex].SuccessfulDraws
				+ View.ShadowTerrains[CascadeIndex].SuccessfulDraws;
			Counters.RejectedDraws =
				Counters.AttemptedDraws - Counters.SuccessfulDraws;
			View.Counters.ShadowAttemptedDraws += Counters.AttemptedDraws;
			View.Counters.ShadowSuccessfulDraws += Counters.SuccessfulDraws;
		}
		if (TimingQuery)
		{
			CommandList.EndGPUTimingQuery(TimingQuery);
			Sink(TimingQuery);
		}
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
