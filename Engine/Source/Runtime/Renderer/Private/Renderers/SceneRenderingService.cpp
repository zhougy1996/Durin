#include "Renderers/SceneRenderingService.h"

#include "Renderers/SceneRenderPipeline.h"
#include "Renderers/MaterialPSOPrecacheCollector.h"

#include "Asset/Asset.h"
#include "Console/ConsoleCommand.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "Misc/Time.h"
#include "CoreGlobals.h"

namespace Durin
{
	FSceneRenderingService::FSceneRenderingService()
		: RDGAllocator(Coordinator)
		, DefaultTextures(Coordinator)
		, EnvironmentLighting(Coordinator, Retirement)
		, SurfaceMaterials(Coordinator, DefaultTextures)
		, DirectionalShadowRenderer(Coordinator)
		, GBufferRenderer(Coordinator)
		, GBufferDebugRenderer(Coordinator, FullscreenGeometry)
		, DeferredDirectionalLightingRenderer(Coordinator, FullscreenGeometry)
		, GroundTruthAmbientOcclusionRenderer(Coordinator, FullscreenGeometry)
		, StaticMeshRenderer(Coordinator, SurfaceMaterials)
		, SkyBoxRenderer(Coordinator, DefaultTextures)
		, PostProcessRenderer(Coordinator, FullscreenGeometry)
		, ContactShadowRenderer(Coordinator, FullscreenGeometry)
		, VolumetricCloudRenderer(Coordinator, FullscreenGeometry)
		, VolumetricCloudShadowRenderer(Coordinator, FullscreenGeometry)
		, EditorAssistanceRenderer(Coordinator, FullscreenGeometry)
	{
	}

	FSceneRenderingService::~FSceneRenderingService() = default;

    auto FSceneRenderingService::BeginFrame_RenderThread(FRHICommandListImmediate& Commands) -> void
    {
        CheckRenderingThread();
        PrunePreparations_RenderThread();
        if (GDynamicRHI) Retirement.Collect(GDynamicRHI->RHIGetQueueCapabilities());
		MaterialPSORequests.Pump([this](const auto& Params, auto& Collection) {
			return RendererPrivate::CollectMaterialPSOs(Params, Collection, Coordinator.GetGeneration_RenderThread());
		});
        for (auto* Scene : PendingSkyScenes)
        {
            UpdateSkyLighting_RenderThread(Commands,*Scene);
            Scene->SkyLighting->WorldUpdateFrame=GRenderFrameCounterRenderThread;
        }
        PendingSkyScenes.clear();
    }

    auto FSceneRenderingService::UpdateSkyLighting_RenderThread(FRHICommandListImmediate& Commands, FScene& Scene) -> void
    {
		FPreparedShaderMapScope ShaderScope(true);
        EnvironmentLighting.UpdateScene_RenderThread(Commands, Scene, RDGAllocator, DefaultTextures.GetCube_RenderThread());
    }

	auto FSceneRenderingService::ReleaseSceneResources_RenderThread(FRHICommandListImmediate& Commands, FScene& Scene) -> void
	{
		RetirePreparations_RenderThread(&Scene);
		EnvironmentLighting.ReleaseScene_RenderThread(Commands, Scene);
	}

	auto FSceneRenderingService::Start(
		FConsoleCommandRegistry& Registry
	) -> bool
	{
		return Coordinator.Start(
			Registry,
			[this](ERendererResourceInvalidationCause Cause) {
				EnqueueResourceInvalidation(Cause);
			}
		);
	}

	auto FSceneRenderingService::Stop() -> void
	{
		Coordinator.Stop();
	}

	auto FSceneRenderingService::InitializeStartupResources_RenderThread(
		FRHICommandListImmediate& CommandList
	) -> void
	{
		check(IsInRenderingThread());
		DefaultTextures.Initialize_RenderThread(CommandList);
	}

	auto FSceneRenderingService::ReleaseResources_RenderThread() -> void
	{
		MaterialPSORequests.Close();
		ReleaseDeviceResources_RenderThread();
		Retirement.Drain([] {
			require(GDynamicRHI != nullptr);
			FRHICommandListImmediate::Get().BlockUntilGPUIdle();
		});
		Coordinator.ReleaseResources_RenderThread();
		VisibilityScratch = {};
	}

