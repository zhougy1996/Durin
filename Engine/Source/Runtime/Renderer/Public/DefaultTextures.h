#pragma once

#include "RendererAPI.h"
#include "RHIResources.h"

	namespace Durin
{
	class FRHITexture;

	// Identifies renderer-owned fallback textures that are always safe to bind.
	enum class EDefaultTexture : uint8
	{
		White,
		Black,
		FlatNormal
	};

	// Renderer-thread-only resolution. Unavailable, failed and released assets all use Fallback.
	RENDERER_API auto GetDefaultTexture_RenderThread(EDefaultTexture Texture) -> FRHITexture*;
	RENDERER_API auto GetDefaultCubeTexture_RenderThread() -> FRHITexture*;
	RENDERER_API auto ResolveTexture_RenderThread(
		const FRHITextureReferenceRef& TextureReference,
		EDefaultTexture Fallback) -> FRHITexture*;
}
