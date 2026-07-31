#pragma once

#include "Renderers/EditorAssistance/EditorAssistanceRenderer.h"

namespace Durin
{
	class FRendererResourceCoordinator;

	class FGizmoRenderer final
	{
	public:
		explicit FGizmoRenderer(FRendererResourceCoordinator& InCoordinator);
		~FGizmoRenderer();

		FGizmoRenderer(const FGizmoRenderer&) = delete;
		auto operator=(const FGizmoRenderer&) -> FGizmoRenderer& = delete;

		auto Prepare_RenderThread(
			FRHICommandListImmediate& CommandList,
			const RendererEditorAssistance::FRequest& Request,
			RendererEditorAssistance::FPrepared& Prepared) -> void;
		auto Draw_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const RendererEditorAssistance::FPrepared& Prepared,
			RendererEditorAssistance::EDepthMode DepthMode) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
