#include "Renderers/SceneRenderingService.h"

#include "Renderers/SceneRenderPipeline.h"

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
		, EnvironmentLighting(Coordinator)
		, SurfaceMaterials(Coordinator, DefaultTextures, EnvironmentLighting)
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

    auto FSceneRenderingService::UpdatePendingScenes_RenderThread(FRHICommandListImmediate& Commands) -> void
    {
        CheckRenderingThread();
        for (auto* Scene : PendingSkyScenes)
        {
            UpdateSkyLighting_RenderThread(Commands,*Scene);
            Scene->SkyLighting->WorldUpdateFrame=GRenderFrameCounterRenderThread;
        }
        PendingSkyScenes.clear();
    }

    auto FSceneRenderingService::UpdateSkyLighting_RenderThread(FRHICommandListImmediate& Commands, FScene& Scene) -> void
    {
        EnvironmentLighting.UpdateScene_RenderThread(Commands, Scene, RDGAllocator, DefaultTextures.GetCube_RenderThread());
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
		ReleaseDeviceResources_RenderThread();
		Coordinator.ReleaseResources_RenderThread();
		VisibilityScratch = {};
	}

	auto FSceneRenderingService::ReleaseDeviceResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		DefaultTextures.ReleaseResources_RenderThread();
		EnvironmentLighting.ReleaseResources_RenderThread();
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
