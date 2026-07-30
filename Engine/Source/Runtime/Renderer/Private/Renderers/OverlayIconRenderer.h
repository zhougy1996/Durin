#pragma once

#include "Renderers/EditorAssistanceRenderer.h"

namespace Durin
{
	class FRendererResourceCoordinator;

	class FOverlayIconRenderer final
	{
	public:
		explicit FOverlayIconRenderer(
			FRendererResourceCoordinator& InCoordinator);
		~FOverlayIconRenderer();

		FOverlayIconRenderer(const FOverlayIconRenderer&) = delete;
		auto operator=(const FOverlayIconRenderer&)
			-> FOverlayIconRenderer& = delete;

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
