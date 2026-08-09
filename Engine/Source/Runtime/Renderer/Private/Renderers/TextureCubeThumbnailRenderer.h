#pragma once

#include <span>

namespace Durin
{
	class FRHICommandListImmediate;
	class FSkyBoxRenderer;
	class FTextureCubePreviewSceneProxy;
	struct FSceneView;

	// Owns the TextureCube preview-proxy draw path.
	class FTextureCubeThumbnailRenderer final
	{
	public:
		auto DrawPrepared_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			std::span<const FTextureCubePreviewSceneProxy* const> Proxies,
			FSkyBoxRenderer& SkyBoxRenderer) -> void;

	private:
		auto DrawProxy_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FTextureCubePreviewSceneProxy& Proxy,
			FSkyBoxRenderer& SkyBoxRenderer) -> void;
	};
} // namespace Durin
