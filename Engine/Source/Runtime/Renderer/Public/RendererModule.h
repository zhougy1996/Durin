#pragma once
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
		RENDERER_API auto CreateScene() -> std::unique_ptr<IScene> override;
		RENDERER_API auto RenderView(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* OutputTarget, bool bPresentOutput) -> void override;

	private:
		auto StopAcceptingSceneCreation() -> void;
		auto RenderScene(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* RenderTarget) -> void;
		auto PrepareEditorAssistance(FRHICommandListImmediate& CommandList, const FSceneView& View) -> void;
		auto DrawEditorAssistance(FRHICommandListImmediate& CommandList, const FSceneView& View, bool bPresentOutput) -> void;

		bool bAcceptingSceneCreation = false;
	};
}
