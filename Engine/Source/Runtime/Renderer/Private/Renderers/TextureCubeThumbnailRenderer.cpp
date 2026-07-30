#include "Renderers/TextureCubeThumbnailRenderer.h"

#include "Renderers/SkyBoxRenderer.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "IScene.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneView.h"

namespace Durin
{
	auto FTextureCubeThumbnailRenderer::DrawScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		IScene* Scene,
		const FSceneView& View,
		FSkyBoxRenderer& SkyBoxRenderer) -> void
	{
		check(IsInRenderingThread());
		auto* RendererScene = dynamic_cast<FScene*>(Scene);
		if (RendererScene == nullptr)
		{
			return;
		}
		for (PrimitiveSceneProxy* Proxy :
			RendererScene->GetPrimitiveSceneProxies())
		{
			if (auto* TextureCubeProxy =
					dynamic_cast<FTextureCubePreviewSceneProxy*>(Proxy))
			{
				DrawProxy_RenderThread(
					CommandList,
					View,
					*TextureCubeProxy,
					SkyBoxRenderer);
			}
		}
	}

	auto FTextureCubeThumbnailRenderer::DrawProxy_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FTextureCubePreviewSceneProxy& Proxy,
		FSkyBoxRenderer& SkyBoxRenderer) -> void
	{
		const FRHITextureReferenceRef& TextureReference =
			Proxy.GetTextureReference();
		if (TextureReference == nullptr)
		{
			return;
		}
		FRHITexture* Texture =
			TextureReference->GetReferencedTexture_RenderThread();
		if (Texture == nullptr)
		{
			return;
		}

		// Content Browser thumbnails favor recognition over inspection: show a
		// wide environment view here and reserve the reflective sphere for the
		// interactive TextureCube editor.
		constexpr float EnvironmentVerticalFieldOfViewDegrees = 100.0f;
		FSceneView EnvironmentView = View;
		const float AspectRatio = static_cast<float>(View.ViewportWidth)
			/ static_cast<float>(std::max(1u, View.ViewportHeight));
		const float YScale = 1.0f
			/ std::tan(
				glm::radians(EnvironmentVerticalFieldOfViewDegrees) * 0.5f);
		EnvironmentView.ProjectionMatrix[1][0] =
			YScale / std::max(AspectRatio, 0.001f);
		EnvironmentView.ProjectionMatrix[2][1] = -YScale;
		EnvironmentView.ViewProjectionMatrix =
			EnvironmentView.ProjectionMatrix * EnvironmentView.ViewMatrix;

		SkyBoxRenderer.DrawTexture_RenderThread(
			CommandList,
			EnvironmentView,
			Texture,
			FSkyBoxSceneData{});
	}
} // namespace Durin
