#pragma once

#include "RHIResources.h"

#include <memory>

namespace Durin
{
	class FFullscreenGeometryResources;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;

	// Owns post-process shaders, output pipelines, and size-keyed scene targets.
	class FPostProcessRenderer final
	{
	public:
		struct FSceneTargets
		{
			FTextureRHIRef Color;
			FTextureRHIRef Depth;
		};

		FPostProcessRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FPostProcessRenderer();

		FPostProcessRenderer(const FPostProcessRenderer&) = delete;
		auto operator=(const FPostProcessRenderer&)
			-> FPostProcessRenderer& = delete;

		auto EnsureResources_RenderThread(
			FRHICommandListImmediate& CommandList) -> bool;
		auto EnsureSceneTargets_RenderThread(uint32 Width, uint32 Height)
			-> FSceneTargets*;
		auto Draw_RenderThread(
			FRHICommandListImmediate& CommandList,
			FRHITexture* SceneColor,
			uint32 Width,
			uint32 Height,
			bool bPresentOutput,
			bool bEnableFXAA,
			bool bHasEditorAssistance) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
