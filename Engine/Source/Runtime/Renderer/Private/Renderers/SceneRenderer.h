#pragma once

#include "Renderers/ContactShadowRenderer.h"
#include "Renderers/EditorAssistance/EditorAssistanceRenderer.h"
#include "Renderers/DirectionalShadowRenderer.h"
#include "Renderers/DeferredDirectionalLightingRenderer.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/GBufferDebugRenderer.h"
#include "Renderers/GroundTruthAmbientOcclusionRenderer.h"
#include "Renderers/PostProcessRenderer.h"
#include "Renderers/SkyBoxRenderer.h"
#include "Renderers/SkeletalMeshRenderer.h"
#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/TerrainRenderer.h"
#include "Renderers/VolumetricCloudRenderer.h"
#include "Renderers/VolumetricCloudShadowRenderer.h"
#include "Renderers/SceneViewState.h"
#include "Renderers/SceneRenderResults.h"
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
	class FSceneRenderGraphExecutor;
	class FSceneRenderPipeline;
	class FSceneRenderFeatureRecorders;
	class FRHICommandListImmediate;
	class FRHITexture;
	class FScene;
	struct FSceneView;

	// Owns renderer resources and concrete feature renderers while preserving
	// the complete render-thread order for one view.
	class FSceneRenderer final
	{
	public:
		FSceneRenderer();
		~FSceneRenderer();

		FSceneRenderer(const FSceneRenderer&) = delete;
		auto operator=(const FSceneRenderer&) -> FSceneRenderer& = delete;

		auto Start(FConsoleCommandRegistry& Registry, FModuleOwnedCallbackGate OwnerGate = {}) -> bool;
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
		RENDERER_API static auto FitViewToOutput(
			const FSceneView& View,
			uint32 Width,
			uint32 Height
		) -> FSceneView;
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
		friend class FSceneRenderGraphExecutor;
		friend class FSceneRenderPipeline;
		friend class FSceneRenderFeatureRecorders;
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
		FTerrainRenderer TerrainRenderer;
		FSkeletalMeshRenderer SkeletalMeshRenderer;
		FSkyBoxRenderer SkyBoxRenderer;
		FPostProcessRenderer PostProcessRenderer;
		FContactShadowVisibilityRenderer ContactShadowRenderer;
		FVolumetricCloudRenderer VolumetricCloudRenderer;
		FVolumetricCloudShadowRenderer VolumetricCloudShadowRenderer;
		FEditorAssistanceRenderer EditorAssistanceRenderer;
		FSceneViewStateRegistry ViewStates;
		uint64 RenderSubmissionSerial = 0;
	};
} // namespace Durin
