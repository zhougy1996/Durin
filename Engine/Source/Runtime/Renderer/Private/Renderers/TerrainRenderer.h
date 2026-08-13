#pragma once

#include "Misc/CoreTypes.h"

namespace Durin
{
	class FDefaultTextureResources;
	class FEnvironmentLightingResources;
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;
	struct FPreparedTerrainDraw;
	struct FPreparedTerrainView;
	struct FRHIUniformBufferRange;
	struct FSceneView;
	enum class ERenderMode : uint8;
	enum class EStaticMeshBasePass : uint8;

	// Owns exact height/topology caches and Terrain shader draw submission.
	class FTerrainRenderer final
	{
	public:
		FTerrainRenderer(FRendererResourceCoordinator& InCoordinator,
			FDefaultTextureResources& InDefaultTextures,
			FEnvironmentLightingResources& InEnvironmentLighting);
		~FTerrainRenderer();

		auto PrepareResources_RenderThread(FRHICommandListImmediate& CommandList,
			FPreparedTerrainView& View) -> bool;
		auto PrepareShadowResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			FPreparedTerrainView& View) -> bool;
		auto ExecuteShadow_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& ShadowView,
			const FRHIUniformBufferRange& FallbackLighting,
			FPreparedTerrainView& View) -> void;
		auto ExecutePass_RenderThread(FRHICommandListImmediate& CommandList,
			const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode, EStaticMeshBasePass Pass,
			FPreparedTerrainView& View) -> void;
		auto ExecutePreparedDraw_RenderThread(FRHICommandListImmediate& CommandList,
			const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode, const FPreparedTerrainDraw& Draw,
			FPreparedTerrainView& View) -> void;
		auto FinalizeExecution_RenderThread(FPreparedTerrainView& View) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		auto EnsureDrawResources_RenderThread(FRHICommandListImmediate& CommandList,
			FPreparedTerrainDraw& Draw, FPreparedTerrainView& View,
			bool bShadowDepth = false) -> bool;
		auto Draw_RenderThread(FRHICommandListImmediate& CommandList,
			const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting,
			ERenderMode RenderMode, const FPreparedTerrainDraw& Draw,
			bool bShadowDepth = false) -> bool;
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		FDefaultTextureResources& DefaultTextures;
		FEnvironmentLightingResources& EnvironmentLighting;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
