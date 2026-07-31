#pragma once

#include "Renderers/EditorAssistance/EditorAssistanceRenderer.h"
#include "Renderers/PostProcessRenderer.h"
#include "Renderers/SkyBoxRenderer.h"
#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/TextureCubeThumbnailRenderer.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "RendererAPI.h"

namespace Durin
{
	class FConsoleCommandRegistry;
	class FRHICommandListImmediate;
	class FRHITexture;
	class IScene;
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

		auto Start(FConsoleCommandRegistry& Registry) -> bool;
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
			IScene* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput) -> void;

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
			IScene* Scene,
			const FSceneView& View,
			FRHITexture* RenderTarget) -> void;

		FRendererResourceCoordinator Coordinator;
		FDefaultTextureResources DefaultTextures;
		FFullscreenGeometryResources FullscreenGeometry;
		FStaticMeshRenderer StaticMeshRenderer;
		FSkyBoxRenderer SkyBoxRenderer;
		FTextureCubeThumbnailRenderer TextureCubeThumbnailRenderer;
		FPostProcessRenderer PostProcessRenderer;
		FEditorAssistanceRenderer EditorAssistanceRenderer;
	};
} // namespace Durin