	auto FSceneRenderingService::ReleaseDeviceResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		MaterialPSORequests.Invalidate();
		RetirePreparations_RenderThread();
		DefaultTextures.ReleaseResources_RenderThread();
		EnvironmentLighting.ReleaseResources_RenderThread(FRHICommandListImmediate::Get());
		SurfaceMaterials.ReleaseResources_RenderThread();
		StaticMeshRenderer.ReleaseResources_RenderThread();
		MeshCommandCache.Reset();
		DirectionalShadowRenderer.ReleaseResources_RenderThread();
		GBufferRenderer.ReleaseResources_RenderThread();
		GBufferDebugRenderer.ReleaseResources_RenderThread();
		DeferredDirectionalLightingRenderer.ReleaseResources_RenderThread();
		GroundTruthAmbientOcclusionRenderer.ReleaseResources_RenderThread();
		SkyBoxRenderer.ReleaseResources_RenderThread();
		EditorAssistanceRenderer.ReleaseResources_RenderThread();
		PostProcessRenderer.ReleaseResources_RenderThread();
		ContactShadowRenderer.ReleaseResources_RenderThread();
		VolumetricCloudRenderer.ReleaseResources_RenderThread();
		VolumetricCloudShadowRenderer.ReleaseResources_RenderThread();
		FullscreenGeometry.ReleaseResources_RenderThread();
		RDGAllocator.Release_RenderThread();
	}

	auto FSceneRenderingService::AddViewState_RenderThread(FSceneViewStateId Id) -> bool
	{
		return ViewStates.Add(Id);
	}

	auto FSceneRenderingService::RemoveViewState_RenderThread(FSceneViewStateId Id) -> bool
	{
		return ViewStates.Remove(Id);
	}

	auto FSceneRenderingService::InvalidateViewState_RenderThread(
		FSceneViewStateId Id
	) -> bool
	{
		return ViewStates.Invalidate(
			Id, ESceneViewDiscontinuity::ManualInvalidation
		);
	}

	auto FSceneRenderingService::InvalidateAllViewStates_RenderThread() -> void
	{
		ViewStates.InvalidateAll(
			ESceneViewDiscontinuity::ManualInvalidation
		);
	}

	auto FSceneRenderingService::ReleaseViewStates_RenderThread() -> size_t
	{
		return ViewStates.ReleaseAll();
	}

	auto FSceneRenderingService::GetViewStateCount_RenderThread() const -> size_t
	{
		check(IsInRenderingThread());
		return ViewStates.Num();
	}

	auto FSceneRenderingService::EnqueueResourceInvalidation(
		ERendererResourceInvalidationCause Cause
	) -> void
	{
		ENQUEUE_RENDER_COMMAND(InvalidateRendererResources)(
			[this, Cause](FRHICommandListImmediate& CommandList) {
				ApplyResourceInvalidation_RenderThread(CommandList, Cause);
			}
		);
	}

	auto FSceneRenderingService::ApplyResourceInvalidation_RenderThread(
		FRHICommandListImmediate& CommandList,
		ERendererResourceInvalidationCause Cause
	) -> void
	{
		check(IsInRenderingThread());
		MaterialPSORequests.Invalidate();
		RetirePreparations_RenderThread();
		if (Cause == ERendererResourceInvalidationCause::Device)
			ViewStates.InvalidateAll(
				ESceneViewDiscontinuity::DeviceInvalidation
			);
		Coordinator.Apply_RenderThread(
			Cause,
			{
				.InvalidateShaderResources =
					[](bool) {},
				.ReleaseDeviceResources =
					[this] {
						ReleaseDeviceResources_RenderThread();
					},
				.RecreateStartupResources =
					[this, &CommandList] {
						check(GDynamicRHI != nullptr);
						DefaultTextures.Initialize_RenderThread(CommandList);
					},
				.RetryFailedResources =
					[this, &CommandList] {
						DefaultTextures.Initialize_RenderThread(CommandList);
						FullscreenGeometry.RetryFailedResources_RenderThread();
					},
			}
		);
	}

	auto FSceneRenderingService::PrunePreparations_RenderThread() -> void
	{
		CheckRenderingThread();
		std::erase_if(Preparations, [](const auto& Owner) {
			const auto State = Owner.Preparation.GetProgress().State;
			if (Owner.Preparation.HasOtherOwners() && State != ERenderResourcePreparationState::Canceled
				&& State != ERenderResourcePreparationState::Stale && State != ERenderResourcePreparationState::Failed) return false;
			Owner.Preparation.Retire_RenderThread(State == ERenderResourcePreparationState::Ready || State == ERenderResourcePreparationState::Pending
				? ERenderResourcePreparationState::Canceled : State);
			return true;
		});
	}

	auto FSceneRenderingService::RetirePreparations_RenderThread(FScene* Scene) -> void
	{
		CheckRenderingThread();
		std::erase_if(Preparations, [Scene](const auto& Owner) {
			if (Scene && Owner.Scene != Scene) return false;
			Owner.Preparation.Retire_RenderThread(ERenderResourcePreparationState::Stale);
			return true;
		});
	}

	auto FSceneRenderingService::PrepareViewResources_RenderThread(
		FRHICommandListImmediate& CommandList, FScene* Scene, const FSceneView& View,
		FRHITexture* OutputTarget, bool bPresentOutput, const FSceneViewRenderOptions& Options,
		const FRenderResourcePreparation& Preparation) -> void
	{
		CheckRenderingThread();
		PrunePreparations_RenderThread();
		if (!Preparation.IsValid()) return;
		const auto Status = Preparation.GetProgress().State;
		if (Status == ERenderResourcePreparationState::Stale || Status == ERenderResourcePreparationState::Canceled
			|| Status == ERenderResourcePreparationState::Failed) return;
		const auto Existing = std::ranges::find_if(Preparations, [&](const auto& Owner) {
			return Owner.Preparation.IsSameRequest(Preparation);
		});
		if (!Scene || !OutputTarget || !GDynamicRHI || (Existing != Preparations.end() && Existing->Scene != Scene))
			return Preparation.Retire_RenderThread(ERenderResourcePreparationState::Failed);
		if (Existing == Preparations.end())
		{
			if (Preparations.size() >= 4) return Preparation.Retire_RenderThread(ERenderResourcePreparationState::Failed);
			Preparations.push_back({Scene, Preparation});
		}
		FRenderResourcePreparationScope Scope(Preparation);
		try
		{
			const auto Result = FSceneRenderPipeline(*this).Execute_RenderThread(CommandList, Scene, View,
				OutputTarget, bPresentOutput, Options, nullptr, nullptr);
			Scope.Complete(Result == ERenderViewResult::Success ? ERenderResourcePreparationState::Ready
				: Result == ERenderViewResult::RendererResourcesPending ? ERenderResourcePreparationState::Pending
				: ERenderResourcePreparationState::Failed);
		}
		catch (const FRenderResourcePreparationYield&) { Scope.Complete(ERenderResourcePreparationState::Pending); }
		catch (const FRenderResourcePreparationCapacityExceeded&) { Scope.Complete(ERenderResourcePreparationState::Failed); }
		catch (const std::bad_alloc&) { Scope.Complete(ERenderResourcePreparationState::Failed); }
		PrunePreparations_RenderThread();
	}

	auto FSceneRenderingService::RenderView_RenderThread(
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		FSceneViewStatistics* OutStatistics,
		FRDGCapture* OutRenderGraphCapture
	) -> ERenderViewResult
	{
		auto TimedView = View;
		if (!std::isfinite(TimedView.MaterialTimeSeconds) || TimedView.MaterialTimeSeconds < 0.0)
			TimedView.MaterialTimeSeconds = FTime::Seconds() - GStartTime;
		return FSceneRenderPipeline(*this).Execute_RenderThread(
			CommandList, Scene, TimedView, OutputTarget, bPresentOutput,
			Options, OutStatistics, OutRenderGraphCapture
		);
	}

} // namespace Durin
