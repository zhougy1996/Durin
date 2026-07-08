#pragma once
#include "RendererAPI.h"
#include "IRendererModule.h"

namespace Durin
{
	class RENDERER_API FRendererModule final : public IRendererModule
	{
	public:
		auto ShutdownModule() -> void override;
		auto CreateScene() -> std::unique_ptr<IScene> override;
		auto PrepareSceneResources(FRHICommandListImmediate& CommandList, IScene* Scene) -> void override;
		auto RenderScene(FRHICommandListImmediate& CommandList, IScene* Scene, FRHITexture* RenderTarget, uint32 Width, uint32 Height) -> void override;
	};
}
