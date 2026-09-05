#include "Renderers/SceneRenderer.h"

#include "Renderers/SceneRenderPipeline.h"

#include "Asset/Asset.h"
#include "Console/ConsoleCommand.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"

namespace Durin
{
	FSceneRenderer::FSceneRenderer()
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
		, SkeletalMeshRenderer(Coordinator, SurfaceMaterials)
		, SkyBoxRenderer(Coordinator, DefaultTextures)
		, PostProcessRenderer(Coordinator, FullscreenGeometry)
		, ContactShadowRenderer(Coordinator, FullscreenGeometry)
		, VolumetricCloudRenderer(Coordinator, FullscreenGeometry)
		, VolumetricCloudShadowRenderer(Coordinator, FullscreenGeometry)
		, EditorAssistanceRenderer(Coordinator, FullscreenGeometry)
	{
	}

	FSceneRenderer::~FSceneRenderer() = default;

	auto FSceneRenderer::Start(
		FConsoleCommandRegistry& Registry
	) -> bool
	{
		FObjectPath EnvironmentPath;
		DEnvironmentLighting* EnvironmentAsset = nullptr;
		std::string PathError;
		FAssetResult EnvironmentResult =
			FObjectPath::TryCreate(
				"/Engine/Renderer/DefaultStudioEnvironment.DefaultStudioEnvironment",
				EnvironmentPath,
				&PathError
			) ?
				LoadObject(EnvironmentPath, EnvironmentAsset) :
				FAssetResult{EAssetError::InvalidPath, std::move(PathError)};
		if (EnvironmentResult && EnvironmentAsset != nullptr)
		{
			EnvironmentLighting.Initialize(EnvironmentAsset->GetData());
		}
		else
		{
			DURIN_ERROR(
				"Failed to load the built-in studio environment: {}",
				EnvironmentResult.Message
			);
		}
		return Coordinator.Start(
			Registry,
			[this](ERendererResourceInvalidationCause Cause) {
				EnqueueResourceInvalidation(Cause);
			}
		);
	}

	auto FSceneRenderer::Stop() -> void
	{
		Coordinator.Stop();
	}

	auto FSceneRenderer::InitializeStartupResources_RenderThread(
		FRHICommandListImmediate& CommandList
	) -> void
	{
		check(IsInRenderingThread());
		DefaultTextures.Initialize_RenderThread(CommandList);
	}

	auto FSceneRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		DefaultTextures.ReleaseResources_RenderThread();
		EnvironmentLighting.ReleaseResources_RenderThread();
		SurfaceMaterials.ReleaseResources_RenderThread();
		StaticMeshRenderer.ReleaseResources_RenderThread();
		SkeletalMeshRenderer.ReleaseResources_RenderThread();
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
		Coordinator.ReleaseResources_RenderThread();
	}

	auto FSceneRenderer::AddViewState_RenderThread(FSceneViewStateId Id) -> bool
	{
		return ViewStates.Add(Id);
	}

	auto FSceneRenderer::RemoveViewState_RenderThread(FSceneViewStateId Id) -> bool
	{
		return ViewStates.Remove(Id);
	}

	auto FSceneRenderer::InvalidateViewState_RenderThread(
		FSceneViewStateId Id
	) -> bool
	{
		return ViewStates.Invalidate(
			Id, ESceneViewDiscontinuity::ManualInvalidation
		);
	}

	auto FSceneRenderer::InvalidateAllViewStates_RenderThread() -> void
	{
		ViewStates.InvalidateAll(
			ESceneViewDiscontinuity::ManualInvalidation
		);
	}

	auto FSceneRenderer::ReleaseViewStates_RenderThread() -> size_t
	{
		return ViewStates.ReleaseAll();
	}

	auto FSceneRenderer::GetViewStateCount_RenderThread() const -> size_t
	{
		check(IsInRenderingThread());
		return ViewStates.Num();
	}

	auto FSceneRenderer::FitViewToOutput(
		const FSceneView& View,
		uint32 Width,
		uint32 Height
	) -> FSceneView
	{
		FSceneView RenderView = View;
		RenderView.ViewportX = 0;
		RenderView.ViewportY = 0;
		RenderView.ViewportWidth = Width;
		RenderView.ViewportHeight = Height;
		if (RenderView.AspectRatioConstraint <= 0.0f)
		{
			return RenderView;
		}

		uint32 ContentWidth = Width;
		uint32 ContentHeight = static_cast<uint32>(
			std::round(ContentWidth / RenderView.AspectRatioConstraint)
		);
		if (ContentHeight > Height)
		{
			ContentHeight = Height;
			ContentWidth = static_cast<uint32>(
				std::round(
					ContentHeight * RenderView.AspectRatioConstraint
				)
			);
		}
		RenderView.ViewportWidth = std::max(1u, ContentWidth);
		RenderView.ViewportHeight = std::max(1u, ContentHeight);
		RenderView.ViewportX = (Width - RenderView.ViewportWidth) / 2;
		RenderView.ViewportY = (Height - RenderView.ViewportHeight) / 2;
		return RenderView;
	}

	auto FSceneRenderer::EnqueueResourceInvalidation(
		ERendererResourceInvalidationCause Cause
	) -> void
	{
		ENQUEUE_RENDER_COMMAND(InvalidateRendererResources)(
			[this, Cause](FRHICommandListImmediate& CommandList) {
				ApplyResourceInvalidation_RenderThread(CommandList, Cause);
			}
		);
	}

	auto FSceneRenderer::ApplyResourceInvalidation_RenderThread(
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
						DefaultTextures.ReleaseResources_RenderThread();
						EnvironmentLighting.ReleaseResources_RenderThread();
						SurfaceMaterials.ReleaseResources_RenderThread();
						StaticMeshRenderer.ReleaseResources_RenderThread();
						SkeletalMeshRenderer.ReleaseResources_RenderThread();
						DirectionalShadowRenderer.ReleaseResources_RenderThread();
						GBufferRenderer.ReleaseResources_RenderThread();
						GBufferDebugRenderer.ReleaseResources_RenderThread();
						DeferredDirectionalLightingRenderer.ReleaseResources_RenderThread();
						GroundTruthAmbientOcclusionRenderer.ReleaseResources_RenderThread();
						SkyBoxRenderer.ReleaseResources_RenderThread();
						PostProcessRenderer.ReleaseResources_RenderThread();
						ContactShadowRenderer.ReleaseResources_RenderThread();
						VolumetricCloudRenderer.ReleaseResources_RenderThread();
						VolumetricCloudShadowRenderer.ReleaseResources_RenderThread();
						EditorAssistanceRenderer.ReleaseResources_RenderThread();
						FullscreenGeometry.ReleaseResources_RenderThread();
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

	auto FSceneRenderer::RenderView_RenderThread(
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
		return FSceneRenderPipeline(*this).Execute_RenderThread(
			CommandList, Scene, View, OutputTarget, bPresentOutput,
			Options, OutStatistics, OutRenderGraphCapture
		);
	}

} // namespace Durin
