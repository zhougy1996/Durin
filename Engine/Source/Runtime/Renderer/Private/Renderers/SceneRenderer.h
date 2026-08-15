#pragma once

#include "Renderers/ContactShadowRenderer.h"
#include "Renderers/EditorAssistance/EditorAssistanceRenderer.h"
#include "Renderers/DirectionalShadowRenderer.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/GBufferDebugRenderer.h"
#include "Renderers/PostProcessRenderer.h"
#include "Renderers/SkyBoxRenderer.h"
#include "Renderers/SkeletalMeshRenderer.h"
#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/TerrainRenderer.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "IRendererModule.h"
#include "RendererAPI.h"

namespace Durin
{
	class FConsoleCommandRegistry;
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

		auto Start(FConsoleCommandRegistry& Registry,
			FModuleOwnedCallbackGate OwnerGate = {}) -> bool;
		auto Stop() -> void;
		auto InitializeStartupResources_RenderThread(
			FRHICommandListImmediate& CommandList) -> void;
		auto ReleaseResources_RenderThread() -> void;
		RENDERER_API static auto FitViewToOutput(
			const FSceneView& View,
			uint32 Width,
			uint32 Height) -> FSceneView;
		auto RenderView_RenderThread(
			FRHICommandListImmediate& CommandList,
			FScene* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FSceneViewStatistics* OutStatistics) -> ERenderViewResult;

		auto GetResourceCoordinator() -> FRendererResourceCoordinator&
		{
			return Coordinator;
		}

		auto GetDefaultTextures() -> FDefaultTextureResources&
		{
			return DefaultTextures;
		}

	private:
		auto EnqueueResourceInvalidation(
			ERendererResourceInvalidationCause Cause) -> void;
		auto ApplyResourceInvalidation_RenderThread(
			FRHICommandListImmediate& CommandList,
			ERendererResourceInvalidationCause Cause) -> void;
		auto RenderScene_RenderThread(
			FRHICommandListImmediate& CommandList,
			struct FPreparedSceneView& PreparedView,
			FRHITexture* RenderTarget) -> bool;

		FRendererResourceCoordinator Coordinator;
		FDefaultTextureResources DefaultTextures;
		FEnvironmentLightingResources EnvironmentLighting;
		FFullscreenGeometryResources FullscreenGeometry;
		FDirectionalShadowRenderer DirectionalShadowRenderer;
		FGBufferRenderer GBufferRenderer;
		FGBufferDebugRenderer GBufferDebugRenderer;
		FStaticMeshRenderer StaticMeshRenderer;
		FTerrainRenderer TerrainRenderer;
		FSkeletalMeshRenderer SkeletalMeshRenderer;
		FSkyBoxRenderer SkyBoxRenderer;
		FPostProcessRenderer PostProcessRenderer;
		FScreenSpaceContactShadowRenderer ContactShadowRenderer;
		FEditorAssistanceRenderer EditorAssistanceRenderer;
	};
} // namespace Durin
