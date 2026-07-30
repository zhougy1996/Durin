#pragma once

#include "Renderers/EditorAssistanceRenderer.h"

namespace Durin
{
	class FRendererResourceCoordinator;

	class FOverlayLineRenderer final
	{
	public:
		explicit FOverlayLineRenderer(
			FRendererResourceCoordinator& InCoordinator);
		~FOverlayLineRenderer();

		FOverlayLineRenderer(const FOverlayLineRenderer&) = delete;
		auto operator=(const FOverlayLineRenderer&)
			-> FOverlayLineRenderer& = delete;

		auto Prepare_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			RenderTargetLayouts::EViewportOutput Output,
			RendererEditorAssistance::FPrepared& Prepared) -> void;
		auto Draw_RenderThread(
			FRHICommandListImmediate& CommandList,
			const RendererEditorAssistance::FPrepared& Prepared,
			RendererEditorAssistance::EDepthMode DepthMode) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
