#pragma once

#include "Misc/CoreTypes.h"

#include <memory>

namespace Durin
{
	class FDefaultTextureResources;
	class FEnvironmentLightingResources;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	class IScene;
	class FStaticMeshSceneProxy;
	class FPrimitiveSceneInfo;
	struct FPreparedStaticMeshSection;
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
			FDefaultTextureResources& InDefaultTextures,
			FEnvironmentLightingResources& InEnvironmentLighting
		);
		~FStaticMeshRenderer();

		FStaticMeshRenderer(const FStaticMeshRenderer&) = delete;
		auto operator=(const FStaticMeshRenderer&)
			-> FStaticMeshRenderer& = delete;

		auto EnsureResources_RenderThread() -> bool;
		auto DrawScene_RenderThread(
			FRHICommandListImmediate& CommandList,
			IScene* Scene,
			const FSceneView& View,
			const FDirectionalLightSceneData& Light,
			ERenderMode RenderMode,
			ERasterMode RasterMode
		) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		auto DrawSection_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FDirectionalLightSceneData& Light,
			ERenderMode RenderMode,
			const FPreparedStaticMeshSection& Item
		) -> void;
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		FDefaultTextureResources& DefaultTextures;
		FEnvironmentLightingResources& EnvironmentLighting;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
