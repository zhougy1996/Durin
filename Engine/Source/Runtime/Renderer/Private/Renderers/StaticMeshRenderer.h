#pragma once

#include "Misc/CoreTypes.h"

#include <memory>

namespace Durin
{
	class FDefaultTextureResources;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	class FStaticMeshSceneProxy;
	enum class ERasterMode : uint8;
	enum class ERenderMode : uint8;
	struct FDirectionalLightSceneData;
	struct FSceneView;

	// Owns StaticMesh shaders, material pipelines, and proxy draw submission.
	class FStaticMeshRenderer final
	{
	public:
		FStaticMeshRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FDefaultTextureResources& InDefaultTextures);
		~FStaticMeshRenderer();

		FStaticMeshRenderer(const FStaticMeshRenderer&) = delete;
		auto operator=(const FStaticMeshRenderer&)
			-> FStaticMeshRenderer& = delete;

		auto EnsureResources_RenderThread() -> bool;
		auto DrawProxy_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FDirectionalLightSceneData& Light,
			ERenderMode RenderMode,
			ERasterMode RasterMode,
			const FStaticMeshSceneProxy& Proxy) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FDefaultTextureResources& DefaultTextures;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
