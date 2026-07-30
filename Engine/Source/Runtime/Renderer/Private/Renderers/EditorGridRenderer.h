#pragma once

#include "Renderers/EditorAssistanceRenderer.h"

namespace Durin
{
	class FFullscreenGeometryResources;
	class FRendererResourceCoordinator;

	class FEditorGridRenderer final
	{
	public:
		FEditorGridRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FEditorGridRenderer();

		FEditorGridRenderer(const FEditorGridRenderer&) = delete;
		auto operator=(const FEditorGridRenderer&)
			-> FEditorGridRenderer& = delete;

		auto Prepare_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			RenderTargetLayouts::EViewportOutput Output,
			RendererEditorAssistance::FPrepared& Prepared) -> void;
		auto Draw_RenderThread(
			FRHICommandListImmediate& CommandList,
			const RendererEditorAssistance::FPrepared& Prepared) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FFullscreenGeometryResources& FullscreenGeometry;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
