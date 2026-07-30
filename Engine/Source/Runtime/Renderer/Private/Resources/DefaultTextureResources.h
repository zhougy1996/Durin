#pragma once

#include "DefaultTextures.h"

namespace Durin
{
	class FRHICommandListImmediate;

	// Owns the renderer's startup fallback textures for one module lifetime.
	class FDefaultTextureResources
	{
	public:
		auto Initialize_RenderThread(FRHICommandListImmediate& CommandList)
			-> void;
		auto Get_RenderThread(EDefaultTexture Texture) const -> FRHITexture*;
		auto GetCube_RenderThread() const -> FRHITexture*;
		auto ReleaseResources_RenderThread() -> void;

	private:
		FTextureRHIRef White;
		FTextureRHIRef Black;
		FTextureRHIRef FlatNormal;
		FTextureRHIRef BlackCube;
	};

	// Binds the public default-texture forwarding API to the active module owner.
	auto GetDefaultTextureResources() -> FDefaultTextureResources&;
	auto SetActiveDefaultTextureResources(
		FDefaultTextureResources* Resources) -> void;
} // namespace Durin
