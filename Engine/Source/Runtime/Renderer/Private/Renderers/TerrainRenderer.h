#pragma once

#include "Misc/CoreTypes.h"

#include <array>
#include <span>
#include <vector>

namespace Durin
{
	class FGBufferRenderer;
	class FRendererResourceCoordinator;
	namespace RendererPrivate { class FSurfaceMaterialResources; }
	class FRHICommandListImmediate;
	struct FPreparedTerrainDraw;
	struct FPreparedTerrainBatch;
	struct FPreparedTerrainView;
	struct FRHIUniformBufferRange;
	struct FSceneView;
	enum class ERenderMode : uint8;
	enum class EStaticMeshBasePass : uint8;

	// Owns exact height/topology caches and Terrain shader draw submission.
	class FTerrainRenderer final
	{
	public:
		FTerrainRenderer(FRendererResourceCoordinator& InCoordinator, RendererPrivate::FSurfaceMaterialResources& InSurfaceMaterials);
		~FTerrainRenderer();

		auto PrepareResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			FPreparedTerrainView& View,
			bool bPrepareLitOpaqueForward
		) -> bool;
		auto PrepareHybridRetainedResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			FPreparedTerrainView& View
		) -> bool;
		auto PrepareShadowResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			FPreparedTerrainView& View
		) -> bool;
		auto ExecuteShadow_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& ShadowView,
			const FRHIUniformBufferRange& FallbackLighting,
			FPreparedTerrainView& View
		) -> void;
		auto ExecutePass_RenderThread(FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EStaticMeshBasePass Pass, FPreparedTerrainView& View) -> void;
		auto ExecutePreparedDraw_RenderThread(FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const FPreparedTerrainDraw& Draw, FPreparedTerrainView& View, bool bHybridRetained = false) -> void;
		auto FinalizeExecution_RenderThread(FPreparedTerrainView& View) -> void;
		auto ExecuteGBuffer_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& SceneView,
			FGBufferRenderer& GBuffer,
			FPreparedTerrainView& View
		) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		auto EnsureDrawResources_RenderThread(FRHICommandListImmediate& CommandList, FPreparedTerrainDraw& Draw, FPreparedTerrainView& View, bool bShadowDepth = false, bool bHybridRetained = false, bool bPrepareForwardPipeline = true) -> bool;
		auto Draw_RenderThread(FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const FPreparedTerrainDraw& Draw, bool bShadowDepth = false, std::span<const std::array<uint32, 2>> InstanceOrigins = {}, uint64* OutDynamicAllocationNanoseconds = nullptr, FGBufferRenderer* GBuffer = nullptr, bool bHybridRetained = false) -> bool;
		auto DrawBatch_RenderThread(FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const std::vector<FPreparedTerrainDraw>& Draws, const FPreparedTerrainBatch& Batch, bool bShadowDepth = false, uint64* OutDynamicAllocationNanoseconds = nullptr, FGBufferRenderer* GBuffer = nullptr) -> bool;
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		RendererPrivate::FSurfaceMaterialResources& SurfaceMaterials;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
