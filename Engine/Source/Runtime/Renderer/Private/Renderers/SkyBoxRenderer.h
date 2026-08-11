#pragma once

#include "RHIResources.h"

#include <memory>

namespace Durin
{
	class FDefaultTextureResources;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	struct FSceneView;
	struct FSkyBoxSceneData;

	// Owns SkyBox shaders, pipelines, geometry, and draw submission.
	class FSkyBoxRenderer final
	{
	public:
		FSkyBoxRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FDefaultTextureResources& InDefaultTextures);
		~FSkyBoxRenderer();

		FSkyBoxRenderer(const FSkyBoxRenderer&) = delete;
		auto operator=(const FSkyBoxRenderer&) -> FSkyBoxRenderer& = delete;

		auto EnsureResources_RenderThread() -> bool;
		auto Draw_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FSkyBoxSceneData& SkyBox) -> void;
		auto DrawTexture_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			FRHITexture* Texture,
			const FSkyBoxSceneData& SkyBox) -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FDefaultTextureResources& DefaultTextures;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
