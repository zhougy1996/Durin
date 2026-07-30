#pragma once

namespace Durin
{
	class FRHICommandListImmediate;
	class FSkyBoxRenderer;
	class IScene;
	class FTextureCubePreviewSceneProxy;
	struct FSceneView;

	// Owns the TextureCube preview-proxy draw path.
	class FTextureCubeThumbnailRenderer final
	{
	public:
		auto DrawScene_RenderThread(
			FRHICommandListImmediate& CommandList,
			IScene* Scene,
			const FSceneView& View,
			FSkyBoxRenderer& SkyBoxRenderer) -> void;

	private:
		auto DrawProxy_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FTextureCubePreviewSceneProxy& Proxy,
			FSkyBoxRenderer& SkyBoxRenderer) -> void;
	};
} // namespace Durin
