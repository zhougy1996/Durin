#pragma once
#include "CoreGlobals.h"
#include "RendererAPI.h"
#include "IRendererModule.h"

namespace Durin
{
	// Owns renderer services, default resources, and the concrete scene rendering pipeline.
	class FRendererModule final : public IRendererModule
	{
	public:
		RENDERER_API auto StartupModule() -> void override;
		RENDERER_API auto ShutdownModule() -> void override;
		RENDERER_API auto ReleaseResources() -> void override;
		RENDERER_API auto CreateScene() -> std::unique_ptr<IScene> override;
		RENDERER_API auto GetViewSettings() const -> FRendererViewSettings override;
		RENDERER_API auto SetViewSettings(const FRendererViewSettings& InSettings) -> void override;
		RENDERER_API auto SetFXAAEnabled(bool bInEnabled) -> void override;
		RENDERER_API auto IsFXAAEnabled() const -> bool override;
		RENDERER_API auto SetRenderMode(ERenderMode Mode) -> void override;
		RENDERER_API auto GetRenderMode() const -> ERenderMode override;
		RENDERER_API auto SetRasterMode(ERasterMode Mode) -> void override;
		RENDERER_API auto GetRasterMode() const -> ERasterMode override;
		RENDERER_API auto RenderView(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* OutputTarget, bool bPresentOutput) -> void override;
		RENDERER_API auto RenderScene(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* RenderTarget) -> void override;

	private:
		auto StopAcceptingSceneCreation() -> void;
		auto PrepareEditorAssistance(FRHICommandListImmediate& CommandList, const FSceneView& View) -> void;
		auto DrawEditorAssistance(FRHICommandListImmediate& CommandList, const FSceneView& View, bool bPresentOutput) -> void;

		FDelegateHandle PreExitHandle;
		bool bAcceptingSceneCreation = false;
	};
}
