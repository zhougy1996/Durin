#pragma once

namespace Durin
{
	class FRHICommandListImmediate;
	class FRHITexture;
	class IScene;

	class IRendererModule : public IModuleInterface
	{
	public:
		virtual auto CreateScene() -> std::unique_ptr<IScene> = 0;
		virtual auto PrepareSceneResources(FRHICommandListImmediate& CommandList, IScene* Scene) -> void = 0;
		virtual auto RenderScene(FRHICommandListImmediate& CommandList, IScene* Scene, FRHITexture* RenderTarget, uint32 Width, uint32 Height) -> void = 0;
	};
}
