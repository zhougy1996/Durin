#pragma once

#include "Renderers/StaticMeshRenderPreparation.h"

#include "Renderers/ContactShadowRenderer.h"
#include "Renderers/EditorAssistance/EditorAssistanceRenderer.h"
#include "Renderers/DirectionalShadowRenderer.h"
#include "Renderers/DeferredDirectionalLightingRenderer.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/GBufferDebugRenderer.h"
#include "Renderers/GroundTruthAmbientOcclusionRenderer.h"
#include "Renderers/PostProcessRenderer.h"
#include "Renderers/SkyBoxRenderer.h"
#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/VolumetricCloudRenderer.h"
#include "Renderers/VolumetricCloudShadowRenderer.h"
#include "Renderers/SceneViewState.h"
#include "Renderers/SceneVisibility.h"
#include "Renderers/SceneRenderResults.h"
#include "Renderers/SceneRenderGraphWarnings.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Renderers/SurfaceMaterial.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Renderers/RendererRDGAllocator.h"
#include "IRendererModule.h"
#include "RendererAPI.h"

namespace Durin
{
	class FConsoleCommandRegistry;
	class FSceneRenderPipeline;
	class FSceneRenderer;
	class FRHICommandListImmediate;
	class FRHITexture;
	class FScene;
	struct FSceneView;

	// Owns persistent resources, feature renderers and scene/view lifecycle services.
	class FSceneRenderingService final
	{
	public:
		RENDERER_API FSceneRenderingService();
		RENDERER_API ~FSceneRenderingService();

		FSceneRenderingService(const FSceneRenderingService&) = delete;
		auto operator=(const FSceneRenderingService&) -> FSceneRenderingService& = delete;

		auto UpdateSkyLighting_RenderThread(FRHICommandListImmediate& Commands, FScene& Scene) -> void;
		std::unordered_set<FScene*> PendingSkyScenes;
		std::function<void(FGPUTimingQueryRHIRef)> ViewGPUTimingSink;
        auto UpdatePendingScenes_RenderThread(FRHICommandListImmediate& Commands) -> void;
        auto Start(FConsoleCommandRegistry& Registry) -> bool;
		auto Stop() -> void;
		auto InitializeStartupResources_RenderThread(
			FRHICommandListImmediate& CommandList
		) -> void;
		auto ReleaseResources_RenderThread() -> void;
		auto AddViewState_RenderThread(FSceneViewStateId Id) -> bool;
		auto RemoveViewState_RenderThread(FSceneViewStateId Id) -> bool;
		auto InvalidateViewState_RenderThread(FSceneViewStateId Id) -> bool;
		auto InvalidateAllViewStates_RenderThread() -> void;
		auto ReleaseViewStates_RenderThread() -> size_t;
		auto GetViewStateCount_RenderThread() const -> size_t;
		auto RenderHitProxies_RenderThread(FRHICommandListImmediate&, FScene*, const FHitProxyRenderRequest&) -> void;
		auto RenderView_RenderThread(
			FRHICommandListImmediate& CommandList,
			FScene* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FSceneViewStatistics* OutStatistics,
			FRDGCapture* OutRenderGraphCapture
		) -> ERenderViewResult;

		auto GetResourceCoordinator() -> FRendererResourceCoordinator&
		{
			return Coordinator;
		}

		auto GetDefaultTextures() -> FDefaultTextureResources&
		{
			return DefaultTextures;
		}

	private:
		friend class FSceneRenderPipeline;
		friend class FSceneRenderer;
		auto ReleaseDeviceResources_RenderThread() -> void;
		auto EnqueueResourceInvalidation(
			ERendererResourceInvalidationCause Cause
		) -> void;
		auto ApplyResourceInvalidation_RenderThread(
			FRHICommandListImmediate& CommandList,
			ERendererResourceInvalidationCause Cause
		) -> void;

		FRendererResourceCoordinator Coordinator;
		FRendererRDGAllocator RDGAllocator;
		FDefaultTextureResources DefaultTextures;
		FEnvironmentLightingResources EnvironmentLighting;
		RendererPrivate::FSurfaceMaterialResources SurfaceMaterials;
		FFullscreenGeometryResources FullscreenGeometry;
		FDirectionalShadowRenderer DirectionalShadowRenderer;
		FGBufferRenderer GBufferRenderer;
		FGBufferDebugRenderer GBufferDebugRenderer;
		FDeferredDirectionalLightingRenderer DeferredDirectionalLightingRenderer;
		FGroundTruthAmbientOcclusionRenderer GroundTruthAmbientOcclusionRenderer;
		FStaticMeshRenderer StaticMeshRenderer;
		FSkyBoxRenderer SkyBoxRenderer;
		FPostProcessRenderer PostProcessRenderer;
		FContactShadowVisibilityRenderer ContactShadowRenderer;
		FVolumetricCloudRenderer VolumetricCloudRenderer;
		FVolumetricCloudShadowRenderer VolumetricCloudShadowRenderer;
		FEditorAssistanceRenderer EditorAssistanceRenderer;
		FSceneViewStateRegistry ViewStates;
		// Used only during serial render-thread preparation; retains candidate capacity.
		FSceneVisibilityResult VisibilityScratch;
		FStaticMeshDrawCommandCache MeshCommandCache;
		uint64 RenderSubmissionSerial = 0;
		FSceneRenderGraphWarnings RenderGraphWarnings;
	};
} // namespace Durin
