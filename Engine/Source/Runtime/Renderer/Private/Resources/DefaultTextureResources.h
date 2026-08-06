#pragma once

#include "DefaultTextures.h"
#include "RenderResourceCreation.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FRendererResourceCoordinator;

	// Owns the renderer's startup fallback textures for one module lifetime.
	class FDefaultTextureResources
	{
	public:
		explicit FDefaultTextureResources(
			FRendererResourceCoordinator& InCoordinator);
		auto Initialize_RenderThread(FRHICommandListImmediate& CommandList)
			-> bool;
		auto Get_RenderThread(EDefaultTexture Texture) const -> FRHITexture*;
		auto GetCube_RenderThread() const -> FRHITexture*;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FPayload
		{
			FTextureRHIRef White;
			FTextureRHIRef Black;
			FTextureRHIRef FlatNormal;
			FTextureRHIRef BlackCube;
		};

		FRendererResourceCoordinator& Coordinator;
		TRenderResourceCreationSlot<FPayload> Slot{
			ERenderResourceGenerationDependency::Device};
	};

	// Binds the public default-texture forwarding API to the active module owner.
	auto GetDefaultTextureResources() -> FDefaultTextureResources&;
	auto SetActiveDefaultTextureResources(
		FDefaultTextureResources* Resources) -> void;
} // namespace Durin
