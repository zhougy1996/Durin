#pragma once

namespace Durin
{
	class FRHICommandListImmediate;
	class FRHITexture;
	class IScene;

	struct FRendererViewSettings
	{
		bool bEnableFXAA = true;
	};

	class IRendererModule : public IModuleInterface
	{
	public:
		virtual auto CreateScene() -> std::unique_ptr<IScene> = 0;
		virtual auto GetViewSettings() const -> FRendererViewSettings = 0;
		virtual auto SetViewSettings(const FRendererViewSettings& InSettings) -> void = 0;
		virtual auto SetFXAAEnabled(bool bInEnabled) -> void = 0;
		virtual auto IsFXAAEnabled() const -> bool = 0;
		virtual auto PrepareSceneResources(FRHICommandListImmediate& CommandList, IScene* Scene) -> void = 0;
		virtual auto RenderView(FRHICommandListImmediate& CommandList, IScene* Scene, FRHITexture* OutputTarget, uint32 Width, uint32 Height, bool bPresentOutput) -> void = 0;
		virtual auto RenderScene(FRHICommandListImmediate& CommandList, IScene* Scene, FRHITexture* RenderTarget, uint32 Width, uint32 Height) -> void = 0;
	};
}
